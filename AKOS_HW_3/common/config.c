#define _GNU_SOURCE
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>

void config_set_defaults(sim_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->num_readers = 3;
    cfg->num_writers = 2;
    cfg->array_size = 10;
    cfg->max_ops = 5;
    cfg->idx_min = 0;
    cfg->idx_max = -1; // По умолчанию будет установлен в array_size - 1
    cfg->min_val = 1;
    cfg->max_val = 40;
    cfg->has_init_array = 0;
    cfg->delay_min_ms = 100;
    cfg->delay_max_ms = 300;
    cfg->work_min_ms = 50;
    cfg->work_max_ms = 150;
    cfg->policy = POLICY_FAIR;
    cfg->seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    cfg->log_file[0] = '\0';
    cfg->config_file[0] = '\0';
}

static char *trim_whitespace(char *str) {
    char *end;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

static int parse_init_array(sim_config_t *cfg, const char *val) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", val);

    int count = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(buf, ", \t", &saveptr);
    while (tok != NULL) {
        if (count >= MAX_ARRAY_SIZE) {
            dprintf(STDERR_FILENO, "Ошибка: превышен максимальный размер массива (%d)\n", MAX_ARRAY_SIZE);
            return -1;
        }
        char *endptr = NULL;
        long num = strtol(tok, &endptr, 10);
        if (*tok == '\0' || (endptr && *endptr != '\0') || num < 1) {
            dprintf(STDERR_FILENO, "Ошибка: элемент массива '%s' должен быть положительным целым числом (>= 1)\n", tok);
            return -1;
        }
        cfg->init_array[count++] = (int)num;
        tok = strtok_r(NULL, ", \t", &saveptr);
    }

    if (count < 1) {
        dprintf(STDERR_FILENO, "Ошибка: задан пустой начальный массив\n");
        return -1;
    }

    cfg->has_init_array = count;
    cfg->array_size = count;
    return 0;
}

static int apply_key_value(sim_config_t *cfg, const char *key, const char *val) {
    if (strcasecmp(key, "readers") == 0 || strcasecmp(key, "num_readers") == 0) {
        cfg->num_readers = atoi(val);
    } else if (strcasecmp(key, "writers") == 0 || strcasecmp(key, "num_writers") == 0) {
        cfg->num_writers = atoi(val);
    } else if (strcasecmp(key, "size") == 0 || strcasecmp(key, "array_size") == 0) {
        cfg->array_size = atoi(val);
    } else if (strcasecmp(key, "ops") == 0 || strcasecmp(key, "max_ops") == 0) {
        cfg->max_ops = atoi(val);
    } else if (strcasecmp(key, "idx_min") == 0) {
        cfg->idx_min = atoi(val);
    } else if (strcasecmp(key, "idx_max") == 0) {
        cfg->idx_max = atoi(val);
    } else if (strcasecmp(key, "min_val") == 0) {
        cfg->min_val = atoi(val);
    } else if (strcasecmp(key, "max_val") == 0) {
        cfg->max_val = atoi(val);
    } else if (strcasecmp(key, "init_array") == 0) {
        return parse_init_array(cfg, val);
    } else if (strcasecmp(key, "delay_min_ms") == 0) {
        cfg->delay_min_ms = atoi(val);
    } else if (strcasecmp(key, "delay_max_ms") == 0) {
        cfg->delay_max_ms = atoi(val);
    } else if (strcasecmp(key, "work_min_ms") == 0) {
        cfg->work_min_ms = atoi(val);
    } else if (strcasecmp(key, "work_max_ms") == 0) {
        cfg->work_max_ms = atoi(val);
    } else if (strcasecmp(key, "policy") == 0) {
        if (strcasecmp(val, "readers") == 0) {
            cfg->policy = POLICY_READERS;
        } else if (strcasecmp(val, "writers") == 0) {
            cfg->policy = POLICY_WRITERS;
        } else {
            cfg->policy = POLICY_FAIR;
        }
    } else if (strcasecmp(key, "seed") == 0) {
        cfg->seed = (unsigned int)strtoul(val, NULL, 0);
    } else if (strcasecmp(key, "log") == 0 || strcasecmp(key, "log_file") == 0) {
        snprintf(cfg->log_file, sizeof(cfg->log_file), "%s", val);
    }
    return 0;
}

