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
#include <errno.h>

#include "common_7_8.h"
#include "../common/utils.h"
#include "../common/config.h"

static volatile sig_atomic_t g_stop = 0;
static independent_shm_t *g_shm_block = NULL;
static sem_t *g_sem_db = SEM_FAILED;
static sem_t *g_sem_rmutex = SEM_FAILED;
static sem_t *g_sem_wmutex = SEM_FAILED;
static sem_t *g_sem_readtry = SEM_FAILED;
static sem_t *g_sem_print = SEM_FAILED;
static int g_log_fd = -1;

static void cleanup_master_ipc(void) {
    if (g_sem_db != SEM_FAILED) { sem_close(g_sem_db); g_sem_db = SEM_FAILED; }
    if (g_sem_rmutex != SEM_FAILED) { sem_close(g_sem_rmutex); g_sem_rmutex = SEM_FAILED; }
    if (g_sem_wmutex != SEM_FAILED) { sem_close(g_sem_wmutex); g_sem_wmutex = SEM_FAILED; }
    if (g_sem_readtry != SEM_FAILED) { sem_close(g_sem_readtry); g_sem_readtry = SEM_FAILED; }
    if (g_sem_print != SEM_FAILED) { sem_close(g_sem_print); g_sem_print = SEM_FAILED; }

    sem_unlink(SEM_DB_7_8);
    sem_unlink(SEM_RMUTEX_7_8);
    sem_unlink(SEM_WMUTEX_7_8);
    sem_unlink(SEM_READTRY_7_8);
    sem_unlink(SEM_PRINT_7_8);

    if (g_shm_block && g_shm_block != MAP_FAILED) {
        munmap(g_shm_block, sizeof(independent_shm_t));
        g_shm_block = NULL;
    }
    shm_unlink(SHM_NAME_7_8);
}

static void sig_handler(int sig) {
    (void)sig;
    g_stop = 1;
    if (g_shm_block) {
        g_shm_block->db.stop_flag = 1;
    }
}

