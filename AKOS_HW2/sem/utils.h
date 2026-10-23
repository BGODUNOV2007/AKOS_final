#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <stdio.h>
#include <pthread.h>

/*
 * Глобальный мьютекс для потокобезопасного вывода в stdout и лог-файл.
 * Объявлен здесь как extern, определён в utils.c.
 */
extern pthread_mutex_t g_print_mutex;

/*
 * Дескриптор лог-файла. Если NULL — запись в файл отключена.
 * Устанавливается в main() до запуска потоков.
 */
extern FILE *g_logfile;

/*
 * Потокобезопасный printf: атомарно записывает строку в stdout
 * и, если открыт лог-файл, дублирует вывод туда.
 */
void safe_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/*
 * Вычисление числа Фибоначчи для n.
 * Если n < 0, берётся |n|. При n > 92 возвращается fib(92),
 * чтобы избежать переполнения int64_t.
 */
int64_t fibonacci(int n);

/*
 * Генерация псевдослучайного целого числа в диапазоне [min, max]
 * с использованием потокобезопасного rand_r (seed — per-thread).
 */
int rand_range(unsigned int *seed, int min, int max);

/*
 * Пауза текущего потока на случайное количество миллисекунд
 * в диапазоне [min_ms, max_ms] (реализована через nanosleep).
 */
void random_sleep_ms(unsigned int *seed, int min_ms, int max_ms);

#endif /* UTILS_H */
