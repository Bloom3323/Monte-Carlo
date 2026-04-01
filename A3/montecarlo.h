#ifndef MONTECARLO_H
#define MONTECARLO_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <signal.h>

#define MAX_WORKERS     8
#define DEFAULT_WORKERS 4
#define DEFAULT_TRIALS  1000000U
#define MAX_JOBS        64

#define SIM_PI    0U
#define SIM_E     1U
#define SIM_SQRT2 2U
#define SIM_COUNT 3U

// Message sent to worker. job_id 0 means shutdown.
typedef struct {
    uint32_t job_id;
    uint32_t trials;
    uint32_t seed;
    uint32_t sim_type;
} job_msg_t;

// Result sent back to parent
typedef struct {
    uint32_t job_id;
    uint32_t worker_id;
    uint32_t sim_type;
    uint32_t trials;
    uint64_t aggregate_value;
    double estimate;
} result_msg_t;

// Track state of each child process
typedef struct {
    pid_t pid;
    int task_fd;
    int result_fd;
    int busy;
    int alive;
    uint32_t current_job;
} worker_info_t;

// Prototypes for worker and math functions
void worker_main(int worker_id, int task_read_fd, int result_write_fd);
double run_simulation(uint32_t sim_type, uint32_t trials, uint32_t seed, uint64_t *agg_out);
int write_all(int fd, const void *buf, size_t len);
int read_all(int fd, void *buf, size_t len);

#endif
