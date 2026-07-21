#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tiered_types.h"

int tv_metadata_save(TV_METADATA *meta, const char *path) {
    if (!meta || !path) return TV_ERR;

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "metadata: cannot write to '%s'\n", path);
        return TV_ERR;
    }

    fprintf(f, "[weighted_striping]\n");
    fprintf(f, "version=%u\n", meta->version);
    fprintf(f, "chunk_size=%u\n", meta->chunk_size);
    fprintf(f, "segment_count=%u\n", meta->segment_count);
    fprintf(f, "disk_count=%u\n", meta->disk_count);

    for (uint32_t i = 0; i < meta->disk_count; i++) {
        fprintf(f, "disk%u_name=%s\n", i, meta->disk_names[i]);
    }

    for (uint32_t i = 0; i < meta->segment_count; i++) {
        TV_SEGMENT *seg = &meta->segments[i];
        fprintf(f, "seg%u_begin=%lu\n", i, (unsigned long)seg->logical_begin);
        fprintf(f, "seg%u_end=%lu\n", i, (unsigned long)seg->logical_end);
        fprintf(f, "seg%u_count=%u\n", i, seg->disk_count);

        fprintf(f, "seg%u_disks=", i);
        for (uint32_t j = 0; j < seg->disk_count; j++) {
            fprintf(f, "%s%u", j ? "," : "", seg->disk_index[j]);
        }
        fprintf(f, "\n");

        fprintf(f, "seg%u_weight=", i);
        for (uint32_t j = 0; j < seg->disk_count; j++) {
            fprintf(f, "%s%u", j ? "," : "", seg->weight[j]);
        }
        fprintf(f, "\n");

        fprintf(f, "seg%u_stripe=%lu\n", i, (unsigned long)seg->stripe_size);
    }

    fclose(f);
    return 0;
}

static int parse_line(char *line, char **key, char **val) {
    char *eq = strchr(line, '=');
    if (!eq) return TV_ERR;
    *eq = 0;
    *key = line;
    *val = eq + 1;
    /* Strip trailing newline */
    char *nl = strchr(*val, '\n');
    if (nl) *nl = 0;
    return 0;
}

int tv_metadata_load(TV_METADATA *meta, const char *path) {
    if (!meta || !path) return TV_ERR;

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "metadata: cannot read '%s'\n", path);
        return TV_ERR;
    }

    memset(meta, 0, sizeof(TV_METADATA));

    char line[1024];
    int current_seg = -1;

    while (fgets(line, sizeof(line), f)) {
        char *key, *val;
        if (parse_line(line, &key, &val) < 0) continue;

        if (strcmp(key, "version") == 0) {
            meta->version = (uint32_t)strtoul(val, NULL, 10);
        } else if (strcmp(key, "chunk_size") == 0) {
            meta->chunk_size = (uint32_t)strtoul(val, NULL, 10);
        } else if (strcmp(key, "segment_count") == 0) {
            meta->segment_count = (uint32_t)strtoul(val, NULL, 10);
            if (meta->segment_count > TV_MAX_SEGS) {
                fprintf(stderr, "metadata: segment_count %u exceeds max %d\n",
                        meta->segment_count, TV_MAX_SEGS);
                fclose(f); return TV_ERR;
            }
        } else if (strcmp(key, "disk_count") == 0) {
            meta->disk_count = (uint32_t)strtoul(val, NULL, 10);
            if (meta->disk_count > TV_MAX_DISKS) {
                fprintf(stderr, "metadata: disk_count %u exceeds max %d\n",
                        meta->disk_count, TV_MAX_DISKS);
                fclose(f); return TV_ERR;
            }
        } else if (strncmp(key, "disk", 4) == 0 && strstr(key, "_name")) {
            /* disk0_name=... */
            char *endp;
            unsigned long idx = strtoul(key + 4, &endp, 10);
            if (endp && strcmp(endp, "_name") == 0 && idx < TV_MAX_DISKS) {
                strncpy(meta->disk_names[idx], val, 63);
                meta->disk_names[idx][63] = 0;
            }
        } else if (strncmp(key, "seg", 3) == 0) {
            char *endp;
            unsigned long idx = strtoul(key + 3, &endp, 10);
            if (idx >= TV_MAX_SEGS) continue;

            if (current_seg < 0 || (int)idx != current_seg) {
                current_seg = (int)idx;
            }

            TV_SEGMENT *seg = &meta->segments[idx];

            if (strcmp(endp, "_begin") == 0) {
                seg->logical_begin = strtoull(val, NULL, 10);
            } else if (strcmp(endp, "_end") == 0) {
                seg->logical_end = strtoull(val, NULL, 10);
            } else if (strcmp(endp, "_count") == 0) {
                seg->disk_count = (uint32_t)strtoul(val, NULL, 10);
            } else if (strcmp(endp, "_stripe") == 0) {
                seg->stripe_size = strtoull(val, NULL, 10);
            } else if (strcmp(endp, "_disks") == 0) {
                char *t = strtok(val, ",");
                int j = 0;
                while (t && j < TV_MAX_DISKS) {
                    uint32_t d = (uint32_t)strtoul(t, NULL, 10);
                    if (d >= meta->disk_count) {
                        fprintf(stderr, "metadata: seg%lu disk index %u >= disk_count %u\n",
                                idx, d, meta->disk_count);
                        fclose(f);
                        return TV_ERR;
                    }
                    seg->disk_index[j++] = d;
                    t = strtok(NULL, ",");
                }
            } else if (strcmp(endp, "_weight") == 0) {
                char *t = strtok(val, ",");
                int j = 0;
                while (t && j < TV_MAX_DISKS) {
                    seg->weight[j++] = (uint32_t)strtoul(t, NULL, 10);
                    t = strtok(NULL, ",");
                }
            }
        }
    }

    fclose(f);
    return 0;
}