static void print_final_statistics(const sim_config_t *cfg, uint64_t total_time_us) {
    char header[] = "\n====================== ИТОГОВАЯ СТАТИСТИКА (MASTER 7-8) ======================\n";
    dprintf(STDOUT_FILENO, "%s", header);
    if (g_log_fd >= 0) dprintf(g_log_fd, "%s", header);

    safe_log(g_log_fd, NULL, "Общее время выполнения: %.2f сек", (double)total_time_us / 1000000.0);
    safe_log(g_log_fd, NULL, "Всего операций чтения: %lu, записи: %lu",
             (unsigned long)g_shm_block->db.total_reads, (unsigned long)g_shm_block->db.total_writes);
    safe_log(g_log_fd, NULL, "Нарушений инвариантов: %d (%s)",
             g_shm_block->db.invariant_violations,
             g_shm_block->db.invariant_violations == 0 ? "ВСЕ ИНВАРИАНТЫ СОБЛЮДЕНЫ" : "ЕСТЬ НАРУШЕНИЯ!");

    for (int i = 0; i < cfg->num_readers; i++) {
        worker_stats_t *st = &g_shm_block->db.reader_stats[i];
        double avg = st->ops_completed > 0 ? (double)st->total_wait_time_us / st->ops_completed / 1000.0 : 0.0;
        safe_log(g_log_fd, NULL, "  Читатель #%d: выполнено операций: %u, среднее ожидание: %.2f мс [мин: %.2f мс, макс: %.2f мс]",
                 i, st->ops_completed, avg, (double)st->min_wait_time_us / 1000.0, (double)st->max_wait_time_us / 1000.0);
    }

    for (int i = 0; i < cfg->num_writers; i++) {
        worker_stats_t *st = &g_shm_block->db.writer_stats[i];
        double avg = st->ops_completed > 0 ? (double)st->total_wait_time_us / st->ops_completed / 1000.0 : 0.0;
        safe_log(g_log_fd, NULL, "  Писатель #%d: выполнено операций: %u, среднее ожидание: %.2f мс [мин: %.2f мс, макс: %.2f мс]",
                 i, st->ops_completed, avg, (double)st->min_wait_time_us / 1000.0, (double)st->max_wait_time_us / 1000.0);
    }

    char final_arr_str[512];
    format_array(final_arr_str, sizeof(final_arr_str), g_shm_block->db.data, g_shm_block->db.size);
    safe_log(g_log_fd, NULL, "Финальное состояние массива: %s", final_arr_str);
    safe_log(g_log_fd, NULL, "Отсортированность финального массива: %s",
             is_array_sorted(g_shm_block->db.data, g_shm_block->db.size) ? "ДА" : "НЕТ");

    char footer[] = "==============================================================================\n";
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

    atexit(cleanup_master_ipc);

    // Удаление возможных старых IPC-ресурсов
    shm_unlink(SHM_NAME_7_8);
    sem_unlink(SEM_DB_7_8);
    sem_unlink(SEM_RMUTEX_7_8);
    sem_unlink(SEM_WMUTEX_7_8);
    sem_unlink(SEM_READTRY_7_8);
    sem_unlink(SEM_PRINT_7_8);

    // Создание POSIX разделяемой памяти
    int shm_fd = shm_open(SHM_NAME_7_8, O_CREAT | O_RDWR | O_EXCL, 0666);
    if (shm_fd < 0) {
        dprintf(STDERR_FILENO, "Master: Ошибка shm_open: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    if (ftruncate(shm_fd, sizeof(independent_shm_t)) != 0) {
        dprintf(STDERR_FILENO, "Master: Ошибка ftruncate: %s\n", strerror(errno));
        close(shm_fd);
        shm_unlink(SHM_NAME_7_8);
        return EXIT_FAILURE;
    }
    g_shm_block = (independent_shm_t *)mmap(NULL, sizeof(independent_shm_t),
                                           PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (g_shm_block == MAP_FAILED) {
        dprintf(STDERR_FILENO, "Master: Ошибка mmap: %s\n", strerror(errno));
        close(shm_fd);
        shm_unlink(SHM_NAME_7_8);
        return EXIT_FAILURE;
    }
    close(shm_fd);

    memset(g_shm_block, 0, sizeof(independent_shm_t));
    g_shm_block->cfg = cfg;
    g_shm_block->db.size = cfg.array_size;

    unsigned int gen_seed = cfg.seed;
    setup_initial_array(g_shm_block->db.data, &cfg, &gen_seed);

    char initial_arr_str[512];
    format_array(initial_arr_str, sizeof(initial_arr_str), g_shm_block->db.data, g_shm_block->db.size);
    dprintf(STDOUT_FILENO, "Master: Начальное состояние базы данных: %s\n\n", initial_arr_str);
    if (g_log_fd >= 0) {
        dprintf(g_log_fd, "Master: Начальное состояние базы данных: %s\n\n", initial_arr_str);
    }

    // Создание именованных семафоров POSIX
    g_sem_db = sem_open(SEM_DB_7_8, O_CREAT | O_EXCL, 0666, 1);
    g_sem_rmutex = sem_open(SEM_RMUTEX_7_8, O_CREAT | O_EXCL, 0666, 1);
    g_sem_wmutex = sem_open(SEM_WMUTEX_7_8, O_CREAT | O_EXCL, 0666, 1);
    g_sem_readtry = sem_open(SEM_READTRY_7_8, O_CREAT | O_EXCL, 0666, 1);
    g_sem_print = sem_open(SEM_PRINT_7_8, O_CREAT | O_EXCL, 0666, 1);

    if (g_sem_db == SEM_FAILED || g_sem_rmutex == SEM_FAILED ||
        g_sem_wmutex == SEM_FAILED || g_sem_readtry == SEM_FAILED ||
        g_sem_print == SEM_FAILED) {
        dprintf(STDERR_FILENO, "Master: Ошибка sem_open: %s\n", strerror(errno));
        munmap(g_shm_block, sizeof(independent_shm_t));
        shm_unlink(SHM_NAME_7_8);
        return EXIT_FAILURE;
    }

    g_shm_block->master_ready = 1;
    safe_log(g_log_fd, g_sem_print, "[MASTER] Инициализация завершена. Ожидание запуска читателей и писателей...");

    uint64_t start_time = current_time_us();

    // Цикл мониторинга независимых процессов
    while (!g_stop) {
        // Проверяем завершение всех участников при заданном max_ops > 0
        if (cfg.max_ops > 0) {
            if (g_shm_block->completed_readers >= cfg.num_readers &&
                g_shm_block->completed_writers >= cfg.num_writers) {
                safe_log(g_log_fd, g_sem_print,
                         "[MASTER] Все %d читателей и %d писателей завершили работу.",
                         cfg.num_readers, cfg.num_writers);
                break;
            }
        }
        usleep(50000); // 50 мс
    }

    if (g_stop) {
        safe_log(g_log_fd, g_sem_print, "[MASTER] Получен сигнал прерывания. Инициирована остановка всех процессов...");
        g_shm_block->db.stop_flag = 1;
        // Даем небольшую паузу активным процессам для выхода
        usleep(200000);
    }

    uint64_t total_time = current_time_us() - start_time;
    print_final_statistics(&cfg, total_time);

    cleanup_master_ipc();

    if (g_log_fd >= 0) close(g_log_fd);
    return EXIT_SUCCESS;
}
