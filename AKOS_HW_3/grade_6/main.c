#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <errno.h>

#include "../common/types.h"
#include "../common/utils.h"
#include "../common/config.h"

typedef struct {
    sem_t sem_db;
    sem_t sem_rmutex;
    sem_t sem_wmutex;
    sem_t sem_readtry;
    sem_t sem_print;
    shared_database_t db;
} shm_with_unnamed_sems_t;

static volatile sig_atomic_t g_stop = 0;
static pid_t g_parent_pid = 0;
static char g_shm_name[128];
static shm_with_unnamed_sems_t *g_shm_block = NULL;
static shared_database_t *g_shm = NULL;

static pid_t g_children[MAX_WORKERS * 2];
static int g_child_count = 0;
static int g_log_fd = -1;

static void cleanup_parent_ipc(void) {
    if (getpid() != g_parent_pid) {
        return;
    }
    if (g_shm_block && g_shm_block != MAP_FAILED) {
        sem_destroy(&g_shm_block->sem_db);
        sem_destroy(&g_shm_block->sem_rmutex);
        sem_destroy(&g_shm_block->sem_wmutex);
        sem_destroy(&g_shm_block->sem_readtry);
        sem_destroy(&g_shm_block->sem_print);
        munmap(g_shm_block, sizeof(shm_with_unnamed_sems_t));
        g_shm_block = NULL;
    }
    if (g_shm_name[0]) {
        shm_unlink(g_shm_name);
        g_shm_name[0] = '\0';
    }
}

static void sig_handler(int sig) {
    (void)sig;
    g_stop = 1;
    if (g_shm) {
        g_shm->stop_flag = 1;
    }
}

static void safe_sem_wait(sem_t *sem) {
    while (sem_wait(sem) != 0) {
        if (errno == EINTR) {
            continue;
        }
        break;
    }
}

static void db_read_lock(sim_config_t *cfg) {
    if (cfg->policy == POLICY_FAIR) {
        safe_sem_wait(&g_shm_block->sem_readtry);
        safe_sem_wait(&g_shm_block->sem_rmutex);
        g_shm->read_count++;
        if (g_shm->read_count == 1) {
            safe_sem_wait(&g_shm_block->sem_db);
        }
        g_shm->active_readers++;
        sem_post(&g_shm_block->sem_rmutex);
        sem_post(&g_shm_block->sem_readtry);
    } else if (cfg->policy == POLICY_READERS) {
        safe_sem_wait(&g_shm_block->sem_rmutex);
        g_shm->read_count++;
        if (g_shm->read_count == 1) {
            safe_sem_wait(&g_shm_block->sem_db);
        }
        g_shm->active_readers++;
        sem_post(&g_shm_block->sem_rmutex);
    } else if (cfg->policy == POLICY_WRITERS) {
        safe_sem_wait(&g_shm_block->sem_readtry);
        safe_sem_wait(&g_shm_block->sem_rmutex);
        g_shm->read_count++;
        if (g_shm->read_count == 1) {
            safe_sem_wait(&g_shm_block->sem_db);
        }
        g_shm->active_readers++;
        sem_post(&g_shm_block->sem_rmutex);
        sem_post(&g_shm_block->sem_readtry);
    }
}

static void db_read_unlock(sim_config_t *cfg) {
    (void)cfg;
    safe_sem_wait(&g_shm_block->sem_rmutex);
    g_shm->active_readers--;
    g_shm->read_count--;
    if (g_shm->read_count == 0) {
        sem_post(&g_shm_block->sem_db);
    }
    sem_post(&g_shm_block->sem_rmutex);
}

