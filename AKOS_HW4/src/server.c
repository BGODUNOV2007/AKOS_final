#include "config.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum stage { IDLE, QUEUED, ACTIVE };
struct peer {
    int fd, id;
    char role;                    // R, W, M (наблюдатель), C (управление)
    enum stage stage;
    uint64_t ticket;
    int index, value, old;
    bool closing, failed;
    char input[256], output[32768];
    size_t in_len, out_len;
};
static struct peer peers[MAX_PEERS];
static struct config cfg;
// Завершённые операции принадлежат логическому ID и переживают обрыв сокета.
static uint64_t completed[2][MAX_ROLE], seq, tickets, cancelled;
static int active_r, active_w, peak_r, monitor_id, logfd = -1;
static int64_t started;
static volatile sig_atomic_t interrupted;
static const char *stop_reason;

static void on_signal(int sig) { (void)sig; interrupted = 1; }
static int compare(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static void array_text(char *s, size_t cap) {
    size_t off = (size_t)snprintf(s, cap, "[");
    for (int i = 0; i < cfg.size; ++i)
        off += (size_t)snprintf(s + off, cap - off, "%s%d", i ? "," : "", cfg.db[i]);
    snprintf(s + off, cap - off, "]");
}

static void put(struct peer *p, const char *fmt, ...) {
    if (p->failed) return;
    size_t room = sizeof(p->output) - p->out_len;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(p->output + p->out_len, room, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n + 1 > room) { p->failed = true; return; }
    p->out_len += (size_t)n;
    p->output[p->out_len++] = '\n';
}

static void event(const char *type, const struct peer *p, const char *fmt, ...) {
    char details[1024], line[MAX_FRAME];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(details, sizeof(details), fmt, ap);
    va_end(ap);
    snprintf(line, sizeof(line), "EVENT %" PRIu64 " %s %c%d ticket=%" PRIu64
             " r=%d w=%d t_ms=%" PRId64 " %s", ++seq, type, p ? p->role : 'S',
             p ? p->id : 0, p ? p->ticket : 0, active_r, active_w, millis()-started, details);
    if (logfd >= 0) dprintf(logfd, "%s\n", line);
    for (int i = 0; i < MAX_PEERS; ++i)
        if (peers[i].fd >= 0 && peers[i].role == 'M' && !peers[i].closing)
            put(&peers[i], "%s", line);
}

static int quota(char role, int id) {
    return (role == 'R' ? cfg.reader_ops : cfg.writer_ops)[id - 1];
}
static uint64_t *progress(char role, int id) { return &completed[role == 'W'][id - 1]; }

static bool all_done(void) {
    for (int kind = 0; kind < 2; ++kind) {
        char role = kind ? 'W' : 'R';
        int count = kind ? cfg.writers : cfg.readers;
        for (int id = 1; id <= count; ++id) {
            int q = quota(role, id);
            if (!q || *progress(role,id) < (uint64_t)q) return false;
        }
    }
    return true;
}

static void release(struct peer *p) {
    if (p->stage == ACTIVE) {
        if (p->role == 'R') --active_r;
        else --active_w;
    }
    p->stage = IDLE;
}

static void cancel(struct peer *p, const char *reason) {
    if (p->stage != IDLE) {
        bool began = p->stage == ACTIVE;
        release(p);
        ++cancelled;
        event("CANCEL", p, "began=%d reason=%s", began, reason);
    }
}

static void disconnect(struct peer *p) {
    cancel(p, "disconnect");
    if (p->role && !stop_reason)
        event("OFFLINE", p, "connection=closed");
    close(p->fd);
    p->fd = -1;
}

static void reject(struct peer *p, const char *why) {
    cancel(p, "protocol");
    put(p, "ERROR %s", why);
    p->closing = true;
}

// На равном приоритете выбирается самый ранний билет. Первая ожидающая
// запись в FIFO останавливает выдачу новых чтений, пока группа не выйдет.
static struct peer *next_waiter(void) {
    struct peer *best = NULL;
    char favored = cfg.policy == READERS ? 'R' : cfg.policy == WRITERS ? 'W' : 0;
    for (int i = 0; i < MAX_PEERS; ++i) {
        struct peer *p = &peers[i];
        if (p->fd < 0 || p->stage != QUEUED || p->failed || p->closing) continue;
        if (!best || (p->role == favored && best->role != favored) ||
            ((p->role == favored) == (best->role == favored) && p->ticket < best->ticket))
            best = p;
    }
    return best;
}

static void schedule(void) {
    while (!active_w && !stop_reason) {
        struct peer *p = next_waiter();
        if (!p || (p->role == 'W' && active_r)) return;
        p->stage = ACTIVE;
        p->old = cfg.db[p->index];
        if (p->role == 'R') {
            if (++active_r > peak_r) peak_r = active_r;
        } else ++active_w;
        event(p->role == 'R' ? "READ_BEGIN" : "WRITE_BEGIN", p,
              "index=%d old=%d", p->index, p->old);
        put(p, "GRANT %" PRIu64 " %d", p->ticket, p->old);
    }
}

static void finish(struct peer *p, char **args, int n) {
    bool reader = p->role == 'R';
    char ticket[32], fib[32];
    snprintf(ticket, sizeof(ticket), "%" PRIu64, p->ticket);
    snprintf(fib, sizeof(fib), "%" PRIu64, fibonacci(p->old));
    if (p->stage != ACTIVE || n != (reader ? 3 : 2) || strcmp(args[1],ticket) ||
        (reader && strcmp(args[2],fib))) { reject(p, "invalid-finish"); return; }
    char db[512], result[768];
    if (reader) {
        snprintf(result, sizeof(result), "index=%d value=%d fib=%s", p->index, p->old, fib);
    } else {
        // До END новое значение хранится только в заявке. Разрыв не меняет БД.
        cfg.db[p->index] = p->value;
        qsort(cfg.db, (size_t)cfg.size, sizeof(int), compare);
        array_text(db, sizeof(db));
        snprintf(result,sizeof(result),"index=%d old=%d new=%d db=%s",p->index,p->old,p->value,db);
    }
    uint64_t done = ++*progress(p->role, p->id);
    release(p);
    event(reader ? "READ_END" : "WRITE_END", p, "op=%" PRIu64 " %s", done, result);
    put(p, "RESULT %c%d op=%" PRIu64 " %s", p->role, p->id, done, result);
    if (done == (uint64_t)quota(p->role,p->id)) {
        p->closing = true;
        put(p, "BYE quota");
    }
    if (all_done()) stop_reason = "complete";
}

static void snapshot(struct peer *to) {
    char db[512];
    array_text(db, sizeof(db));
    put(to,"SNAP seq=%" PRIu64 " r=%d w=%d db=%s", seq,active_r,active_w,db);
    for (int i = 0; i < MAX_PEERS; ++i) {
        struct peer *p = &peers[i];
        if (p->fd < 0 || !p->role || p->closing) continue;
        const char *stage[] = {"idle", "queued", "active"};
        put(to, "PEER %c%d %s ticket=%" PRIu64 " index=%d",p->role,p->id,stage[p->stage],
            p->ticket,p->index);
    }
    for (int k = 0; k < 2; ++k) {
        int count = k ? cfg.writers : cfg.readers;
        char role = k ? 'W' : 'R';
        for (int id = 1; id <= count; ++id)
            put(to,"PROGRESS %c%d done=%" PRIu64 " quota=%d",role,id,*progress(role,id),quota(role,id));
    }
    put(to, "SNAP_END");
}

static struct peer *find_peer(char role, int id) {
    for (int i = 0; i < MAX_PEERS; ++i)
        if (peers[i].fd >= 0 && peers[i].role == role && peers[i].id == id && !peers[i].closing)
            return &peers[i];
    return NULL;
}

static void hello(struct peer *p, char **a, int n) {
    if (n == 1 && !strcmp(a[0],"WATCH")) {
        p->role = 'M'; p->id = ++monitor_id;
        put(p,"WATCHING M%d",p->id);
        snapshot(p);
    } else if (n == 1 && !strcmp(a[0],"CONTROL")) {
        p->role = 'C';
        put(p,"CONTROL_OK");
    } else if (n == 3 && !strcmp(a[0],"HELLO") && (!strcmp(a[1],"R") || !strcmp(a[1],"W"))) {
        char role = a[1][0];
        int id, count = role == 'R' ? cfg.readers : cfg.writers;
        if (!number(a[2],1,count,&id)) { reject(p,"unknown-id"); return; }
        if (find_peer(role,id)) { reject(p,"id-in-use"); return; }
        uint64_t done = *progress(role,id);
        int limit = quota(role,id);
        if (limit && done == (uint64_t)limit) { put(p,"BYE quota"); p->closing=true; return; }
        p->role = role; p->id = id;
        struct range hold = role == 'R' ? cfg.read : cfg.write;
        put(p,"READY %" PRIu64 " %d %d %d %d %d %d %d %d %d %d",done,limit,
            cfg.index.lo,cfg.index.hi,cfg.value.lo,cfg.value.hi,cfg.gap.lo,cfg.gap.hi,
            hold.lo,hold.hi,cfg.seed);
    } else { reject(p,"unknown-hello"); return; }
    event("ONLINE",p,"connection=open");
}

static void control(struct peer *p, char **a, int n) {
    if (n == 1 && !strcmp(a[0],"LIST")) snapshot(p);
    else if (n == 1 && !strcmp(a[0],"STOP")) stop_reason = "control";
    else if (n == 3 && !strcmp(a[0],"DROP")) {
        int id;
        if (strlen(a[1]) != 1 || !strchr("RWM",a[1][0]) || !number(a[2],1,2147483647,&id)) {
            reject(p,"invalid-target"); return;
        }
        struct peer *target = find_peer(a[1][0],id);
        if (!target) { reject(p,"target-offline"); return; }
        cancel(target,"drop");
        put(target,"STOP dropped");
        target->closing = true;
    } else { reject(p,"unknown-control"); return; }
    put(p,"OK");
    p->closing = true;
}

static void command(struct peer *p, char *line) {
    char *a[5];
    int n = split(line,a,5);
    if (n < 1) { reject(p,"bad-frame"); return; }
    if (!p->role) { hello(p,a,n); return; }
    if (p->role == 'C') { control(p,a,n); return; }
    if (p->role == 'M') { reject(p,"observer-read-only"); return; }
    if (!strcmp(a[0],"END")) { finish(p,a,n); return; }
    if (strcmp(a[0],"REQ") || p->stage != IDLE || n != (p->role == 'R' ? 2 : 3) ||
        !number(a[1],cfg.index.lo,cfg.index.hi,&p->index) ||
        (p->role == 'W' && !number(a[2],cfg.value.lo,cfg.value.hi,&p->value))) {
        reject(p,"invalid-request"); return;
    }
    p->ticket = ++tickets;
    p->stage = QUEUED;
    event("REQUEST",p,"index=%d",p->index);
}

static void receive_data(struct peer *p) {
    char block[1024];
    ssize_t n = recv(p->fd, block, sizeof(block), 0);
    if (n <= 0) {
        if (!n || (errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK)) p->failed = true;
        return;
    }
    for (ssize_t j = 0; j < n && !p->closing && !stop_reason; ++j) {
        char c = block[j];
        if (c == '\n') {
            p->input[p->in_len] = 0;
            command(p,p->input);
            p->in_len = 0;
        } else if (!c || p->in_len == sizeof(p->input)-1) {
            reject(p,"bad-frame");
        } else p->input[p->in_len++] = c;
    }
}

static void flush(struct peer *p) {
    ssize_t n = send(p->fd,p->output,p->out_len,MSG_NOSIGNAL);
    if (n > 0) {
        p->out_len -= (size_t)n;
        memmove(p->output,p->output+n,p->out_len);
    } else if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) p->failed = true;
}

