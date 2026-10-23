#include "reader.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

void *reader_thread(void *arg)
{
    reader_args_t *ra = (reader_args_t *)arg;

    /*
     * Уникальный seed для rand_r: XOR от времени и ID потока,
     * чтобы разные читатели выбирали разные индексы.
     */
    unsigned int seed = (unsigned int)(time(NULL) ^ ((unsigned int)ra->id * 1000u + 7u));

    int op = 0;
    /* Если num_ops == 0, работаем бесконечно (до установки stop_flag) */
    while ((ra->num_ops == 0 || op < ra->num_ops)
           && !atomic_load(&ra->db->stop_flag))
    {
        op++;
        safe_printf("[Читатель %d] Запрашивает доступ (операция %d/%s)\n",
                    ra->id, op, ra->num_ops ? "?" : "∞");

        /* === Захват блокировки на чтение === */
        db_read_lock(ra->db);

        /* Повторная проверка флага после пробуждения */
        if (atomic_load(&ra->db->stop_flag)) {
            db_read_unlock(ra->db);
            break;
        }

        safe_printf("[Читатель %d] Начало чтения\n", ra->id);

        /* Читаем случайный элемент массива */
        int index = rand_range(&seed, 0, ra->db->size - 1);
        int value = ra->db->data[index];

        safe_printf("[Читатель %d] Прочитал data[%d] = %d\n",
                    ra->id, index, value);

        /* === Освобождение блокировки на чтение === */
        db_read_unlock(ra->db);

        safe_printf("[Читатель %d] Конец чтения\n", ra->id);

        /*
         * Вычисляем Фибоначчи ПОСЛЕ освобождения блокировки:
         * вычисление может занять время, и нет смысла блокировать БД.
         */
        int64_t fib = fibonacci(value);
        safe_printf("[Читатель %d] Фибоначчи(%d) = %ld\n",
                    ra->id, value, (long)fib);

        /* Пауза перед следующей операцией */
        random_sleep_ms(&seed, ra->sleep_min_ms, ra->sleep_max_ms);
    }

    safe_printf("[Читатель %d] Завершил работу (выполнено %d операций)\n",
                ra->id, op);
    return NULL;
}
