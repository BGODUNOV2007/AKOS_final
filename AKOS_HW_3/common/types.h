#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#define MAX_ARRAY_SIZE 1024
#define MAX_WORKERS    128

typedef enum {
    POLICY_READERS = 0, // Приоритет читателей (writers starve)
    POLICY_WRITERS = 1, // Приоритет писателей (readers starve)
    POLICY_FAIR    = 2  // Справедливая очередь (FIFO turnstile)
} access_policy_t;

typedef struct {
    int num_readers;        // Количество читателей (-r, по умолч. 3)
    int num_writers;        // Количество писателей (-w, по умолч. 2)
    int array_size;         // Размер массива БД (-s, по умолч. 10)
    int max_ops;            // Число операций на участника (-o, 0 = бесконечно, по умолч. 5)
    int idx_min;            // Мин. выбираемый индекс массива (по умолч. 0)
    int idx_max;            // Макс. выбираемый индекс массива (-1 = array_size - 1)
    int min_val;            // Мин. значение элементов (по умолч. 1)
    int max_val;            // Макс. значение элементов (по умолч. 40)
    int has_init_array;     // 1, если задано начальное содержимое массива вручную
    int init_array[MAX_ARRAY_SIZE]; // Заданное пользователем начальное содержимое
    int delay_min_ms;       // Мин. задержка между операциями в мс (по умолч. 100)
    int delay_max_ms;       // Макс. задержка между операциями в мс (по умолч. 300)
    int work_min_ms;        // Мин. длительность выполнения чтения/записи в мс (по умолч. 50)
    int work_max_ms;        // Макс. длительность выполнения чтения/записи в мс (по умолч. 150)
    access_policy_t policy; // Политика предоставления доступа (-p)
    unsigned int seed;      // Сид генератора случайных чисел
    char log_file[256];     // Путь к лог-файлу (-l)
    char config_file[256];  // Путь к конфигурационному файлу (-c)
} sim_config_t;

// Статистика участника (читателя или писателя)
typedef struct {
    uint32_t ops_completed;
    uint64_t total_wait_time_us;
    uint64_t max_wait_time_us;
    uint64_t min_wait_time_us;
} worker_stats_t;

// Состояние базы данных в разделяемой памяти
typedef struct {
    int data[MAX_ARRAY_SIZE];
    int size;
    
    // Счетчики синхронизации
    int read_count;
    int write_count;
    
    // Фактические активные участники внутри критической секции (для инвариантов)
    int active_readers;
    int active_writers;
    
    // Счетчики нарушений инвариантов (для самодиагностики)
    int invariant_violations;

    // Флаг остановки (0 - работает, 1 - завершение)
    volatile int stop_flag;

    // Статистика
    uint64_t total_reads;
    uint64_t total_writes;
    worker_stats_t reader_stats[MAX_WORKERS];
    worker_stats_t writer_stats[MAX_WORKERS];
} shared_database_t;

#endif // TYPES_H
