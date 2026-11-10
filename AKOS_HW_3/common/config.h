#ifndef CONFIG_H
#define CONFIG_H

#include "types.h"

// Инициализация конфигурации значениями по умолчанию
void config_set_defaults(sim_config_t *cfg);

// Загрузка конфигурации из файла
int config_load_file(sim_config_t *cfg, const char *filepath);

// Разбор аргументов командной строки
int config_parse_args(sim_config_t *cfg, int argc, char **argv);

// Печать справки по использованию программы
void config_print_help(const char *prog_name);

// Вывод итоговой конфигурации в дескриптор файла
void config_print_summary(const sim_config_t *cfg, int out_fd);

#endif // CONFIG_H
