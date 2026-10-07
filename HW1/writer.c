#include "writer.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int cmp_int(const void *a, const void *b)
{
    return (*(const int *)a) - (*(const int *)b);
}

void *writer_thread(void *arg)
{
    writer_args_t *wa = (writer_args_t *)arg;
    unsigned int seed = (unsigned int)(time(NULL) ^ (wa->id * 2000 + 13));

    for (int op = 0; op < wa->num_ops; op++) {
        safe_printf("[Писатель %d] Хочет писать (операция %d/%d)\n",
                    wa->id, op + 1, wa->num_ops);

        db_write_lock(wa->db);

        safe_printf("[Писатель %d] Начал запись\n", wa->id);

        int index     = rand_range(&seed, 0, wa->db->size - 1);
        int new_value = rand_range(&seed, 1, wa->max_val);
        int old_value = wa->db->data[index];

        wa->db->data[index] = new_value;

        qsort(wa->db->data, (size_t)wa->db->size, sizeof(int), cmp_int);

        safe_printf("[Писатель %d] Заменил [%d]=%d -> %d. Массив: ",
                    wa->id, index, old_value, new_value);
        db_print(wa->db);

        db_write_unlock(wa->db);

        safe_printf("[Писатель %d] Закончил запись\n", wa->id);

        random_sleep_ms(&seed, wa->sleep_min_ms, wa->sleep_max_ms);
    }

    safe_printf("[Писатель %d] Завершил все операции\n", wa->id);
    return NULL;
}
