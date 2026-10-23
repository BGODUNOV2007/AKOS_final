#define _GNU_SOURCE

#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

/* Глобальный мьютекс для атомарного вывода строк */
pthread_mutex_t g_print_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Дескриптор лог-файла (NULL = лог отключён) */
FILE *g_logfile = NULL;

/*
 * safe_printf — потокобезопасная запись форматированной строки.
 * Захватывает g_print_mutex, пишет в stdout (и дублирует в g_logfile),
 * сбрасывает буферы и освобождает мьютекс.
 */
void safe_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    pthread_mutex_lock(&g_print_mutex);
    vprintf(fmt, args);
    fflush(stdout);

    /* Дублируем вывод в лог-файл, если он открыт */
    if (g_logfile) {
        va_end(args);
        va_start(args, fmt);
        vfprintf(g_logfile, fmt, args);
        fflush(g_logfile);
    }

    pthread_mutex_unlock(&g_print_mutex);

    va_end(args);
}

/*
 * fibonacci — итеративное вычисление числа Фибоначчи.
 * Диапазон: [0, 92] (fib(92) = 7540113804746346429 помещается в int64_t).
 */
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

/*
 * rand_range — возвращает случайное число в диапазоне [min, max].
 * Если min >= max, возвращает min.
 * Использует rand_r для потокобезопасности (каждый поток имеет свой seed).
 */
int rand_range(unsigned int *seed, int min, int max)
{
    if (min >= max) return min;
    return min + (int)(rand_r(seed) % (unsigned int)(max - min + 1));
}

/*
 * random_sleep_ms — усыпляет текущий поток на случайное время [min_ms, max_ms].
 * Реализовано через nanosleep для совместимости с POSIX.
 */
void random_sleep_ms(unsigned int *seed, int min_ms, int max_ms)
{
    int ms = rand_range(seed, min_ms, max_ms);
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
