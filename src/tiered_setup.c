#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>

#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wstringop-truncation"

#define VERSION "1.1.0"
#define MAX_DISKS 8
#define MAX_NAME 64
#define MAX_PATH 512

static volatile sig_atomic_t bench_interrupted = 0;

static void bench_signal_handler(int sig) {
    (void)sig;
    bench_interrupted = 1;
}

static int is_valid_name(const char *name) {
    if (!name || !*name) return 0;
    for (const char *p = name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-'))
            return 0;
    }
    return 1;
}

static int run(const char *fmt, ...) {
    char cmd[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    return system(cmd);
}

static int run_sudo(const char *fmt, ...) {
    char cmd[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    char tmpf[] = "/tmp/.tv_cmd_XXXXXX";
    int tf = mkstemp(tmpf);
    if (tf < 0) return -1;
    dprintf(tf, "%s\n", cmd);
    close(tf);
    chmod(tmpf, 0700);
    char full[4300];
    snprintf(full, sizeof(full), "sudo bash %s 2>&1", tmpf);
    int ret = system(full);
    unlink(tmpf);
    return ret;
}

#define LVM_CFG " --config 'devices{scan=[\\\"/dev/mapper\\\"] obtain_device_list_from_udev=0 filter=[\\\"a|.*\\\"]}'"

typedef struct {
    char disk[32];
    char model[128];
    char tran[16];
    long long size_gb;
    double speed_write;
    double speed_read;
    long long carve_gb;
    int is_root;
} disk_t;

static long long sysfs_size_gb(const char *disk) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/block/%s/size", disk);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    long long sec = 0;
    if (fscanf(f, "%lld", &sec) != 1) sec = 0;
    fclose(f);
    return sec * 512 / (1024LL * 1024 * 1024);
}

static void sysfs_model(const char *disk, char *out, size_t len) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/block/%s/device/model", disk);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(out, len, f)) {
            out[strcspn(out, "\n")] = 0;
            char *p = out + strlen(out) - 1;
            while (p > out && *p == ' ') *p-- = 0;
        }
        fclose(f);
    } else {
        strncpy(out, disk, len - 1);
        out[len - 1] = 0;
    }
}

static void sysfs_tran(const char *disk, char *out, size_t len) {
    out[0] = 0;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "lsblk -d -o NAME,TRAN 2>/dev/null | awk '$1==\"%s\"{print $2}'", disk);
    FILE *p = popen(cmd, "r");
    if (p) {
        if (fgets(out, len, p)) out[strcspn(out, "\n")] = 0;
        pclose(p);
    }
}

static int is_root_disk(const char *disk) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "lsblk -n -o MOUNTPOINT /dev/%s 2>/dev/null | grep -qE '^/$|/home|/swap'", disk);
    return system(cmd) == 0;
}

