#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <stddef.h>
#include <semaphore.h>
#include "types.h"

// Вычисление n-го числа Фибоначчи
uint64_t calculate_fibonacci(int n);

// Проверка отсортированности массива
int is_array_sorted(const int *arr, int size);

// Сортировка массива (qsort)
void sort_array(int *arr, int size);

// Генерация случайного числа в диапазоне [min_val, max_val] с использованием локального seed
int random_range(unsigned int *seedp, int min_val, int max_val);

// Текущее монотонное время в микросекундах
uint64_t current_time_us(void);

// Задержка в миллисекундах
void sleep_ms(int ms);

// Форматирование текущего времени в строку [HH:MM:SS.mmm]
void get_timestamp_str(char *buf, size_t buflen);

// Форматирование массива в строку вида "[1, 3, 5, 8, 12]"
void format_array(char *buf, size_t buflen, const int *arr, int size);

// Безопасный логгер через системные вызовы / дескрипторы файлов (dprintf)
// Защищен семафором print_sem для исключения перемешивания строк процессов
void safe_log(int log_fd, sem_t *print_sem, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

// Инициализация начального массива случайными отсортированными числами
void init_sorted_array(int *arr, int size, int min_val, int max_val, unsigned int *seedp);

// Заполнение массива из конфигурации (пользовательского или случайного) с сортировкой
void setup_initial_array(int *arr, const sim_config_t *cfg, unsigned int *seedp);

#endif // UTILS_H
