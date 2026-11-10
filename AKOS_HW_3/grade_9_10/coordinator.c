#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <mqueue.h>
#include <time.h>
#include <errno.h>

#include "ipc_protocol.h"
#include "../common/utils.h"
#include "../common/config.h"

static volatile sig_atomic_t g_stop = 0;
static ctrl_shared_state_t *g_ctrl = NULL;
static sem_t *g_sem_db = SEM_FAILED;
static sem_t *g_sem_rmutex = SEM_FAILED;
static sem_t *g_sem_wmutex = SEM_FAILED;
static sem_t *g_sem_readtry = SEM_FAILED;
static sem_t *g_sem_print = SEM_FAILED;
static mqd_t g_req_mq = (mqd_t)-1;
static int g_log_fd = -1;

static void cleanup_coordinator_ipc(void) {
    if (g_req_mq != (mqd_t)-1) {
        mq_close(g_req_mq);
        g_req_mq = (mqd_t)-1;
    }
    mq_unlink(MQ_REQUEST_NAME);

    if (g_sem_db != SEM_FAILED) { sem_close(g_sem_db); g_sem_db = SEM_FAILED; }
    if (g_sem_rmutex != SEM_FAILED) { sem_close(g_sem_rmutex); g_sem_rmutex = SEM_FAILED; }
    if (g_sem_wmutex != SEM_FAILED) { sem_close(g_sem_wmutex); g_sem_wmutex = SEM_FAILED; }
    if (g_sem_readtry != SEM_FAILED) { sem_close(g_sem_readtry); g_sem_readtry = SEM_FAILED; }
    if (g_sem_print != SEM_FAILED) { sem_close(g_sem_print); g_sem_print = SEM_FAILED; }

    sem_unlink(SEM_DB_9_10);
    sem_unlink(SEM_RMUTEX_9_10);
    sem_unlink(SEM_WMUTEX_9_10);
    sem_unlink(SEM_READTRY_9_10);
    sem_unlink(SEM_PRINT_9_10);

    if (g_ctrl && g_ctrl != MAP_FAILED) {
        munmap(g_ctrl, sizeof(ctrl_shared_state_t));
        g_ctrl = NULL;
    }
    shm_unlink(SHM_CTRL_NAME_9_10);
}

static int g_local_data[MAX_ARRAY_SIZE];
static int g_local_size = 0;

static void sig_handler(int sig) {
    (void)sig;
    g_stop = 1;
    if (g_ctrl) {
        g_ctrl->stop_flag = 1;
    }
}

static void print_final_statistics(const sim_config_t *cfg, uint64_t total_time_us) {
    char header[] = "\n====================== ИТОГОВАЯ СТАТИСТИКА (COORDINATOR 9-10) ======================\n";
    dprintf(STDOUT_FILENO, "%s", header);
    if (g_log_fd >= 0) dprintf(g_log_fd, "%s", header);

    safe_log(g_log_fd, NULL, "Общее время выполнения: %.2f сек", (double)total_time_us / 1000000.0);
    safe_log(g_log_fd, NULL, "Всего операций чтения: %lu, записи: %lu (через очереди сообщений POSIX)",
             (unsigned long)g_ctrl->total_reads, (unsigned long)g_ctrl->total_writes);
    safe_log(g_log_fd, NULL, "Нарушений инвариантов: %d (%s)",
             g_ctrl->invariant_violations,
             g_ctrl->invariant_violations == 0 ? "ВСЕ ИНВАРИАНТЫ СОБЛЮДЕНЫ" : "ЕСТЬ НАРУШЕНИЯ!");

    for (int i = 0; i < cfg->num_readers; i++) {
        worker_stats_t *st = &g_ctrl->reader_stats[i];
        double avg = st->ops_completed > 0 ? (double)st->total_wait_time_us / st->ops_completed / 1000.0 : 0.0;
        safe_log(g_log_fd, NULL, "  Читатель #%d: выполнено операций: %u, среднее ожидание: %.2f мс [мин: %.2f мс, макс: %.2f мс]",
                 i, st->ops_completed, avg, (double)st->min_wait_time_us / 1000.0, (double)st->max_wait_time_us / 1000.0);
    }

    for (int i = 0; i < cfg->num_writers; i++) {
        worker_stats_t *st = &g_ctrl->writer_stats[i];
        double avg = st->ops_completed > 0 ? (double)st->total_wait_time_us / st->ops_completed / 1000.0 : 0.0;
        safe_log(g_log_fd, NULL, "  Писатель #%d: выполнено операций: %u, среднее ожидание: %.2f мс [мин: %.2f мс, макс: %.2f мс]",
                 i, st->ops_completed, avg, (double)st->min_wait_time_us / 1000.0, (double)st->max_wait_time_us / 1000.0);
    }

    char final_arr_str[512];
    format_array(final_arr_str, sizeof(final_arr_str), g_local_data, g_local_size);
    safe_log(g_log_fd, NULL, "Финальное состояние массива в памяти сервера: %s", final_arr_str);
    safe_log(g_log_fd, NULL, "Отсортированность финального массива: %s",
             is_array_sorted(g_local_data, g_local_size) ? "ДА" : "НЕТ");

    char footer[] = "====================================================================================\n";
    dprintf(STDOUT_FILENO, "%s", footer);
    if (g_log_fd >= 0) dprintf(g_log_fd, "%s", footer);
}

