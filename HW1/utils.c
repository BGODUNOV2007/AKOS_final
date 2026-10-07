#define _GNU_SOURCE

#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;

void safe_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    pthread_mutex_lock(&print_mutex);
    vprintf(fmt, args);
    fflush(stdout);
    pthread_mutex_unlock(&print_mutex);

    va_end(args);
}

int64_t fibonacci(int n)
{

    if (n < 0) n = -n;
    if (n > 92) n = 92;

    if (n <= 1) return n;

    int64_t a = 0, b = 1;
    for (int i = 2; i <= n; i++) {
        int64_t tmp = a + b;
        a = b;
        b = tmp;
    }
    return b;
}

int rand_range(unsigned int *seed, int min, int max)
{
    if (min >= max) return min;
    return min + (int)(rand_r(seed) % (max - min + 1));
}

void random_sleep_ms(unsigned int *seed, int min_ms, int max_ms)
{
    int ms = rand_range(seed, min_ms, max_ms);
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