static void db_write_lock(sim_config_t *cfg) {
    if (cfg->policy == POLICY_FAIR) {
        safe_sem_wait(&g_shm_block->sem_readtry);
        safe_sem_wait(&g_shm_block->sem_db);
        sem_post(&g_shm_block->sem_readtry);
    } else if (cfg->policy == POLICY_READERS) {
        safe_sem_wait(&g_shm_block->sem_db);
    } else if (cfg->policy == POLICY_WRITERS) {
        safe_sem_wait(&g_shm_block->sem_wmutex);
        g_shm->write_count++;
        if (g_shm->write_count == 1) {
            safe_sem_wait(&g_shm_block->sem_readtry);
        }
        sem_post(&g_shm_block->sem_wmutex);

        safe_sem_wait(&g_shm_block->sem_db);
    }
    g_shm->active_writers++;
}

static void db_write_unlock(sim_config_t *cfg) {
    g_shm->active_writers--;
    sem_post(&g_shm_block->sem_db);
    if (cfg->policy == POLICY_WRITERS) {
        safe_sem_wait(&g_shm_block->sem_wmutex);
        g_shm->write_count--;
        if (g_shm->write_count == 0) {
            sem_post(&g_shm_block->sem_readtry);
        }
        sem_post(&g_shm_block->sem_wmutex);
    }
}

static void reader_process(int id, sim_config_t *cfg) {
    unsigned int seed = cfg->seed + (unsigned int)id * 1013 + (unsigned int)getpid();
    int ops_done = 0;
    const char *policy_str = (cfg->policy == POLICY_READERS) ? "readers" :
                             (cfg->policy == POLICY_WRITERS) ? "writers" : "fair";

    while (!g_stop && !g_shm->stop_flag) {
        if (cfg->max_ops > 0 && ops_done >= cfg->max_ops) {
            break;
        }

        sleep_ms(random_range(&seed, cfg->delay_min_ms, cfg->delay_max_ms));
        if (g_stop || g_shm->stop_flag) break;

        uint64_t t_req = current_time_us();
        safe_log(g_log_fd, &g_shm_block->sem_print,
                 "[REQUEST] Читатель #%d (PID %d): запрос доступа на чтение (политика: %s)",
                 id, getpid(), policy_str);

        db_read_lock(cfg);
        uint64_t t_wait = current_time_us() - t_req;

        worker_stats_t *st = &g_shm->reader_stats[id];
        st->total_wait_time_us += t_wait;
        if (st->ops_completed == 0 || t_wait > st->max_wait_time_us) st->max_wait_time_us = t_wait;
        if (st->ops_completed == 0 || t_wait < st->min_wait_time_us) st->min_wait_time_us = t_wait;

        safe_log(g_log_fd, &g_shm_block->sem_print,
                 "[START_READ] Читатель #%d (PID %d): вошел в БД (активных читателей: %d, время ожидания: %.2f мс)",
                 id, getpid(), g_shm->active_readers, (double)t_wait / 1000.0);

        if (g_shm->active_writers > 0) {
            g_shm->invariant_violations++;
            safe_log(g_log_fd, &g_shm_block->sem_print,
                     "!!! [INVARIANT VIOLATION] Читатель #%d читает одновременно с писателем (active_writers=%d)!",
                     id, g_shm->active_writers);
        }

        sleep_ms(random_range(&seed, cfg->work_min_ms, cfg->work_max_ms));

        int idx = random_range(&seed, cfg->idx_min, cfg->idx_max);
        int val = g_shm->data[idx];
        uint64_t fib = calculate_fibonacci(val);

        safe_log(g_log_fd, &g_shm_block->sem_print,
                 "[READ] Читатель #%d (PID %d): прочитал data[%d] = %d; вычислено Fib(%d) = %llu",
                 id, getpid(), idx, val, val, (unsigned long long)fib);

        g_shm->total_reads++;
        st->ops_completed++;
        ops_done++;

        db_read_unlock(cfg);

        safe_log(g_log_fd, &g_shm_block->sem_print,
                 "[END_READ] Читатель #%d (PID %d): завершил чтение (осталось читателей в БД: %d)",
                 id, getpid(), g_shm->active_readers);
    }

    exit(EXIT_SUCCESS);
}