static bool nonblocking(int fd) {
    int flags = fcntl(fd,F_GETFL);
    return flags >= 0 && fcntl(fd,F_SETFL,flags | O_NONBLOCK) == 0;
}
static void accept_peer(int listener) {
    int fd = accept(listener,NULL,NULL);
    if (fd < 0) return;
    if (nonblocking(fd)) {
        for (int i = 0; i < MAX_PEERS; ++i) if (peers[i].fd < 0) {
            peers[i] = (struct peer){.fd = fd};
            return;
        }
        (void)send(fd,"ERROR capacity\n",15,MSG_NOSIGNAL);
    }
    close(fd);
}

static void shutdown_peers(void) {
    char db[512], summary[768];
    array_text(db,sizeof(db));
    uint64_t total[2] = {0};
    for (int i = 0; i < MAX_PEERS; ++i)
        if (peers[i].fd >= 0) cancel(&peers[i],stop_reason);
    for (int k = 0; k < 2; ++k)
        for (int i = 0; i < MAX_ROLE; ++i) total[k] += completed[k][i];
    snprintf(summary,sizeof(summary),"reason=%s reads=%" PRIu64 " writes=%" PRIu64
             " cancelled=%" PRIu64 " peak_readers=%d db=%s",stop_reason,total[0],total[1],cancelled,peak_r,db);
    event("SUMMARY",NULL,"%s",summary);
    dprintf(STDOUT_FILENO,"STOP %s\n",summary);
    for (int i = 0; i < MAX_PEERS; ++i) if (peers[i].fd >= 0 && !peers[i].closing) {
        put(&peers[i],"STOP %s",stop_reason);
        peers[i].closing = true;
    }
}

