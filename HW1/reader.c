#include "reader.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

void *reader_thread(void *arg)
{
    reader_args_t *ra = (reader_args_t *)arg;
    unsigned int seed = (unsigned int)(time(NULL) ^ (ra->id * 1000 + 7));

    for (int op = 0; op < ra->num_ops; op++) {
        safe_printf("[Читатель %d] Хочет читать (операция %d/%d)\n",
                    ra->id, op + 1, ra->num_ops);

        db_read_lock(ra->db);

        safe_printf("[Читатель %d] Начал чтение\n", ra->id);

        int index = rand_range(&seed, 0, ra->db->size - 1);
        int value = ra->db->data[index];

        safe_printf("[Читатель %d] Прочитал значение %d по индексу %d\n",
                    ra->id, value, index);

        db_read_unlock(ra->db);

        safe_printf("[Читатель %d] Закончил чтение\n", ra->id);

        int64_t fib = fibonacci(value);
        safe_printf("[Читатель %d] Фибоначчи(%d) = %ld\n",
                    ra->id, value, (long)fib);

        random_sleep_ms(&seed, ra->sleep_min_ms, ra->sleep_max_ms);
    }

    safe_printf("[Читатель %d] Завершил все операции\n", ra->id);
    return NULL;
}