static int bench_disk(const char *disk, double *write_spd, double *read_spd) {
    *write_spd = *read_spd = 0;
    char mp[512] = {0};
    int need_unmount = 0;

    struct sigaction sa_new, sa_old_int, sa_old_term;
    memset(&sa_new, 0, sizeof(sa_new));
    sa_new.sa_handler = bench_signal_handler;
    sigemptyset(&sa_new.sa_mask);
    sa_new.sa_flags = 0;
    sigaction(SIGINT, &sa_new, &sa_old_int);
    sigaction(SIGTERM, &sa_new, &sa_old_term);
    bench_interrupted = 0;

    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "awk -v d=/dev/%s '$1==d{print $2}' /proc/mounts", disk);
        FILE *fp = popen(cmd, "r");
        if (fp) { if (!fgets(mp, sizeof(mp), fp)) mp[0] = 0; pclose(fp); }
        mp[strcspn(mp, "\n")] = 0;
    }

    if (mp[0] == 0) {
        fprintf(stderr, "    /dev/%s is not mounted, attempting to mount...\n", disk);
        snprintf(mp, sizeof(mp), "/tmp/db_%s", disk);
        mkdir(mp, 0755);
        char mcmd[512];
        snprintf(mcmd, sizeof(mcmd), "sudo mount /dev/%s %s 2>/dev/null", disk, mp);
        int mret = system(mcmd);
        if (mret == 0) {
            char chcmd[512];
            snprintf(chcmd, sizeof(chcmd), "sudo chmod 1777 %s 2>/dev/null", mp);
            (void)!system(chcmd);
        }
        if (mret != 0) {
            fprintf(stderr, "    mount failed (exit=%d), trying udisksctl...\n", mret);
            snprintf(mcmd, sizeof(mcmd), "udisksctl mount -b /dev/%s 2>/dev/null", disk);
            FILE *fp = popen(mcmd, "r");
            if (fp) {
                char line[512];
                while (fgets(line, sizeof(line), fp)) {
                    if (strstr(line, "at ")) {
                        char *at = strstr(line, "at ") + 3;
                        at[strcspn(at, "\n")] = 0;
                        strncpy(mp, at, sizeof(mp) - 1);
                        mp[sizeof(mp) - 1] = 0;
                    }
                }
                pclose(fp);
            }
        }
        if (mret != 0 && strncmp(mp, "/tmp/db_", 8) == 0) {
            fprintf(stderr, "    ERROR: /dev/%s cannot be mounted. Filesystem may be corrupted.\n", disk);
            fprintf(stderr, "    Reformat with: sudo mkfs.ext4 /dev/%s\n", disk);
            rmdir(mp);
            return -1;
        }
        need_unmount = 1;
    }

    if (mp[0] == 0) {
        fprintf(stderr, "Warning: cannot mount /dev/%s for benchmark\n", disk);
        return -1;
    }

    int iterations = 5;
    int keep = 3;
    long long file_size = 512LL * 1024 * 1024;
    int block_size = 1024 * 1024;

    double *ws = calloc(iterations, sizeof(double));
    double *rs = calloc(iterations, sizeof(double));
    if (!ws || !rs) { free(ws); free(rs); return -1; }

    for (int iter = 0; iter < iterations; iter++) {
        if (bench_interrupted) break;

        char testpath[600];
        snprintf(testpath, sizeof(testpath), "%s/.bench_%s", mp, disk);

        int fd = open(testpath, O_RDWR | O_CREAT | O_DIRECT, 0644);
        if (fd < 0) fd = open(testpath, O_RDWR | O_CREAT, 0644);
        if (fd < 0) { fprintf(stderr, "    [debug] open(%s) failed\n", testpath); continue; }
        (void)!ftruncate(fd, 0);
        void *buf = NULL;
        if (posix_memalign(&buf, 4096, block_size) != 0 || !buf) { close(fd); continue; }
        memset(buf, 'A', block_size);

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        long long written = 0;
        while (written < file_size) {
            ssize_t n = pwrite(fd, buf, block_size, written);
            if (n <= 0) break;
            written += n;
        }
        fdatasync(fd);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        ws[iter] = (double)written / (1024.0 * 1024.0) / elapsed;

        (void)!system("sync && sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null");

        lseek(fd, 0, SEEK_SET);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        long long readtotal = 0;
        while (readtotal < file_size) {
            ssize_t n = pread(fd, buf, block_size, readtotal);
            if (n <= 0) break;
            readtotal += n;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        rs[iter] = (double)readtotal / (1024.0 * 1024.0) / elapsed;

        free(buf);
        close(fd);
        unlink(testpath);
    }

    if (iterations >= 3) {
        for (int i = 0; i < iterations - 1; i++)
            for (int j = i + 1; j < iterations; j++)
                if (ws[j] > ws[i]) { double t = ws[i]; ws[i] = ws[j]; ws[j] = t; }
        for (int i = 0; i < iterations - 1; i++)
            for (int j = i + 1; j < iterations; j++)
                if (rs[j] > rs[i]) { double t = rs[i]; rs[i] = rs[j]; rs[j] = t; }
        double sw = 0, sr = 0;
        for (int i = 1; i < keep + 1; i++) { sw += ws[i]; sr += rs[i]; }
        *write_spd = sw / keep;
        *read_spd = sr / keep;
    }

    if (need_unmount && mp[0]) {
        char ucmd[512];
        snprintf(ucmd, sizeof(ucmd), "sudo umount %s 2>/dev/null", mp);
        (void)!system(ucmd);
        rmdir(mp);
    }

    sigaction(SIGINT, &sa_old_int, NULL);
    sigaction(SIGTERM, &sa_old_term, NULL);

    free(ws);
    free(rs);
    return (*write_spd > 0 || *read_spd > 0) ? 0 : -1;
}