static void writer_process(int id, sim_config_t *cfg) {
    unsigned int seed = cfg->seed + (unsigned int)id * 2027 + (unsigned int)getpid();
    int ops_done = 0;
    const char *policy_str = (cfg->policy == POLICY_READERS) ? "readers" :
                             (cfg->policy == POLICY_WRITERS) ? "writers" : "fair";

    while (!g_stop && !g_shm->stop_flag) {
        if (cfg->max_ops > 0 && ops_done >= cfg->max_ops) {
            break;
        }

        sleep_ms(random_range(&seed, cfg->delay_min_ms, cfg->delay_max_ms));
        if (g_stop || g_shm->stop_flag) break;

        uint64_t t_req = current_time_us();
        safe_log(g_log_fd, &g_shm_block->sem_print,
                 "[REQUEST] Писатель #%d (PID %d): запрос исключительного доступа на запись (политика: %s)",
                 id, getpid(), policy_str);

        db_write_lock(cfg);
        uint64_t t_wait = current_time_us() - t_req;

        worker_stats_t *st = &g_shm->writer_stats[id];
        st->total_wait_time_us += t_wait;
        if (st->ops_completed == 0 || t_wait > st->max_wait_time_us) st->max_wait_time_us = t_wait;
        if (st->ops_completed == 0 || t_wait < st->min_wait_time_us) st->min_wait_time_us = t_wait;

        safe_log(g_log_fd, &g_shm_block->sem_print,
                 "[START_WRITE] Писатель #%d (PID %d): получил исключительный доступ (active_writers: %d, ожидание: %.2f мс)",
                 id, getpid(), g_shm->active_writers, (double)t_wait / 1000.0);

        if (g_shm->active_writers != 1 || g_shm->active_readers > 0) {
            g_shm->invariant_violations++;
            safe_log(g_log_fd, &g_shm_block->sem_print,
                     "!!! [INVARIANT VIOLATION] Не эксклюзивный доступ писателя #%d! (active_writers=%d, active_readers=%d)",
                     id, g_shm->active_writers, g_shm->active_readers);
        }

        sleep_ms(random_range(&seed, cfg->work_min_ms, cfg->work_max_ms));

        int idx = random_range(&seed, cfg->idx_min, cfg->idx_max);
        int old_val = g_shm->data[idx];
        int new_val = random_range(&seed, cfg->min_val, cfg->max_val);
        g_shm->data[idx] = new_val;

        sort_array(g_shm->data, g_shm->size);

        if (!is_array_sorted(g_shm->data, g_shm->size)) {
            g_shm->invariant_violations++;
            safe_log(g_log_fd, &g_shm_block->sem_print,
                     "!!! [INVARIANT VIOLATION] Нарушен порядок сортировки массива!");
        }

        char arr_str[512];
        format_array(arr_str, sizeof(arr_str), g_shm->data, g_shm->size);

        safe_log(g_log_fd, &g_shm_block->sem_print,
                 "[WRITE] Писатель #%d (PID %d): заменил data[%d] (%d -> %d), массив отсортирован",
                 id, getpid(), idx, old_val, new_val);
        safe_log(g_log_fd, &g_shm_block->sem_print,
                 "[DB_STATE] Писатель #%d: текущий массив: %s", id, arr_str);

        g_shm->total_writes++;
        st->ops_completed++;
        ops_done++;

        db_write_unlock(cfg);

        safe_log(g_log_fd, &g_shm_block->sem_print,
                 "[END_WRITE] Писатель #%d (PID %d): завершил запись, освободил блокировку БД",
                 id, getpid());
    }

    exit(EXIT_SUCCESS);
}

