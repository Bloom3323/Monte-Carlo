// simulate.c - the actual monte carlo simulations
// each function takes a trial count and seed, returns an estimate
// uses rand_r() so each worker has independent reproducible randomness

#include "montecarlo.h"

// estimates pi by throwing random points at a quarter circle
static double estimate_pi(uint32_t trials, uint32_t seed, uint32_t *hits_out) {
    unsigned int rng = seed;
    uint32_t hits = 0;
    for (uint32_t i = 0; i < trials; i++) {
        double x = (double)rand_r(&rng) / RAND_MAX;
        double y = (double)rand_r(&rng) / RAND_MAX;
        if (x*x + y*y <= 1.0) hits++;
    }
    *hits_out = hits;
    return 4.0 * (double)hits / (double)trials;
}

// estimates e using the random sum thing
// keep adding random numbers until sum > 1, count how many it takes
// the average number of draws should converge to e
static double estimate_e(uint32_t trials, uint32_t seed, uint32_t *hits_out) {
    unsigned int rng = seed;
    uint64_t total_draws = 0;

    for (uint32_t i = 0; i < trials; i++) {
        double sum = 0.0;
        uint32_t draws = 0;
        while (sum < 1.0) {
            sum += (double)rand_r(&rng) / RAND_MAX;
            draws++;
        }
        total_draws += draws;
    }
    *hits_out = (uint32_t)(total_draws & 0xFFFFFFFF);
    return (double)total_draws / (double)trials;
}

// estimates sqrt(2)
// generate x in [0,2], check if x^2 < 2
// fraction of hits = sqrt(2)/2, so multiply by 2
static double estimate_sqrt2(uint32_t trials, uint32_t seed, uint32_t *hits_out) {
    unsigned int rng = seed;
    uint32_t hits = 0;
    for (uint32_t i = 0; i < trials; i++) {
        double x = 2.0 * (double)rand_r(&rng) / RAND_MAX;
        if (x * x < 2.0) hits++;
    }
    *hits_out = hits;
    return 2.0 * (double)hits / (double)trials;
}

double run_simulation(uint32_t sim_type, uint32_t trials,
                      uint32_t seed, uint32_t *hits_out) {
    switch (sim_type) {
    case SIM_PI:    return estimate_pi(trials, seed, hits_out);
    case SIM_E:     return estimate_e(trials, seed, hits_out);
    case SIM_SQRT2: return estimate_sqrt2(trials, seed, hits_out);
    default:
        fprintf(stderr, "unknown sim type: %u\n", sim_type);
        *hits_out = 0;
        return 0.0;
    }
}

const char *sim_type_name(uint32_t sim_type) {
    switch (sim_type) {
    case SIM_PI:    return "Estimate pi";
    case SIM_E:     return "Estimate e";
    case SIM_SQRT2: return "Estimate sqrt(2)";
    default:        return "Unknown";
    }
}

double sim_true_value(uint32_t sim_type) {
    switch (sim_type) {
    case SIM_PI:    return M_PI;
    case SIM_E:     return M_E;
    case SIM_SQRT2: return sqrt(2.0);
    default:        return 0.0;
    }
}
