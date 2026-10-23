#define _GNU_SOURCE

#include "database.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Вспомогательная функция сравнения для qsort ---- */
static int cmp_int(const void *a, const void *b)
{
    return (*(const int *)a) - (*(const int *)b);
}

/* ================================================================
 *  Инициализация и уничтожение
 * ================================================================ */

int db_init(database_t *db, int size, int max_val, access_policy_t policy)
{
    db->size   = size;
    db->policy = policy;

    /* Выделяем память под массив */
    db->data = malloc(sizeof(int) * (size_t)size);
    if (!db->data) return -1;

    /* Заполняем случайными числами в диапазоне [1, max_val] и сортируем */
    for (int i = 0; i < size; i++)
        db->data[i] = 1 + rand() % max_val;
    qsort(db->data, (size_t)size, sizeof(int), cmp_int);

    /* Инициализируем мьютекс монитора */
    pthread_mutex_init(&db->mutex, NULL);

    /*
     * Семафоры инициализируются в 0: поток, вызвавший sem_wait,
     * будет заблокирован до тех пор, пока другой поток не сделает sem_post.
     */
    sem_init(&db->read_sem,  0, 0);
    sem_init(&db->write_sem, 0, 0);

    /* Сбрасываем счётчики */
    db->active_readers  = 0;
    db->active_writers  = 0;
    db->waiting_readers = 0;
    db->waiting_writers = 0;

    /* Инициализируем счётчики ticket lock для FAIR-политики */
    db->order_ticket  = 0;
    db->order_serving = 0;

    /* Флаг останова сброшен */
    atomic_store(&db->stop_flag, 0);

    return 0;
}

void db_destroy(database_t *db)
{
    free(db->data);
    db->data = NULL;
    pthread_mutex_destroy(&db->mutex);
    sem_destroy(&db->read_sem);
    sem_destroy(&db->write_sem);
}

/* ================================================================
 *  Политика READERS_PREF (приоритет читателям)
 *
 *  Читатели: ждут только пока работает писатель.
 *  Писатели: ждут пока есть хотя бы один активный читатель
 *             ИЛИ другой активный писатель.
 *  Недостаток: писатели могут голодать при непрерывном потоке читателей.
 * ================================================================ */

static void read_lock_readers_pref(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    if (db->active_writers > 0) {
        /* Есть активный писатель — встаём в очередь */
        db->waiting_readers++;
        pthread_mutex_unlock(&db->mutex);
        sem_wait(&db->read_sem);   /* Блокируемся до сигнала от писателя */
        /* После пробуждения мьютекс НЕ удерживается (см. unlock) */
    } else {
        db->active_readers++;
        pthread_mutex_unlock(&db->mutex);
    }
}

static void read_unlock_readers_pref(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    db->active_readers--;

    if (db->active_readers == 0 && db->waiting_writers > 0) {
        /* Последний читатель вышел — пускаем одного ожидающего писателя */
        db->waiting_writers--;
        db->active_writers = 1;
        sem_post(&db->write_sem);
    }
    pthread_mutex_unlock(&db->mutex);
}

static void write_lock_readers_pref(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    if (db->active_readers > 0 || db->active_writers > 0) {
        /* Кто-то работает — ждём */
        db->waiting_writers++;
        pthread_mutex_unlock(&db->mutex);
        sem_wait(&db->write_sem);
    } else {
        db->active_writers = 1;
        pthread_mutex_unlock(&db->mutex);
    }
}

static void write_unlock_readers_pref(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    db->active_writers = 0;

    if (db->waiting_readers > 0) {
        /*
         * Приоритет читателям: пробуждаем ВСЕХ ожидающих читателей.
         * Каждый sem_post разблокирует ровно один поток, поэтому
         * делаем столько post-ов, сколько читателей ждёт.
         */
        int to_wake = db->waiting_readers;
        db->active_readers  += to_wake;
        db->waiting_readers  = 0;
        pthread_mutex_unlock(&db->mutex);
        for (int i = 0; i < to_wake; i++)
            sem_post(&db->read_sem);
    } else if (db->waiting_writers > 0) {
        /* Читателей нет — пускаем одного писателя */
        db->waiting_writers--;
        db->active_writers = 1;
        pthread_mutex_unlock(&db->mutex);
        sem_post(&db->write_sem);
    } else {
        pthread_mutex_unlock(&db->mutex);
    }
}

/* ================================================================
 *  Политика WRITERS_PREF (приоритет писателям)
 *
 *  Читатели: ждут, пока есть хотя бы ОДИН ожидающий или активный писатель.
 *  Писатели: ждут только пока работает другой писатель или читатель.
 *  Недостаток: читатели могут голодать при непрерывном потоке писателей.
 * ================================================================ */

static void read_lock_writers_pref(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    if (db->active_writers > 0 || db->waiting_writers > 0) {
        /* Есть писатели — ждём */
        db->waiting_readers++;
        pthread_mutex_unlock(&db->mutex);
        sem_wait(&db->read_sem);
    } else {
        db->active_readers++;
        pthread_mutex_unlock(&db->mutex);
    }
}

static void read_unlock_writers_pref(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    db->active_readers--;

    if (db->active_readers == 0 && db->waiting_writers > 0) {
        /* Последний читатель: пускаем писателя */
        db->waiting_writers--;
        db->active_writers = 1;
        pthread_mutex_unlock(&db->mutex);
        sem_post(&db->write_sem);
    } else {
        pthread_mutex_unlock(&db->mutex);
    }
}

