#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <mqueue.h>
#include <time.h>
#include <errno.h>

#include "ipc_protocol.h"
#include "../common/utils.h"

static volatile sig_atomic_t g_stop = 0;
static ctrl_shared_state_t *g_ctrl = NULL;
static sem_t *g_sem_db = SEM_FAILED;
static sem_t *g_sem_rmutex = SEM_FAILED;
static sem_t *g_sem_wmutex = SEM_FAILED;
static sem_t *g_sem_readtry = SEM_FAILED;
static sem_t *g_sem_print = SEM_FAILED;
static mqd_t g_req_mq = (mqd_t)-1;
static mqd_t g_resp_mq = (mqd_t)-1;
static char g_resp_q_name[64];

static void cleanup_writer_ipc(void) {
    if (g_resp_q_name[0]) {
        mq_unlink(g_resp_q_name);
        g_resp_q_name[0] = '\0';
    }
    if (g_resp_mq != (mqd_t)-1) {
        mq_close(g_resp_mq);
        g_resp_mq = (mqd_t)-1;
    }
    if (g_req_mq != (mqd_t)-1) {
        mq_close(g_req_mq);
        g_req_mq = (mqd_t)-1;
    }
    if (g_sem_db != SEM_FAILED) { sem_close(g_sem_db); g_sem_db = SEM_FAILED; }
    if (g_sem_rmutex != SEM_FAILED) { sem_close(g_sem_rmutex); g_sem_rmutex = SEM_FAILED; }
    if (g_sem_wmutex != SEM_FAILED) { sem_close(g_sem_wmutex); g_sem_wmutex = SEM_FAILED; }
    if (g_sem_readtry != SEM_FAILED) { sem_close(g_sem_readtry); g_sem_readtry = SEM_FAILED; }
    if (g_sem_print != SEM_FAILED) { sem_close(g_sem_print); g_sem_print = SEM_FAILED; }
    if (g_ctrl && g_ctrl != MAP_FAILED) {
        munmap(g_ctrl, sizeof(ctrl_shared_state_t));
        g_ctrl = NULL;
    }
}

static void sig_handler(int sig) {
    (void)sig;
    g_stop = 1;
}

static void safe_sem_wait(sem_t *sem) {
    while (sem_wait(sem) != 0) {
        if (errno == EINTR) {
            continue;
        }
        break;
    }
}

static void db_write_lock(sim_config_t *cfg) {
    if (cfg->policy == POLICY_FAIR) {
        safe_sem_wait(g_sem_readtry);
        safe_sem_wait(g_sem_db);
        sem_post(g_sem_readtry);
    } else if (cfg->policy == POLICY_READERS) {
        safe_sem_wait(g_sem_db);
    } else if (cfg->policy == POLICY_WRITERS) {
        safe_sem_wait(g_sem_wmutex);
        g_ctrl->write_count++;
        if (g_ctrl->write_count == 1) {
            safe_sem_wait(g_sem_readtry);
        }
        sem_post(g_sem_wmutex);

        safe_sem_wait(g_sem_db);
    }
    g_ctrl->active_writers++;
}

static void db_write_unlock(sim_config_t *cfg) {
    g_ctrl->active_writers--;
    sem_post(g_sem_db);
    if (cfg->policy == POLICY_WRITERS) {
        safe_sem_wait(g_sem_wmutex);
        g_ctrl->write_count--;
        if (g_ctrl->write_count == 0) {
            sem_post(g_sem_readtry);
        }
        sem_post(g_sem_wmutex);
    }
}

