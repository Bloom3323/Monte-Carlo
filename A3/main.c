// main.c - parent/controller process
// spawns a pool of workers, distributes simulation jobs over pipes,
// collects results using select(), and aggregates the final estimate

#include "montecarlo.h"

// circular job queue - holds jobs waiting to be dispatched to workers
typedef struct {
    job_msg_t jobs[MAX_JOBS];
    int head;
    int count;
} job_queue_t;

static void spawn_one_worker(worker_info_t *workers, int idx, int n_total);
static void spawn_workers(worker_info_t *workers, int n);
static void run_jobs(worker_info_t *workers, int n_workers,
                     int n_jobs, uint32_t trials, uint32_t sim_type);
static void shutdown_workers(worker_info_t *workers, int n_workers);
static void reap_workers(worker_info_t *workers, int n_workers);
static void print_usage(const char *prog);
static long parse_long(const char *str, const char *name, long lo, long hi);

int main(int argc, char *argv[]) {
    int n_workers = DEFAULT_WORKERS;
    uint32_t trials = DEFAULT_TRIALS;
    int n_jobs = -1;
    uint32_t sim_type = SIM_PI;
    int run_all = 0;
    int opt;

    // ignore SIGPIPE so broken pipes give us EPIPE instead of killing us
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    while ((opt = getopt(argc, argv, "w:t:j:s:h")) != -1) {
        switch (opt) {
        case 'w':
            n_workers = (int)parse_long(optarg, "workers", 1, MAX_WORKERS);
            break;
        case 't':
            trials = (uint32_t)parse_long(optarg, "trials", 1, UINT32_MAX);
            break;
        case 'j':
            n_jobs = (int)parse_long(optarg, "jobs", 1, MAX_JOBS);
            break;
        case 's': {
            long s = parse_long(optarg, "sim_type", 0, SIM_COUNT);
            if (s == SIM_COUNT) {
                run_all = 1;
            } else {
                sim_type = (uint32_t)s;
            }
            break;
        }
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (n_jobs < 0) n_jobs = n_workers * 3;

    printf("=== Monte Carlo Parallel Estimator ===\n");
    printf("Workers: %d, Trials per job: %u, Jobs: %d, Total trials: %lu\n\n",
           n_workers, trials, n_jobs, (unsigned long)trials * n_jobs);

    if (run_all) {
        for (uint32_t s = 0; s < SIM_COUNT; s++) {
            worker_info_t workers[MAX_WORKERS];
            memset(workers, 0, sizeof(workers));

            printf("--- %s ---\n\n", sim_type_name(s));
            spawn_workers(workers, n_workers);
            run_jobs(workers, n_workers, n_jobs, trials, s);
            shutdown_workers(workers, n_workers);
            reap_workers(workers, n_workers);
            printf("\n");
        }
    } else {
        worker_info_t workers[MAX_WORKERS];
        memset(workers, 0, sizeof(workers));

        printf("--- %s ---\n\n", sim_type_name(sim_type));
        spawn_workers(workers, n_workers);
        run_jobs(workers, n_workers, n_jobs, trials, sim_type);
        shutdown_workers(workers, n_workers);
        reap_workers(workers, n_workers);
    }

    return 0;
}

// parse a long from string with bounds checking (replaces atoi)
static long parse_long(const char *str, const char *name, long lo, long hi) {
    char *end;
    errno = 0;
    long val = strtol(str, &end, 10);
    if (errno != 0 || *end != '\0' || end == str) {
        fprintf(stderr, "error: invalid value for %s: '%s'\n", name, str);
        exit(1);
    }
    if (val < lo || val > hi) {
        fprintf(stderr, "error: %s must be between %ld and %ld\n", name, lo, hi);
        exit(1);
    }
    return val;
}

// spawns a single worker at index idx
// needs n_total so child can close fds from siblings
static void spawn_one_worker(worker_info_t *workers, int idx, int n_total) {
    int task_pipe[2];
    int result_pipe[2];

    if (pipe(task_pipe) < 0) {
        perror("pipe");
        exit(1);
    }
    if (pipe(result_pipe) < 0) {
        perror("pipe");
        close(task_pipe[0]);
        close(task_pipe[1]);
        exit(1);
    }

    fflush(stdout);
    fflush(stderr);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }

    if (pid == 0) {
        // child - close the ends we dont need
        close(task_pipe[1]);
        close(result_pipe[0]);

        // close fds from other workers so EOF works right
        for (int j = 0; j < n_total; j++) {
            if (j == idx) continue;
            if (workers[j].task_fd >= 0) close(workers[j].task_fd);
            if (workers[j].result_fd >= 0) close(workers[j].result_fd);
        }

        worker_main(idx, task_pipe[0], result_pipe[1]);
        _exit(1); // shouldnt get here
    }

    // parent side
    close(task_pipe[0]);
    close(result_pipe[1]);

    workers[idx].pid = pid;
    workers[idx].task_fd = task_pipe[1];
    workers[idx].result_fd = result_pipe[0];
    workers[idx].busy = 0;
    workers[idx].alive = 1;
    workers[idx].current_job = 0;

    printf("spawned worker %d (pid %d)\n", idx, pid);
}

static void spawn_workers(worker_info_t *workers, int n) {
    // initialize all fds to -1 first so spawn_one_worker's
    // sibling-close loop doesnt close garbage fds
    for (int i = 0; i < n; i++) {
        workers[i].task_fd = -1;
        workers[i].result_fd = -1;
        workers[i].alive = 0;
    }
    for (int i = 0; i < n; i++) {
        spawn_one_worker(workers, i, n);
    }
    printf("\n");
}

// push a job onto the back of the queue
static void queue_push(job_queue_t *q, const job_msg_t *job) {
    int idx = (q->head + q->count) % MAX_JOBS;
    q->jobs[idx] = *job;
    q->count++;
}

// pop a job from the front of the queue
static job_msg_t queue_pop(job_queue_t *q) {
    job_msg_t job = q->jobs[q->head];
    q->head = (q->head + 1) % MAX_JOBS;
    q->count--;
    return job;
}

// try to send a job from the queue to a specific worker
// returns 1 if a job was sent, 0 if queue empty
static int dispatch_to_worker(worker_info_t *w, int widx, job_queue_t *q) {
    if (q->count == 0) return 0;
    if (!w->alive || w->busy) return 0;

    job_msg_t job = queue_pop(q);

    if (write_all(w->task_fd, &job, sizeof(job)) < 0) {
        fprintf(stderr, "failed to send job %u to worker %d: %s\n",
                job.job_id, widx, strerror(errno));
        // put the job back so we can retry with another worker
        queue_push(q, &job);
        return 0;
    }

    w->busy = 1;
    w->current_job = job.job_id;
    return 1;
}

// the main select loop - uses select() to multiplex across worker result pipes.
// dispatches jobs dynamically and collects results as workers finish.
// if a worker crashes mid-job, we respawn it and requeue the lost job.
static void run_jobs(worker_info_t *workers, int n_workers,
                     int n_jobs, uint32_t trials, uint32_t sim_type) {
    srand((unsigned int)time(NULL));

    // build all the jobs upfront and put them in the queue
    job_queue_t queue;
    memset(&queue, 0, sizeof(queue));

    for (int j = 0; j < n_jobs; j++) {
        job_msg_t job;
        memset(&job, 0, sizeof(job));
        job.job_id = (uint32_t)(j + 1);
        job.trials = trials;
        job.seed = (uint32_t)rand();
        job.sim_type = sim_type;
        queue_push(&queue, &job);
    }

    // send initial batch - one job per worker (or fewer if less jobs than workers)
    printf("dispatching %d jobs across %d workers...\n", n_jobs, n_workers);
    for (int i = 0; i < n_workers; i++) {
        dispatch_to_worker(&workers[i], i, &queue);
    }

    // collect results using select
    int results_collected = 0;
    double total_estimate = 0.0;
    uint64_t total_trials = 0;
    uint64_t total_hits = 0;

    // store results so we can print them in order at the end
    result_msg_t results[MAX_JOBS];
    int got_result[MAX_JOBS];
    memset(got_result, 0, sizeof(got_result));

    printf("collecting results...\n\n");

    while (results_collected < n_jobs) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        int max_fd = -1;

        // add all busy workers' result pipes to the fd set
        for (int i = 0; i < n_workers; i++) {
            if (workers[i].alive && workers[i].busy) {
                FD_SET(workers[i].result_fd, &read_fds);
                if (workers[i].result_fd > max_fd) {
                    max_fd = workers[i].result_fd;
                }
            }
        }

        if (max_fd < 0) {
            // no busy workers but still have jobs... shouldn't happen
            // unless all workers died. try to dispatch remaining jobs
            int dispatched = 0;
            for (int i = 0; i < n_workers; i++) {
                if (dispatch_to_worker(&workers[i], i, &queue)) {
                    dispatched = 1;
                }
            }
            if (!dispatched) {
                fprintf(stderr, "error: no workers available and %d jobs remain\n",
                        n_jobs - results_collected);
                exit(1);
            }
            continue;
        }

        int ready = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            exit(1);
        }

        // check which workers have results ready
        for (int i = 0; i < n_workers; i++) {
            if (!workers[i].alive || !workers[i].busy) continue;
            if (!FD_ISSET(workers[i].result_fd, &read_fds)) continue;

            result_msg_t result;
            int rc = read_all(workers[i].result_fd, &result, sizeof(result));

            if (rc != 0) {
                // worker died or pipe broke
                fprintf(stderr, "worker %d failed (pid %d): %s\n",
                        i, workers[i].pid,
                        rc == 1 ? "unexpected EOF" : strerror(errno));

                // close old fds
                close(workers[i].task_fd);
                close(workers[i].result_fd);
                workers[i].task_fd = -1;
                workers[i].result_fd = -1;
                workers[i].alive = 0;

                // reap the dead child
                int status;
                waitpid(workers[i].pid, &status, 0);
                if (WIFSIGNALED(status)) {
                    fprintf(stderr, "  killed by signal %d\n", WTERMSIG(status));
                } else if (WIFEXITED(status)) {
                    fprintf(stderr, "  exited with status %d\n", WEXITSTATUS(status));
                }

                // requeue the job that was lost
                uint32_t lost_job = workers[i].current_job;
                if (lost_job > 0) {
                    fprintf(stderr, "  requeuing job %u\n", lost_job);
                    job_msg_t retry;
                    memset(&retry, 0, sizeof(retry));
                    retry.job_id = lost_job;
                    retry.trials = trials;
                    retry.seed = (uint32_t)rand(); // new seed since old one is gone
                    retry.sim_type = sim_type;
                    queue_push(&queue, &retry);
                }

                workers[i].busy = 0;
                workers[i].current_job = 0;

                // try to respawn the worker
                fprintf(stderr, "  respawning worker %d...\n", i);
                spawn_one_worker(workers, i, n_workers);

                // give the new worker a job if there is one
                dispatch_to_worker(&workers[i], i, &queue);
                continue;
            }

            // got a good result
            int jidx = (int)result.job_id - 1;
            if (jidx >= 0 && jidx < n_jobs) {
                results[jidx] = result;
                got_result[jidx] = 1;
            }
            results_collected++;

            total_estimate += result.estimate;
            total_trials += result.trials;
            total_hits += result.hits;

            workers[i].busy = 0;
            workers[i].current_job = 0;

            // send next job to this worker since its now free
            dispatch_to_worker(&workers[i], i, &queue);
        }
    }

    // print results in job order for nice output
    printf("Job\tWorker\tTrials\t\tHits\t\tEstimate\n");
    printf("---\t------\t------\t\t----\t\t--------\n");
    for (int j = 0; j < n_jobs; j++) {
        if (got_result[j]) {
            printf("%u\t%u\t%u\t\t%u\t\t%.8f\n",
                   results[j].job_id, results[j].worker_id,
                   results[j].trials, results[j].hits, results[j].estimate);
        } else {
            printf("%d\t?\t?\t\t?\t\t? (missing)\n", j + 1);
        }
    }

    double avg = total_estimate / n_jobs;
    double true_val = sim_true_value(sim_type);
    double abs_err = fabs(avg - true_val);
    double rel_err = (true_val != 0.0) ? (abs_err / true_val * 100.0) : 0.0;

    printf("\n--- Results: %s ---\n", sim_type_name(sim_type));
    printf("Total trials:     %lu\n", (unsigned long)total_trials);
    printf("Total hits:       %lu\n", (unsigned long)total_hits);
    printf("Avg estimate:     %.10f\n", avg);
    printf("True value:       %.10f\n", true_val);
    printf("Absolute error:   %.10f\n", abs_err);
    printf("Relative error:   %.6f%%\n", rel_err);
    printf("---\n");
}

