#ifndef WRITER_H
#define WRITER_H

#include "database.h"

typedef struct {
    int          id;
    database_t  *db;
    int          num_ops;
    int          max_val;
    int          sleep_min_ms;
    int          sleep_max_ms;
} writer_args_t;

void *writer_thread(void *arg);

#endif