int main(int argc, char **argv) {
    int id = -1;
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--id") == 0) && i + 1 < argc) {
            id = atoi(argv[i + 1]);
            break;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int shm_fd = shm_open(SHM_CTRL_NAME_9_10, O_RDWR, 0);
    if (shm_fd < 0) {
        dprintf(STDERR_FILENO, "Writer (MQ): Ошибка открытия управляющей shm '%s': %s (запущен ли coordinator?)\n",
                SHM_CTRL_NAME_9_10, strerror(errno));
        return EXIT_FAILURE;
    }

    g_ctrl = (ctrl_shared_state_t *)mmap(NULL, sizeof(ctrl_shared_state_t),
                                        PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (g_ctrl == MAP_FAILED) {
        dprintf(STDERR_FILENO, "Writer (MQ): Ошибка mmap: %s\n", strerror(errno));
        close(shm_fd);
        return EXIT_FAILURE;
    }
    close(shm_fd);

    if (!g_ctrl->coordinator_ready) {
        dprintf(STDERR_FILENO, "Writer (MQ): Coordinator еще не готов. Завершение.\n");
        munmap(g_ctrl, sizeof(ctrl_shared_state_t));
        return EXIT_FAILURE;
    }

    g_sem_db = sem_open(SEM_DB_9_10, 0);
    g_sem_rmutex = sem_open(SEM_RMUTEX_9_10, 0);
    g_sem_wmutex = sem_open(SEM_WMUTEX_9_10, 0);
    g_sem_readtry = sem_open(SEM_READTRY_9_10, 0);
    g_sem_print = sem_open(SEM_PRINT_9_10, 0);

    if (g_sem_db == SEM_FAILED || g_sem_rmutex == SEM_FAILED ||
        g_sem_wmutex == SEM_FAILED || g_sem_readtry == SEM_FAILED ||
        g_sem_print == SEM_FAILED) {
        dprintf(STDERR_FILENO, "Writer (MQ): Ошибка открытия семафоров: %s\n", strerror(errno));
        munmap(g_ctrl, sizeof(ctrl_shared_state_t));
        return EXIT_FAILURE;
    }

    if (id < 0) {
        safe_sem_wait(g_sem_wmutex);
        id = g_ctrl->registered_writers++;
        sem_post(g_sem_wmutex);
    }
    if (id < 0 || id >= MAX_WORKERS) {
        dprintf(STDERR_FILENO, "Writer (MQ): недопустимый ID %d (допустимо 0..%d)\n", id, MAX_WORKERS - 1);
        cleanup_writer_ipc();
        return EXIT_FAILURE;
    }

    snprintf(g_resp_q_name, sizeof(g_resp_q_name), "%sw_%d_%d", MQ_RESP_PREFIX, id, getpid());
    mq_unlink(g_resp_q_name);

    struct mq_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.mq_maxmsg = 5;
    attr.mq_msgsize = sizeof(db_response_msg_t);

    g_resp_mq = mq_open(g_resp_q_name, O_CREAT | O_RDWR | O_EXCL, 0666, &attr);
    if (g_resp_mq == (mqd_t)-1) {
        dprintf(STDERR_FILENO, "Writer (MQ): Ошибка создания очереди ответа '%s': %s\n",
                g_resp_q_name, strerror(errno));
        munmap(g_ctrl, sizeof(ctrl_shared_state_t));
        return EXIT_FAILURE;
    }
    atexit(cleanup_writer_ipc);

    g_req_mq = mq_open(MQ_REQUEST_NAME, O_WRONLY);
    if (g_req_mq == (mqd_t)-1) {
        dprintf(STDERR_FILENO, "Writer (MQ): Ошибка открытия очереди запросов '%s': %s\n",
                MQ_REQUEST_NAME, strerror(errno));
        cleanup_writer_ipc();
        return EXIT_FAILURE;
    }

    sim_config_t *cfg = &g_ctrl->cfg;
    unsigned int seed = cfg->seed + (unsigned int)id * 2027 + (unsigned int)getpid();
    int ops_done = 0;
    const char *policy_str = (cfg->policy == POLICY_READERS) ? "readers" :
                             (cfg->policy == POLICY_WRITERS) ? "writers" : "fair";

    safe_log(-1, g_sem_print, "[ATTACH] Писатель #%d (PID %d) подключился через очередь сообщений", id, getpid());

    while (!g_stop && !g_ctrl->stop_flag) {
        if (cfg->max_ops > 0 && ops_done >= cfg->max_ops) {
            break;
        }

        sleep_ms(random_range(&seed, cfg->delay_min_ms, cfg->delay_max_ms));
        if (g_stop || g_ctrl->stop_flag) break;

        uint64_t t_req = current_time_us();
        safe_log(-1, g_sem_print,
                 "[REQUEST] Писатель #%d (PID %d): запрос исключительного доступа на запись (политика: %s)",
                 id, getpid(), policy_str);

        db_write_lock(cfg);
        uint64_t t_wait = current_time_us() - t_req;

        worker_stats_t *st = &g_ctrl->writer_stats[id];
        st->total_wait_time_us += t_wait;
        if (st->ops_completed == 0 || t_wait > st->max_wait_time_us) st->max_wait_time_us = t_wait;
        if (st->ops_completed == 0 || t_wait < st->min_wait_time_us) st->min_wait_time_us = t_wait;

        safe_log(-1, g_sem_print,
                 "[START_WRITE] Писатель #%d (PID %d): получил исключительный доступ (active_writers: %d, ожидание: %.2f мс)",
                 id, getpid(), g_ctrl->active_writers, (double)t_wait / 1000.0);

        if (g_ctrl->active_writers != 1 || g_ctrl->active_readers > 0) {
            g_ctrl->invariant_violations++;
            safe_log(-1, g_sem_print,
                     "!!! [INVARIANT VIOLATION] Не эксклюзивный доступ писателя #%d! (writers=%d, readers=%d)",
                     id, g_ctrl->active_writers, g_ctrl->active_readers);
        }

        sleep_ms(random_range(&seed, cfg->work_min_ms, cfg->work_max_ms));

        int idx = random_range(&seed, cfg->idx_min, cfg->idx_max);
        int new_val = random_range(&seed, cfg->min_val, cfg->max_val);

        db_request_msg_t req;
        memset(&req, 0, sizeof(req));
        req.type = REQ_WRITE;
        req.worker_id = id;
        req.is_writer = 1;
        req.pid = getpid();
        req.index = idx;
        req.new_value = new_val;
        snprintf(req.resp_queue, sizeof(req.resp_queue), "%s", g_resp_q_name);

        mq_send(g_req_mq, (const char *)&req, sizeof(req), 0);

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;

        db_response_msg_t resp;
        if (mq_timedreceive(g_resp_mq, (char *)&resp, sizeof(resp), NULL, &ts) < 0) {
            db_write_unlock(cfg);
            break;
        }

        char arr_str[512];
        format_array(arr_str, sizeof(arr_str), resp.data, resp.array_size);

        safe_log(-1, g_sem_print,
                 "[WRITE] Писатель #%d (PID %d): заменил (через MQ) data[%d] (%d -> %d), массив отсортирован",
                 id, getpid(), idx, resp.old_value, resp.new_value);
        safe_log(-1, g_sem_print,
                 "[DB_STATE] Писатель #%d: текущий массив: %s", id, arr_str);

        g_ctrl->total_writes++;
        st->ops_completed++;
        ops_done++;

        db_write_unlock(cfg);

        safe_log(-1, g_sem_print,
                 "[END_WRITE] Писатель #%d (PID %d): завершил запись, освободил блокировку БД",
                 id, getpid());
    }

    if (!g_stop && !g_ctrl->stop_flag) {
        db_request_msg_t exit_msg;
        memset(&exit_msg, 0, sizeof(exit_msg));
        exit_msg.type = REQ_EXIT;
        exit_msg.worker_id = id;
        exit_msg.is_writer = 1;
        exit_msg.pid = getpid();
        mq_send(g_req_mq, (const char *)&exit_msg, sizeof(exit_msg), 0);

        safe_sem_wait(g_sem_wmutex);
        g_ctrl->completed_writers++;
        sem_post(g_sem_wmutex);
    }

    safe_log(-1, g_sem_print, "[DETACH] Писатель #%d (PID %d) завершил работу и отключился", id, getpid());

    cleanup_writer_ipc();
    return EXIT_SUCCESS;
}