static int cmp_speed(const void *a, const void *b) {
    double sa = ((disk_t *)a)->speed_write;
    double sb = ((disk_t *)b)->speed_write;
    return (sb > sa) - (sb < sa);
}

static int cmd_list(void) {
    printf("%-12s %-8s %-28s %-12s %-8s %-8s\n",
           "DEVICE", "TYPE", "MODEL", "TRAN", "SIZE", "SPEED");
    printf("%-12s %-8s %-28s %-12s %-8s %-8s\n",
           "------------", "--------", "----------------------------", "------------", "--------", "--------");

    FILE *p = popen("lsblk -d -o NAME,TYPE 2>/dev/null | awk '$2==\"disk\"{print $1}'", "r");
    if (!p) return 1;

    char ln[64];
    while (fgets(ln, sizeof(ln), p)) {
        ln[strcspn(ln, "\n")] = 0;
        if (!strncmp(ln, "loop", 4) || !strncmp(ln, "ram", 3)) continue;

        char model[128], tran[16];
        sysfs_model(ln, model, sizeof(model));
        sysfs_tran(ln, tran, sizeof(tran));
        long long gb = sysfs_size_gb(ln);

        char size_str[32];
        if (gb >= 1024) snprintf(size_str, sizeof(size_str), "%.1fT", gb / 1024.0);
        else snprintf(size_str, sizeof(size_str), "%lldG", gb);

        int root = is_root_disk(ln);
        printf("%-12s %-8s %-28s %-12s %-8s %-8s%s\n",
               ln, "disk", model, tran, size_str, "-",
               root ? " [ROOT]" : "");
    }
    pclose(p);
    printf("\n[ROOT] = System disk, cannot be carved with dm-linear\n");
    return 0;
}

