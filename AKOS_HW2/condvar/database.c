#define _GNU_SOURCE

#include "database.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp_int(const void *a, const void *b)
{
    return (*(const int *)a) - (*(const int *)b);
}

/* ================================================================
 *  Инициализация / Уничтожение
 * ================================================================ */

int db_init(database_t *db, int size, int max_val, access_policy_t policy)
{
    db->size   = size;
    db->policy = policy;
    db->data   = malloc(sizeof(int) * (size_t)size);
    if (!db->data) return -1;

    for (int i = 0; i < size; i++)
        db->data[i] = 1 + rand() % max_val;
    qsort(db->data, (size_t)size, sizeof(int), cmp_int);

    pthread_mutex_init(&db->mutex,     NULL);
    pthread_cond_init (&db->can_read,  NULL);
    pthread_cond_init (&db->can_write, NULL);

    db->active_readers  = 0;
    db->active_writers  = 0;
    db->waiting_readers = 0;
    db->waiting_writers = 0;
    db->order_ticket    = 0;
    db->order_serving   = 0;

    atomic_store(&db->stop_flag, 0);
    return 0;
}

void db_destroy(database_t *db)
{
    free(db->data);
    db->data = NULL;
    pthread_mutex_destroy(&db->mutex);
    pthread_cond_destroy (&db->can_read);
    pthread_cond_destroy (&db->can_write);
}

/* ================================================================
 *  READERS_PREF — приоритет читателям
 *  Читатель ждёт только пока работает писатель.
 *  Писатель ждёт пока есть активные читатели или другой писатель.
 * ================================================================ */

static void read_lock_readers_pref(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    db->waiting_readers++;
    /* pthread_cond_wait атомарно освобождает мьютекс и спит */
    while (db->active_writers > 0)
        pthread_cond_wait(&db->can_read, &db->mutex);
    db->waiting_readers--;
    db->active_readers++;
    pthread_mutex_unlock(&db->mutex);
}

static void read_unlock_readers_pref(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    db->active_readers--;
    if (db->active_readers == 0)
        pthread_cond_signal(&db->can_write); /* последний читатель будит писателя */
    pthread_mutex_unlock(&db->mutex);
}

static void write_lock_readers_pref(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    db->waiting_writers++;
    while (db->active_readers > 0 || db->active_writers > 0)
        pthread_cond_wait(&db->can_write, &db->mutex);
    db->waiting_writers--;
    db->active_writers = 1;
    pthread_mutex_unlock(&db->mutex);
}

static void write_unlock_readers_pref(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    db->active_writers = 0;
    /*
     * Приоритет читателям: broadcast будит всех ожидающих читателей.
     * Если читателей нет, будим одного ожидающего писателя.
     */
    pthread_cond_broadcast(&db->can_read);
    pthread_cond_signal  (&db->can_write);
    pthread_mutex_unlock (&db->mutex);
}

/* ================================================================
 *  WRITERS_PREF — приоритет писателям
 *  Читатель ждёт, пока есть хотя бы один ожидающий или активный писатель.
 * ================================================================ */

static void read_lock_writers_pref(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    db->waiting_readers++;
    while (db->active_writers > 0 || db->waiting_writers > 0)
        pthread_cond_wait(&db->can_read, &db->mutex);
    db->waiting_readers--;
    db->active_readers++;
    pthread_mutex_unlock(&db->mutex);
}

static void read_unlock_writers_pref(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    db->active_readers--;
    if (db->active_readers == 0)
        pthread_cond_signal(&db->can_write);
    pthread_mutex_unlock(&db->mutex);
}

static void write_lock_writers_pref(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    db->waiting_writers++;
    while (db->active_readers > 0 || db->active_writers > 0)
        pthread_cond_wait(&db->can_write, &db->mutex);
    db->waiting_writers--;
    db->active_writers = 1;
    pthread_mutex_unlock(&db->mutex);
}

static void write_unlock_writers_pref(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    db->active_writers = 0;
    /* Приоритет писателям: сначала будим следующего писателя */
    if (db->waiting_writers > 0)
        pthread_cond_signal   (&db->can_write);
    else
        pthread_cond_broadcast(&db->can_read);
    pthread_mutex_unlock(&db->mutex);
}

/* ================================================================
 *  FAIR — справедливая очередь (ticket lock + cond_broadcast)
 *
 *  Каждый поток берёт номерок order_ticket++.
 *  Получает доступ только когда order_serving == его номерок.
 *  Это гарантирует FIFO-порядок и отсутствие голодания.
 *  Читатели с соседними номерками могут войти пакетом (оба сразу).
 * ================================================================ */

static void read_lock_fair(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    unsigned long my_ticket = db->order_ticket++;
    db->waiting_readers++;
    while (my_ticket != db->order_serving || db->active_writers > 0)
        pthread_cond_wait(&db->can_read, &db->mutex);
    db->waiting_readers--;
    db->active_readers++;
    db->order_serving++;
    /*
     * Пробуждаем всех: следующий в очереди читатель проверит свой номерок
     * и тоже войдёт (пакетный пропуск). Писатель встанет в ожидание.
     */
    pthread_cond_broadcast(&db->can_read);
    pthread_cond_broadcast(&db->can_write);
    pthread_mutex_unlock(&db->mutex);
}

static void read_unlock_fair(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    db->active_readers--;
    if (db->active_readers == 0)
        pthread_cond_broadcast(&db->can_write);
    pthread_mutex_unlock(&db->mutex);
}

static void write_lock_fair(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    unsigned long my_ticket = db->order_ticket++;
    db->waiting_writers++;
    while (my_ticket != db->order_serving ||
           db->active_readers > 0 ||
           db->active_writers > 0)
        pthread_cond_wait(&db->can_write, &db->mutex);
    db->waiting_writers--;
    db->active_writers = 1;
    db->order_serving++;
    pthread_mutex_unlock(&db->mutex);
}

static void write_unlock_fair(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    db->active_writers = 0;
    pthread_cond_broadcast(&db->can_read);
    pthread_cond_broadcast(&db->can_write);
    pthread_mutex_unlock(&db->mutex);
}

/* ================================================================
 *  Публичный интерфейс
 * ================================================================ */

void db_read_lock(database_t *db)
{
    switch (db->policy) {
        case POLICY_READERS_PREF: read_lock_readers_pref(db); break;
        case POLICY_WRITERS_PREF: read_lock_writers_pref(db); break;
        case POLICY_FAIR:         read_lock_fair(db);         break;
    }
}

void db_read_unlock(database_t *db)
{
    switch (db->policy) {
        case POLICY_READERS_PREF: read_unlock_readers_pref(db); break;
        case POLICY_WRITERS_PREF: read_unlock_writers_pref(db); break;
        case POLICY_FAIR:         read_unlock_fair(db);         break;
    }
}

void db_write_lock(database_t *db)
{
    switch (db->policy) {
        case POLICY_READERS_PREF: write_lock_readers_pref(db); break;
        case POLICY_WRITERS_PREF: write_lock_writers_pref(db); break;
        case POLICY_FAIR:         write_lock_fair(db);         break;
    }
}

void db_write_unlock(database_t *db)
{
    switch (db->policy) {
        case POLICY_READERS_PREF: write_unlock_readers_pref(db); break;
        case POLICY_WRITERS_PREF: write_unlock_writers_pref(db); break;
        case POLICY_FAIR:         write_unlock_fair(db);         break;
    }
}

void db_print(const database_t *db)
{
    printf("[");
    for (int i = 0; i < db->size; i++) {
        if (i > 0) printf(", ");
        printf("%d", db->data[i]);
    }
    printf("]");
}