int main(int argc, char **argv) {
    if (argc != 4 && argc != 5) {
        dprintf(2,"Запуск: %s IPv4 порт конфигурация [журнал]\n",argv[0]); return 2;
    }
    if (!load_config(argv[3],&cfg)) return 2;
    qsort(cfg.db,(size_t)cfg.size,sizeof(int),compare);
    int listener = tcp_open(argv[1],argv[2],true);
    if (listener < 0 || !nonblocking(listener)) { perror("listen"); return 1; }
    if (argc == 5 && (logfd = open(argv[4],O_WRONLY|O_CREAT|O_TRUNC,0644)) < 0) { perror(argv[4]); return 1; }
    struct sigaction act = {.sa_handler = on_signal};
    sigemptyset(&act.sa_mask);
    sigaction(SIGINT,&act,NULL); sigaction(SIGTERM,&act,NULL);
    for (int i = 0; i < MAX_PEERS; ++i) peers[i].fd = -1;
    started = millis();
    char initial[512];
    array_text(initial,sizeof(initial));
    const char *policies[] = {"fair","readers","writers"};
    event("INIT",NULL,"db=%s policy=%s seed=%d readers=%d writers=%d",
          initial,policies[cfg.policy],cfg.seed,cfg.readers,cfg.writers);
    dprintf(1,"LISTEN %s %s readers=%d writers=%d\n",argv[1],argv[2],cfg.readers,cfg.writers);
    int64_t deadline = 0;
    for (;;) {
        if (interrupted && !stop_reason) stop_reason = "signal";
        if (stop_reason && !deadline) {
            close(listener); listener = -1;
            shutdown_peers();
            deadline = millis()+1500;
        }
        struct pollfd fds[MAX_PEERS+1];
        fds[0] = (struct pollfd){listener,POLLIN,0};
        int live = 0;
        for (int i = 0; i < MAX_PEERS; ++i) {
            struct peer *p = &peers[i];
            if (p->fd >= 0 && (p->failed || (p->closing && !p->out_len))) disconnect(p);
            short mask = p->closing ? 0 : POLLIN;
            if (p->out_len) mask |= POLLOUT;
            fds[i+1] = (struct pollfd){p->fd,mask,0};
            live += p->fd >= 0;
        }
        if (deadline && (!live || millis() >= deadline)) break;
        int ready = poll(fds,MAX_PEERS+1,100);
        if (ready < 0 && errno != EINTR) { perror("poll"); stop_reason = "io-error"; break; }
        if (ready < 0) continue;
        for (int i = 0; i < MAX_PEERS; ++i) {
            struct peer *p = &peers[i];
            short ev = fds[i+1].revents;
            if ((ev & POLLIN) && !p->closing && !stop_reason) receive_data(p);
            if ((ev & POLLOUT) && p->out_len) flush(p);
            if (ev & (POLLHUP|POLLERR|POLLNVAL)) p->failed = true;
            if (p->fd >= 0 && p->failed) disconnect(p);
        }
        if (!stop_reason && (fds[0].revents & POLLIN)) accept_peer(listener);
        schedule();
    }
    for (int i = 0; i < MAX_PEERS; ++i) if (peers[i].fd >= 0) close(peers[i].fd);
    if (logfd >= 0) close(logfd);
    return stop_reason && !strcmp(stop_reason,"io-error");
}
