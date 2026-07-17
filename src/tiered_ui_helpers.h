#ifndef TIERED_UI_HELPERS_H
#define TIERED_UI_HELPERS_H

#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

typedef struct {
    char disk[32];
    char model[128];
    char tran[16];
    long long size_gb;
    double speed_write;
    double speed_read;
    int is_root;
    int is_mounted;
    int selected;
    int carve_gb;
} ui_disk_t;

static inline void parse_bench_output(const char *out, ui_disk_t *d) {
    d->speed_write = 0;
    d->speed_read = 0;
    char needle[64];
    snprintf(needle, sizeof(needle), "  /dev/%s:", d->disk);
    char *line = strstr(out, needle);
    if (!line) return;
    char *eol = strchr(line, '\n');
    int line_len = eol ? (int)(eol - line) : (int)strlen(line);
    char *w = strstr(line, "Write");
    if (w && (int)(w - line) < line_len) {
        w += 5;
        while (*w == ' ' || *w == ':') w++;
        d->speed_write = atof(w);
    }
    char *r = strstr(line, "Read");
    if (r && (int)(r - line) < line_len) {
        r += 4;
        while (*r == ' ' || *r == ':') r++;
        d->speed_read = atof(r);
    }
}

static inline int bench_disk_done(const char *disk, const char *bench_buf) {
    char needle[64];
    snprintf(needle, sizeof(needle), "  /dev/%s: Write", disk);
    return strstr(bench_buf, needle) != NULL;
}

#pragma GCC diagnostic pop

#endif
