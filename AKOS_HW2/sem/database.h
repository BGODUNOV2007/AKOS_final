#ifndef DATABASE_H
#define DATABASE_H

#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>

/*
 * Политика доступа к базе данных.
 * READERS_PREF — приоритет читателям: писатели ждут, пока читателей не останется.
 * WRITERS_PREF — приоритет писателям: новые читатели ждут, пока есть хоть один ожидающий писатель.
 * FAIR          — справедливая очередь (ticket lock): порядок FIFO для всех потоков.
 */
typedef enum {
    POLICY_READERS_PREF,
    POLICY_WRITERS_PREF,
    POLICY_FAIR
} access_policy_t;

/*
 * Структура базы данных.
 * Инкапсулирует данные (отсортированный массив) и все примитивы
 * синхронизации (мьютекс + семафоры) для реализации трёх политик.
 */
typedef struct {
    /* --- Данные --- */
    int  *data;          /* Указатель на отсортированный массив */
    int   size;          /* Число элементов массива */

    /* --- Политика --- */
    access_policy_t policy;

    /* --- Общий мьютекс монитора (защищает счётчики) --- */
    pthread_mutex_t mutex;

    /*
     * Семафор для читателей: сигнализирует, что чтение разрешено.
     * Инициализируется в 0; пробуждает ожидающих читателей через sem_post.
     */
    sem_t read_sem;

    /*
     * Семафор для писателей: сигнализирует, что запись разрешена.
     * Инициализируется в 0; пробуждает ожидающего писателя через sem_post.
     */
    sem_t write_sem;

    /* --- Счётчики состояния --- */
    int active_readers;   /* Читателей внутри секции чтения прямо сейчас */
    int active_writers;   /* Писателей внутри секции записи (0 или 1) */
    int waiting_readers;  /* Читателей, ожидающих доступа */
    int waiting_writers;  /* Писателей, ожидающих доступа */

    /* --- Для FAIR-политики: ticket lock --- */
    unsigned long order_ticket;   /* Счётчик выданных номерков */
    unsigned long order_serving;  /* Номерок, обслуживаемый в данный момент */

    /*
     * Атомарный флаг останова: устанавливается в 1 по сигналу SIGINT.
     * Потоки проверяют его перед каждой новой операцией.
     */
    atomic_int stop_flag;
} database_t;

/* Инициализация/уничтожение */
int  db_init(database_t *db, int size, int max_val, access_policy_t policy);
void db_destroy(database_t *db);

/* Блокировки чтения/записи */
void db_read_lock(database_t *db);
void db_read_unlock(database_t *db);
void db_write_lock(database_t *db);
void db_write_unlock(database_t *db);

/* Вывод текущего состояния массива в stdout */
void db_print(const database_t *db);

#endif /* DATABASE_H */
