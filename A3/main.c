#include "montecarlo.h"
#include <inttypes.h>

// Simple circular buffer for the job queue
typedef struct {
    job_msg_t jobs[MAX_JOBS];
    int head;
    int count;
} job_queue_t;

static void queue_push(job_queue_t *q, const job_msg_t *job) {
    if (q->count >= MAX_JOBS) {
        fprintf(stderr, "Queue full!\n");
        exit(1);
    }
    int idx = (q->head + q->count) % MAX_JOBS;
    q->jobs[idx] = *job;
    q->count++;
}

static job_msg_t queue_pop(job_queue_t *q) {
    if (q->count <= 0) {
        fprintf(stderr, "Error: Queue underflow!\n");
        exit(1);
    }
    job_msg_t job = q->jobs[q->head];
    q->head = (q->head + 1) % MAX_JOBS;
    q->count--;
    return job;
}

// Helper to make random seeds for jobs
static uint32_t make_job_seed(uint32_t sim_type, uint32_t job_id) {
    return (sim_type * 1000) + (job_id * 777) + 12345; 
}

// Safe argument parser to prevent atoi crashes
static long parse_long(const char *str, long min, long max, const char *name) {
    char *end;
    long val = strtol(str, &end, 10);
    if (*end != '\0' || val < min || val > max) {
        fprintf(stderr, "Error: %s must be between %ld and %ld\n", name, min, max);
        exit(1);
    }
    return val;
}

// Send a job to a free worker
static int dispatch_to_worker(worker_info_t *worker, job_queue_t *queue) {
    if (!worker->alive || worker->busy || queue->count == 0) {
        return 0;
    }

    job_msg_t job = queue_pop(queue);
    
    if (write_all(worker->task_fd, &job, sizeof(job)) == -1) {
        // if write fails, put the job back so we don't lose it
        queue_push(queue, &job);
        return 0;
    }

    worker->busy = 1;
    worker->current_job = job.job_id;
    return 1;
}

void spawn_one_worker(worker_info_t *workers, int idx, int n_total) {
    int task_pipe[2];
    int result_pipe[2];

    // create pipes separately so we can clean up properly on failure
    if (pipe(task_pipe) == -1) {
        perror("pipe task");
        exit(1);
    }
    if (pipe(result_pipe) == -1) {
        perror("pipe result");
        close(task_pipe[0]);
        close(task_pipe[1]);
        exit(1);
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        close(task_pipe[0]);
        close(task_pipe[1]);
        close(result_pipe[0]);
        close(result_pipe[1]);
        exit(1);
    }

    if (pid == 0) { // Child
        close(task_pipe[1]);
        close(result_pipe[0]);

        // Close pipes belonging to other sibling workers so we don't hang
        for (int j = 0; j < n_total; j++) {
            if (j != idx) {
                if (workers[j].task_fd >= 0) close(workers[j].task_fd);
                if (workers[j].result_fd >= 0) close(workers[j].result_fd);
            }
        }

        worker_main(idx, task_pipe[0], result_pipe[1]);
        _exit(1); // Should never reach here
    }

    // Parent
    close(task_pipe[0]);
    close(result_pipe[1]);

    workers[idx].pid = pid;
    workers[idx].task_fd = task_pipe[1];
    workers[idx].result_fd = result_pipe[0];
    workers[idx].busy = 0;
    workers[idx].alive = 1;
    workers[idx].current_job = 0;
}