// Чтение файла конфигурации строго через низкоуровневые файловые дескрипторы (open/read/close)
int config_load_file(sim_config_t *cfg, const char *filepath) {
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    char buf[16384];
    size_t total_read = 0;
    while (total_read < sizeof(buf) - 1) {
        ssize_t n = read(fd, buf + total_read, sizeof(buf) - 1 - total_read);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (n == 0) break;
        total_read += (size_t)n;
    }
    close(fd);
    buf[total_read] = '\0';

    char *saveptr = NULL;
    char *line = strtok_r(buf, "\r\n", &saveptr);
    while (line != NULL) {
        char *p = trim_whitespace(line);
        if (*p != '#' && *p != ';' && *p != '\0') {
            char *eq = strchr(p, '=');
            if (eq) {
                *eq = '\0';
                char *key = trim_whitespace(p);
                char *val = trim_whitespace(eq + 1);
                if (apply_key_value(cfg, key, val) != 0) {
                    return -1;
                }
            }
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }
    return 0;
}

void config_print_help(const char *prog_name) {
    dprintf(STDOUT_FILENO,
        "Использование: %s [опции]\n\n"
        "Параметры командной строки:\n"
        "  -r, --readers <N>       Количество читателей (по умолчанию: 3)\n"
        "  -w, --writers <N>       Количество писателей (по умолчанию: 2)\n"
        "  -s, --size <N>          Размер массива базы данных (по умолчанию: 10, макс. %d)\n"
        "  -o, --ops <N>           Количество операций на участника (0 = бесконечно, по умолчанию: 5)\n"
        "  -p, --policy <NAME>     Политика доступа: readers, writers, fair (по умолчанию: fair)\n"
        "      --idx-min <N>       Минимальный выбираемый индекс массива (по умолчанию: 0)\n"
        "      --idx-max <N>       Максимальный выбираемый индекс массива (по умолчанию: size - 1)\n"
        "      --min-val <N>       Минимальное значение элемента >= 1 (по умолчанию: 1)\n"
        "      --max-val <N>       Максимальное значение элемента (по умолчанию: 40)\n"
        "      --init-array <LIST> Начальное содержимое массива через запятую (напр. \"5,1,9,3\")\n"
        "      --delay-min <ms>    Мин. пауза между операциями (по умолчанию: 100 мс)\n"
        "      --delay-max <ms>    Макс. пауза между операциями (по умолчанию: 300 мс)\n"
        "      --work-min <ms>     Мин. длительность работы (по умолчанию: 50 мс)\n"
        "      --work-max <ms>     Макс. длительность работы (по умолчанию: 150 мс)\n"
        "      --seed <N>          Начальное значение ГСЧ (seed)\n"
        "  -c, --config <file>     Путь к файлу конфигурации\n"
        "  -l, --log <file>        Путь к файлу для сохранения протокола и статистики\n"
        "  -h, --help              Вывод этой справочной информации\n",
        prog_name, MAX_ARRAY_SIZE);
}

int config_parse_args(sim_config_t *cfg, int argc, char **argv) {
    static struct option long_options[] = {
        {"readers",    required_argument, 0, 'r'},
        {"writers",    required_argument, 0, 'w'},
        {"size",       required_argument, 0, 's'},
        {"ops",        required_argument, 0, 'o'},
        {"policy",     required_argument, 0, 'p'},
        {"min-val",    required_argument, 0, 1001},
        {"max-val",    required_argument, 0, 1002},
        {"delay-min",  required_argument, 0, 1003},
        {"delay-max",  required_argument, 0, 1004},
        {"work-min",   required_argument, 0, 1005},
        {"work-max",   required_argument, 0, 1006},
        {"seed",       required_argument, 0, 1007},
        {"idx-min",    required_argument, 0, 1008},
        {"idx-max",    required_argument, 0, 1009},
        {"init-array", required_argument, 0, 1010},
        {"config",     required_argument, 0, 'c'},
        {"log",        required_argument, 0, 'l'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    // Сначала ищем аргумент -c/--config, чтобы загрузить файл конфигурации до применения флагов
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            snprintf(cfg->config_file, sizeof(cfg->config_file), "%s", argv[i + 1]);
            if (config_load_file(cfg, cfg->config_file) != 0) {
                dprintf(STDERR_FILENO, "Ошибка: не удалось прочитать файл конфигурации '%s'\n", cfg->config_file);
                return -1;
            }
            break;
        }
    }

    optind = 1;
    int opt;
    while ((opt = getopt_long(argc, argv, "r:w:s:o:p:c:l:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'r':
                cfg->num_readers = atoi(optarg);
                break;
            case 'w':
                cfg->num_writers = atoi(optarg);
                break;
            case 's':
                cfg->array_size = atoi(optarg);
                break;
            case 'o':
                cfg->max_ops = atoi(optarg);
                break;
            case 'p':
                if (strcasecmp(optarg, "readers") == 0) {
                    cfg->policy = POLICY_READERS;
                } else if (strcasecmp(optarg, "writers") == 0) {
                    cfg->policy = POLICY_WRITERS;
                } else if (strcasecmp(optarg, "fair") == 0) {
                    cfg->policy = POLICY_FAIR;
                } else {
                    dprintf(STDERR_FILENO, "Неизвестная политика: %s (допустимы: readers, writers, fair)\n", optarg);
                    return -1;
                }
                break;
            case 1001:
                cfg->min_val = atoi(optarg);
                break;
            case 1002:
                cfg->max_val = atoi(optarg);
                break;
            case 1003:
                cfg->delay_min_ms = atoi(optarg);
                break;
            case 1004:
                cfg->delay_max_ms = atoi(optarg);
                break;
            case 1005:
                cfg->work_min_ms = atoi(optarg);
                break;
            case 1006:
                cfg->work_max_ms = atoi(optarg);
                break;
            case 1007:
                cfg->seed = (unsigned int)strtoul(optarg, NULL, 0);
                break;
            case 1008:
                cfg->idx_min = atoi(optarg);
                break;
            case 1009:
                cfg->idx_max = atoi(optarg);
                break;
            case 1010:
                if (parse_init_array(cfg, optarg) != 0) {
                    return -1;
                }
                break;
            case 'c':
                // уже обработан выше
                break;
            case 'l':
                snprintf(cfg->log_file, sizeof(cfg->log_file), "%s", optarg);
                break;
            case 'h':
                config_print_help(argv[0]);
                exit(EXIT_SUCCESS);
            default:
                config_print_help(argv[0]);
                return -1;
        }
    }

    if (cfg->has_init_array > 0) {
        cfg->array_size = cfg->has_init_array;
    }

    // Проверка корректности диапазонов
    if (cfg->num_readers < 1 || cfg->num_readers > MAX_WORKERS) {
        dprintf(STDERR_FILENO, "Ошибка: неверное количество читателей (1..%d)\n", MAX_WORKERS);
        return -1;
    }
    if (cfg->num_writers < 1 || cfg->num_writers > MAX_WORKERS) {
        dprintf(STDERR_FILENO, "Ошибка: неверное количество писателей (1..%d)\n", MAX_WORKERS);
        return -1;
    }
    if (cfg->array_size < 1 || cfg->array_size > MAX_ARRAY_SIZE) {
        dprintf(STDERR_FILENO, "Ошибка: неверный размер массива (1..%d)\n", MAX_ARRAY_SIZE);
        return -1;
    }
    if (cfg->idx_max < 0) {
        cfg->idx_max = cfg->array_size - 1;
    }
    if (cfg->idx_min < 0 || cfg->idx_max >= cfg->array_size || cfg->idx_min > cfg->idx_max) {
        dprintf(STDERR_FILENO, "Ошибка: неверный диапазон индексов [%d, %d] для массива размера %d\n",
                cfg->idx_min, cfg->idx_max, cfg->array_size);
        return -1;
    }
    if (cfg->min_val < 1 || cfg->min_val > cfg->max_val) {
        dprintf(STDERR_FILENO, "Ошибка: min_val должен быть >= 1 и <= max_val\n");
        return -1;
    }
    if (cfg->delay_min_ms > cfg->delay_max_ms) {
        cfg->delay_max_ms = cfg->delay_min_ms;
    }
    if (cfg->work_min_ms > cfg->work_max_ms) {
        cfg->work_max_ms = cfg->work_min_ms;
    }

    return 0;
}

void config_print_summary(const sim_config_t *cfg, int out_fd) {
    const char *policy_str = (cfg->policy == POLICY_READERS) ? "readers (readers preference)" :
                             (cfg->policy == POLICY_WRITERS) ? "writers (writers preference)" :
                                                               "fair (FIFO turnstile)";
    dprintf(out_fd, "================== ПАРАМЕТРЫ МОДЕЛИРОВАНИЯ ==================\n");
    dprintf(out_fd, "  Читателей:            %d\n", cfg->num_readers);
    dprintf(out_fd, "  Писателей:            %d\n", cfg->num_writers);
    dprintf(out_fd, "  Размер массива:       %d%s\n", cfg->array_size,
            cfg->has_init_array ? " (задан вручную)" : "");
    dprintf(out_fd, "  Диапазон индексов:    [%d, %d]\n", cfg->idx_min, cfg->idx_max);
    dprintf(out_fd, "  Операций на поток:    %d%s\n", cfg->max_ops, cfg->max_ops == 0 ? " (до прерывания Ctrl+C)" : "");
    dprintf(out_fd, "  Диапазон чисел:       [%d, %d]\n", cfg->min_val, cfg->max_val);
    dprintf(out_fd, "  Политика доступа:     %s\n", policy_str);
    dprintf(out_fd, "  Задержка между опер.: [%d, %d] мс\n", cfg->delay_min_ms, cfg->delay_max_ms);
    dprintf(out_fd, "  Длительность работы:  [%d, %d] мс\n", cfg->work_min_ms, cfg->work_max_ms);
    dprintf(out_fd, "  Начальный seed:       %u\n", cfg->seed);
    if (cfg->log_file[0]) {
        dprintf(out_fd, "  Лог-файл:             %s\n", cfg->log_file);
    }
    if (cfg->config_file[0]) {
        dprintf(out_fd, "  Конфигурационный файл:%s\n", cfg->config_file);
    }
    dprintf(out_fd, "=============================================================\n");
}
