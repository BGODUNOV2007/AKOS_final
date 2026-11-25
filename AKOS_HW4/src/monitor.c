#include "common.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 3) { dprintf(2,"Запуск: %s IPv4 порт\n",argv[0]); return 2; }
    int fd = tcp_open(argv[1],argv[2],false);
    if (fd < 0) { perror("connect"); return 1; }
    if (send_line(fd,"WATCH") < 0) return 1;
    char line[MAX_FRAME];
    while (read_line(fd,line,sizeof(line)) == 1) {
        dprintf(1,"%s\n",line);
        if (!strncmp(line,"STOP ",5)) { close(fd); return 0; }
        if (!strncmp(line,"ERROR ",6)) return 1;
    }
    dprintf(2,"Связь с сервером потеряна\n");
    return 1;
}
