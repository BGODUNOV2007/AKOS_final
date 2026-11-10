#ifndef IPC_PROTOCOL_H
#define IPC_PROTOCOL_H

#include "../common/types.h"
#include <mqueue.h>

#define MAX_MQ_ARRAY_SIZE 128

#define MQ_REQUEST_NAME      "/akos_hw3_mq_req_9_10"
#define MQ_RESP_PREFIX       "/akos_hw3_mq_resp_9_10_"
#define SHM_CTRL_NAME_9_10   "/akos_hw3_shm_ctrl_9_10"

#define SEM_DB_9_10          "/akos_hw3_sem_db_9_10"
#define SEM_RMUTEX_9_10      "/akos_hw3_sem_rmutex_9_10"
#define SEM_WMUTEX_9_10      "/akos_hw3_sem_wmutex_9_10"
#define SEM_READTRY_9_10     "/akos_hw3_sem_readtry_9_10"
#define SEM_PRINT_9_10       "/akos_hw3_sem_print_9_10"

typedef enum {
    REQ_READ = 1,
    REQ_WRITE = 2,
    REQ_EXIT = 3
} req_type_t;

// Сообщение запроса от читателя или писателя к координатору БД
typedef struct {
    req_type_t type;
    int worker_id;       // Номер читателя или писателя
    int is_writer;       // 0 = читатель, 1 = писатель
    pid_t pid;
    int index;           // Индекс элемента
    int new_value;       // Новое значение (для писателя)
    char resp_queue[64]; // Имя персональной очереди для получения ответа
} db_request_msg_t;

// Сообщение ответа от координатора БД читателю или писателю
typedef struct {
    int status;          // 0 = Успех, -1 = Остановка
    int index;           // Индекс элемента
    int read_value;      // Прочитанное значение
    int old_value;       // Старое значение (для писателя)
    int new_value;       // Новое записанное значение
    int array_size;      // Текущий размер массива
    int data[MAX_MQ_ARRAY_SIZE]; // Снимок отсортированного массива
} db_response_msg_t;

// Разделяемое управляющее состояние для координации семафоров и статистики
typedef struct {
    sim_config_t cfg;
    
    // Счетчики синхронизации
    int read_count;
    int write_count;
    int active_readers;
    int active_writers;
    int invariant_violations;
    volatile int stop_flag;
    
    int registered_readers;
    int registered_writers;
    int completed_readers;
    int completed_writers;
    int coordinator_ready;

    uint64_t total_reads;
    uint64_t total_writes;
    worker_stats_t reader_stats[MAX_WORKERS];
    worker_stats_t writer_stats[MAX_WORKERS];
} ctrl_shared_state_t;

#endif // IPC_PROTOCOL_H