static void print_final_statistics(const sim_config_t *cfg, uint64_t total_time_us) {
    char header[] = "\n====================== ИТОГОВАЯ СТАТИСТИКА ======================\n";
    dprintf(STDOUT_FILENO, "%s", header);
    if (g_log_fd >= 0) dprintf(g_log_fd, "%s", header);

    safe_log(g_log_fd, NULL, "Общее время выполнения: %.2f сек", (double)total_time_us / 1000000.0);
    safe_log(g_log_fd, NULL, "Всего операций чтения: %lu, записи: %lu",
             (unsigned long)g_shm->total_reads, (unsigned long)g_shm->total_writes);
    safe_log(g_log_fd, NULL, "Нарушений инвариантов: %d (%s)",
             g_shm->invariant_violations,
             g_shm->invariant_violations == 0 ? "ВСЕ ИНВАРИАНТЫ СОБЛЮДЕНЫ" : "ЕСТЬ НАРУШЕНИЯ!");

    for (int i = 0; i < cfg->num_readers; i++) {
        worker_stats_t *st = &g_shm->reader_stats[i];
        double avg = st->ops_completed > 0 ? (double)st->total_wait_time_us / st->ops_completed / 1000.0 : 0.0;
        safe_log(g_log_fd, NULL, "  Читатель #%d: выполнено операций: %u, среднее ожидание: %.2f мс [мин: %.2f мс, макс: %.2f мс]",
                 i, st->ops_completed, avg, (double)st->min_wait_time_us / 1000.0, (double)st->max_wait_time_us / 1000.0);
    }

    for (int i = 0; i < cfg->num_writers; i++) {
        worker_stats_t *st = &g_shm->writer_stats[i];
        double avg = st->ops_completed > 0 ? (double)st->total_wait_time_us / st->ops_completed / 1000.0 : 0.0;
        safe_log(g_log_fd, NULL, "  Писатель #%d: выполнено операций: %u, среднее ожидание: %.2f мс [мин: %.2f мс, макс: %.2f мс]",
                 i, st->ops_completed, avg, (double)st->min_wait_time_us / 1000.0, (double)st->max_wait_time_us / 1000.0);
    }

    char final_arr_str[512];
    format_array(final_arr_str, sizeof(final_arr_str), g_shm->data, g_shm->size);
    safe_log(g_log_fd, NULL, "Финальное состояние массива: %s", final_arr_str);
    safe_log(g_log_fd, NULL, "Отсортированность финального массива: %s",
             is_array_sorted(g_shm->data, g_shm->size) ? "ДА" : "НЕТ");

    char footer[] = "=================================================================\n";
    dprintf(STDOUT_FILENO, "%s", footer);
    if (g_log_fd >= 0) dprintf(g_log_fd, "%s", footer);
}

