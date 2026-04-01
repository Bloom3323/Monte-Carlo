// worker.c - child process logic
// each worker sits in a loop reading job_msg_t from its task pipe,
// runs the requested simulation, and writes a result_msg_t back

#include "montecarlo.h"

void worker_main(int worker_id, int task_read_fd, int result_write_fd) {
    job_msg_t job;
    result_msg_t result;

    while (1) {
        int rc = read_all(task_read_fd, &job, sizeof(job));

        if (rc == 1) break;  // parent closed pipe
        if (rc < 0) {
            fprintf(stderr, "worker %d: read error: %s\n",
                    worker_id, strerror(errno));
            break;
        }

        // job_id 0 = time to shut down
        if (job.job_id == 0) break;

        uint32_t hits = 0;
        double est = run_simulation(job.sim_type, job.trials, job.seed, &hits);

        // send result back
        memset(&result, 0, sizeof(result));
        result.job_id = job.job_id;
        result.worker_id = (uint32_t)worker_id;
        result.trials = job.trials;
        result.hits = hits;
        result.estimate = est;

        if (write_all(result_write_fd, &result, sizeof(result)) < 0) {
            fprintf(stderr, "worker %d: write error: %s\n",
                    worker_id, strerror(errno));
            break;
        }
    }

    close(task_read_fd);
    close(result_write_fd);
    exit(0);
}
