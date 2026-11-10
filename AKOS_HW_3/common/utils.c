#define _GNU_SOURCE
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <sys/time.h>
#include <string.h>
#include <errno.h>

uint64_t calculate_fibonacci(int n) {
    if (n <= 0) return 0;
    if (n == 1) return 1;
    // Ограничиваем n до 90 во избежание 64-битного переполнения
    if (n > 90) {
        n = (n % 45) + 1;
    }
    uint64_t a = 0;
    uint64_t b = 1;
    for (int i = 2; i <= n; i++) {
        uint64_t c = a + b;
        a = b;
        b = c;
    }
    return b;
}

int is_array_sorted(const int *arr, int size) {
    for (int i = 0; i < size - 1; i++) {
        if (arr[i] > arr[i + 1]) {
            return 0;
        }
    }
    return 1;
}

static int compare_ints(const void *a, const void *b) {
    int arg1 = *(const int *)a;
    int arg2 = *(const int *)b;
    if (arg1 < arg2) return -1;
    if (arg1 > arg2) return 1;
    return 0;
}

void sort_array(int *arr, int size) {
    qsort(arr, size, sizeof(int), compare_ints);
}

int random_range(unsigned int *seedp, int min_val, int max_val) {
    if (min_val >= max_val) return min_val;
    int r = rand_r(seedp);
    return min_val + (r % (max_val - min_val + 1));
}

uint64_t current_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

void sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

void get_timestamp_str(char *buf, size_t buflen) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_info;
    localtime_r(&tv.tv_sec, &tm_info);
    snprintf(buf, buflen, "[%02d:%02d:%02d.%03ld]",
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec, tv.tv_usec / 1000);
}

void format_array(char *buf, size_t buflen, const int *arr, int size) {
    size_t offset = 0;
    offset += snprintf(buf + offset, buflen - offset, "[");
    for (int i = 0; i < size; i++) {
        if (i > 0) {
            offset += snprintf(buf + offset, buflen - offset, ", ");
        }
        if (offset >= buflen - 16) {
            offset += snprintf(buf + offset, buflen - offset, "... (%d items)]", size);
            return;
        }
        offset += snprintf(buf + offset, buflen - offset, "%d", arr[i]);
    }
    snprintf(buf + offset, buflen - offset, "]");
}

void safe_log(int log_fd, sem_t *print_sem, const char *fmt, ...) {
    char time_str[32];
    get_timestamp_str(time_str, sizeof(time_str));

    char msg_buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    int locked = 0;
    if (print_sem != NULL) {
        while (1) {
            if (sem_wait(print_sem) == 0) {
                locked = 1;
                break;
            }
            if (errno != EINTR) break;
        }
    }

    // Вывод в стандартный вывод через системный дескриптор файла STDOUT_FILENO
    dprintf(STDOUT_FILENO, "%s %s\n", time_str, msg_buf);

    // При наличии лог-файла — запись в дескриптор лог-файла
    if (log_fd >= 0) {
        dprintf(log_fd, "%s %s\n", time_str, msg_buf);
    }

    if (print_sem != NULL && locked) {
        sem_post(print_sem);
    }
}

void init_sorted_array(int *arr, int size, int min_val, int max_val, unsigned int *seedp) {
    for (int i = 0; i < size; i++) {
        arr[i] = random_range(seedp, min_val, max_val);
    }
    sort_array(arr, size);
}

void setup_initial_array(int *arr, const sim_config_t *cfg, unsigned int *seedp) {
    if (cfg->has_init_array) {
        for (int i = 0; i < cfg->array_size; i++) {
            arr[i] = cfg->init_array[i];
        }
        sort_array(arr, cfg->array_size);
    } else {
        init_sorted_array(arr, cfg->array_size, cfg->min_val, cfg->max_val, seedp);
    }
}