static int cmd_bench(int argc, char *argv[]) {
    char *disk_list = NULL;
    int sequential = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--disks") == 0 && i + 1 < argc) {
            disk_list = argv[++i];
        } else if (strcmp(argv[i], "--sequential") == 0) {
            sequential = 1;
        }
    }

    if (!disk_list) {
        fprintf(stderr, "Usage: tiered_setup --bench --disks sda,sdb,sdc [--sequential]\n");
        return 1;
    }

    char disks[MAX_DISKS][32];
    int nd = 0;
    char buf[1024];
    strncpy(buf, disk_list, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *tok = strtok(buf, ",");
    while (tok && nd < MAX_DISKS) {
        if (!is_valid_name(tok)) {
            fprintf(stderr, "Error: invalid disk name '%s'\n", tok);
            return 1;
        }
        strncpy(disks[nd], tok, 31);
        disks[nd][31] = 0;
        nd++;
        tok = strtok(NULL, ",");
    }

    disk_t info[MAX_DISKS];
    for (int i = 0; i < nd; i++) {
        memset(&info[i], 0, sizeof(disk_t));
        strncpy(info[i].disk, disks[i], 31);
        sysfs_model(disks[i], info[i].model, sizeof(info[i].model));
        sysfs_tran(disks[i], info[i].tran, sizeof(info[i].tran));
        info[i].size_gb = sysfs_size_gb(disks[i]);
    }

    if (sequential || nd <= 1) {
        printf("Benchmarking %d disk(s) sequentially...\n", nd);
        for (int i = 0; i < nd; i++) {
            printf("  Testing /dev/%s ... ", info[i].disk);
            fflush(stdout);
            if (bench_disk(info[i].disk, &info[i].speed_write, &info[i].speed_read) == 0) {
                printf("Write: %.0f MB/s  Read: %.0f MB/s\n", info[i].speed_write, info[i].speed_read);
            } else {
                printf("FAILED\n");
            }
        }
    } else {
        printf("Benchmarking %d disks in parallel...\n", nd);

        typedef struct { pid_t pid; int pipe_fd; int idx; } bench_child_t;
        bench_child_t children[MAX_DISKS];
        int nchildren = 0;

        for (int i = 0; i < nd; i++) {
            int pipefd[2];
            if (pipe(pipefd) < 0) { fprintf(stderr, "pipe failed\n"); continue; }

            pid_t pid = fork();
            if (pid < 0) { close(pipefd[0]); close(pipefd[1]); continue; }

            if (pid == 0) {
                close(pipefd[0]);
                double w = 0, r = 0;
                int ret = bench_disk(info[i].disk, &w, &r);
                struct { int ret; double w; double r; } result = { ret, w, r };
                (void)!write(pipefd[1], &result, sizeof(result));
                close(pipefd[1]);
                _exit(0);
            }

            close(pipefd[1]);
            children[nchildren].pid = pid;
            children[nchildren].pipe_fd = pipefd[0];
            children[nchildren].idx = i;
            nchildren++;
            printf("  Started benchmark for /dev/%s (pid %d)\n", info[i].disk, (int)pid);
        }

        int done = 0;
        while (done < nchildren) {
            int status;
            for (int c = 0; c < nchildren; c++) {
                if (children[c].pid <= 0) continue;
                pid_t ret = waitpid(children[c].pid, &status, WNOHANG);
                if (ret > 0) {
                    struct { int ret; double w; double r; } result = { -1, 0, 0 };
                    (void)!read(children[c].pipe_fd, &result, sizeof(result));
                    close(children[c].pipe_fd);
                    int idx = children[c].idx;
                    info[idx].speed_write = result.w;
                    info[idx].speed_read = result.r;
                    if (result.ret == 0)
                        printf("  /dev/%s: Write %.0f MB/s  Read %.0f MB/s\n",
                               info[idx].disk, result.w, result.r);
                    else
                        printf("  /dev/%s: FAILED\n", info[idx].disk);
                    children[c].pid = 0;
                    done++;
                }
            }
            if (done < nchildren) usleep(100000);
        }
    }

    double total_w = 0;
    printf("\n%-28s %-12s %-8s %-8s %-8s\n", "ID", "Type", "Size", "Write", "Read");
    printf("%-28s %-12s %-8s %-8s %-8s\n", "----------------------------", "------------", "--------", "--------", "--------");
    for (int i = 0; i < nd; i++) {
        char id[256];
        snprintf(id, sizeof(id), "%s(%s)", info[i].model, info[i].disk);

        char size_str[32];
        if (info[i].size_gb >= 1024) snprintf(size_str, sizeof(size_str), "%.1fT", info[i].size_gb / 1024.0);
        else snprintf(size_str, sizeof(size_str), "%lldG", info[i].size_gb);

        char w[16] = "-", r[16] = "-";
        if (info[i].speed_write > 0) { snprintf(w, sizeof(w), "%.0f", info[i].speed_write); total_w += info[i].speed_write; }
        if (info[i].speed_read > 0) snprintf(r, sizeof(r), "%.0f", info[i].speed_read);
        printf("%-28s %-12s %-8s %-8s %-8s\n", id, info[i].tran, size_str, w, r);
    }
    printf("\nTotal theoretical write speed: %.0f MB/s\n", total_w);
    printf("Estimated actual: ~%.0f MB/s (92-97%% efficiency)\n", total_w * 0.94);

    return 0;
}

static void cleanup_create(const char *name, disk_t *valid, int valid_disks) {
    fprintf(stderr, "  Rolling back...\n");
    run_sudo("umount /dev/mapper/tv_vg_%s-tv_lv_%s 2>/dev/null", name, name);
    run_sudo("lvremove --config 'devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}' -f tv_vg_%s/tv_lv_%s 2>/dev/null", name, name);
    run_sudo("vgremove --config 'devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}' -f tv_vg_%s 2>/dev/null", name);
    for (int i = 0; i < valid_disks; i++) {
        char target[64];
        snprintf(target, sizeof(target), "tv_%s_carve", valid[i].disk);
        run_sudo("pvremove --config 'devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}' -ff -y /dev/mapper/%s 2>/dev/null", target);
        run_sudo("dmsetup remove %s 2>/dev/null", target);
    }
    fprintf(stderr, "  Rollback complete.\n");
}

