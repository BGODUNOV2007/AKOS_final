#include "common.h"
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef ROLE
#error "Укажите роль R или W при сборке"
#endif

// Поток случайных чисел определяется (seed, роль, ID, порядковый номер).
// Повтор незавершённой операции после переподключения повторяет выбор.
static uint64_t draw(uint64_t *state) {
    uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static int choose(uint64_t *state, int lo, int hi) {
    return lo + (int)(draw(state) % (unsigned)(hi - lo + 1));
}

static void receive(int fd, char *line) {
    int rc = read_line(fd,line,MAX_FRAME);
    if (rc <= 0) { dprintf(2,"Соединение с сервером потеряно\n"); exit(1); }
    dprintf(1,"%s\n",line);
    if (!strncmp(line,"STOP ",5) || !strncmp(line,"BYE ",4)) exit(0);
    if (!strncmp(line,"ERROR ",6)) exit(1);
}

// Даже во время модельной задержки сокет доступен для сообщения STOP.
static void pause_client(int fd, int ms) {
    struct pollfd p = {fd,POLLIN,0};
    int64_t until = millis() + ms;
    for (;;) {
        int64_t remaining = until - millis();
        int rc = poll(&p,1,remaining > 0 ? (int)remaining : 0);
        if (rc == 0) return;
        if (rc < 0 && errno == EINTR) continue;
        if (rc > 0) {
            char line[MAX_FRAME];
            receive(fd,line);
        }
        dprintf(2,"Ошибка ожидания или неожиданный ответ\n"); exit(1);
    }
}

int main(int argc, char **argv) {
    int id;
    if (argc != 4 || !number(argv[3],1,MAX_ROLE,&id)) {
        dprintf(2,"Запуск: %s IPv4 порт ID\n",argv[0]); return 2;
    }
    int fd = tcp_open(argv[1],argv[2],false);
    if (fd < 0) { perror("connect"); return 1; }
    if (send_line(fd,"HELLO %c %d",ROLE,id) < 0) return 1;
    char line[MAX_FRAME];
    receive(fd,line);
    uint64_t done;
    int quota, il, ih, vl, vh, gl, gh, hl, hh, seed;
    if (sscanf(line,"READY %" SCNu64 " %d %d %d %d %d %d %d %d %d %d",
         &done,&quota,&il,&ih,&vl,&vh,&gl,&gh,&hl,&hh,&seed) != 11 ||
        il < 0 || ih >= MAX_DB || il > ih || vl < 1 || vh > MAX_VALUE || vl > vh ||
        gl < 0 || gh > 60000 || gl > gh || hl < 0 || hh > 60000 || hl > hh) return 1;
    for (;;) {
        uint64_t state = ((uint64_t)(unsigned)seed << 32) ^ ((uint64_t)id << 1) ^
                         (uint64_t)(ROLE == 'W') ^ (done * UINT64_C(0xd1342543de82ef95));
        int index = choose(&state,il,ih);
        int value = choose(&state,vl,vh);
        int hold = choose(&state,hl,hh);
        pause_client(fd,choose(&state,gl,gh));
        dprintf(1,"REQUEST %c%d op=%" PRIu64 " index=%d\n",ROLE,id,done+1,index);
        int rc = ROLE == 'R' ? send_line(fd,"REQ %d",index) : send_line(fd,"REQ %d %d",index,value);
        if (rc < 0) return 1;
        receive(fd,line);
        uint64_t ticket;
        int old;
        if (sscanf(line,"GRANT %" SCNu64 " %d",&ticket,&old) != 2 || old < 1 || old > MAX_VALUE) return 1;
        dprintf(1,"%s_BEGIN %c%d ticket=%" PRIu64 " index=%d old=%d\n",
            ROLE == 'R' ? "READ" : "WRITE",ROLE,id,ticket,index,old);
        pause_client(fd,hold);
        rc = ROLE == 'R' ? send_line(fd,"END %" PRIu64 " %" PRIu64,ticket,fibonacci(old))
                         : send_line(fd,"END %" PRIu64,ticket);
        if (rc < 0) return 1;
        receive(fd,line);
        if (strncmp(line,"RESULT ",7)) return 1;
        ++done;
        if (quota && done == (uint64_t)quota) {
            receive(fd,line); // После последнего результата сервер посылает BYE.
            return 1;
        }
    }
}
