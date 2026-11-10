#ifndef COMMON_7_8_H
#define COMMON_7_8_H

#include "../common/types.h"
#include <semaphore.h>

#define SHM_NAME_7_8       "/akos_hw3_shm_7_8"
#define SEM_DB_7_8         "/akos_hw3_sem_db_7_8"
#define SEM_RMUTEX_7_8     "/akos_hw3_sem_rmutex_7_8"
#define SEM_WMUTEX_7_8     "/akos_hw3_sem_wmutex_7_8"
#define SEM_READTRY_7_8    "/akos_hw3_sem_readtry_7_8"
#define SEM_PRINT_7_8      "/akos_hw3_sem_print_7_8"

typedef struct {
    sim_config_t cfg;
    shared_database_t db;
    int registered_readers;
    int registered_writers;
    int completed_readers;
    int completed_writers;
    int master_ready;
} independent_shm_t;

#endif // COMMON_7_8_H
