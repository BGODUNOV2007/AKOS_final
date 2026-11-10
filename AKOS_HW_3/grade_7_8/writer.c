#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <errno.h>

#include "common_7_8.h"
#include "../common/utils.h"

static volatile sig_atomic_t g_stop = 0;
static independent_shm_t *g_shm_block = NULL;
static sem_t *g_sem_db = SEM_FAILED;
static sem_t *g_sem_rmutex = SEM_FAILED;
static sem_t *g_sem_wmutex = SEM_FAILED;
static sem_t *g_sem_readtry = SEM_FAILED;
static sem_t *g_sem_print = SEM_FAILED;

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
        g_shm_block->db.write_count++;
        if (g_shm_block->db.write_count == 1) {
            safe_sem_wait(g_sem_readtry);
        }
        sem_post(g_sem_wmutex);

        safe_sem_wait(g_sem_db);
    }
    g_shm_block->db.active_writers++;
}

static void db_write_unlock(sim_config_t *cfg) {
    g_shm_block->db.active_writers--;
    sem_post(g_sem_db);
    if (cfg->policy == POLICY_WRITERS) {
        safe_sem_wait(g_sem_wmutex);
        g_shm_block->db.write_count--;
        if (g_shm_block->db.write_count == 0) {
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

    int shm_fd = shm_open(SHM_NAME_7_8, O_RDWR, 0);
    if (shm_fd < 0) {
        dprintf(STDERR_FILENO, "Writer: Ошибка подключения к shm '%s': %s (запущен ли master?)\n",
                SHM_NAME_7_8, strerror(errno));
        return EXIT_FAILURE;
    }

    g_shm_block = (independent_shm_t *)mmap(NULL, sizeof(independent_shm_t),
                                           PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (g_shm_block == MAP_FAILED) {
        dprintf(STDERR_FILENO, "Writer: Ошибка mmap: %s\n", strerror(errno));
        close(shm_fd);
        return EXIT_FAILURE;
    }
    close(shm_fd);

    if (!g_shm_block->master_ready) {
        dprintf(STDERR_FILENO, "Writer: Master еще не готов. Завершение.\n");
        munmap(g_shm_block, sizeof(independent_shm_t));
        return EXIT_FAILURE;
    }

    g_sem_db = sem_open(SEM_DB_7_8, 0);
    g_sem_rmutex = sem_open(SEM_RMUTEX_7_8, 0);
    g_sem_wmutex = sem_open(SEM_WMUTEX_7_8, 0);
    g_sem_readtry = sem_open(SEM_READTRY_7_8, 0);
    g_sem_print = sem_open(SEM_PRINT_7_8, 0);

    if (g_sem_db == SEM_FAILED || g_sem_rmutex == SEM_FAILED ||
        g_sem_wmutex == SEM_FAILED || g_sem_readtry == SEM_FAILED ||
        g_sem_print == SEM_FAILED) {
        dprintf(STDERR_FILENO, "Writer: Ошибка открытия семафоров: %s\n", strerror(errno));
        munmap(g_shm_block, sizeof(independent_shm_t));
        return EXIT_FAILURE;
    }

    if (id < 0) {
        safe_sem_wait(g_sem_wmutex);
        id = g_shm_block->registered_writers++;
        sem_post(g_sem_wmutex);
    }
    if (id < 0 || id >= MAX_WORKERS) {
        dprintf(STDERR_FILENO, "Writer: недопустимый ID %d (допустимо 0..%d)\n", id, MAX_WORKERS - 1);
        sem_close(g_sem_db); sem_close(g_sem_rmutex); sem_close(g_sem_wmutex);
        sem_close(g_sem_readtry); sem_close(g_sem_print);
        munmap(g_shm_block, sizeof(independent_shm_t));
        return EXIT_FAILURE;
    }

    sim_config_t *cfg = &g_shm_block->cfg;
    unsigned int seed = cfg->seed + (unsigned int)id * 2027 + (unsigned int)getpid();
    int ops_done = 0;
    const char *policy_str = (cfg->policy == POLICY_READERS) ? "readers" :
                             (cfg->policy == POLICY_WRITERS) ? "writers" : "fair";

    safe_log(-1, g_sem_print, "[ATTACH] Писатель #%d (PID %d) подключился к БД", id, getpid());

    while (!g_stop && !g_shm_block->db.stop_flag) {
        if (cfg->max_ops > 0 && ops_done >= cfg->max_ops) {
            break;
        }

        sleep_ms(random_range(&seed, cfg->delay_min_ms, cfg->delay_max_ms));
        if (g_stop || g_shm_block->db.stop_flag) break;

        uint64_t t_req = current_time_us();
        safe_log(-1, g_sem_print,
                 "[REQUEST] Писатель #%d (PID %d): запрос исключительного доступа на запись (политика: %s)",
                 id, getpid(), policy_str);

        db_write_lock(cfg);
        uint64_t t_wait = current_time_us() - t_req;

        worker_stats_t *st = &g_shm_block->db.writer_stats[id];
        st->total_wait_time_us += t_wait;
        if (st->ops_completed == 0 || t_wait > st->max_wait_time_us) st->max_wait_time_us = t_wait;
        if (st->ops_completed == 0 || t_wait < st->min_wait_time_us) st->min_wait_time_us = t_wait;

        safe_log(-1, g_sem_print,
                 "[START_WRITE] Писатель #%d (PID %d): получил исключительный доступ (active_writers: %d, ожидание: %.2f мс)",
                 id, getpid(), g_shm_block->db.active_writers, (double)t_wait / 1000.0);

        if (g_shm_block->db.active_writers != 1 || g_shm_block->db.active_readers > 0) {
            g_shm_block->db.invariant_violations++;
            safe_log(-1, g_sem_print,
                     "!!! [INVARIANT VIOLATION] Не эксклюзивный доступ писателя #%d! (writers=%d, readers=%d)",
                     id, g_shm_block->db.active_writers, g_shm_block->db.active_readers);
        }

        sleep_ms(random_range(&seed, cfg->work_min_ms, cfg->work_max_ms));

        int idx = random_range(&seed, cfg->idx_min, cfg->idx_max);
        int old_val = g_shm_block->db.data[idx];
        int new_val = random_range(&seed, cfg->min_val, cfg->max_val);
        g_shm_block->db.data[idx] = new_val;

        sort_array(g_shm_block->db.data, g_shm_block->db.size);

        if (!is_array_sorted(g_shm_block->db.data, g_shm_block->db.size)) {
            g_shm_block->db.invariant_violations++;
            safe_log(-1, g_sem_print,
                     "!!! [INVARIANT VIOLATION] Нарушен порядок сортировки массива!");
        }

        char arr_str[512];
        format_array(arr_str, sizeof(arr_str), g_shm_block->db.data, g_shm_block->db.size);

        safe_log(-1, g_sem_print,
                 "[WRITE] Писатель #%d (PID %d): заменил data[%d] (%d -> %d), массив отсортирован",
                 id, getpid(), idx, old_val, new_val);
        safe_log(-1, g_sem_print,
                 "[DB_STATE] Писатель #%d: текущий массив: %s", id, arr_str);

        g_shm_block->db.total_writes++;
        st->ops_completed++;
        ops_done++;

        db_write_unlock(cfg);

        safe_log(-1, g_sem_print,
                 "[END_WRITE] Писатель #%d (PID %d): завершил запись, освободил блокировку БД",
                 id, getpid());
    }

    if (!g_stop && !g_shm_block->db.stop_flag) {
        safe_sem_wait(g_sem_wmutex);
        g_shm_block->completed_writers++;
        sem_post(g_sem_wmutex);
    }

    safe_log(-1, g_sem_print, "[DETACH] Писатель #%d (PID %d) завершил работу и отключился", id, getpid());

    sem_close(g_sem_db);
    sem_close(g_sem_rmutex);
    sem_close(g_sem_wmutex);
    sem_close(g_sem_readtry);
    sem_close(g_sem_print);
    munmap(g_shm_block, sizeof(independent_shm_t));

    return EXIT_SUCCESS;
}
