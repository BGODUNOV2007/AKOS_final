/**
 * ============================================================
 *  Задача «Читатели и писатели» — Вариант 2
 *  Задание 2: многопоточная реализация на семафорах (sem_t)
 *  Дисциплина: Архитектура компьютера и операционные системы
 * ============================================================
 *
 *  Запуск:
 *    ./rw_sem [-r N] [-w N] [-s N] [-m N] [-o N] [-p POLICY]
 *             [--sleep-min MS] [--sleep-max MS] [--log FILE]
 *
 *  Прерывание по Ctrl+C (SIGINT) корректно останавливает все потоки.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <getopt.h>
#include <pthread.h>

#include "database.h"
#include "reader.h"
#include "writer.h"
#include "utils.h"

/* ===================== Значения по умолчанию ===================== */
#define DEFAULT_NUM_READERS   3
#define DEFAULT_NUM_WRITERS   2
#define DEFAULT_ARRAY_SIZE   10
#define DEFAULT_MAX_VALUE   100
#define DEFAULT_OPS           5
#define DEFAULT_SLEEP_MIN   100   /* мс */
#define DEFAULT_SLEEP_MAX   500   /* мс */

/*
 * Глобальный указатель на БД — нужен обработчику сигнала,
 * который не может получать параметры обычным образом.
 */
static database_t *g_db = NULL;

/*
 * Обработчик SIGINT (Ctrl+C).
 * Устанавливает атомарный флаг останова, после чего все потоки
 * при следующей проверке завершат текущий цикл и выйдут.
 */
static void sigint_handler(int sig)
{
    (void)sig;
    if (g_db) {
        atomic_store(&g_db->stop_flag, 1);
        /* Пробуждаем потоки, которые спят на семафорах */
        sem_post(&g_db->read_sem);
        sem_post(&g_db->write_sem);
    }
}

static void print_usage(const char *prog)
{
    printf("Использование: %s [OPTIONS]\n\n", prog);
    printf("Параметры:\n");
    printf("  -r, --readers  N       Количество читателей          (по умолчанию %d)\n", DEFAULT_NUM_READERS);
    printf("  -w, --writers  N       Количество писателей          (по умолчанию %d)\n", DEFAULT_NUM_WRITERS);
    printf("  -s, --size     N       Размер массива                (по умолчанию %d)\n", DEFAULT_ARRAY_SIZE);
    printf("  -m, --max-val  N       Макс. значение элемента       (по умолчанию %d)\n", DEFAULT_MAX_VALUE);
    printf("  -o, --ops      N       Операций на поток (0=∞)       (по умолчанию %d)\n", DEFAULT_OPS);
    printf("  -p, --policy   POLICY  Политика: readers|writers|fair (по умолчанию fair)\n");
    printf("      --sleep-min  MS    Мин. задержка между операциями (по умолчанию %d мс)\n", DEFAULT_SLEEP_MIN);
    printf("      --sleep-max  MS    Макс. задержка между операциями (по умолчанию %d мс)\n", DEFAULT_SLEEP_MAX);
    printf("  -l, --log      FILE    Файл для сохранения лога\n");
    printf("  -h, --help             Показать эту справку\n");
}

