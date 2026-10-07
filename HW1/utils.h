#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <pthread.h>

void safe_printf(const char *fmt, ...);

int64_t fibonacci(int n);

int rand_range(unsigned int *seed, int min, int max);

void random_sleep_ms(unsigned int *seed, int min_ms, int max_ms);

#endif