static void write_lock_writers_pref(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    if (db->active_readers > 0 || db->active_writers > 0) {
        db->waiting_writers++;
        pthread_mutex_unlock(&db->mutex);
        sem_wait(&db->write_sem);
    } else {
        db->active_writers = 1;
        pthread_mutex_unlock(&db->mutex);
    }
}

static void write_unlock_writers_pref(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    db->active_writers = 0;

    if (db->waiting_writers > 0) {
        /* Приоритет писателям: сначала будим следующего писателя */
        db->waiting_writers--;
        db->active_writers = 1;
        pthread_mutex_unlock(&db->mutex);
        sem_post(&db->write_sem);
    } else if (db->waiting_readers > 0) {
        /* Писателей больше нет — будим всех читателей */
        int to_wake = db->waiting_readers;
        db->active_readers  += to_wake;
        db->waiting_readers  = 0;
        pthread_mutex_unlock(&db->mutex);
        for (int i = 0; i < to_wake; i++)
            sem_post(&db->read_sem);
    } else {
        pthread_mutex_unlock(&db->mutex);
    }
}

/* ================================================================
 *  Политика FAIR (справедливая очередь, ticket lock)
 *
 *  Каждый поток (читатель или писатель) получает уникальный номерок (ticket).
 *  Доступ предоставляется только тому, чей номерок совпадает с order_serving.
 *  После получения доступа order_serving увеличивается.
 *  Читатели, идущие подряд, могут входить одновременно (пакетный пропуск).
 *  Ни читатели, ни писатели не могут голодать.
 * ================================================================ */

static void read_lock_fair(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    unsigned long my_ticket = db->order_ticket++;

    if (my_ticket != db->order_serving || db->active_writers > 0) {
        /* Не наша очередь или пишет писатель — ждём */
        db->waiting_readers++;
        pthread_mutex_unlock(&db->mutex);
        sem_wait(&db->read_sem);
        /*
         * После пробуждения счётчики уже обновлены тем, кто послал сигнал
         * (write_unlock_fair), поэтому дополнительных действий не нужно.
         */
        return;
    }

    /* Наша очередь и нет писателя — входим немедленно */
    db->active_readers++;
    db->order_serving++;  /* Пропускаем следующего в очереди */
    pthread_mutex_unlock(&db->mutex);

    /*
     * Если следующий в очереди тоже читатель, он уже ждёт на своём семафоре.
     * Пробуждаем его — пусть тоже войдёт (пакетный пропуск читателей).
     */
    pthread_mutex_lock(&db->mutex);
    if (db->waiting_readers > 0 && db->active_writers == 0) {
        /* Проверяем, что следующий ticket — читатель (он разбудит сам себя) */
        db->waiting_readers--;
        db->active_readers++;
        db->order_serving++;
        pthread_mutex_unlock(&db->mutex);
        sem_post(&db->read_sem);
    } else {
        pthread_mutex_unlock(&db->mutex);
    }
}

static void read_unlock_fair(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    db->active_readers--;

    if (db->active_readers == 0) {
        /* Все читатели вышли — будим следующего в очереди */
        if (db->waiting_writers > 0) {
            db->waiting_writers--;
            db->active_writers = 1;
            db->order_serving++;
            pthread_mutex_unlock(&db->mutex);
            sem_post(&db->write_sem);
        } else if (db->waiting_readers > 0) {
            db->waiting_readers--;
            db->active_readers++;
            db->order_serving++;
            pthread_mutex_unlock(&db->mutex);
            sem_post(&db->read_sem);
        } else {
            pthread_mutex_unlock(&db->mutex);
        }
    } else {
        pthread_mutex_unlock(&db->mutex);
    }
}

static void write_lock_fair(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    unsigned long my_ticket = db->order_ticket++;

    if (my_ticket != db->order_serving || db->active_readers > 0 || db->active_writers > 0) {
        db->waiting_writers++;
        pthread_mutex_unlock(&db->mutex);
        sem_wait(&db->write_sem);
        return;  /* После пробуждения счётчики уже выставлены */
    }

    db->active_writers = 1;
    db->order_serving++;
    pthread_mutex_unlock(&db->mutex);
}

static void write_unlock_fair(database_t *db)
{
    pthread_mutex_lock(&db->mutex);
    db->active_writers = 0;

    /* Будим следующего в очереди: сначала проверяем читателей (пакетный пропуск) */
    if (db->waiting_readers > 0) {
        int to_wake = db->waiting_readers;
        db->active_readers  += to_wake;
        db->waiting_readers  = 0;
        db->order_serving   += (unsigned long)to_wake;
        pthread_mutex_unlock(&db->mutex);
        for (int i = 0; i < to_wake; i++)
            sem_post(&db->read_sem);
    } else if (db->waiting_writers > 0) {
        db->waiting_writers--;
        db->active_writers = 1;
        db->order_serving++;
        pthread_mutex_unlock(&db->mutex);
        sem_post(&db->write_sem);
    } else {
        pthread_mutex_unlock(&db->mutex);
    }
}

/* ================================================================
 *  Публичный интерфейс: диспетчеризация по политике
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
