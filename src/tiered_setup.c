#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <dirent.h>
#include <linux/fs.h>
#include <math.h>
#include <limits.h>
#include <errno.h>
#include "tiered_common.h"
#include "tiered_sched.h"
#include "version.h"

static volatile sig_atomic_t bench_interrupted = 0;

static void bench_signal_handler(int sig) {
    (void)sig;
    bench_interrupted = 1;
}

static void print_usage(const char *prog) {
    printf("TieredVol — Tiered Storage Volume Manager v%s\n\n", VERSION);
    printf("Usage:\n");
    printf("  %s --list                              List all disks\n", prog);
    printf("  %s --bench --disks sda,sdb,sdc         Benchmark disks (parallel)\n", prog);
    printf("  %s --bench --disks sda,sdb --sequential Benchmark disks (sequential)\n", prog);
    printf("  %s --create --name NAME --disks ...    Create tiered volume\n", prog);
    printf("  %s --create --name NAME --disks ... --scheduler  Create with weighted I/O scheduler\n", prog);
    printf("  %s --remove --name NAME                Remove tiered volume\n", prog);
    printf("  %s --destroy --name NAME               Remove tiered volume\n", prog);
    printf("  %s --status                            Show status\n", prog);
    printf("  %s --version                           Show version\n", prog);
    printf("\nExamples:\n");
    printf("  sudo %s --create --name fastpool --disks sdb:300,sdc:200 --fs ext4 --mount /mnt/fast\n", prog);
    printf("  sudo %s --remove --name fastpool\n", prog);
}