void run_jobs(int n_workers, int n_jobs, uint32_t trials, uint32_t sim_type) {
    worker_info_t workers[MAX_WORKERS];
    job_queue_t queue;
    result_msg_t results[MAX_JOBS];
    job_msg_t all_jobs[MAX_JOBS];
    int seen[MAX_JOBS] = {0};
    
    memset(&queue, 0, sizeof(queue));

    for (int i = 0; i < n_workers; i++) {
        workers[i].task_fd = -1;
        workers[i].result_fd = -1;
        workers[i].alive = 0;
        spawn_one_worker(workers, i, n_workers);
    }

    for (int i = 0; i < n_jobs; i++) {
        all_jobs[i].job_id = i + 1;
        all_jobs[i].trials = trials;
        all_jobs[i].seed = make_job_seed(sim_type, i + 1);
        all_jobs[i].sim_type = sim_type;
        queue_push(&queue, &all_jobs[i]);
    }

    for (int i = 0; i < n_workers; i++) {
        dispatch_to_worker(&workers[i], &queue);
    }

    int results_collected = 0;
    
    while (results_collected < n_jobs) {
        fd_set read_fds;
        FD_ZERO(&read_fds); // select modifies the set, zero it every time
        int max_fd = -1;

        for (int i = 0; i < n_workers; i++) {
            if (workers[i].alive && workers[i].busy) {
                FD_SET(workers[i].result_fd, &read_fds);
                if (workers[i].result_fd > max_fd) {
                    max_fd = workers[i].result_fd;
                }
            }
        }

        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) == -1) {
            if (errno == EINTR) continue; 
            perror("select");
            exit(1);
        }

        for (int i = 0; i < n_workers; i++) {
            if (workers[i].alive && workers[i].busy && FD_ISSET(workers[i].result_fd, &read_fds)) {
                result_msg_t res;
                int rc = read_all(workers[i].result_fd, &res, sizeof(res));
                
                    // Check for read errors, out-of-bounds job IDs, mismatched data, or duplicates
                    if (rc != 0 || 
                    res.job_id == 0 || res.job_id > (uint32_t)n_jobs ||
                    res.sim_type != sim_type || 
                    res.trials != trials || 
                    seen[res.job_id - 1] == 1) {
                    
                    fprintf(stderr, "Worker %d failed or sent bad data! Respawning...\n", i);
                    close(workers[i].task_fd);
                    close(workers[i].result_fd);
                    waitpid(workers[i].pid, NULL, 0); // reap the dead one
                    
                    uint32_t lost_job = workers[i].current_job;
                    queue_push(&queue, &all_jobs[lost_job - 1]); // put job back
                    
                    spawn_one_worker(workers, i, n_workers);
                    dispatch_to_worker(&workers[i], &queue);
                    continue;
                }

                // If it passes all checks, record it
                seen[res.job_id - 1] = 1;
                results[res.job_id - 1] = res;
                results_collected++;
                workers[i].busy = 0;
              
                dispatch_to_worker(&workers[i], &queue);
            }
        }
    }

    // Shut down workers cleanly by sending job_id 0
    job_msg_t shutdown_msg;
    memset(&shutdown_msg, 0, sizeof(shutdown_msg));
    
    for (int i = 0; i < n_workers; i++) {
        if (workers[i].alive) {
            if (write_all(workers[i].task_fd, &shutdown_msg, sizeof(shutdown_msg)) == -1) {
                fprintf(stderr, "Warning: shutdown write failed for worker %d\n", i);
            }
            close(workers[i].task_fd);
            close(workers[i].result_fd);
            waitpid(workers[i].pid, NULL, 0); // avoid zombies
        }
    }

    // combine all raw totals for the final estimate
    uint64_t total_aggregate = 0;
    uint64_t total_trials = 0;
    double true_value = 0.0;

    if (sim_type == SIM_PI) true_value = acos(-1.0);
    else if (sim_type == SIM_E) true_value = exp(1.0);
    else true_value = sqrt(2.0);

    printf("Simulation: ");
    if (sim_type == SIM_PI) printf("pi\n");
    else if (sim_type == SIM_E) printf("e\n");
    else printf("sqrt(2)\n");

    printf("Workers: %d, Jobs: %d, Trials/job: %u\n\n", n_workers, n_jobs, trials);
    printf("%-5s %-7s %s\n", "Job", "Worker", "Estimate");

    for (int i = 0; i < n_jobs; i++) {
        total_aggregate += results[i].aggregate_value;
        total_trials += results[i].trials;
        printf("%-5u %-7u %.6f\n",
            results[i].job_id,
            results[i].worker_id,
            results[i].estimate);
    }

    double final_estimate = 0.0;
    if (sim_type == SIM_PI) final_estimate = 4.0 * (double)total_aggregate / (double)total_trials;
    else if (sim_type == SIM_E) final_estimate = (double)total_aggregate / (double)total_trials;
    else final_estimate = 2.0 * (double)total_aggregate / (double)total_trials;

    printf("\nTotal Trials: %" PRIu64 "\n", total_trials);
    printf("Total Aggregate: %" PRIu64 "\n", total_aggregate);
    printf("Final Combined Estimate: %.8f\n", final_estimate);
    printf("True Value: %.8f\n", true_value);
    printf("Absolute Error: %.8f\n", fabs(final_estimate - true_value));

int main(int argc, char *argv[]) {
    int n_workers = DEFAULT_WORKERS;
    uint32_t trials = DEFAULT_TRIALS;
    int n_jobs = -1; // -1 so we can set default based on workers later
    uint32_t sim_type = SIM_PI;

    signal(SIGPIPE, SIG_IGN);

    int opt;
    while ((opt = getopt(argc, argv, "w:t:j:s:")) != -1) {
        switch (opt) {
            case 'w': n_workers = parse_long(optarg, 1, MAX_WORKERS, "workers"); break;
            case 't': trials = parse_long(optarg, 1, 1000000000, "trials"); break;
            case 'j': n_jobs = parse_long(optarg, 1, MAX_JOBS, "jobs"); break;
            case 's': sim_type = parse_long(optarg, 0, 2, "sim_type"); break;
            default:
                fprintf(stderr, "Usage: %s [-w workers] [-t trials] [-j jobs] [-s sim(0=pi,1=e,2=sqrt2)]\n", argv[0]);
                exit(1);
        }
    }

    if (n_jobs == -1) n_jobs = n_workers * 3;
    printf("Starting Monte Carlo Simulator...\n");
    run_jobs(n_workers, n_jobs, trials, sim_type);
    return 0;
}
