#include "common.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int id = 0;
    bool drop = argc == 6 && !strcmp(argv[3],"drop") && strlen(argv[4]) == 1 &&
                strchr("RWM",argv[4][0]) && number(argv[5],1,2147483647,&id);
    bool simple = argc == 4 && (!strcmp(argv[3],"list") || !strcmp(argv[3],"stop"));
    if (!drop && !simple) {
        dprintf(2,"Запуск: %s IPv4 порт list|stop|drop {R|W|M} ID\n",argv[0]); return 2;
    }
    int fd = tcp_open(argv[1],argv[2],false);
    if (fd < 0) { perror("connect"); return 1; }
    char line[MAX_FRAME];
    if (send_line(fd,"CONTROL") < 0 || read_line(fd,line,sizeof(line)) != 1 ||
        strcmp(line,"CONTROL_OK")) return 1;
    int rc = drop ? send_line(fd,"DROP %s %d",argv[4],id) :
                    send_line(fd,"%s",!strcmp(argv[3],"list") ? "LIST" : "STOP");
    if (rc < 0) return 1;
    while (read_line(fd,line,sizeof(line)) == 1) {
        dprintf(1,"%s\n",line);
        if (!strcmp(line,"OK")) { close(fd); return 0; }
    }
    return 1;
}