// sends shutdown signal (job_id=0) to all live workers, then closes task pipes
static void shutdown_workers(worker_info_t *workers, int n_workers) {
    job_msg_t shutdown_msg;
    memset(&shutdown_msg, 0, sizeof(shutdown_msg));
    shutdown_msg.job_id = 0;

    for (int i = 0; i < n_workers; i++) {
        if (!workers[i].alive) continue;
        if (workers[i].task_fd >= 0) {
            write_all(workers[i].task_fd, &shutdown_msg, sizeof(shutdown_msg));
            close(workers[i].task_fd);
            workers[i].task_fd = -1;
        }
    }
}

static void reap_workers(worker_info_t *workers, int n_workers) {
    for (int i = 0; i < n_workers; i++) {
        if (!workers[i].alive) continue;

        int status;
        pid_t p = waitpid(workers[i].pid, &status, 0);
        if (p < 0) {
            fprintf(stderr, "waitpid failed for worker %d: %s\n", i, strerror(errno));
        } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            fprintf(stderr, "worker %d exited with status %d\n", i, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "worker %d killed by signal %d\n", i, WTERMSIG(status));
        }
        if (workers[i].result_fd >= 0) {
            close(workers[i].result_fd);
            workers[i].result_fd = -1;
        }
        workers[i].alive = 0;
    }
}

static void print_usage(const char *prog) {
    printf("Usage: %s [-w workers] [-t trials] [-j jobs] [-s sim_type]\n\n", prog);
    printf("  -w workers    number of worker processes (1-%d, default %d)\n",
           MAX_WORKERS, DEFAULT_WORKERS);
    printf("  -t trials     trials per job (default %d)\n", DEFAULT_TRIALS);
    printf("  -j jobs       total jobs to run (default workers*3)\n");
    printf("  -s sim_type   0=pi, 1=e, 2=sqrt(2), 3=all (default 0)\n");
    printf("  -h            show this message\n");
}
