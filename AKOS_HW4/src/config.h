#ifndef RW_CONFIG_H
#define RW_CONFIG_H
#include "common.h"

enum policy { FAIR, READERS, WRITERS };
struct range { int lo, hi; };
struct config {
    int db[MAX_DB], size, readers, writers, seed;
    int reader_ops[MAX_ROLE], writer_ops[MAX_ROLE];
    struct range index, value, gap, read, write;
    enum policy policy;
};
bool load_config(const char *path, struct config *c);
#endif