int main(int argc, char **argv) {
    sim_config_t cfg;
    config_set_defaults(&cfg);

    if (config_parse_args(&cfg, argc, argv) != 0) {
        return EXIT_FAILURE;
    }

    if (cfg.log_file[0]) {
        g_log_fd = open(cfg.log_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (g_log_fd < 0) {
            dprintf(STDERR_FILENO, "Предупреждение: не удалось открыть лог-файл '%s': %s\n",
                    cfg.log_file, strerror(errno));
        }
    }

    config_print_summary(&cfg, STDOUT_FILENO);
    if (g_log_fd >= 0) {
        config_print_summary(&cfg, g_log_fd);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    pid_t parent_pid = getpid();
    g_parent_pid = parent_pid;
    atexit(cleanup_parent_ipc);
    snprintf(g_shm_name, sizeof(g_shm_name), "/akos_hw3_shm_6_%d", parent_pid);
    shm_unlink(g_shm_name);

    int shm_fd = shm_open(g_shm_name, O_CREAT | O_RDWR | O_EXCL, 0666);
    if (shm_fd < 0) {
        dprintf(STDERR_FILENO, "Ошибка shm_open: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    if (ftruncate(shm_fd, sizeof(shm_with_unnamed_sems_t)) != 0) {
        dprintf(STDERR_FILENO, "Ошибка ftruncate: %s\n", strerror(errno));
        close(shm_fd);
        shm_unlink(g_shm_name);
        return EXIT_FAILURE;
    }
    g_shm_block = (shm_with_unnamed_sems_t *)mmap(NULL, sizeof(shm_with_unnamed_sems_t),
                                                  PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (g_shm_block == MAP_FAILED) {
        dprintf(STDERR_FILENO, "Ошибка mmap: %s\n", strerror(errno));
        close(shm_fd);
        shm_unlink(g_shm_name);
        return EXIT_FAILURE;
    }
    close(shm_fd);

    g_shm = &g_shm_block->db;
    memset(g_shm, 0, sizeof(shared_database_t));
    g_shm->size = cfg.array_size;
    unsigned int gen_seed = cfg.seed;
    setup_initial_array(g_shm->data, &cfg, &gen_seed);

    char initial_arr_str[512];
    format_array(initial_arr_str, sizeof(initial_arr_str), g_shm->data, g_shm->size);
    dprintf(STDOUT_FILENO, "Начальное состояние базы данных: %s\n\n", initial_arr_str);
    if (g_log_fd >= 0) {
        dprintf(g_log_fd, "Начальное состояние базы данных: %s\n\n", initial_arr_str);
    }

    if (sem_init(&g_shm_block->sem_db, 1, 1) != 0 ||
        sem_init(&g_shm_block->sem_rmutex, 1, 1) != 0 ||
        sem_init(&g_shm_block->sem_wmutex, 1, 1) != 0 ||
        sem_init(&g_shm_block->sem_readtry, 1, 1) != 0 ||
        sem_init(&g_shm_block->sem_print, 1, 1) != 0) {
        dprintf(STDERR_FILENO, "Ошибка sem_init: %s\n", strerror(errno));
        munmap(g_shm_block, sizeof(shm_with_unnamed_sems_t));
        shm_unlink(g_shm_name);
        return EXIT_FAILURE;
    }

    uint64_t start_time = current_time_us();

    for (int i = 0; i < cfg.num_readers; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            dprintf(STDERR_FILENO, "Ошибка fork читателя #%d: %s\n", i, strerror(errno));
            break;
        } else if (pid == 0) {
            reader_process(i, &cfg);
        } else {
            g_children[g_child_count++] = pid;
        }
    }

    for (int i = 0; i < cfg.num_writers; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            dprintf(STDERR_FILENO, "Ошибка fork писателя #%d: %s\n", i, strerror(errno));
            break;
        } else if (pid == 0) {
            writer_process(i, &cfg);
        } else {
            g_children[g_child_count++] = pid;
        }
    }

    int term_sent = 0;
    int wait_cycles = 0;
    while (g_child_count > 0) {
        if (g_stop && !term_sent) {
            term_sent = 1;
            for (int i = 0; i < g_child_count; i++) {
                if (g_children[i] > 0) {
                    kill(g_children[i], SIGTERM);
                }
            }
        }

        int status;
        pid_t wpid = waitpid(-1, &status, WNOHANG);
        if (wpid > 0) {
            for (int i = 0; i < g_child_count; i++) {
                if (g_children[i] == wpid) {
                    g_children[i] = g_children[g_child_count - 1];
                    g_child_count--;
                    break;
                }
            }
        } else if (wpid == 0) {
            usleep(10000); // 10 мс
            if (term_sent) {
                wait_cycles++;
                if (wait_cycles > 100) { // Таймаут 1 сек после отправки SIGTERM
                    for (int i = 0; i < g_child_count; i++) {
                        if (g_children[i] > 0) {
                            kill(g_children[i], SIGKILL);
                        }
                    }
                }
            }
        } else {
            if (errno == EINTR) continue;
            break;
        }
    }

    uint64_t total_time = current_time_us() - start_time;
    print_final_statistics(&cfg, total_time);

    cleanup_parent_ipc();

    if (g_log_fd >= 0) close(g_log_fd);
    return EXIT_SUCCESS;
}
