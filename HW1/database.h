#ifndef DATABASE_H
#define DATABASE_H

#include <pthread.h>

typedef enum {
    POLICY_READERS_PREF,
    POLICY_WRITERS_PREF,
    POLICY_FAIR
} access_policy_t;

typedef struct {

    int  *data;
    int   size;

    access_policy_t policy;

    pthread_mutex_t mutex;
    pthread_cond_t  can_read;
    pthread_cond_t  can_write;

    int active_readers;
    int active_writers;
    int waiting_readers;
    int waiting_writers;

    unsigned long order_ticket;
    unsigned long order_serving;
} database_t;

int db_init(database_t *db, int size, int max_val, access_policy_t policy);

void db_destroy(database_t *db);

void db_read_lock(database_t *db);

void db_read_unlock(database_t *db);

void db_write_lock(database_t *db);

void db_write_unlock(database_t *db);

void db_print(database_t *db);

#endif