int main(int argc, char **argv) {
    sim_config_t cfg;
    config_set_defaults(&cfg);

    if (config_parse_args(&cfg, argc, argv) != 0) {
        return EXIT_FAILURE;
    }
    if (cfg.array_size > MAX_MQ_ARRAY_SIZE) {
        dprintf(STDERR_FILENO,
                "Coordinator: размер массива (%d) превышает предел для очереди сообщений POSIX (%d)\n",
                cfg.array_size, MAX_MQ_ARRAY_SIZE);
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

    atexit(cleanup_coordinator_ipc);

    // Очистка старых IPC-ресурсов
    mq_unlink(MQ_REQUEST_NAME);
    shm_unlink(SHM_CTRL_NAME_9_10);
    sem_unlink(SEM_DB_9_10);
    sem_unlink(SEM_RMUTEX_9_10);
    sem_unlink(SEM_WMUTEX_9_10);
    sem_unlink(SEM_READTRY_9_10);
    sem_unlink(SEM_PRINT_9_10);

    // Создание управляющего блока в shm (для счетчиков синхронизации и статистики)
    int shm_fd = shm_open(SHM_CTRL_NAME_9_10, O_CREAT | O_RDWR | O_EXCL, 0666);
    if (shm_fd < 0) {
        dprintf(STDERR_FILENO, "Coordinator: Ошибка shm_open: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    if (ftruncate(shm_fd, sizeof(ctrl_shared_state_t)) != 0) {
        dprintf(STDERR_FILENO, "Coordinator: Ошибка ftruncate: %s\n", strerror(errno));
        close(shm_fd);
        shm_unlink(SHM_CTRL_NAME_9_10);
        return EXIT_FAILURE;
    }
    g_ctrl = (ctrl_shared_state_t *)mmap(NULL, sizeof(ctrl_shared_state_t),
                                        PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (g_ctrl == MAP_FAILED) {
        dprintf(STDERR_FILENO, "Coordinator: Ошибка mmap: %s\n", strerror(errno));
        close(shm_fd);
        shm_unlink(SHM_CTRL_NAME_9_10);
        return EXIT_FAILURE;
    }
    close(shm_fd);

    memset(g_ctrl, 0, sizeof(ctrl_shared_state_t));
    g_ctrl->cfg = cfg;

    // Инициализация локальной базы данных в приватной памяти координатора
    g_local_size = cfg.array_size;
    unsigned int gen_seed = cfg.seed;
    setup_initial_array(g_local_data, &cfg, &gen_seed);

    char initial_arr_str[512];
    format_array(initial_arr_str, sizeof(initial_arr_str), g_local_data, g_local_size);
    dprintf(STDOUT_FILENO, "Coordinator: Начальное состояние базы данных: %s\n\n", initial_arr_str);
    if (g_log_fd >= 0) {
        dprintf(g_log_fd, "Coordinator: Начальное состояние базы данных: %s\n\n", initial_arr_str);
    }

    // Создание именованных семафоров POSIX
    g_sem_db = sem_open(SEM_DB_9_10, O_CREAT | O_EXCL, 0666, 1);
    g_sem_rmutex = sem_open(SEM_RMUTEX_9_10, O_CREAT | O_EXCL, 0666, 1);
    g_sem_wmutex = sem_open(SEM_WMUTEX_9_10, O_CREAT | O_EXCL, 0666, 1);
    g_sem_readtry = sem_open(SEM_READTRY_9_10, O_CREAT | O_EXCL, 0666, 1);
    g_sem_print = sem_open(SEM_PRINT_9_10, O_CREAT | O_EXCL, 0666, 1);

    if (g_sem_db == SEM_FAILED || g_sem_rmutex == SEM_FAILED ||
        g_sem_wmutex == SEM_FAILED || g_sem_readtry == SEM_FAILED ||
        g_sem_print == SEM_FAILED) {
        dprintf(STDERR_FILENO, "Coordinator: Ошибка sem_open: %s\n", strerror(errno));
        munmap(g_ctrl, sizeof(ctrl_shared_state_t));
        shm_unlink(SHM_CTRL_NAME_9_10);
        return EXIT_FAILURE;
    }

    // Создание очереди сообщений POSIX для входящих запросов
    struct mq_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(db_request_msg_t);

    g_req_mq = mq_open(MQ_REQUEST_NAME, O_CREAT | O_RDWR | O_EXCL, 0666, &attr);
    if (g_req_mq == (mqd_t)-1) {
        dprintf(STDERR_FILENO, "Coordinator: Ошибка mq_open: %s\n", strerror(errno));
        sem_close(g_sem_db);
        sem_close(g_sem_rmutex);
        sem_close(g_sem_wmutex);
        sem_close(g_sem_readtry);
        sem_close(g_sem_print);
        munmap(g_ctrl, sizeof(ctrl_shared_state_t));
        shm_unlink(SHM_CTRL_NAME_9_10);
        return EXIT_FAILURE;
    }

    g_ctrl->coordinator_ready = 1;
    safe_log(g_log_fd, g_sem_print,
             "[COORDINATOR] Очередь сообщений '%s' создана. Ожидание запросов...", MQ_REQUEST_NAME);

    uint64_t start_time = current_time_us();

    // Главный цикл координатора
    while (!g_stop) {
        // Проверка завершения всех участников при max_ops > 0
        if (cfg.max_ops > 0) {
            if (g_ctrl->completed_readers >= cfg.num_readers &&
                g_ctrl->completed_writers >= cfg.num_writers) {
                safe_log(g_log_fd, g_sem_print,
                         "[COORDINATOR] Все %d читателей и %d писателей завершили работу.",
                         cfg.num_readers, cfg.num_writers);
                break;
            }
        }

        // Ожидание входящего запроса с таймаутом 100 мс (для периодической проверки stop_flag)
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000L; // 100 мс
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }

        db_request_msg_t req;
        ssize_t bytes_read = mq_timedreceive(g_req_mq, (char *)&req, sizeof(req), NULL, &ts);

        if (bytes_read < 0) {
            if (errno == ETIMEDOUT || errno == EINTR) {
                continue;
            }
            dprintf(STDERR_FILENO, "Coordinator: Ошибка mq_timedreceive: %s\n", strerror(errno));
            break;
        }

        // Обработка запроса
        if (req.type == REQ_READ) {
            // Чтение элемента
            db_response_msg_t resp;
            memset(&resp, 0, sizeof(resp));
            resp.status = 0;
            resp.index = req.index;
            if (req.index >= 0 && req.index < g_local_size) {
                resp.read_value = g_local_data[req.index];
            } else {
                resp.read_value = g_local_data[0];
            }

            // Отправка ответа в очередь читателя
            mqd_t resp_mq = mq_open(req.resp_queue, O_WRONLY);
            if (resp_mq != (mqd_t)-1) {
                mq_send(resp_mq, (const char *)&resp, sizeof(resp), 0);
                mq_close(resp_mq);
            }
        } else if (req.type == REQ_WRITE) {
            // Запись элемента
            db_response_msg_t resp;
            memset(&resp, 0, sizeof(resp));
            resp.status = 0;
            resp.index = req.index;
            int idx = (req.index >= 0 && req.index < g_local_size) ? req.index : 0;
            resp.old_value = g_local_data[idx];
            resp.new_value = req.new_value;

            g_local_data[idx] = req.new_value;
            sort_array(g_local_data, g_local_size);

            if (!is_array_sorted(g_local_data, g_local_size)) {
                g_ctrl->invariant_violations++;
            }

            int copy_cnt = (g_local_size > MAX_MQ_ARRAY_SIZE) ? MAX_MQ_ARRAY_SIZE : g_local_size;
            resp.array_size = copy_cnt;
            memcpy(resp.data, g_local_data, sizeof(int) * (size_t)copy_cnt);

            // Отправка ответа в очередь писателя
            mqd_t resp_mq = mq_open(req.resp_queue, O_WRONLY);
            if (resp_mq != (mqd_t)-1) {
                mq_send(resp_mq, (const char *)&resp, sizeof(resp), 0);
                mq_close(resp_mq);
            }
        } else if (req.type == REQ_EXIT) {
            // Уведомление об отключении участника
            safe_log(g_log_fd, g_sem_print,
                     "[COORDINATOR] Получено сообщение о завершении от %s #%d (PID %d)",
                     req.is_writer ? "писателя" : "читателя", req.worker_id, req.pid);
        }
    }

    if (g_stop) {
        safe_log(g_log_fd, g_sem_print, "[COORDINATOR] Сигнал прерывания. Завершение работы...");
        g_ctrl->stop_flag = 1;
        usleep(200000);
    }

    uint64_t total_time = current_time_us() - start_time;
    print_final_statistics(&cfg, total_time);

    cleanup_coordinator_ipc();

    if (g_log_fd >= 0) close(g_log_fd);
    return EXIT_SUCCESS;
}