int main(int argc, char *argv[])
{
    /* --- Параметры по умолчанию --- */
    int num_readers = DEFAULT_NUM_READERS;
    int num_writers = DEFAULT_NUM_WRITERS;
    int array_size  = DEFAULT_ARRAY_SIZE;
    int max_value   = DEFAULT_MAX_VALUE;
    int ops         = DEFAULT_OPS;
    int sleep_min   = DEFAULT_SLEEP_MIN;
    int sleep_max   = DEFAULT_SLEEP_MAX;
    access_policy_t policy = POLICY_FAIR;
    char *logfile_path = NULL;

    /* --- Парсинг аргументов командной строки --- */
    static struct option long_options[] = {
        {"readers",   required_argument, 0, 'r'},
        {"writers",   required_argument, 0, 'w'},
        {"size",      required_argument, 0, 's'},
        {"max-val",   required_argument, 0, 'm'},
        {"ops",       required_argument, 0, 'o'},
        {"policy",    required_argument, 0, 'p'},
        {"sleep-min", required_argument, 0,  1 },
        {"sleep-max", required_argument, 0,  2 },
        {"log",       required_argument, 0, 'l'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "r:w:s:m:o:p:l:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'r': num_readers = atoi(optarg); break;
            case 'w': num_writers = atoi(optarg); break;
            case 's': array_size  = atoi(optarg); break;
            case 'm': max_value   = atoi(optarg); break;
            case 'o': ops         = atoi(optarg); break;
            case 'p':
                if      (strcmp(optarg, "readers") == 0) policy = POLICY_READERS_PREF;
                else if (strcmp(optarg, "writers") == 0) policy = POLICY_WRITERS_PREF;
                else if (strcmp(optarg, "fair")    == 0) policy = POLICY_FAIR;
                else {
                    fprintf(stderr, "Неизвестная политика: %s\n", optarg);
                    return 1;
                }
                break;
            case 1:   sleep_min    = atoi(optarg);  break;
            case 2:   sleep_max    = atoi(optarg);  break;
            case 'l': logfile_path = optarg;        break;
            case 'h': print_usage(argv[0]); return 0;
            default:  print_usage(argv[0]); return 1;
        }
    }

    /* --- Валидация параметров --- */
    if (num_readers < 0 || num_writers < 0 || array_size <= 0 || max_value <= 0) {
        fprintf(stderr, "Ошибка: некорректные параметры\n");
        return 1;
    }
    if (sleep_min > sleep_max) {
        fprintf(stderr, "Ошибка: sleep-min > sleep-max\n");
        return 1;
    }

    /* --- Открытие лог-файла (если задан) --- */
    if (logfile_path) {
        g_logfile = fopen(logfile_path, "w");
        if (!g_logfile) {
            perror("Не удалось открыть лог-файл");
            return 1;
        }
    }

    /* --- Инициализация ГПСЧ --- */
    srand((unsigned int)time(NULL));

    /* --- Вывод конфигурации --- */
    const char *policy_names[] = {
        "readers (приоритет читателям)",
        "writers (приоритет писателям)",
        "fair    (справедливая очередь)"
    };
    safe_printf("=== Читатели и писатели (семафоры) ===\n");
    safe_printf("Читателей: %d | Писателей: %d\n", num_readers, num_writers);
    safe_printf("Размер массива: %d | Макс. значение: %d\n", array_size, max_value);
    if (ops > 0)
        safe_printf("Операций на поток: %d\n", ops);
    else
        safe_printf("Операций на поток: ∞ (до Ctrl+C)\n");
    safe_printf("Задержка: %d–%d мс\n", sleep_min, sleep_max);
    safe_printf("Политика: %s\n", policy_names[policy]);
    if (logfile_path)
        safe_printf("Лог: %s\n", logfile_path);
    safe_printf("=======================================\n\n");

    /* --- Инициализация базы данных --- */
    database_t db;
    if (db_init(&db, array_size, max_value, policy) != 0) {
        fprintf(stderr, "Ошибка инициализации БД\n");
        return 1;
    }
    g_db = &db;   /* Для обработчика сигнала */

    {
        char arr_buf[4096];
        int pos = 0;
        pos += snprintf(arr_buf + pos, sizeof(arr_buf) - (size_t)pos, "[");
        for (int i = 0; i < db.size && pos < (int)sizeof(arr_buf) - 10; i++) {
            if (i > 0) pos += snprintf(arr_buf + pos, sizeof(arr_buf) - (size_t)pos, ", ");
            pos += snprintf(arr_buf + pos, sizeof(arr_buf) - (size_t)pos, "%d", db.data[i]);
        }
        snprintf(arr_buf + pos, sizeof(arr_buf) - (size_t)pos, "]");
        safe_printf("Начальный массив: %s\n\n", arr_buf);
    }

    /* --- Установка обработчика SIGINT --- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    /* SA_RESTART позволяет системным вызовам (nanosleep) перезапускаться */
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        perror("sigaction");
        db_destroy(&db);
        return 1;
    }

    /* --- Выделение памяти под потоки и их аргументы --- */
    int total = num_readers + num_writers;
    pthread_t     *threads = malloc(sizeof(pthread_t)     * (size_t)(total > 0 ? total : 1));
    reader_args_t *r_args  = malloc(sizeof(reader_args_t) * (size_t)(num_readers > 0 ? num_readers : 1));
    writer_args_t *w_args  = malloc(sizeof(writer_args_t) * (size_t)(num_writers > 0 ? num_writers : 1));

    if (!threads || !r_args || !w_args) {
        fprintf(stderr, "Ошибка выделения памяти\n");
        free(threads); free(r_args); free(w_args);
        db_destroy(&db);
        return 1;
    }

    /* --- Запуск потоков-читателей --- */
    for (int i = 0; i < num_readers; i++) {
        r_args[i].id           = i + 1;
        r_args[i].db           = &db;
        r_args[i].num_ops      = ops;
        r_args[i].sleep_min_ms = sleep_min;
        r_args[i].sleep_max_ms = sleep_max;
        if (pthread_create(&threads[i], NULL, reader_thread, &r_args[i]) != 0) {
            fprintf(stderr, "Ошибка создания потока читателя %d\n", i + 1);
        }
    }

    /* --- Запуск потоков-писателей --- */
    for (int i = 0; i < num_writers; i++) {
        w_args[i].id           = i + 1;
        w_args[i].db           = &db;
        w_args[i].num_ops      = ops;
        w_args[i].max_val      = max_value;
        w_args[i].sleep_min_ms = sleep_min;
        w_args[i].sleep_max_ms = sleep_max;
        if (pthread_create(&threads[num_readers + i], NULL, writer_thread, &w_args[i]) != 0) {
            fprintf(stderr, "Ошибка создания потока писателя %d\n", i + 1);
        }
    }

    /* --- Ожидание завершения всех потоков --- */
    for (int i = 0; i < total; i++)
        pthread_join(threads[i], NULL);

    /* --- Итоговый вывод --- */
    safe_printf("\n=======================================\n");
    if (atomic_load(&db.stop_flag))
        safe_printf("Работа прервана пользователем (SIGINT).\n");
    else
        safe_printf("Все потоки завершили работу.\n");
    safe_printf("Итоговый массив: ");
    db_print(&db);
    safe_printf("\n=======================================\n");

    /* --- Очистка ресурсов --- */
    free(threads);
    free(r_args);
    free(w_args);
    db_destroy(&db);

    if (g_logfile) {
        fclose(g_logfile);
        printf("Лог сохранён в: %s\n", logfile_path);
    }

    return 0;
}