static int safe_execvp(const char *path, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(path, argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int run_quiet(const char *path, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execvp(path, argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int run_capture(const char *path, char *const argv[], char *out, size_t outsize) {
    int pfd[2];
    if (pipe(pfd) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
        close(pfd[1]);
        execvp(path, argv);
        _exit(127);
    }
    close(pfd[1]);
    size_t total = 0;
    while (total < outsize - 1) {
        ssize_t n = read(pfd[0], out + total, outsize - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    close(pfd[0]);
    out[total] = 0;
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int run_sudo_argv(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char *sudo_argv[64];
        sudo_argv[0] = "sudo";
        int i = 1;
        for (char *const *a = argv; *a && i < 62; a++) sudo_argv[i++] = *a;
        sudo_argv[i] = NULL;
        execvp("sudo", sudo_argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int run_sudo_quiet(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        char *sudo_argv[64];
        sudo_argv[0] = "sudo";
        int i = 1;
        for (char *const *a = argv; *a && i < 62; a++) sudo_argv[i++] = *a;
        sudo_argv[i] = NULL;
        execvp("sudo", sudo_argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

typedef struct {
    char disk[32];
    char model[128];
    char tran[16];
    long long size_gb;
    double speed_write;
    double speed_read;
    long long carve_gb;
    int is_root;
    int is_mounted;
} disk_t;

typedef struct {
    char name[32];
    char tran[16];
    int is_root;
    int is_mounted;
} disk_info_t;

typedef struct { int ret; double w; double r; } bench_result_t;

static void make_target(char *out, size_t sz, const char *disk) {
    snprintf(out, sz, "tv_%s_carve", disk);
}

static long long sysfs_size_gb(const char *disk) {
    char path[256];
    int n = snprintf(path, sizeof(path), "/sys/block/%s/size", disk);
    if (n < 0 || n >= (int)sizeof(path)) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    long long sec = 0;
    if (fscanf(f, "%lld", &sec) != 1) sec = 0;
    fclose(f);
    return sec * 512 / (1024LL * 1024 * 1024);
}

static void sysfs_model(const char *disk, char *out, size_t len) {
    char path[256];
    int n = snprintf(path, sizeof(path), "/sys/block/%s/device/model", disk);
    if (n < 0 || n >= (int)sizeof(path)) { strncpy(out, disk, len - 1); out[len - 1] = 0; return; }
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(out, len, f)) {
            out[strcspn(out, "\n")] = 0;
            if (out[0] == 0) { strncpy(out, disk, len - 1); out[len - 1] = 0; fclose(f); return; }
            char *p = out + strlen(out) - 1;
            while (p > out && *p == ' ') *p-- = 0;
        }
        fclose(f);
    } else {
        strncpy(out, disk, len - 1);
        out[len - 1] = 0;
    }
}

static int is_virtual_disk(const char *name) {
    return strncmp(name, "loop", 4) == 0 || strncmp(name, "ram", 3) == 0 || strncmp(name, "dm-", 3) == 0;
}

static int load_all_disk_info(disk_info_t *out, int max) {
    int n = 0;
    FILE *p = popen("lsblk -d -o NAME,TRAN,MOUNTPOINT 2>/dev/null", "r");
    if (p) {
        char line[256];
        if (!fgets(line, sizeof(line), p)) { pclose(p); return n; }
        while (fgets(line, sizeof(line), p) && n < max) {
            line[strcspn(line, "\n")] = 0;
            char *name = strtok(line, " ");
            char *tran = strtok(NULL, " ");
            char *mnt = strtok(NULL, " ");
            if (!name) continue;
            if (is_virtual_disk(name)) continue;
            strncpy(out[n].name, name, sizeof(out[n].name) - 1);
            out[n].name[sizeof(out[n].name) - 1] = 0;
            strncpy(out[n].tran, tran ? tran : "unknown", sizeof(out[n].tran) - 1);
            out[n].tran[sizeof(out[n].tran) - 1] = 0;
            out[n].is_root = 0;
            out[n].is_mounted = 0;
            if (mnt && strcmp(mnt, "/") == 0)
                out[n].is_root = 1;
            else if (mnt && mnt[0] != 0)
                out[n].is_mounted = 1;
            n++;
        }
        pclose(p);
    }
    return n;
}

static void find_mount_for_disk(const char *disk, char *mp, size_t mp_size) {
    mp[0] = 0;
    char dev_pattern[64];
    snprintf(dev_pattern, sizeof(dev_pattern), "/dev/%s ", disk);
    FILE *fp = fopen("/proc/mounts", "r");
    if (fp) {
        char mnt_line[512];
        while (fgets(mnt_line, sizeof(mnt_line), fp)) {
            if (strncmp(mnt_line, dev_pattern, strlen(dev_pattern)) == 0) {
                char fmt[32];
                snprintf(fmt, sizeof(fmt), "%%*s %%%zus", mp_size - 1);
                sscanf(mnt_line, fmt, mp);
                break;
            }
        }
        fclose(fp);
    }
}

static int bench_disk(const char *disk, double *write_spd, double *read_spd, int warmup) {
    *write_spd = *read_spd = 0;
    char mp[512] = {0};
    int need_unmount = 0;
    int result = -1;
    double *ws = NULL;
    double *rs = NULL;
    char cleanup_path[600] = "";

    struct sigaction sa_new, sa_old_int, sa_old_term;
    memset(&sa_new, 0, sizeof(sa_new));
    sa_new.sa_handler = bench_signal_handler;
    sigemptyset(&sa_new.sa_mask);
    sa_new.sa_flags = 0;
    sigaction(SIGINT, &sa_new, &sa_old_int);
    sigaction(SIGTERM, &sa_new, &sa_old_term);
    bench_interrupted = 0;

    find_mount_for_disk(disk, mp, sizeof(mp));

    if (mp[0] == 0) {
        fprintf(stderr, "    /dev/%s is not mounted, attempting to mount...\n", disk);
        snprintf(mp, sizeof(mp), "/tmp/db_%s", disk);
        mkdir(mp, 0755);
        char devpath_b[64];
        snprintf(devpath_b, sizeof(devpath_b), "/dev/%s", disk);
        char *mount_argv[] = {"sudo", "mount", devpath_b, mp, NULL};
        int mret = run_quiet("sudo", mount_argv);
        if (mret == 0) {
            char *chmod_argv[] = {"sudo", "chmod", "0755", mp, NULL};
            (void)run_quiet("sudo", chmod_argv);
        }
        if (mret != 0) {
            fprintf(stderr, "    mount failed (exit=%d), trying udisksctl...\n", mret);
            char *udisks_argv[] = {"udisksctl", "mount", "-b", devpath_b, NULL};
            char uout[512] = "";
            run_capture("udisksctl", udisks_argv, uout, sizeof(uout));
            char *at = strstr(uout, "at ");
            if (at) {
                at += 3;
                at[strcspn(at, "\n")] = 0;
                strncpy(mp, at, sizeof(mp) - 1);
                mp[sizeof(mp) - 1] = 0;
            }
        }
        if (mret != 0 && strncmp(mp, "/tmp/db_", 8) == 0) {
            rmdir(mp);
            mp[0] = 0;
            fprintf(stderr, "    /dev/%s has no filesystem, using raw device benchmark\n", disk);
            char devpath_raw[64];
            snprintf(devpath_raw, sizeof(devpath_raw), "/dev/%s", disk);
            uint64_t raw_speed = 0;
            if (tv_benchmark(devpath_raw, &raw_speed, warmup) == 0) {
                *write_spd = (double)raw_speed;
                *read_spd = (double)raw_speed;
                result = 0;
            }
            goto cleanup;
        }
        need_unmount = 1;
    }

    if (mp[0] == 0) {
        fprintf(stderr, "Warning: cannot mount /dev/%s for benchmark\n", disk);
        goto cleanup;
    }

    {
        int iterations = 5;
        long long file_size = 512LL * 1024 * 1024;
        int block_size = 1024 * 1024;
        int o_direct_warned = 0;

        ws = calloc(iterations, sizeof(double));
        rs = calloc(iterations, sizeof(double));
        if (!ws || !rs) goto cleanup;

        /* SLC cache warm-up: write 2GB to exhaust SLC cache before benchmarking */
        if (warmup) {
            char warmup_path[PATH_MAX];
            snprintf(warmup_path, sizeof(warmup_path), "%s/.bench_warmup_%s", mp, disk);
            int wfd = open(warmup_path, O_RDWR | O_CREAT | O_DIRECT, 0644);
            if (wfd < 0) wfd = open(warmup_path, O_RDWR | O_CREAT, 0644);
            if (wfd >= 0) {
                void *wbuf = NULL;
                if (posix_memalign(&wbuf, 4096, block_size) == 0 && wbuf) {
                    memset(wbuf, 0xAB, block_size);
                    fprintf(stderr, "  Warming up SLC cache (2GB)...\n");
                    long long warmup_target = 2LL * 1024 * 1024 * 1024;
                    long long warmup_written = 0;
                    while (warmup_written < warmup_target && !bench_interrupted) {
                        ssize_t n = pwrite(wfd, wbuf, block_size, warmup_written);
                        if (n <= 0) break;
                        warmup_written += n;
                    }
                    fdatasync(wfd);
                    free(wbuf);
                    fprintf(stderr, "  Warm-up complete, starting benchmark...\n");
                }
                close(wfd);
                unlink(warmup_path);
            }
        }

        int completed = 0;
        for (int iter = 0; iter < iterations; iter++) {
            if (bench_interrupted) break;

            char testpath[PATH_MAX];
            snprintf(testpath, sizeof(testpath), "%s/.bench_%s", mp, disk);
            snprintf(cleanup_path, sizeof(cleanup_path), "%s", testpath);

            int fd = open(testpath, O_RDWR | O_CREAT | O_DIRECT, 0644);
            if (fd < 0) {
                if (!o_direct_warned) {
                    fprintf(stderr, "    WARNING: O_DIRECT not supported, using buffered I/O (speeds may be inflated)\n");
                    o_direct_warned = 1;
                }
                fd = open(testpath, O_RDWR | O_CREAT, 0644);
            }
            if (fd < 0) { fprintf(stderr, "    [debug] open(%s) failed\n", testpath); continue; }
            if (ftruncate(fd, 0) != 0) { close(fd); continue; }
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

            char devpath[64];
            snprintf(devpath, sizeof(devpath), "/dev/%s", disk);
            int fd_disk = open(devpath, O_RDONLY);
            if (fd_disk >= 0) { ioctl(fd_disk, BLKFLSBUF, NULL); close(fd_disk); }

            clock_gettime(CLOCK_MONOTONIC, &t1);
            double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            if (elapsed <= 0.0) elapsed = 0.001;
            ws[completed] = (double)written / (1024.0 * 1024.0) / elapsed;

            sync();
            int dcfd = open("/proc/sys/vm/drop_caches", O_WRONLY);
            if (dcfd >= 0) { (void)!write(dcfd, "3", 1); close(dcfd); }

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
            if (elapsed <= 0.0) elapsed = 0.001;
            rs[completed] = (double)readtotal / (1024.0 * 1024.0) / elapsed;
            completed++;

            free(buf);
            close(fd);
            unlink(testpath);
            cleanup_path[0] = 0;
        }

        for (int i = 0; i < completed; i++) {
            if (isnan(ws[i])) ws[i] = 0;
            if (isnan(rs[i])) rs[i] = 0;
        }

        if (completed >= 3) {
            for (int i = 0; i < completed - 1; i++)
                for (int j = i + 1; j < completed; j++)
                    if (ws[j] > ws[i]) { double t = ws[i]; ws[i] = ws[j]; ws[j] = t; }
            for (int i = 0; i < completed - 1; i++)
                for (int j = i + 1; j < completed; j++)
                    if (rs[j] > rs[i]) { double t = rs[i]; rs[i] = rs[j]; rs[j] = t; }
            double sw = 0, sr = 0;
            int cnt = 0;
            for (int i = 1; i < completed - 1; i++) { sw += ws[i]; sr += rs[i]; cnt++; }
            if (cnt > 0) { *write_spd = sw / cnt; *read_spd = sr / cnt; }
        } else if (completed > 0) {
            double sw = 0, sr = 0;
            for (int i = 0; i < completed; i++) { sw += ws[i]; sr += rs[i]; }
            *write_spd = sw / completed;
            *read_spd = sr / completed;
        }
    }

    if (need_unmount && mp[0]) {
        char *umount_argv[] = {"sudo", "umount", mp, NULL};
        (void)run_sudo_argv(umount_argv);
        rmdir(mp);
        mp[0] = 0;
    }

    result = (*write_spd > 0 || *read_spd > 0) ? 0 : -1;

cleanup:
    if (cleanup_path[0]) unlink(cleanup_path);
    if (need_unmount && mp[0] && result != 0) {
        char *umount_argv[] = {"sudo", "umount", mp, NULL};
        (void)run_sudo_argv(umount_argv);
        rmdir(mp);
    }
    sigaction(SIGINT, &sa_old_int, NULL);
    sigaction(SIGTERM, &sa_old_term, NULL);
    free(ws);
    free(rs);
    return result;
}

static double cmp_score(const disk_t *d) {
    return d->speed_write * 0.6 + d->speed_read * 0.4;
}

static int cmp_speed(const void *a, const void *b) {
    double sa = cmp_score((const disk_t *)a);
    double sb = cmp_score((const disk_t *)b);
    if (isnan(sa) && isnan(sb)) return 0;
    if (isnan(sa)) return 1;
    if (isnan(sb)) return -1;
    return (sb > sa) - (sb < sa);
}

typedef struct { pid_t pid; int pipe_fd; int idx; } bench_child_t;
typedef void (*bench_interrupt_fn)(void *ctx);

static int run_parallel_bench(disk_t *disks, int ndisks, int warmup,
    bench_interrupt_fn on_interrupt, void *interrupt_ctx) {
    bench_child_t children[MAX_DISKS];
    int nchildren = 0;
    for (int i = 0; i < ndisks; i++) {
        int pipefd[2];
        if (pipe(pipefd) < 0) { fprintf(stderr, "pipe failed\n"); continue; }
        pid_t pid = fork();
        if (pid < 0) { close(pipefd[0]); close(pipefd[1]); continue; }
        if (pid == 0) {
            close(pipefd[0]);
            double w = 0, r = 0;
            int ret = bench_disk(disks[i].disk, &w, &r, warmup);
            bench_result_t result = { ret, w, r };
            const char *wp = (const char *)&result;
            size_t remaining = sizeof(result);
            while (remaining > 0) {
                ssize_t n = write(pipefd[1], wp, remaining);
                if (n <= 0) break;
                wp += n;
                remaining -= n;
            }
            close(pipefd[1]);
            _exit(0);
        }
        close(pipefd[1]);
        children[nchildren].pid = pid;
        children[nchildren].pipe_fd = pipefd[0];
        children[nchildren].idx = i;
        nchildren++;
        printf("  Started benchmark for /dev/%s\n", disks[i].disk);
    }
    int done = 0;
    while (done < nchildren) {
        if (bench_interrupted) {
            for (int c = 0; c < nchildren; c++) {
                if (children[c].pid > 0) kill(children[c].pid, SIGTERM);
            }
            for (int c = 0; c < nchildren; c++) {
                if (children[c].pid > 0) {
                    waitpid(children[c].pid, NULL, 0);
                    close(children[c].pipe_fd);
                }
            }
            fprintf(stderr, "\nBenchmark interrupted.\n");
            if (on_interrupt) on_interrupt(interrupt_ctx);
            return 1;
        }
        int status;
        for (int c = 0; c < nchildren; c++) {
            if (children[c].pid <= 0) continue;
            pid_t ret = waitpid(children[c].pid, &status, WNOHANG);
            if (ret > 0) {
                bench_result_t result = { -1, 0, 0 };
                ssize_t nread = read(children[c].pipe_fd, &result, sizeof(result));
                close(children[c].pipe_fd);
                int idx = children[c].idx;
                if (nread == sizeof(result)) {
                    disks[idx].speed_write = result.w;
                    disks[idx].speed_read = result.r;
                    if (result.ret == 0)
                        printf("  /dev/%s: Write %.0f MB/s  Read %.0f MB/s\n",
                               disks[idx].disk, result.w, result.r);
                    else
                        printf("  /dev/%s: FAILED\n", disks[idx].disk);
                } else {
                    printf("  /dev/%s: pipe read error\n", disks[idx].disk);
                }
                children[c].pid = 0;
                done++;
            }
        }
        if (done < nchildren) usleep(100000);
    }
    return 0;
}

static int cmd_list(void) {
    printf("%-12s %-8s %-28s %-12s %-8s %-8s\n",
           "DEVICE", "TYPE", "MODEL", "TRAN", "SIZE", "SPEED");
    printf("%-12s %-8s %-28s %-12s %-8s %-8s\n",
           "------------", "--------", "----------------------------", "------------", "--------", "--------");

    disk_info_t info[MAX_DISKS];
    int ninfo = load_all_disk_info(info, MAX_DISKS);

    for (int i = 0; i < ninfo; i++) {
        char model[128];
        sysfs_model(info[i].name, model, sizeof(model));
        long long gb = sysfs_size_gb(info[i].name);

        char size_str[32];
        if (gb >= 1024) snprintf(size_str, sizeof(size_str), "%.1fT", gb / 1024.0);
        else snprintf(size_str, sizeof(size_str), "%lldG", gb);

        printf("%-12s %-8s %-28s %-12s %-8s %-8s%s%s\n",
               info[i].name, "disk", model, info[i].tran, size_str, "-",
               info[i].is_root ? " [ROOT]" : "",
               info[i].is_mounted ? " [MOUNTED]" : "");
    }
    printf("\n[ROOT] = System disk, cannot be carved with dm-linear\n");
    printf("[MOUNTED] = Mounted partition, cannot be carved with dm-linear\n");
    return 0;
}

static int cmd_bench(int argc, char *argv[]) {
    char *disk_list = NULL;
    int sequential = 0, warmup = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--disks") == 0 && i + 1 < argc) {
            disk_list = argv[++i];
        } else if (strcmp(argv[i], "--sequential") == 0) {
            sequential = 1;
        } else if (strcmp(argv[i], "--warmup") == 0) {
            warmup = 1;
        }
    }

    if (!disk_list) {
        fprintf(stderr, "Usage: tiered_setup --bench --disks sda,sdb,sdc [--sequential] [--warmup]\n");
        return 1;
    }

    char disks[MAX_DISKS][32];
    int nd = 0;
    char buf[1024];
    strncpy(buf, disk_list, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *tok = strtok(buf, ",");
    while (tok && nd < MAX_DISKS) {
        if (!tiered_is_valid_name(tok)) {
            fprintf(stderr, "Error: invalid disk name '%s'\n", tok);
            return 1;
        }
        strncpy(disks[nd], tok, 31);
        disks[nd][31] = 0;
        nd++;
        tok = strtok(NULL, ",");
    }

    if (nd == 0) {
        fprintf(stderr, "Error: no valid disks\n");
        return 1;
    }

    disk_t info[MAX_DISKS];
    for (int i = 0; i < nd; i++) {
        memset(&info[i], 0, sizeof(disk_t));
        strncpy(info[i].disk, disks[i], 31);
        sysfs_model(disks[i], info[i].model, sizeof(info[i].model));
        info[i].size_gb = sysfs_size_gb(disks[i]);
    }

    disk_info_t dinfo[MAX_DISKS];
    int ninfo = load_all_disk_info(dinfo, MAX_DISKS);
    for (int i = 0; i < nd; i++) {
        for (int j = 0; j < ninfo; j++) {
            if (strcmp(disks[i], dinfo[j].name) == 0) {
                strncpy(info[i].tran, dinfo[j].tran, sizeof(info[i].tran) - 1);
                break;
            }
        }
    }

    if (sequential || nd <= 1) {
        printf("Benchmarking %d disk(s) sequentially...\n", nd);
        for (int i = 0; i < nd; i++) {
            printf("  Testing /dev/%s ... ", info[i].disk);
            fflush(stdout);
            if (bench_disk(info[i].disk, &info[i].speed_write, &info[i].speed_read, warmup) == 0) {
                printf("Write: %.0f MB/s  Read: %.0f MB/s\n", info[i].speed_write, info[i].speed_read);
            } else {
                printf("FAILED\n");
            }
        }
    } else {
        printf("Benchmarking %d disks in parallel...\n", nd);

        if (run_parallel_bench(info, nd, warmup, NULL, NULL) == 1)
            return 1;
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
    {
        char umount_lv[256];
        snprintf(umount_lv, sizeof(umount_lv), "/dev/mapper/tv_vg_%s-tv_lv_%s", name, name);
        char *umount_argv[] = {"sudo", "umount", umount_lv, NULL};
        (void)run_quiet("sudo", umount_argv);
    }
    {
        char full_lv[256];
        snprintf(full_lv, sizeof(full_lv), "tv_vg_%s/tv_lv_%s", name, name);
        char *const lv_argv[] = {"lvremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", full_lv, NULL};
        (void)safe_execvp("lvremove", lv_argv);
    }
    {
        char vg_name[128];
        snprintf(vg_name, sizeof(vg_name), "tv_vg_%s", name);
        char *const vg_argv2[] = {"vgremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", vg_name, NULL};
        (void)safe_execvp("vgremove", vg_argv2);
    }
    for (int i = 0; i < valid_disks; i++) {
        char target[64];
        make_target(target, sizeof(target), valid[i].disk);
        {
            char devpath[128];
            snprintf(devpath, sizeof(devpath), "/dev/mapper/%s", target);
            char *const pv_argv[] = {"pvremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-ff", "-y", devpath, NULL};
            (void)safe_execvp("pvremove", pv_argv);
        }
        {
            char *const dm_argv[] = {"sudo", "dmsetup", "remove", target, NULL};
            (void)run_sudo_argv(dm_argv);
        }
    }
    fprintf(stderr, "  Rollback complete.\n");
}

static int cmd_create(int argc, char *argv[]) {
    char *name = NULL;
    char *disk_spec = NULL;
    char *fs = "ext4";
    char *mount_point = NULL;
    int stripe_size_kb = 512;
    int user_stripesize = 0;
    int use_scheduler = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) name = argv[++i];
        else if (strcmp(argv[i], "--disks") == 0 && i + 1 < argc) disk_spec = argv[++i];
        else if (strcmp(argv[i], "--fs") == 0 && i + 1 < argc) fs = argv[++i];
        else if (strcmp(argv[i], "--mount") == 0 && i + 1 < argc) mount_point = argv[++i];
        else if (strcmp(argv[i], "--scheduler") == 0) use_scheduler = 1;
        else if (strcmp(argv[i], "--stripesize") == 0 && i + 1 < argc) {
            char *endptr;
            long val = strtol(argv[++i], &endptr, 10);
            if (*endptr != '\0' || endptr == argv[i] || val <= 0 || val > 1048576) {
                fprintf(stderr, "Error: invalid stripesize '%s'\n", argv[i]);
                return 1;
            }
            stripe_size_kb = (int)val;
            user_stripesize = 1;
        }
    }

    if (!name || !disk_spec) {
        fprintf(stderr, "Usage: tiered_setup --create --name NAME --disks sdb:300,sdc:200 [--fs ext4] [--mount /mnt/fast]\n");
        return 1;
    }

    if (!tiered_is_valid_name(name)) {
        fprintf(stderr, "Error: invalid name '%s' (only a-z, A-Z, 0-9, ., _, - allowed)\n", name);
        return 1;
    }

    if (!tiered_is_valid_fs(fs)) {
        fprintf(stderr, "Error: invalid filesystem '%s' (ext4/ext3/xfs/btrfs/none)\n", fs);
        return 1;
    }

    if (mount_point && !tiered_is_valid_mount(mount_point)) {
        fprintf(stderr, "Error: mount point invalid (only / a-z 0-9 . _ - allowed)\n");
        return 1;
    }

    if (use_scheduler && (mount_point || strcmp(fs, "ext4") != 0)) {
        fprintf(stderr, "Warning: --fs and --mount are ignored with --scheduler\n");
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
            char *endptr;
            long long val = strtoll(colon + 1, &endptr, 10);
            if (*endptr != '\0' || endptr == colon + 1 || val <= 0) {
                fprintf(stderr, "Error: invalid carve size '%s'\n", colon + 1);
                return 1;
            }
            disks_arr[nd].carve_gb = val;
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

    disk_info_t dinfo[MAX_DISKS];
    int ninfo = load_all_disk_info(dinfo, MAX_DISKS);

    int valid_disks = 0;
    disk_t valid[MAX_DISKS];

    for (int i = 0; i < nd; i++) {
        sysfs_model(disks_arr[i].disk, disks_arr[i].model, sizeof(disks_arr[i].model));
        disks_arr[i].size_gb = sysfs_size_gb(disks_arr[i].disk);

        disks_arr[i].is_root = 0;
        disks_arr[i].is_mounted = 0;
        for (int j = 0; j < ninfo; j++) {
            if (strcmp(disks_arr[i].disk, dinfo[j].name) == 0) {
                strncpy(disks_arr[i].tran, dinfo[j].tran, sizeof(disks_arr[i].tran) - 1);
                disks_arr[i].is_root = dinfo[j].is_root;
                disks_arr[i].is_mounted = dinfo[j].is_mounted;
                break;
            }
        }

        if (disks_arr[i].is_root) {
            printf("  WARNING: /dev/%s is system disk, skipping (cannot carve)\n", disks_arr[i].disk);
            continue;
        }

        if (disks_arr[i].is_mounted) {
            printf("  WARNING: /dev/%s is mounted, skipping (cannot carve)\n", disks_arr[i].disk);
            continue;
        }

        {
            char target[64];
            make_target(target, sizeof(target), disks_arr[i].disk);
            char devpath[128];
            snprintf(devpath, sizeof(devpath), "/dev/mapper/%s", target);
            struct stat st;
            if (stat(devpath, &st) == 0) {
                fprintf(stderr, "Error: /dev/%s is already carved as %s.\n"
                        "       Please remove the existing volume first:\n"
                        "         sudo tiered_setup --remove --name <volume_name>\n"
                        "       WARNING: Removing the volume will destroy all data on it.\n"
                        "       Please back up your data before proceeding.\n",
                        disks_arr[i].disk, target);
                return 1;
            }
        }

        if (disks_arr[i].size_gb <= 1) {
            fprintf(stderr, "Error: /dev/%s size not detected or too small\n", disks_arr[i].disk);
            return 1;
        }

        if (disks_arr[i].carve_gb <= 0) {
            disks_arr[i].carve_gb = disks_arr[i].size_gb - 1;
        }
        if (disks_arr[i].carve_gb > disks_arr[i].size_gb - 1) {
            fprintf(stderr, "Error: /dev/%s has %lldGB, cannot carve %lldGB\n",
                    disks_arr[i].disk, disks_arr[i].size_gb, disks_arr[i].carve_gb);
            return 1;
        }

        valid[valid_disks++] = disks_arr[i];
    }

    if (valid_disks == 0) {
        fprintf(stderr, "Error: no usable disks (all are system disks)\n");
        return 1;
    }

    printf("  Benchmarking %d disks in parallel...\n", valid_disks);
    if (run_parallel_bench(valid, valid_disks, 1, NULL, NULL) == 1) {
        cleanup_create(name, valid, valid_disks);
        return 1;
    }

    qsort(valid, valid_disks, sizeof(disk_t), cmp_speed);

    long long total_gb = 0;
    for (int i = 0; i < valid_disks; i++) total_gb += valid[i].carve_gb;

    if (!user_stripesize) {
        int has_sata = 0;
        for (int i = 0; i < valid_disks; i++) {
            if (strcmp(valid[i].tran, "sata") == 0 || strcmp(valid[i].tran, "sas") == 0) { has_sata = 1; break; }
        }
        stripe_size_kb = has_sata ? 512 : 64;
    }

    printf("\nConfiguration:\n");
    printf("  Name: %s\n", name);
    printf("  Disks: %d (usable)\n", valid_disks);
    printf("  Total: %lldGB\n", total_gb);
    printf("  Filesystem: %s\n", fs);
    printf("  Stripesize: %dKB\n", stripe_size_kb);
    if (mount_point) printf("  Mount: %s\n", mount_point);
    printf("\n");

    double total_speed = 0;
    printf("  %-12s %-10s %-10s %-8s %-10s %-10s\n", "DEVICE", "CARVE", "REMAIN", "SPEED", "TIER", "RATIO");
    printf("  %-12s %-10s %-10s %-8s %-10s %-10s\n", "------------", "----------", "----------", "--------", "----------", "----------");
    for (int i = 0; i < valid_disks; i++) total_speed += valid[i].speed_write;
    for (int i = 0; i < valid_disks; i++) {
        double ratio = (total_speed > 0) ? valid[i].speed_write / total_speed * 100 : 0;
        long long remain = valid[i].size_gb - valid[i].carve_gb;
        printf("  %-12s %-8lldGB %-8lldGB %-8.0f %-10s %-10.1f%%\n",
               valid[i].disk, valid[i].carve_gb, remain, valid[i].speed_write,
               (i == 0) ? "FAST" : (i == valid_disks - 1) ? "SLOW" : "MED",
               ratio);
    }
    printf("  Total theoretical speed: %.0f MB/s\n", total_speed);
    printf("  Estimated actual: ~%.0f MB/s\n", total_speed * 0.94);
    printf("\n");

    printf("  ================================================================\n");
    printf("  WARNING: The following disks will have their beginning sectors\n");
    printf("           overwritten. Any data on the carved portion will be\n");
    printf("           PERMANENTLY LOST.\n");
    printf("  ================================================================\n");
    for (int i = 0; i < valid_disks; i++) {
        printf("    /dev/%s  — carving %lldGB of %lldGB\n",
               valid[i].disk, valid[i].carve_gb, valid[i].size_gb);
    }
    printf("\n  Please back up your data before proceeding.\n");
    printf("  Type YES to confirm (anything else to abort): ");
    fflush(stdout);
    char confirm[16] = "";
    if (!fgets(confirm, sizeof(confirm), stdin) || strncmp(confirm, "YES", 3) != 0) {
        fprintf(stderr, "\nAborted by user.\n");
        return 1;
    }
    printf("\n");

    printf("Step 1: Cleaning up old targets...\n");
    for (int i = 0; i < valid_disks; i++) {
        char mp[512];
        find_mount_for_disk(valid[i].disk, mp, sizeof(mp));
        if (mp[0]) {
            char *umount_argv[] = {"sudo", "umount", mp, NULL};
            (void)run_sudo_argv(umount_argv);
        }
        char target[64];
        make_target(target, sizeof(target), valid[i].disk);
        {
            char *const dm_argv[] = {"sudo", "dmsetup", "remove", target, NULL};
            (void)run_sudo_quiet(dm_argv);
        }
    }
    {
        char full_lv[256];
        snprintf(full_lv, sizeof(full_lv), "tv_vg_%s/tv_lv_%s", name, name);
        char *const lv_argv[] = {"lvremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", full_lv, NULL};
        (void)run_quiet("lvremove", lv_argv);
    }
    {
        char vg_name[128];
        snprintf(vg_name, sizeof(vg_name), "tv_vg_%s", name);
        char *const vg_argv[] = {"vgremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", vg_name, NULL};
        (void)run_quiet("vgremove", vg_argv);
    }
    for (int i = 0; i < valid_disks; i++) {
        char target[64];
        make_target(target, sizeof(target), valid[i].disk);
        char devpath[128];
        snprintf(devpath, sizeof(devpath), "/dev/mapper/%s", target);
        char *const pv_argv[] = {"pvremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-ff", "-y", devpath, NULL};
        (void)run_quiet("pvremove", pv_argv);
    }

    printf("Step 2: Creating carved targets...\n");
    for (int i = 0; i < valid_disks; i++) {
        long long sectors = valid[i].carve_gb * 1024LL * 1024 * 1024 / 512;
        char target[64];
        make_target(target, sizeof(target), valid[i].disk);

        char table[128];
        int tlen = snprintf(table, sizeof(table), "0 %lld linear /dev/%s 0\n", sectors, valid[i].disk);
        if (tlen < 0 || tlen >= (int)sizeof(table)) {
            fprintf(stderr, "Error: dm table too long for %s\n", valid[i].disk);
            cleanup_create(name, valid, i);
            return 1;
        }
        int pfd[2];
        if (pipe(pfd) < 0) { fprintf(stderr, "Error: pipe failed\n"); cleanup_create(name, valid, i); return 1; }
        pid_t pid = fork();
        if (pid < 0) {
            close(pfd[0]); close(pfd[1]);
            fprintf(stderr, "Error: fork failed\n");
            cleanup_create(name, valid, i);
            return 1;
        }
        if (pid == 0) {
            close(pfd[1]);
            dup2(pfd[0], STDIN_FILENO);
            close(pfd[0]);
            char *dm_argv[] = {"sudo", "dmsetup", "create", target, NULL};
            execvp("sudo", dm_argv);
            _exit(127);
        }
        close(pfd[0]);
        {
            const char *wp = table;
            size_t remaining = tlen;
            while (remaining > 0) {
                ssize_t nw = write(pfd[1], wp, remaining);
                if (nw <= 0) break;
                wp += nw;
                remaining -= nw;
            }
            close(pfd[1]);
            if (remaining > 0) {
                fprintf(stderr, "Error: failed to write dm table for %s\n", valid[i].disk);
                cleanup_create(name, valid, i);
                return 1;
            }
        }
        int status;
        waitpid(pid, &status, 0);

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

    if (use_scheduler) {
        printf("Step 3: Building weighted segments...\n");

        TV_DISK tv_disks[TV_MAX_DISKS];
        int n_tv_disks = valid_disks;
        for (int i = 0; i < valid_disks; i++) {
            memset(&tv_disks[i], 0, sizeof(TV_DISK));
            tv_disks[i].id = i;
            strncpy(tv_disks[i].name, valid[i].disk, 63);
            tv_disks[i].free_size = (uint64_t)valid[i].carve_gb * 1024LL * 1024 * 1024;
            tv_disks[i].speed = (uint64_t)valid[i].speed_write;
            tv_disks[i].weight = 0;
            tv_disks[i].physical_offset = 0;

            char devpath[128];
            snprintf(devpath, sizeof(devpath), "/dev/mapper/tv_%s_carve", valid[i].disk);
            tv_disks[i].fd = open(devpath, O_RDWR);
            if (tv_disks[i].fd < 0) {
                fprintf(stderr, "Error: cannot open %s: %s\n", devpath, strerror(errno));
                for (int j = 0; j < i; j++) close(tv_disks[j].fd);
                cleanup_create(name, valid, valid_disks);
                return 1;
            }
        }

        TV_METADATA meta;
        memset(&meta, 0, sizeof(meta));
        meta.version = 1;
        meta.chunk_size = TV_CHUNK_SIZE;
        meta.disk_count = (uint32_t)n_tv_disks;
        for (int i = 0; i < n_tv_disks; i++) {
            strncpy(meta.disk_names[i], tv_disks[i].name, 63);
        }

        TV_SEGMENT segs[TV_MAX_SEGS];
        int nsegs = 0;
        if (tv_build_segments(tv_disks, n_tv_disks, segs, &nsegs) < 0) {
            fprintf(stderr, "Error: failed to build segments\n");
            for (int i = 0; i < n_tv_disks; i++) close(tv_disks[i].fd);
            cleanup_create(name, valid, valid_disks);
            return 1;
        }

        meta.segment_count = (uint32_t)nsegs;
        memcpy(meta.segments, segs, sizeof(TV_SEGMENT) * nsegs);

        printf("  Segments: %d\n", nsegs);
        for (int i = 0; i < nsegs; i++) {
            printf("  Segment %d: %lu - %lu (%u disks, stripe=%luKB)\n",
                   i, (unsigned long)segs[i].logical_begin,
                   (unsigned long)segs[i].logical_end,
                   segs[i].disk_count,
                   (unsigned long)(segs[i].stripe_size / 1024));
        }

        char config_dir[] = "/etc/tieredvol";
        mkdir(config_dir, 0755);

        char config_path[256];
        snprintf(config_path, sizeof(config_path), "/etc/tieredvol/%s.scheduler", name);

        if (tv_metadata_save(&meta, config_path) < 0) {
            fprintf(stderr, "Error: failed to save metadata to %s\n", config_path);
            for (int i = 0; i < n_tv_disks; i++) close(tv_disks[i].fd);
            cleanup_create(name, valid, valid_disks);
            return 1;
        }
        printf("  Metadata saved to %s\n", config_path);

        for (int i = 0; i < n_tv_disks; i++) close(tv_disks[i].fd);

        printf("\n=== Weighted I/O Scheduler volume '%s' created ===\n", name);
        printf("  Use tiered_io to manage this volume.\n");
        printf("  NOTE: This volume uses the weighted I/O scheduler, not LVM striping.\n");
        return 0;
    }

    printf("Step 3: Creating LVM physical volumes...\n");
    for (int i = 0; i < valid_disks; i++) {
        char target[64];
        make_target(target, sizeof(target), valid[i].disk);
        char devpath[128];
        snprintf(devpath, sizeof(devpath), "/dev/mapper/%s", target);
        char *const pvcreate_argv[] = {"pvcreate", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", devpath, NULL};
        if (safe_execvp("pvcreate", pvcreate_argv) != 0) {
            fprintf(stderr, "Error: pvcreate failed for %s\n", target);
            cleanup_create(name, valid, valid_disks);
            return 1;
        }
        printf("  PV: %s\n", devpath);
    }

    printf("Step 4: Creating volume group...\n");
    {
        char vg_name[128];
        snprintf(vg_name, sizeof(vg_name), "tv_vg_%s", name);
        int argc_vg = 0;
        char *args[16];
        args[argc_vg++] = "vgcreate";
        args[argc_vg++] = "--config";
        args[argc_vg++] = "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}";
        args[argc_vg++] = "-f";
        args[argc_vg++] = vg_name;
        for (int i = 0; i < valid_disks; i++) {
            char target[64];
            make_target(target, sizeof(target), valid[i].disk);
            char devpath[128];
            snprintf(devpath, sizeof(devpath), "/dev/mapper/%s", target);
            args[argc_vg] = strdup(devpath);
            if (!args[argc_vg]) {
                fprintf(stderr, "Error: out of memory\n");
                for (int j = 5; j < argc_vg; j++) free(args[j]);
                return 1;
            }
            argc_vg++;
        }
        args[argc_vg] = NULL;
        int ret = safe_execvp("vgcreate", args);
        for (int i = 5; i < argc_vg; i++) free(args[i]);
        if (ret != 0) {
            fprintf(stderr, "Error: vgcreate failed for tv_vg_%s\n", name);
            cleanup_create(name, valid, valid_disks);
            return 1;
        }
    }
    printf("  VG: tv_vg_%s\n", name);

    printf("Step 5: Creating striped logical volume...\n");
    {
        char vg_name[128], lv_name[128], stripes_str[16], stripe_str[16];
        snprintf(vg_name, sizeof(vg_name), "tv_vg_%s", name);
        snprintf(lv_name, sizeof(lv_name), "tv_lv_%s", name);
        snprintf(stripes_str, sizeof(stripes_str), "%d", valid_disks);
        snprintf(stripe_str, sizeof(stripe_str), "%dk", stripe_size_kb);
        char free_arg[] = "100%FREE";
        char *const lv_argv[] = {"lvcreate", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-l", free_arg, "-i", stripes_str, "-I", stripe_str, "-n", lv_name, vg_name, NULL};
        if (safe_execvp("lvcreate", lv_argv) != 0) {
            fprintf(stderr, "Error: lvcreate failed\n");
            cleanup_create(name, valid, valid_disks);
            return 1;
        }
    }
    printf("  LV: /dev/mapper/tv_vg_%s-tv_lv_%s (%d stripes, %dKB stripesize)\n",
           name, name, valid_disks, stripe_size_kb);

    char lv_path[256];
    snprintf(lv_path, sizeof(lv_path), "/dev/mapper/tv_vg_%s-tv_lv_%s", name, name);

    if (strcmp(fs, "none") != 0) {
        printf("Step 6: Formatting as %s...\n", fs);
        char mkfs_name[64];
        snprintf(mkfs_name, sizeof(mkfs_name), "mkfs.%s", fs);
        char *const mkfs_argv[] = {mkfs_name, lv_path, NULL};
        if (safe_execvp(mkfs_name, mkfs_argv) != 0) {
            fprintf(stderr, "Error: mkfs.%s failed\n", fs);
            cleanup_create(name, valid, valid_disks);
            return 1;
        }
    } else {
        printf("Step 6: Skipped formatting (raw)\n");
    }

    if (mount_point && strcmp(fs, "none") != 0) {
        printf("Step 7: Mounting...\n");
        char *mkdir_argv[] = {"sudo", "mkdir", "-p", mount_point, NULL};
        (void)run_sudo_argv(mkdir_argv);
        char *mount_argv[] = {"sudo", "mount", lv_path, mount_point, NULL};
        if (run_sudo_argv(mount_argv) != 0) {
            fprintf(stderr, "Error: mount failed\n");
            cleanup_create(name, valid, valid_disks);
            return 1;
        }
        printf("  Mounted at %s\n", mount_point);
    } else {
        printf("Step 7: Skipped mounting\n");
    }

    printf("\nSaving config...\n");
    {
        char conf_path[] = "/tmp/.tv_conf_XXXXXX";
        int conf_fd = mkstemp(conf_path);
        if (conf_fd >= 0) {
            FILE *cf = fdopen(conf_fd, "w");
            if (cf) {
                fprintf(cf, "[general]\nname=%s\ncount=%d\nfs=%s\nstripesize=%d\n", name, valid_disks, fs, stripe_size_kb);
                if (mount_point) fprintf(cf, "mount=%s\n", mount_point);
                fprintf(cf, "total_gb=%lld\n\n", total_gb);
                for (int i = 0; i < valid_disks; i++) {
                    fprintf(cf, "[disk.%d]\ndevice=%s\nsize_gb=%lld\nspeed=%.0f\n\n",
                            i, valid[i].disk, valid[i].carve_gb, valid[i].speed_write);
                }
                fclose(cf);
                char *mkdir_argv[] = {"sudo", "mkdir", "-p", "/etc/tieredvol", NULL};
                (void)run_sudo_argv(mkdir_argv);
                char dest[512];
                snprintf(dest, sizeof(dest), "/etc/tieredvol/%s.conf", name);
                char *mv_argv[] = {"sudo", "mv", "-f", conf_path, dest, NULL};
                int mv_ret = run_sudo_argv(mv_argv);
                if (mv_ret != 0) {
                    fprintf(stderr, "Warning: failed to save config\n");
                    unlink(conf_path);
                }
            } else {
                close(conf_fd);
                unlink(conf_path);
            }
        }
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

    if (!tiered_is_valid_name(name)) {
        fprintf(stderr, "Error: invalid name '%s'\n", name);
        return 1;
    }

    printf("=== TieredVol: Removing '%s' ===\n", name);

    /* Check for scheduler volume first */
    char sched_path[256];
    snprintf(sched_path, sizeof(sched_path), "/etc/tieredvol/%s.scheduler", name);
    FILE *sf = fopen(sched_path, "r");
    if (sf) {
        fclose(sf);
        printf("  Detected weighted I/O scheduler volume\n");

        /* Read disk names from .scheduler metadata */
        char targets[MAX_DISKS][64];
        int ntargets = 0;

        TV_METADATA sched_meta;
        memset(&sched_meta, 0, sizeof(sched_meta));
        if (tv_metadata_load(&sched_meta, sched_path) == 0) {
            for (uint32_t i = 0; i < sched_meta.disk_count && ntargets < MAX_DISKS; i++) {
                make_target(targets[ntargets], sizeof(targets[0]), sched_meta.disk_names[i]);
                ntargets++;
            }
        }

        /* Remove dm-linear targets (retry up to 3 times for kernel release) */
        printf("Removing dm-linear targets...\n");
        if (ntargets == 0) {
            fprintf(stderr, "  Warning: could not read metadata, attempting to find targets by name pattern\n");
            FILE *p = popen("sudo dmsetup ls 2>/dev/null", "r");
            if (p) {
                char line[256];
                while (fgets(line, sizeof(line), p) && ntargets < MAX_DISKS) {
                    line[strcspn(line, "\n")] = 0;
                    char *tok = strtok(line, "\t ");
                    if (!tok) continue;
                    if (strncmp(tok, "tv_", 3) == 0 && strstr(tok, "_carve")) {
                        snprintf(targets[ntargets], sizeof(targets[0]), "%s", tok);
                        ntargets++;
                    }
                }
                pclose(p);
            }
        }
        for (int i = 0; i < ntargets; i++) {
            int removed = 0;
            for (int retry = 0; retry < 3; retry++) {
                char *const dm_argv[] = {"sudo", "dmsetup", "remove", targets[i], NULL};
                if (run_sudo_argv(dm_argv) == 0) {
                    removed = 1;
                    break;
                }
                if (retry < 2) {
                    fprintf(stderr, "  %s busy, retrying in 1s...\n", targets[i]);
                    sleep(1);
                }
            }
            if (removed)
                printf("  Removed %s\n", targets[i]);
            else
                fprintf(stderr, "  Failed to remove %s after retries\n", targets[i]);
        }

        /* Remove .scheduler and .conf metadata */
        printf("Removing scheduler metadata...\n");
        {
            char *rm_argv[] = {"sudo", "rm", "-f", sched_path, NULL};
            (void)run_sudo_argv(rm_argv);
        }
        {
            char conf_path_cleanup[256];
            snprintf(conf_path_cleanup, sizeof(conf_path_cleanup), "/etc/tieredvol/%s.conf", name);
            char *rm_argv[] = {"sudo", "rm", "-f", conf_path_cleanup, NULL};
            (void)run_sudo_argv(rm_argv);
        }
        {
            char *rmdir_argv[] = {"sudo", "rmdir", "/etc/tieredvol", NULL};
            (void)run_sudo_argv(rmdir_argv);
        }

        printf("\n=== Remove Complete ===\n");
        return 0;
    }

    /* LVM volume path */

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
                make_target(targets[ntargets], sizeof(targets[0]), disk);
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
            int orphan_count = 0;
            while (fgets(line, sizeof(line), p) && ntargets < MAX_DISKS) {
                line[strcspn(line, "\n")] = 0;
                char *tok = strtok(line, "\t ");
                if (!tok) continue;
                if (strncmp(tok, "tv_", 3) == 0 && strstr(tok, "_carve")) {
                    snprintf(targets[ntargets], sizeof(targets[0]), "%s", tok);
                    ntargets++;
                    orphan_count++;
                }
            }
            pclose(p);
            if (orphan_count > 0) {
                fprintf(stderr, "  ERROR: Found %d orphan dm targets matching 'tv_*_carve'.\n", orphan_count);
                for (int i = 0; i < ntargets; i++)
                    fprintf(stderr, "    - %s\n", targets[i]);
                fprintf(stderr, "  Cannot determine which belong to '%s' without config file.\n", name);
                fprintf(stderr, "  Please recreate /etc/tieredvol/%s.conf or remove them manually.\n", name);
                return 1;
            }
        }
    }

    printf("Unmounting...\n");
    {
        char lv_path[256];
        snprintf(lv_path, sizeof(lv_path), "/dev/mapper/tv_vg_%s-tv_lv_%s", name, name);
        char *umount_argv[] = {"sudo", "umount", lv_path, NULL};
        (void)run_sudo_argv(umount_argv);
    }

    printf("Removing LV...\n");
    {
        char full_lv[256];
        snprintf(full_lv, sizeof(full_lv), "tv_vg_%s/tv_lv_%s", name, name);
        char *const lv_argv[] = {"lvremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", full_lv, NULL};
        (void)safe_execvp("lvremove", lv_argv);
    }

    printf("Removing VG...\n");
    {
        char vg_name[128];
        snprintf(vg_name, sizeof(vg_name), "tv_vg_%s", name);
        char *const vg_argv[] = {"vgremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", vg_name, NULL};
        (void)safe_execvp("vgremove", vg_argv);
    }

    printf("Removing PV and dm targets...\n");
    for (int i = 0; i < ntargets; i++) {
        char devpath[128];
        snprintf(devpath, sizeof(devpath), "/dev/mapper/%s", targets[i]);
        {
            char *const pv_argv[] = {"pvremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-ff", "-y", devpath, NULL};
            (void)safe_execvp("pvremove", pv_argv);
        }
        {
            char *const dm_argv[] = {"sudo", "dmsetup", "remove", targets[i], NULL};
            (void)run_sudo_argv(dm_argv);
        }
        printf("  Removed %s\n", targets[i]);
    }

    printf("Removing config...\n");
    {
        char *rm_argv[] = {"sudo", "rm", "-f", conf_path, NULL};
        (void)run_sudo_argv(rm_argv);
        char *rmdir_argv[] = {"sudo", "rmdir", "/etc/tieredvol", NULL};
        (void)run_sudo_argv(rmdir_argv);
    }

    printf("\n=== Remove Complete ===\n");
    return 0;
}

static int cmd_status(void) {
    printf("=== TieredVol Status ===\n\n");

    printf("DM Targets:\n");
    {
        DIR *d = opendir("/dev/mapper");
        if (d) {
            struct dirent *ent;
            int found = 0;
            while ((ent = readdir(d))) {
                if (strncmp(ent->d_name, "tv_", 3) == 0) {
                    printf("  /dev/mapper/%s\n", ent->d_name);
                    found = 1;
                }
            }
            closedir(d);
            if (!found) printf("  None\n");
        }
    }

    printf("\nSaved Configs:\n");
    {
        DIR *d = opendir("/etc/tieredvol");
        if (d) {
            struct dirent *ent;
            int found = 0;
            while ((ent = readdir(d))) {
                if (strstr(ent->d_name, ".conf") || strstr(ent->d_name, ".scheduler")) {
                    printf("  /etc/tieredvol/%s\n", ent->d_name);
                    found = 1;
                }
            }
            closedir(d);
            if (!found) printf("  None\n");
        } else {
            printf("  None\n");
        }
    }

    return 0;
}

int main(int argc, char *argv[]) {
    if (geteuid() != 0) {
        fprintf(stderr, "Error: tiered_setup requires root privileges.\n");
        fprintf(stderr, "Please run: sudo %s\n", argv[0]);
        return 1;
    }

    const char *deps[] = {"dmsetup", "vgcreate", "pvcreate", NULL};
    for (int i = 0; deps[i]; i++) {
        char path_buf[256];
        const char *path_env = getenv("PATH");
        if (!path_env) path_env = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
        int found = 0;
        char *path_copy = strdup(path_env);
        if (path_copy) {
            char *saveptr = NULL;
            char *dir = strtok_r(path_copy, ":", &saveptr);
            while (dir) {
                snprintf(path_buf, sizeof(path_buf), "%s/%s", dir, deps[i]);
                if (access(path_buf, X_OK) == 0) { found = 1; break; }
                dir = strtok_r(NULL, ":", &saveptr);
            }
            free(path_copy);
        }
        if (!found) {
            fprintf(stderr, "Error: '%s' not found. Install lvm2: apt install lvm2\n", deps[i]);
            return 1;
        }
    }

    if (argc < 2) {
        print_usage(argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "--list") == 0) return cmd_list();
    if (strcmp(argv[1], "--bench") == 0) return cmd_bench(argc, argv);
    if (strcmp(argv[1], "--create") == 0) return cmd_create(argc, argv);
    if (strcmp(argv[1], "--remove") == 0 || strcmp(argv[1], "--destroy") == 0) return cmd_remove(argc, argv);
    if (strcmp(argv[1], "--status") == 0) return cmd_status();
    if (strcmp(argv[1], "--version") == 0) { printf("TieredVol %s\n", VERSION); return 0; }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    return 1;
}
