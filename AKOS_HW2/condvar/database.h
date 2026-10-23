#ifndef DATABASE_H
#define DATABASE_H

#include <pthread.h>
#include <stdatomic.h>

/*
 * Политика доступа к базе данных.
 */
typedef enum {
    POLICY_READERS_PREF,  /* Приоритет читателям */
    POLICY_WRITERS_PREF,  /* Приоритет писателям */
    POLICY_FAIR           /* Справедливая очередь (ticket lock) */
} access_policy_t;

/*
 * Структура базы данных.
 * Синхронизация построена на мьютексе + условных переменных (pthread_cond_t),
 * в отличие от семафорной версии (sem/).
 */
typedef struct {
    /* --- Данные --- */
    int  *data;
    int   size;

    /* --- Политика --- */
    access_policy_t policy;

    /* --- Монитор: один мьютекс + два условия --- */
    pthread_mutex_t mutex;

    /*
     * can_read  — сигнализирует читателям, что доступ открыт.
     * can_write — сигнализирует писателям, что БД свободна.
     */
    pthread_cond_t  can_read;
    pthread_cond_t  can_write;

    /* --- Счётчики состояния --- */
    int active_readers;
    int active_writers;
    int waiting_readers;
    int waiting_writers;

    /* --- Для FAIR-политики --- */
    unsigned long order_ticket;
    unsigned long order_serving;

    /* Атомарный флаг останова (устанавливается обработчиком SIGINT) */
    atomic_int stop_flag;
} database_t;

int  db_init(database_t *db, int size, int max_val, access_policy_t policy);
void db_destroy(database_t *db);

void db_read_lock(database_t *db);
void db_read_unlock(database_t *db);
void db_write_lock(database_t *db);
void db_write_unlock(database_t *db);

void db_print(const database_t *db);

#endif /* DATABASE_H */
