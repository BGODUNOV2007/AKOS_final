

#include <getopt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "database.h"
#include "reader.h"
#include "utils.h"
#include "writer.h"

#define DEFAULT_NUM_READERS 3
#define DEFAULT_NUM_WRITERS 2
#define DEFAULT_ARRAY_SIZE 10
#define DEFAULT_MAX_VALUE 100
#define DEFAULT_OPS 5
#define DEFAULT_SLEEP_MIN 100
#define DEFAULT_SLEEP_MAX 500

static void print_usage(const char* prog)
{
    printf("Использование: %s [OPTIONS]\n\n", prog);
    printf("Параметры:\n");
    printf("  -r, --readers     N  Количество читателей (по умолчанию %d)\n",
           DEFAULT_NUM_READERS);
    printf("  -w, --writers     N  Количество писателей (по умолчанию %d)\n",
           DEFAULT_NUM_WRITERS);
    printf("  -s, --size        N  Размер массива (по умолчанию %d)\n",
           DEFAULT_ARRAY_SIZE);
    printf("  -m, --max-val     N  Макс. значение элемента (по умолчанию %d)\n",
           DEFAULT_MAX_VALUE);
    printf("  -o, --ops         N  Операций на каждый поток(по умолчанию %d)\n",
           DEFAULT_OPS);
    printf("  -p, --policy POLICY  Политика: readers|writers|fair (по "
           "умолчанию fair)\n");
    printf("  --sleep-min  MS  Мин. задержка между операциями (по умолчанию %d "
           "мс)\n",
           DEFAULT_SLEEP_MIN);
    printf("  --sleep-max  MS  Макс. задержка между операциями (по умолчанию "
           "%d мс)\n",
           DEFAULT_SLEEP_MAX);
    printf("  -h, --help           Показать эту справку\n");
}

int main(int argc, char* argv[])
{

    int num_readers = DEFAULT_NUM_READERS;
    int num_writers = DEFAULT_NUM_WRITERS;
    int array_size = DEFAULT_ARRAY_SIZE;
    int max_value = DEFAULT_MAX_VALUE;
    int ops = DEFAULT_OPS;
    int sleep_min = DEFAULT_SLEEP_MIN;
    int sleep_max = DEFAULT_SLEEP_MAX;
    access_policy_t policy = POLICY_FAIR;

    static struct option long_options[] = {
        {"readers", required_argument, 0, 'r'},
        {"writers", required_argument, 0, 'w'},
        {"size", required_argument, 0, 's'},
        {"max-val", required_argument, 0, 'm'},
        {"ops", required_argument, 0, 'o'},
        {"policy", required_argument, 0, 'p'},
        {"sleep-min", required_argument, 0, 1},
        {"sleep-max", required_argument, 0, 2},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};

    int opt;
    while((opt = getopt_long(argc, argv, "r:w:s:m:o:p:h", long_options,
                             NULL)) != -1)
    {
        switch(opt)
        {
        case 'r':
            num_readers = atoi(optarg);
            break;
        case 'w':
            num_writers = atoi(optarg);
            break;
        case 's':
            array_size = atoi(optarg);
            break;
        case 'm':
            max_value = atoi(optarg);
            break;
        case 'o':
            ops = atoi(optarg);
            break;
        case 'p':
            if(strcmp(optarg, "readers") == 0)
                policy = POLICY_READERS_PREF;
            else if(strcmp(optarg, "writers") == 0)
                policy = POLICY_WRITERS_PREF;
            else if(strcmp(optarg, "fair") == 0)
                policy = POLICY_FAIR;
            else
            {
                fprintf(stderr, "Неизвестная политика: %s\n", optarg);
                return 1;
            }
            break;
        case 1:
            sleep_min = atoi(optarg);
            break;
        case 2:
            sleep_max = atoi(optarg);
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    srand((unsigned int)time(NULL));

    const char* policy_names[] = {"readers (приоритет читателям)",
                                  "writers (приоритет писателям)",
                                  "fair (справедливая очередь)"};
    printf("=== Задача «Читатели и писатели» ===\n");
    printf("Читателей: %d, Писателей: %d\n", num_readers, num_writers);
    printf("Размер массива: %d, Макс. значение: %d\n", array_size, max_value);
    printf("Операций на поток: %d\n", ops);
    printf("Задержка: %d–%d мс\n", sleep_min, sleep_max);
    printf("Политика: %s\n", policy_names[policy]);
    printf("====================================\n\n");

    database_t db;
    if(db_init(&db, array_size, max_value, policy) != 0)
    {
        fprintf(stderr, "Ошибка инициализации БД\n");
        return 1;
    }

    printf("Начальный массив: ");
    db_print(&db);
    printf("\n");

    int total = num_readers + num_writers;
    pthread_t* threads = malloc(sizeof(pthread_t) * (size_t)total);
    reader_args_t* r_args = malloc(sizeof(reader_args_t) * (size_t)num_readers);
    writer_args_t* w_args = malloc(sizeof(writer_args_t) * (size_t)num_writers);

    if(!threads || !r_args || !w_args)
    {
        fprintf(stderr, "Ошибка выделения памяти\n");
        return 1;
    }

    for(int i = 0; i < num_readers; i++)
    {
        r_args[i].id = i + 1;
        r_args[i].db = &db;
        r_args[i].num_ops = ops;
        r_args[i].sleep_min_ms = sleep_min;
        r_args[i].sleep_max_ms = sleep_max;
        pthread_create(&threads[i], NULL, reader_thread, &r_args[i]);
    }

    for(int i = 0; i < num_writers; i++)
    {
        w_args[i].id = i + 1;
        w_args[i].db = &db;
        w_args[i].num_ops = ops;
        w_args[i].max_val = max_value;
        w_args[i].sleep_min_ms = sleep_min;
        w_args[i].sleep_max_ms = sleep_max;
        pthread_create(&threads[num_readers + i], NULL, writer_thread,
                       &w_args[i]);
    }

    for(int i = 0; i < total; i++)
        pthread_join(threads[i], NULL);

    printf("\n====================================\n");
    printf("Все потоки завершили работу.\n");
    printf("Итоговый массив: ");
    db_print(&db);
    printf("====================================\n");

    free(threads);
    free(r_args);
    free(w_args);
    db_destroy(&db);

    return 0;
}
