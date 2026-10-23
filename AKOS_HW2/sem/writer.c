#include "writer.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Вспомогательная функция сравнения для qsort */
static int cmp_int(const void *a, const void *b)
{
    return (*(const int *)a) - (*(const int *)b);
}

void *writer_thread(void *arg)
{
    writer_args_t *wa = (writer_args_t *)arg;

    /* Уникальный seed: XOR от времени и ID, смещённый относительно читателей */
    unsigned int seed = (unsigned int)(time(NULL) ^ ((unsigned int)wa->id * 2000u + 13u));

    int op = 0;
    while ((wa->num_ops == 0 || op < wa->num_ops)
           && !atomic_load(&wa->db->stop_flag))
    {
        op++;
        safe_printf("[Писатель %d] Запрашивает доступ (операция %d/%s)\n",
                    wa->id, op, wa->num_ops ? "?" : "∞");

        /* === Захват эксклюзивной блокировки на запись === */
        db_write_lock(wa->db);

        /* Проверяем флаг после пробуждения — возможно, пришёл SIGINT */
        if (atomic_load(&wa->db->stop_flag)) {
            db_write_unlock(wa->db);
            break;
        }

        safe_printf("[Писатель %d] Начало записи\n", wa->id);

        /* Выбираем случайный индекс и генерируем новое значение */
        int index     = rand_range(&seed, 0, wa->db->size - 1);
        int old_value = wa->db->data[index];
        int new_value = rand_range(&seed, 1, wa->max_val);

        /* Записываем новое значение */
        wa->db->data[index] = new_value;

        /* Восстанавливаем сортировку */
        qsort(wa->db->data, (size_t)wa->db->size, sizeof(int), cmp_int);

        /* Формируем строку массива в буфере, чтобы весь вывод был атомарным */
        char arr_buf[4096];
        int  pos = 0;
        pos += snprintf(arr_buf + pos, sizeof(arr_buf) - (size_t)pos, "[");
        for (int j = 0; j < wa->db->size && pos < (int)sizeof(arr_buf) - 10; j++) {
            if (j > 0) pos += snprintf(arr_buf + pos, sizeof(arr_buf) - (size_t)pos, ", ");
            pos += snprintf(arr_buf + pos, sizeof(arr_buf) - (size_t)pos, "%d", wa->db->data[j]);
        }
        snprintf(arr_buf + pos, sizeof(arr_buf) - (size_t)pos, "]");

        safe_printf("[Писатель %d] data[%d]: %d \xe2\x86\x92 %d | Массив: %s\n",
                    wa->id, index, old_value, new_value, arr_buf);

        /* === Освобождение блокировки на запись === */
        db_write_unlock(wa->db);

        safe_printf("[Писатель %d] Конец записи\n", wa->id);

        /* Пауза перед следующей операцией */
        random_sleep_ms(&seed, wa->sleep_min_ms, wa->sleep_max_ms);
    }

    safe_printf("[Писатель %d] Завершил работу (выполнено %d операций)\n",
                wa->id, op);
    return NULL;
}
