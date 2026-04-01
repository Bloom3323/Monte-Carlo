/*
 * montecarlo.h - shared definitions for the parallel monte carlo estimator
 * defines message structs, worker info, and function prototypes
 */
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
#include <limits.h>

#define MAX_WORKERS     8
#define DEFAULT_WORKERS 4
#define DEFAULT_TRIALS  1000000
#define MAX_JOBS        64

// message sent from parent to worker (fixed-size binary struct)
// job_id of 0 is a special shutdown signal
typedef struct {
    uint32_t job_id;    // 0 means shutdown
    uint32_t trials;
    uint32_t seed;
    uint32_t sim_type;  // 0=pi, 1=e, 2=sqrt2
} job_msg_t;

// result sent back from worker to parent (fixed-size binary struct)
typedef struct {
    uint32_t job_id;
    uint32_t worker_id;
    uint32_t trials;
    uint32_t hits;
    double estimate;
} result_msg_t;

// keeps track of each worker process
typedef struct {
    pid_t pid;
    int task_fd;    // write end (parent sends tasks here)
    int result_fd;  // read end (parent reads results here)
    int busy;       // 1 if worker has an outstanding job
    int alive;      // 1 if worker process is running
    uint32_t current_job;  // job_id currently assigned (for requeue on crash)
} worker_info_t;

// worker.c
void worker_main(int worker_id, int task_read_fd, int result_write_fd);

// simulate.c
double run_simulation(uint32_t sim_type, uint32_t trials,
                      uint32_t seed, uint32_t *hits_out);
const char *sim_type_name(uint32_t sim_type);
double sim_true_value(uint32_t sim_type);

// protocol.c - handles partial reads/writes so we always get full messages
int write_all(int fd, const void *buf, size_t len);
int read_all(int fd, void *buf, size_t len);

#define SIM_PI    0
#define SIM_E     1
#define SIM_SQRT2 2
#define SIM_COUNT 3

#endif
