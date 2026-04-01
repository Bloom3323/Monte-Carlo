#include "montecarlo.h"

// --- PIPE HELPERS ---
// Need these because read/write don't always grab all bytes at once
int write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        remaining -= n;
    }
    return 0;
}

int read_all(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    size_t remaining = len;
    int first = 1;

    while (remaining > 0) {
        ssize_t n = read(fd, p, remaining);
        if (n == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            if (first) return 1; // clean EOF
            return -1; // cut off halfway
        }
        first = 0;
        p += n;
        remaining -= n;
    }
    return 0;
}

// --- MATH SIMULATIONS ---
static double uniform_01(unsigned int *rng) {
    return (double)rand_r(rng) / ((double)RAND_MAX + 1.0);
}

double run_simulation(uint32_t sim_type, uint32_t trials, uint32_t seed, uint64_t *agg_out) {
    unsigned int rng = seed;
    uint64_t hits = 0;
    
    if (sim_type == SIM_PI) {
        for (uint32_t i = 0; i < trials; i++) {
            double x = uniform_01(&rng);
            double y = uniform_01(&rng);
            if (x * x + y * y <= 1.0) hits++;
        }
        *agg_out = hits;
        return 4.0 * (double)hits / (double)trials;
        
    } else if (sim_type == SIM_E) {
        uint64_t total_draws = 0;
        for (uint32_t i = 0; i < trials; i++) {
            double sum = 0.0;
            uint32_t draws = 0;
            while (sum <= 1.0) {
                sum += uniform_01(&rng);
                draws++;
            }
            total_draws += draws;
        }
        *agg_out = total_draws;
        return (double)total_draws / (double)trials;
        
    } else { // SIM_SQRT2
        for (uint32_t i = 0; i < trials; i++) {
            double x = 2.0 * uniform_01(&rng);
            if (x * x < 2.0) hits++;
        }
        *agg_out = hits;
        return 2.0 * (double)hits / (double)trials;
    }
}

// --- CHILD PROCESS MAIN LOOP ---
void worker_main(int worker_id, int task_read_fd, int result_write_fd) {
    job_msg_t job;
    result_msg_t result;

    while (1) {
        int rc = read_all(task_read_fd, &job, sizeof(job));
        
        // break if pipe closed or error
        if (rc == 1 || rc == -1) {
            break;
        }
        
        // job id 0 is our shutdown signal from the parent
        if (job.job_id == 0) {
            break;
        }

        result.job_id = job.job_id;
        result.worker_id = worker_id;
        result.sim_type = job.sim_type;
        result.trials = job.trials;
        result.estimate = run_simulation(job.sim_type, job.trials, job.seed, &result.aggregate_value);

        if (write_all(result_write_fd, &result, sizeof(result)) == -1) {
            break; 
        }
    }

    // exit cleanly
    close(task_read_fd);
    close(result_write_fd);
    
    // use _exit in the child
    _exit(0);
}