static int cmd_create(int argc, char *argv[]) {
    char *name = NULL;
    char *disk_spec = NULL;
    char *fs = "ext4";
    char *mount_point = NULL;
    int stripe_size_kb = 512;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) name = argv[++i];
        else if (strcmp(argv[i], "--disks") == 0 && i + 1 < argc) disk_spec = argv[++i];
        else if (strcmp(argv[i], "--fs") == 0 && i + 1 < argc) fs = argv[++i];
        else if (strcmp(argv[i], "--mount") == 0 && i + 1 < argc) mount_point = argv[++i];
        else if (strcmp(argv[i], "--stripesize") == 0 && i + 1 < argc) stripe_size_kb = atoi(argv[++i]);
    }

    if (!name || !disk_spec) {
        fprintf(stderr, "Usage: tiered_setup --create --name NAME --disks sdb:300,sdc:200 [--fs ext4] [--mount /mnt/fast]\n");
        return 1;
    }

    if (!is_valid_name(name)) {
        fprintf(stderr, "Error: invalid name '%s' (only a-z, A-Z, 0-9, ., _, - allowed)\n", name);
        return 1;
    }

    disk_t disks_arr[MAX_DISKS];
    int nd = 0;
    char buf[1024];
    strncpy(buf, disk_spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *tok = strtok(buf, ",");
    while (tok && nd < MAX_DISKS) {
        memset(&disks_arr[nd], 0, sizeof(disk_t));
        char *colon = strchr(tok, ':');
        if (colon) {
            *colon = 0;
            strncpy(disks_arr[nd].disk, tok, 31);
            disks_arr[nd].disk[31] = 0;
            disks_arr[nd].carve_gb = atoll(colon + 1);
        } else {
            strncpy(disks_arr[nd].disk, tok, 31);
            disks_arr[nd].disk[31] = 0;
            disks_arr[nd].carve_gb = 0;
        }
        nd++;
        tok = strtok(NULL, ",");
    }

    if (nd == 0) {
        fprintf(stderr, "Error: no disks specified\n");
        return 1;
    }

    printf("=== TieredVol: Creating '%s' ===\n", name);

    int valid_disks = 0;
    disk_t valid[MAX_DISKS];

    for (int i = 0; i < nd; i++) {
        sysfs_model(disks_arr[i].disk, disks_arr[i].model, sizeof(disks_arr[i].model));
        sysfs_tran(disks_arr[i].disk, disks_arr[i].tran, sizeof(disks_arr[i].tran));
        disks_arr[i].size_gb = sysfs_size_gb(disks_arr[i].disk);
        disks_arr[i].is_root = is_root_disk(disks_arr[i].disk);

        if (disks_arr[i].is_root) {
            printf("  WARNING: /dev/%s is system disk, skipping (cannot carve)\n", disks_arr[i].disk);
            continue;
        }

        if (disks_arr[i].carve_gb <= 0) {
            disks_arr[i].carve_gb = disks_arr[i].size_gb - 1;
        }
        if (disks_arr[i].carve_gb > disks_arr[i].size_gb - 1) {
            fprintf(stderr, "Error: /dev/%s has %lldGB, cannot carve %lldGB\n",
                    disks_arr[i].disk, disks_arr[i].size_gb, disks_arr[i].carve_gb);
            return 1;
        }

        printf("  Benchmarking /dev/%s ... ", disks_arr[i].disk);
        fflush(stdout);
        bench_disk(disks_arr[i].disk, &disks_arr[i].speed_write, &disks_arr[i].speed_read);
        printf("Write: %.0f MB/s\n", disks_arr[i].speed_write);

        valid[valid_disks++] = disks_arr[i];
    }

    if (valid_disks == 0) {
        fprintf(stderr, "Error: no usable disks (all are system disks)\n");
        return 1;
    }

    qsort(valid, valid_disks, sizeof(disk_t), cmp_speed);

    long long total_gb = 0;
    for (int i = 0; i < valid_disks; i++) total_gb += valid[i].carve_gb;

    printf("\nConfiguration:\n");
    printf("  Name: %s\n", name);
    printf("  Disks: %d (usable)\n", valid_disks);
    printf("  Total: %lldGB\n", total_gb);
    printf("  Filesystem: %s\n", fs);
    printf("  Stripesize: %dKB\n", stripe_size_kb);
    if (mount_point) printf("  Mount: %s\n", mount_point);
    printf("\n");

    double total_speed = 0;
    printf("  %-12s %-8s %-8s %-10s %-10s\n", "DEVICE", "CARVE", "SPEED", "TIER", "RATIO");
    printf("  %-12s %-8s %-8s %-10s %-10s\n", "------------", "--------", "--------", "----------", "----------");
    for (int i = 0; i < valid_disks; i++) total_speed += valid[i].speed_write;
    for (int i = 0; i < valid_disks; i++) {
        double ratio = (total_speed > 0) ? valid[i].speed_write / total_speed * 100 : 0;
        printf("  %-12s %-8lldGB %-8.0f %-10s %-10.1f%%\n",
               valid[i].disk, valid[i].carve_gb, valid[i].speed_write,
               (i == 0) ? "FAST" : (i == valid_disks - 1) ? "SLOW" : "MED",
               ratio);
    }
    printf("  Total theoretical speed: %.0f MB/s\n", total_speed);
    printf("  Estimated actual: ~%.0f MB/s\n", total_speed * 0.94);
    printf("\n");

    printf("Step 1: Cleaning up old targets...\n");
    for (int i = 0; i < valid_disks; i++) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "awk -v d=/dev/%s '$1==d{print $2}' /proc/mounts", valid[i].disk);
        FILE *fp = popen(cmd, "r");
        if (fp) {
            char mnt[512];
            while (fgets(mnt, sizeof(mnt), fp)) {
                mnt[strcspn(mnt, "\n")] = 0;
                run_sudo("umount %s 2>/dev/null", mnt);
            }
            pclose(fp);
        }
        char target[64];
        snprintf(target, sizeof(target), "tv_%s_carve", valid[i].disk);
        run_sudo("dmsetup remove %s 2>/dev/null", target);
    }
    run_sudo("lvremove -f tv_vg_%s/tv_lv_%s 2>/dev/null", name, name);
    run_sudo("vgremove -f tv_vg_%s 2>/dev/null", name);
    for (int i = 0; i < valid_disks; i++) {
        char target[64];
        snprintf(target, sizeof(target), "tv_%s_carve", valid[i].disk);
        run_sudo("pvremove -ff -y /dev/mapper/%s 2>/dev/null", target);
    }

    printf("Step 2: Creating carved targets...\n");
    for (int i = 0; i < valid_disks; i++) {
        long long sectors = valid[i].carve_gb * 1024LL * 1024 * 1024 / 512;
        char target[64];
        snprintf(target, sizeof(target), "tv_%s_carve", valid[i].disk);

        run_sudo("printf '0 %lld linear /dev/%s 0\\n' > /tmp/.tv_dmtable && "
                 "dmsetup create %s < /tmp/.tv_dmtable",
                 sectors, valid[i].disk, target);

        char devpath[128];
        snprintf(devpath, sizeof(devpath), "/dev/mapper/%s", target);
        struct stat st;
        if (stat(devpath, &st) != 0) {
            fprintf(stderr, "Error: failed to create %s\n", target);
            cleanup_create(name, valid, i);
            return 1;
        }
        printf("  Created %s (%lldGB)\n", target, valid[i].carve_gb);
    }

    printf("Step 3: Creating LVM physical volumes...\n");
    for (int i = 0; i < valid_disks; i++) {
        char target[64];
        snprintf(target, sizeof(target), "tv_%s_carve", valid[i].disk);
        if (run_sudo("pvcreate -f /dev/mapper/%s", target) != 0) {
            fprintf(stderr, "Error: pvcreate failed for %s\n", target);
            cleanup_create(name, valid, valid_disks);
            return 1;
        }
        printf("  PV: /dev/mapper/%s\n", target);
    }

    printf("Step 4: Creating volume group...\n");
    char vg_cmd[4096];
    snprintf(vg_cmd, sizeof(vg_cmd),
        "vgcreate --config 'devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}' tv_vg_%s", name);
    for (int i = 0; i < valid_disks; i++) {
        char target[64];
        snprintf(target, sizeof(target), "tv_%s_carve", valid[i].disk);
        size_t len = strlen(vg_cmd);
        snprintf(vg_cmd + len, sizeof(vg_cmd) - len, " /dev/mapper/%s", target);
    }
    if (run_sudo("%s", vg_cmd) != 0) {
        fprintf(stderr, "Error: vgcreate failed for tv_vg_%s\n", name);
        cleanup_create(name, valid, valid_disks);
        return 1;
    }
    printf("  VG: tv_vg_%s\n", name);

    printf("Step 5: Creating striped logical volume...\n");
    if (run_sudo("lvcreate --config 'devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}' "
             "-l 100%%FREE -i %d -I %dk -n tv_lv_%s tv_vg_%s",
             valid_disks, stripe_size_kb, name, name) != 0) {
        fprintf(stderr, "Error: lvcreate failed\n");
        cleanup_create(name, valid, valid_disks);
        return 1;
    }
    printf("  LV: /dev/mapper/tv_vg_%s-tv_lv_%s (%d stripes, %dKB stripesize)\n",
           name, name, valid_disks, stripe_size_kb);

    char lv_path[256];
    snprintf(lv_path, sizeof(lv_path), "/dev/mapper/tv_vg_%s-tv_lv_%s", name, name);

    if (strcmp(fs, "none") != 0) {
        printf("Step 6: Formatting as %s...\n", fs);
        if (run_sudo("mkfs.%s %s", fs, lv_path) != 0) {
            fprintf(stderr, "Error: mkfs.%s failed\n", fs);
            cleanup_create(name, valid, valid_disks);
            return 1;
        }
    } else {
        printf("Step 6: Skipped formatting (raw)\n");
    }

    if (mount_point && strcmp(fs, "none") != 0) {
        printf("Step 7: Mounting...\n");
        run_sudo("mkdir -p %s", mount_point);
        if (run_sudo("mount %s %s", lv_path, mount_point) != 0) {
            fprintf(stderr, "Error: mount failed\n");
            cleanup_create(name, valid, valid_disks);
            return 1;
        }
        printf("  Mounted at %s\n", mount_point);
    } else {
        printf("Step 7: Skipped mounting\n");
    }

    printf("\nSaving config...\n");
    char conf_path[256];
    snprintf(conf_path, sizeof(conf_path), "/tmp/tieredvol_%s.conf", name);
    FILE *cf = fopen(conf_path, "w");
    if (cf) {
        fprintf(cf, "[general]\nname=%s\ncount=%d\nfs=%s\nstripesize=%d\n", name, valid_disks, fs, stripe_size_kb);
        if (mount_point) fprintf(cf, "mount=%s\n", mount_point);
        fprintf(cf, "total_gb=%lld\n\n", total_gb);
        for (int i = 0; i < valid_disks; i++) {
            fprintf(cf, "[disk.%d]\ndevice=%s\nsize_gb=%lld\nspeed=%.0f\n\n",
                    i, valid[i].disk, valid[i].carve_gb, valid[i].speed_write);
        }
        fclose(cf);
        run_sudo("mkdir -p /etc/tieredvol && cp %s /etc/tieredvol/%s.conf", conf_path, name);
        run("rm -f %s", conf_path);
    }

    printf("\n=== Complete! ===\n");
    printf("Device: %s\n", lv_path);
    printf("Size: %lldGB\n", total_gb);
    printf("Stripes: %d x %dKB\n", valid_disks, stripe_size_kb);
    if (mount_point) printf("Mount: %s\n", mount_point);
    printf("\nVerify with:\n");
    printf("  df -h %s\n", mount_point ? mount_point : lv_path);
    printf("  sudo fio --name=test --filename=%s --rw=write --bs=1M --size=10G\n",
           mount_point ? mount_point : lv_path);

    return 0;
}

