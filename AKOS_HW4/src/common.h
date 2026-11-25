#ifndef RW_COMMON_H
#define RW_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_FRAME 2048
#define MAX_DB 128
#define MAX_ROLE 32
#define MAX_PEERS 128
#define MAX_VALUE 93  /* F(93) помещается в uint64_t. */
#define MAX_OPS 1000000

bool number(const char *s, int lo, int hi, int *out);
int split(char *s, char **words, int cap);
uint64_t fibonacci(int n);
int64_t millis(void);
int tcp_open(const char *ip, const char *port, bool listener);
int send_line(int fd, const char *fmt, ...);
int read_line(int fd, char *buf, size_t cap);
#endif
