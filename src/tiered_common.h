#ifndef TIERED_COMMON_H
#define TIERED_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline int tiered_is_valid_name(const char *name) {
    if (!name || !*name) return 0;
    for (const char *p = name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-'))
            return 0;
    }
    return 1;
}

static inline int tiered_is_valid_fs(const char *fs) {
    if (!fs || !*fs) return 0;
    const char *ok[] = {"ext4","ext3","xfs","btrfs","none",NULL};
    for (int i = 0; ok[i]; i++) if (strcmp(fs, ok[i]) == 0) return 1;
    return 0;
}

static inline int tiered_is_physical_disk(const char *name) {
    if (!name || !*name) return 0;
    if (strncmp(name, "loop", 4) == 0) return 0;
    if (strncmp(name, "ram", 3) == 0) return 0;
    if (strncmp(name, "zram", 4) == 0) return 0;
    if (strncmp(name, "fd", 2) == 0) return 0;
    if (strncmp(name, "sr", 2) == 0) return 0;
    return 1;
}

static inline int tiered_mounted_disks(const char *disks[], int ndisks) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;
    char line[512];
    int mounted = 0;
    while (fgets(line, sizeof(line), f)) {
        char dev[256];
        if (sscanf(line, "%255s", dev) != 1) continue;
        if (strncmp(dev, "/dev/", 5) != 0) continue;
        const char *base = dev + 5;
        /* Strip trailing partition number to get base disk name */
        char base_disk[256];
        strncpy(base_disk, base, sizeof(base_disk) - 1);
        base_disk[sizeof(base_disk) - 1] = '\0';
        int len = (int)strlen(base_disk);
        while (len > 0 && base_disk[len - 1] >= '0' && base_disk[len - 1] <= '9')
            base_disk[--len] = '\0';
        /* Handle nvme style: nvme0n1p1 -> strip p + digits -> nvme0n1 */
        if (len > 0 && base_disk[len - 1] == 'p') {
            base_disk[--len] = '\0';
            while (len > 0 && base_disk[len - 1] >= '0' && base_disk[len - 1] <= '9')
                base_disk[--len] = '\0';
        }
        for (int i = 0; i < ndisks; i++) {
            if (disks[i] && strcmp(base_disk, disks[i]) == 0) {
                fprintf(stderr, "Error: %s (%s) is currently mounted — cannot use\n", disks[i], dev);
                mounted = 1;
            }
            /* Also check exact partition match */
            if (disks[i] && strcmp(base, disks[i]) == 0) {
                fprintf(stderr, "Error: %s is currently mounted — cannot use\n", disks[i]);
                mounted = 1;
            }
        }
    }
    fclose(f);
    return mounted;
}

static inline int tiered_is_valid_mount(const char *mp) {
    if (!mp || mp[0] != '/' || strlen(mp) >= 4096) return 0;
    for (const char *p = mp; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '/' || *p == '.' ||
              *p == '_' || *p == '-'))
            return 0;
    }
    if (strstr(mp, "/../") || strcmp(mp, "/..") == 0 ||
        (strlen(mp) >= 3 && strncmp(mp + strlen(mp) - 3, "/..", 3) == 0))
        return 0;
    return 1;
}

#endif