static int cmd_remove(int argc, char *argv[]) {
    char *name = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) name = argv[++i];
    }

    if (!name) {
        fprintf(stderr, "Usage: tiered_setup --remove --name NAME\n");
        return 1;
    }

    printf("=== TieredVol: Removing '%s' ===\n", name);

    char targets[MAX_DISKS][64];
    int ntargets = 0;

    char conf_path[256];
    snprintf(conf_path, sizeof(conf_path), "/etc/tieredvol/%s.conf", name);
    FILE *cf = fopen(conf_path, "r");
    if (cf) {
        char line[256];
        while (fgets(line, sizeof(line), cf) && ntargets < MAX_DISKS) {
            if (strncmp(line, "device=", 7) == 0) {
                char disk[32];
                sscanf(line + 7, "%31s", disk);
                snprintf(targets[ntargets], sizeof(targets[0]), "tv_%s_carve", disk);
                ntargets++;
            }
        }
        fclose(cf);
    }

    if (ntargets == 0) {
        printf("  (No config file, scanning dm targets...)\n");
        FILE *p = popen("sudo dmsetup ls 2>/dev/null", "r");
        if (p) {
            char line[256];
            while (fgets(line, sizeof(line), p) && ntargets < MAX_DISKS) {
                line[strcspn(line, "\n")] = 0;
                if (strncmp(line, "tv_", 3) == 0 && strstr(line, "_carve")) {
                    snprintf(targets[ntargets], sizeof(targets[0]), "%s", line);
                    ntargets++;
                }
            }
            pclose(p);
        }
    }

    printf("Unmounting...\n");
    run_sudo("umount /dev/mapper/tv_vg_%s-tv_lv_%s 2>/dev/null", name, name);

    printf("Removing LV...\n");
    run_sudo("lvremove --config 'devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}' -f tv_vg_%s/tv_lv_%s 2>/dev/null", name, name);

    printf("Removing VG...\n");
    run_sudo("vgremove --config 'devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}' -f tv_vg_%s 2>/dev/null", name);

    printf("Removing PV and dm targets...\n");
    for (int i = 0; i < ntargets; i++) {
        run_sudo("pvremove --config 'devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}' -ff -y /dev/mapper/%s 2>/dev/null", targets[i]);
        run_sudo("dmsetup remove %s 2>/dev/null", targets[i]);
        printf("  Removed %s\n", targets[i]);
    }

    printf("Removing config...\n");
    run_sudo("rm -f /etc/tieredvol/%s.conf", name);
    run_sudo("rmdir /etc/tieredvol 2>/dev/null");

    printf("\n=== Remove Complete ===\n");
    return 0;
}

