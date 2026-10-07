#ifndef READER_H
#define READER_H

#include "database.h"

typedef struct
{
    int id;
    database_t* db;
    int num_ops;
    int sleep_min_ms;
    int sleep_max_ms;
} reader_args_t;

void* reader_thread(void* arg);

#endif
