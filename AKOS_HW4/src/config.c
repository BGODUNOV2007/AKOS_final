#include "config.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) ++s;
    size_t len = strlen(s);
    while (len && isspace((unsigned char)s[len - 1])) s[--len] = 0;
    return s;
}

static int list(char *s, int *a, int cap, int lo, int hi) {
    int n = 0;
    for (;;) {
        char *comma = strchr(s, ',');
        if (comma) *comma = 0;
        if (n == cap || !number(trim(s), lo, hi, &a[n])) return -1;
        ++n;
        if (!comma) return n;
        s = comma + 1;
    }
}

static bool interval(char *s, struct range *r, int lo, int hi) {
    char *dash = strchr(s, '-');
    if (!dash) return false;
    *dash++ = 0;
    return number(trim(s), lo, hi, &r->lo) && number(trim(dash), r->lo, hi, &r->hi);
}

static bool expand(int *ops, int supplied, int count) {
    if (supplied != 1) return count == supplied;
    for (int i = 1; i < count; ++i) ops[i] = ops[0];
    return true;
}

bool load_config(const char *path, struct config *c) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return false; }
    const char *keys[] = {"db","readers","writers","reader_ops","writer_ops","index",
                          "value","gap_ms","read_ms","write_ms","policy","seed"};
    char buf[1024];
    int seen = 0, line = 0, nr = 0, nw = 0;
    bool ok = true;
    while (ok && fgets(buf, sizeof(buf), f)) {
        ++line;
        char *comment = strchr(buf, '#');
        if (comment) *comment = 0;
        char *key = trim(buf);
        if (!*key) continue;
        char *eq = strchr(key, '=');
        if (!eq) { ok = false; break; }
        *eq++ = 0;
        key = trim(key);
        char *v = trim(eq);
        int k = 0;
        while (k < 12 && strcmp(key, keys[k])) ++k;
        if (k == 12 || (seen & (1 << k))) { ok = false; break; }
        seen |= 1 << k;
        switch (k) {
        case 0: c->size = list(v, c->db, MAX_DB, 1, MAX_VALUE); ok = c->size > 0; break;
        case 1: ok = number(v, 0, MAX_ROLE, &c->readers); break;
        case 2: ok = number(v, 0, MAX_ROLE, &c->writers); break;
        case 3: nr = list(v, c->reader_ops, MAX_ROLE, 0, MAX_OPS); ok = nr > 0; break;
        case 4: nw = list(v, c->writer_ops, MAX_ROLE, 0, MAX_OPS); ok = nw > 0; break;
        case 5: ok = interval(v, &c->index, 0, MAX_DB - 1); break;
        case 6: ok = interval(v, &c->value, 1, MAX_VALUE); break;
        case 7: ok = interval(v, &c->gap, 0, 60000); break;
        case 8: ok = interval(v, &c->read, 0, 60000); break;
        case 9: ok = interval(v, &c->write, 0, 60000); break;
        case 10:
            if (!strcmp(v,"fair")) c->policy = FAIR;
            else if (!strcmp(v,"readers")) c->policy = READERS;
            else if (!strcmp(v,"writers")) c->policy = WRITERS;
            else ok = false;
            break;
        case 11: ok = number(v, 0, 2147483647, &c->seed); break;
        }
    }
    ok = ok && !ferror(f) && seen == 4095 && c->index.hi < c->size &&
         c->readers + c->writers > 0 && expand(c->reader_ops, nr, c->readers) &&
         expand(c->writer_ops, nw, c->writers);
    fclose(f);
    if (!ok) fprintf(stderr, "%s (строка %d): проверьте ключи, диапазоны и число квот\n", path, line);
    return ok;
}