static int cmd_status(void) {
    printf("=== TieredVol Status ===\n\n");

    printf("DM Targets:\n");
    FILE *p = popen("ls /dev/mapper/tv_* 2>/dev/null", "r");
    if (p) {
        char line[256];
        int found = 0;
        while (fgets(line, sizeof(line), p)) {
            found = 1;
            line[strcspn(line, "\n")] = 0;
            printf("  %s\n", line);
        }
        pclose(p);
        if (!found) printf("  None\n");
    }

    printf("\nSaved Configs:\n");
    p = popen("ls /etc/tieredvol/*.conf 2>/dev/null", "r");
    if (p) {
        char line[256];
        int found = 0;
        while (fgets(line, sizeof(line), p)) {
            found = 1;
            line[strcspn(line, "\n")] = 0;
            printf("  %s\n", line);
        }
        pclose(p);
        if (!found) printf("  None\n");
    }

    return 0;
}

int main(int argc, char *argv[]) {
    if (geteuid() != 0) {
        fprintf(stderr, "Error: tiered_setup requires root privileges.\n");
        fprintf(stderr, "Please run: sudo %s\n", argv[0]);
        return 1;
    }

    if (argc < 2) {
        printf("TieredVol — Tiered Storage Volume Manager v%s\n\n", VERSION);
        printf("Usage:\n");
        printf("  tiered_setup --list                              List all disks\n");
        printf("  tiered_setup --bench --disks sda,sdb,sdc         Benchmark disks (parallel)\n");
        printf("  tiered_setup --create --name NAME --disks ...    Create tiered volume\n");
        printf("  tiered_setup --remove --name NAME                Remove tiered volume\n");
        printf("  tiered_setup --status                            Show status\n");
        printf("  tiered_setup --version                           Show version\n");
        printf("\nExamples:\n");
        printf("  sudo tiered_setup --create --name fastpool --disks sdb:300,sdc:200 --fs ext4 --mount /mnt/fast\n");
        printf("  sudo tiered_setup --remove --name fastpool\n");
        return 0;
    }

    if (strcmp(argv[1], "--list") == 0) return cmd_list();
    if (strcmp(argv[1], "--bench") == 0) return cmd_bench(argc, argv);
    if (strcmp(argv[1], "--create") == 0) return cmd_create(argc, argv);
    if (strcmp(argv[1], "--remove") == 0 || strcmp(argv[1], "--destroy") == 0) return cmd_remove(argc, argv);
    if (strcmp(argv[1], "--status") == 0) return cmd_status();
    if (strcmp(argv[1], "--version") == 0) { printf("TieredVol %s\n", VERSION); return 0; }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        argc = 1;
        return main(1, argv);
    }

    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    return 1;
}
