#define _GNU_SOURCE
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <limits.h>
#include <fcntl.h>
#include <time.h>
#include "tiered_common.h"
#include "tiered_ui_helpers.h"
#include "version.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wstringop-truncation"

#define MAX_DISKS 8
#define MIN_COLS 80
#define MIN_ROWS 20

static ui_disk_t disks[MAX_DISKS];
static int ndisks = 0;
static char status_msg[256] = "";
static char tool_path[PATH_MAX] = "";
static char vol_name[128] = "";
static char mount_point[256] = "";

static pid_t bench_pid = -1;
static int bench_rd_fd = -1;
static char bench_buf[16384] = "";
static int bench_buf_len = 0;
static int bench_finished = 0;
static time_t bench_start_time = 0;

static double saved_speed_w[MAX_DISKS], saved_speed_r[MAX_DISKS];

static int system_dirty_ratio = -1;
static int system_dirty_bg_ratio = -1;

static void detect_tool_path() {
    char *base = getenv("TIERED_VOL_DIR");
    if (base && base[0]) {
        snprintf(tool_path, sizeof(tool_path), "%s/tiered_setup", base);
        return;
    }
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = 0;
        char *last_slash = strrchr(exe_path, '/');
        if (last_slash) {
            *last_slash = 0;
            snprintf(tool_path, sizeof(tool_path), "%s/tiered_setup", exe_path);
            return;
        }
    }
    snprintf(tool_path, sizeof(tool_path), "tiered_setup");
}

static int run_cmd(const char *cmd, char *out, int outsize);

static void detect_existing_volume() {
    char dm_out[2048] = "";
    run_cmd("sudo dmsetup ls 2>/dev/null", dm_out, sizeof(dm_out));
    char *line = strtok(dm_out, "\n");
    while (line) {
        if (strstr(line, "tv_")) {
            char vg[128] = "";
            if (sscanf(line, "%127s", vg) == 1) {
                char *lv_sep = strstr(vg, "-tv_lv_");
                if (lv_sep) {
                    *lv_sep = 0;
                    if (strncmp(vg, "tv_vg_", 6) == 0) {
                        strncpy(vol_name, vg + 6, sizeof(vol_name) - 1);
                    }
                }
            }
            break;
        }
        line = strtok(NULL, "\n");
    }
    if (vol_name[0]) {
        char mnt_out[2048] = "";
        run_cmd("mount | grep /dev/mapper/tv_", mnt_out, sizeof(mnt_out));
        char *mline = strtok(mnt_out, "\n");
        while (mline) {
            char *on = strstr(mline, " on ");
            if (on) {
                on += 4;
                char *sp = strchr(on, ' ');
                if (sp) {
                    int len = sp - on;
                    if (len > 0 && len < (int)sizeof(mount_point)) {
                        strncpy(mount_point, on, len);
                        mount_point[len] = 0;
                    }
                }
            }
            mline = strtok(NULL, "\n");
        }
    }
}

static int run_cmd(const char *cmd, char *out, int outsize) {
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    if (out) {
        int n = fread(out, 1, outsize - 1, p);
        out[n] = 0;
    }
    int ret = pclose(p);
    if (ret == -1) return -1;
    return WIFEXITED(ret) ? WEXITSTATUS(ret) : -1;
}

static void parse_disk_list() {
    char out[4096] = "";
    char cmd[PATH_MAX + 256];
    snprintf(cmd, sizeof(cmd), "%s --list 2>/dev/null", tool_path);
    run_cmd(cmd, out, sizeof(out));
    ndisks = 0;
    char *line = strtok(out, "\n");
    while (line && ndisks < MAX_DISKS) {
        if (line[0] == '-' || line[0] == '[' || strlen(line) < 5) {
            line = strtok(NULL, "\n");
            continue;
        }
        ui_disk_t *d = &disks[ndisks];
        memset(d, 0, sizeof(*d));
        char model_buf[128] = "", tran_buf[16] = "", size_buf[32] = "";
        int n = sscanf(line, "%31s %7s %127[^0-9] %15s %31s",
                       d->disk, (char[8]){0}, model_buf, tran_buf, size_buf);
        if (n < 2) { line = strtok(NULL, "\n"); continue; }
        strncpy(d->model, model_buf, sizeof(d->model) - 1);
        strncpy(d->tran, tran_buf, sizeof(d->tran) - 1);
        if (size_buf[0]) {
            d->size_gb = atoll(size_buf);
            size_t slen = strlen(size_buf);
            if (slen > 0 && size_buf[slen - 1] == 'T') d->size_gb *= 1024;
        }
        if (strstr(line, "[ROOT]")) d->is_root = 1;
        ndisks++;
        line = strtok(NULL, "\n");
    }

    char mnt_out[4096] = "";
    run_cmd("awk '{print $1, $2}' /proc/mounts 2>/dev/null", mnt_out, sizeof(mnt_out));
    for (int i = 0; i < ndisks; i++) {
        if (disks[i].is_root) continue;
        char pattern[64];
        snprintf(pattern, sizeof(pattern), "/dev/%s ", disks[i].disk);
        if (strstr(mnt_out, pattern)) disks[i].is_mounted = 1;
    }
}

static void drain_pipe_to_buf(void) {
    char tmp[4096];
    ssize_t n;
    while ((n = read(bench_rd_fd, tmp, sizeof(tmp) - 1)) > 0) {
        tmp[n] = 0;
        if (bench_buf_len + (int)n >= (int)sizeof(bench_buf) - 1) {
            for (int i = 0; i < ndisks; i++) parse_bench_output(bench_buf, &disks[i]);
            bench_buf_len = 0;
            bench_buf[0] = 0;
        }
        memcpy(bench_buf + bench_buf_len, tmp, n);
        bench_buf_len += n;
        bench_buf[bench_buf_len] = 0;
    }
}

static void auto_bench_start() {
    char disklist[512] = "";
    for (int i = 0; i < ndisks; i++) {
        if (!disks[i].is_root) {
            if (disklist[0]) strcat(disklist, ",");
            strcat(disklist, disks[i].disk);
        }
    }
    if (!disklist[0]) { bench_finished = 1; return; }

    int pipefd[2];
    if (pipe(pipefd) < 0) { bench_finished = 1; return; }

    bench_pid = fork();
    if (bench_pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        bench_finished = 1;
        return;
    }
    if (bench_pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        char *bench_argv[] = {tool_path, "--bench", "--disks", disklist, NULL};
        execvp(tool_path, bench_argv);
        _exit(127);
    }
    close(pipefd[1]);
    bench_rd_fd = pipefd[0];
    fcntl(bench_rd_fd, F_SETFL, fcntl(bench_rd_fd, F_GETFL, 0) | O_NONBLOCK);
    bench_start_time = time(NULL);
}

static void auto_bench_poll() {
    if (bench_finished || bench_rd_fd < 0) return;

    drain_pipe_to_buf();

    int status;
    if (bench_pid > 0 && waitpid(bench_pid, &status, WNOHANG) > 0) {
        drain_pipe_to_buf();
        close(bench_rd_fd);
        bench_rd_fd = -1;
        bench_finished = 1;
        bench_pid = -1;

        for (int i = 0; i < ndisks; i++) {
            disks[i].speed_write = 0;
            disks[i].speed_read = 0;
            parse_bench_output(bench_buf, &disks[i]);
        }
    }
}

static void save_speeds(void) {
    for (int i = 0; i < ndisks; i++) {
        saved_speed_w[i] = disks[i].speed_write;
        saved_speed_r[i] = disks[i].speed_read;
    }
}

static void restore_speeds(void) {
    for (int i = 0; i < ndisks; i++) {
        disks[i].speed_write = saved_speed_w[i];
        disks[i].speed_read = saved_speed_r[i];
    }
}

static void draw_box(int y, int x, int h, int w, const char *title) {
    attron(A_BOLD);
    mvhline(y, x, ACS_HLINE, w);
    mvhline(y + h - 1, x, ACS_HLINE, w);
    mvvline(y, x, ACS_VLINE, h);
    mvvline(y, x + w - 1, ACS_VLINE, h);
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
    if (title) {
        char tbuf[256];
        snprintf(tbuf, sizeof(tbuf), "%.*s", w - 4, title);
        mvprintw(y, x + 2, " %s ", tbuf);
    }
    attroff(A_BOLD);
}

static void draw_status_bar(const char *msg, const char *hint) {
    int maxy = getmaxy(stdscr);
    int maxx = getmaxx(stdscr);
    attron(A_REVERSE);
    mvhline(maxy - 1, 0, ' ', maxx);
    if (msg) mvprintw(maxy - 1, 1, " %s", msg);
    if (hint) {
        int hint_x = maxx - (int)strlen(hint) - 3;
        if (hint_x < 1) hint_x = 1;
        mvprintw(maxy - 1, hint_x, " %s ", hint);
    }
    attroff(A_REVERSE);
}

static int check_terminal_size(void) {
    int maxy = getmaxy(stdscr);
    int maxx = getmaxx(stdscr);
    if (maxy < MIN_ROWS || maxx < MIN_COLS) {
        clear();
        int col = (maxx - 36) / 2;
        if (col < 0) col = 0;
        mvprintw(maxy / 2 - 1, col, "Terminal too small (%dx%d)", maxx, maxy);
        mvprintw(maxy / 2 + 1, col, "Minimum: %dx%d — please resize", MIN_COLS, MIN_ROWS);
        draw_status_bar("Resize terminal or press Q to exit", NULL);
        refresh();
        return 0;
    }
    return 1;
}

typedef struct {
    long long total_kb;
    long long available_kb;
    int original_dirty_ratio;
    int original_dirty_bg_ratio;
    int current_borrow_mb;
} ram_cache_t;

static ram_cache_t ram_cache = {0, 0, 20, 10, 0};

static void read_meminfo(ram_cache_t *rc) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %lld kB", &rc->total_kb) == 1) continue;
        if (sscanf(line, "MemAvailable: %lld kB", &rc->available_kb) == 1) continue;
    }
    fclose(f);
}

static int read_sysctl_int(const char *key) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "sysctl -n %s 2>/dev/null", key);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    int val = -1;
    if (fscanf(p, "%d", &val) != 1) val = -1;
    pclose(p);
    return val;
}

static int mb_to_dirty_ratio(long long total_kb, int mb) {
    if (total_kb <= 0) return 0;
    long long borrow_kb = (long long)mb * 1024;
    int ratio = (int)(borrow_kb * 100 / total_kb);
    if (ratio < 1) ratio = 1;
    if (ratio > 80) ratio = 80;
    return ratio;
}

static void screen_ram_cache() {
    if (!check_terminal_size()) return;
    read_meminfo(&ram_cache);
    if (system_dirty_ratio < 0) system_dirty_ratio = read_sysctl_int("vm.dirty_ratio");
    if (system_dirty_bg_ratio < 0) system_dirty_bg_ratio = read_sysctl_int("vm.dirty_background_ratio");
    ram_cache.original_dirty_ratio = system_dirty_ratio;
    ram_cache.original_dirty_bg_ratio = system_dirty_bg_ratio;
    if (ram_cache.original_dirty_ratio < 0) ram_cache.original_dirty_ratio = 20;
    if (ram_cache.original_dirty_bg_ratio < 0) ram_cache.original_dirty_bg_ratio = 10;
    int total_mb = ram_cache.total_kb / 1024;
    int avail_mb = ram_cache.available_kb / 1024;
    int max_borrow = avail_mb - 2048;
    if (max_borrow < 0) max_borrow = 0;
    if (max_borrow > total_mb / 2) max_borrow = total_mb / 2;
    int borrow_mb = ram_cache.current_borrow_mb;
    int step = 128;
    int sel = 0;
    int need_refresh = 1;
    while (1) {
        if (need_refresh) {
            clear();
            int maxy = getmaxy(stdscr);
            int maxx = getmaxx(stdscr);
            attron(A_BOLD | COLOR_PAIR(4));
            mvprintw(0, (maxx - 22) / 2, "=== RAM Cache ===");
            attroff(A_BOLD | COLOR_PAIR(4));
            int bw = 56;
            int bh = 18;
            int by = 2;
            if (by + bh > maxy - 1) bh = maxy - by - 1;
            int bx = (maxx - bw) / 2;
            draw_box(by, bx, bh, bw, "RAM Cache (128MB steps)");
            int y = by + 2;
            attron(A_BOLD);
            mvprintw(y, bx + 3, "System RAM:       %d GB", total_mb / 1024);
            y++;
            mvprintw(y, bx + 3, "Available:        %d GB", avail_mb / 1024);
            y++;
            mvprintw(y, bx + 3, "Max borrow:       %d GB", max_borrow / 1024);
            y += 2;
            int new_dirty = mb_to_dirty_ratio(ram_cache.total_kb, borrow_mb);
            int new_bg = new_dirty / 2;
            if (new_bg < 1) new_bg = 1;
            if (new_bg > 40) new_bg = 40;
            mvprintw(y, bx + 3, "Original dirty_ratio:  %d%%", ram_cache.original_dirty_ratio);
            y++;
            mvprintw(y, bx + 3, "Original dirty_bg:     %d%%", ram_cache.original_dirty_bg_ratio);
            y += 2;
            attron(COLOR_PAIR(4));
            mvprintw(y, bx + 3, "Borrow:    %d MB (%d x 128MB)", borrow_mb, borrow_mb / step);
            attroff(COLOR_PAIR(4));
            y++;
            mvprintw(y, bx + 3, "           %d GB", borrow_mb / 1024);
            y += 2;
            attron(A_BOLD);
            mvprintw(y, bx + 3, "New dirty_ratio:  %d%%", new_dirty);
            y++;
            mvprintw(y, bx + 3, "New dirty_bg:     %d%%", new_bg);
            attroff(A_BOLD);
            y++;
            int safe_mb = avail_mb - borrow_mb;
            if (safe_mb < 0) safe_mb = 0;
            attron(COLOR_PAIR(safe_mb < 2048 ? 3 : 1));
            mvprintw(y, bx + 3, "Safe remaining:   %d GB", safe_mb / 1024);
            attroff(COLOR_PAIR(safe_mb < 2048 ? 3 : 1));
            y++;
            y += 2;
            if (sel == 0) attron(A_REVERSE);
            mvprintw(y, bx + 8, " [Apply]  ");
            if (sel == 0) attroff(A_REVERSE);
            if (sel == 1) attron(A_REVERSE);
            mvprintw(y, bx + 20, " [Reset]  ");
            if (sel == 1) attroff(A_REVERSE);
            if (sel == 2) attron(A_REVERSE);
            mvprintw(y, bx + 32, " [Back]   ");
            if (sel == 2) attroff(A_REVERSE);
            mvprintw(y + 3, bx + 2, "%s",
                     sel == 0 ? "Left/Right:Adjust  Up/Down:Select" : "Up/Down:Select  Enter:OK");
            if (ram_cache.current_borrow_mb > 0) {
                attron(COLOR_PAIR(4));
                mvprintw(y + 5, bx + 2, "ACTIVE: %d MB borrowed", ram_cache.current_borrow_mb);
                attroff(COLOR_PAIR(4));
            }
            draw_status_bar("Left/Right:Adjust  Enter:Select  Q:Back", NULL);
            refresh();
            need_refresh = 0;
        }
        int ch = getch();
        switch (ch) {
            case KEY_LEFT:
                if (sel == 0) {
                    borrow_mb -= step;
                    if (borrow_mb < 0) borrow_mb = 0;
                    need_refresh = 1;
                }
                break;
            case KEY_RIGHT:
                if (sel == 0) {
                    borrow_mb += step;
                    if (borrow_mb > max_borrow) borrow_mb = max_borrow;
                    need_refresh = 1;
                }
                break;
            case KEY_UP:
                sel = (sel + 2) % 3;
                need_refresh = 1;
                break;
            case KEY_DOWN:
                sel = (sel + 1) % 3;
                need_refresh = 1;
                break;
            case '\n': case KEY_ENTER:
                if (sel == 0) {
                    int new_dirty = mb_to_dirty_ratio(ram_cache.total_kb, borrow_mb);
                    int new_bg = new_dirty / 2;
                    if (new_bg < 1) new_bg = 1;
                    if (new_bg > 40) new_bg = 40;
                    char cmd[256];
                    snprintf(cmd, sizeof(cmd), "sysctl -w vm.dirty_ratio=%d vm.dirty_background_ratio=%d",
                             new_dirty, new_bg);
                    (void)!system(cmd);
                    ram_cache.current_borrow_mb = borrow_mb;
                    snprintf(status_msg, sizeof(status_msg), "RAM cache: %d MB applied", borrow_mb);
                    need_refresh = 1;
                } else if (sel == 1) {
                    char cmd[256];
                    snprintf(cmd, sizeof(cmd), "sysctl -w vm.dirty_ratio=%d vm.dirty_background_ratio=%d",
                             ram_cache.original_dirty_ratio, ram_cache.original_dirty_bg_ratio);
                    (void)!system(cmd);
                    ram_cache.current_borrow_mb = 0;
                    borrow_mb = 0;
                    snprintf(status_msg, sizeof(status_msg), "RAM cache: reset to original");
                    need_refresh = 1;
                } else {
                    return;
                }
                break;
            case 'q': case 'Q': case 27:
                return;
            case KEY_RESIZE:
                need_refresh = 1;
                break;
        }
    }
}

static int screen_main() {
    if (!check_terminal_size()) return 6;
    const char *items[] = {
        "Disk List",
        "Benchmark",
        "Create Volume",
        "Volume Status",
        "RAM Cache",
        "Destroy Volume",
        "Exit"
    };
    int nitems = 7;
    int sel = 0;
    while (1) {
        clear();
        int maxx = getmaxx(stdscr);
        attron(A_BOLD | COLOR_PAIR(1));
        mvprintw(1, (maxx - 40) / 2, "========================================");
        mvprintw(2, (maxx - 34) / 2, "  TieredVol Storage Manager v%s", VERSION);
        mvprintw(3, (maxx - 40) / 2, "========================================");
        attroff(A_BOLD | COLOR_PAIR(1));
        int bw = 36;
        int bh = nitems + 4;
        int by = 5;
        int bx = (maxx - bw) / 2;
        draw_box(by, bx, bh, bw, "Main Menu");
        for (int i = 0; i < nitems; i++) {
            if (i == sel) {
                attron(A_REVERSE | A_BOLD);
                mvprintw(by + 2 + i, bx + 4, " > %-28s ", items[i]);
                attroff(A_REVERSE | A_BOLD);
            } else {
                mvprintw(by + 2 + i, bx + 4, "   %-28s ", items[i]);
            }
        }
        draw_status_bar(status_msg, "Q:Exit");
        refresh();
        int ch = getch();
        switch (ch) {
            case KEY_UP: sel = (sel - 1 + nitems) % nitems; break;
            case KEY_DOWN: sel = (sel + 1) % nitems; break;
            case '\n': case KEY_ENTER: return sel;
            case 'q': case 'Q': case 27: return 6;
            case KEY_RESIZE: break;
        }
    }
}

static void screen_disk_list() {
    if (!check_terminal_size()) return;
    if (ndisks == 0) { snprintf(status_msg, sizeof(status_msg), "No disks found!"); return; }
    auto_bench_poll();
    save_speeds();
    parse_disk_list();
    restore_speeds();
    while (1) {
        clear();
        int maxx = getmaxx(stdscr);
        attron(A_BOLD | COLOR_PAIR(2));
        mvprintw(0, (maxx - 16) / 2, "=== Disk List ===");
        attroff(A_BOLD | COLOR_PAIR(2));
        int bw = maxx - 4;
        int bh = ndisks + 6;
        int by = 2;
        draw_box(by, 1, bh, bw, "Disks");
        attron(A_BOLD);
        if (bench_finished) {
            mvprintw(by + 2, 3, "%-10s %-6s %-26s %-6s %-8s %8s %8s %s",
                     "DEVICE", "TYPE", "MODEL", "TRAN", "SIZE", "Write", "Read", "STATUS");
        } else {
            mvprintw(by + 2, 3, "%-10s %-6s %-26s %-6s %-8s %s",
                     "DEVICE", "TYPE", "MODEL", "TRAN", "SIZE", "STATUS");
        }
        mvhline(by + 3, 2, ACS_HLINE, bw - 2);
        attroff(A_BOLD);
        for (int i = 0; i < ndisks; i++) {
            ui_disk_t *d = &disks[i];
            if (d->is_root) attron(COLOR_PAIR(3));
            else if (d->is_mounted) attron(COLOR_PAIR(4));
            const char *tag = d->is_root ? " [ROOT]" : d->is_mounted ? " [MOUNTED]" : "";
            if (bench_finished) {
                char w_str[16] = "-", r_str[16] = "-";
                if (d->speed_write > 0) snprintf(w_str, sizeof(w_str), "%.0f", d->speed_write);
                if (d->speed_read > 0) snprintf(r_str, sizeof(r_str), "%.0f", d->speed_read);
                mvprintw(by + 4 + i, 3, "%-10s %-6s %-26s %-6s %lldG %7s %7s%s",
                         d->disk, "disk", d->model, d->tran, d->size_gb,
                         w_str, r_str, tag);
            } else {
                mvprintw(by + 4 + i, 3, "%-10s %-6s %-26s %-6s %lldG%s",
                         d->disk, "disk", d->model, d->tran, d->size_gb, tag);
            }
            if (d->is_root) attroff(COLOR_PAIR(3));
            else if (d->is_mounted) attroff(COLOR_PAIR(4));
        }
        int tipy = by + bh + 1;
        if (tipy < getmaxy(stdscr) - 1) {
            if (bench_finished)
                mvprintw(tipy, 3, "[ROOT] = System disk  |  Speeds in MB/s");
            else
                mvprintw(tipy, 3, "[ROOT] = System disk, cannot be carved with dm-linear");
        }
        draw_status_bar("Q:Back  B:Re-benchmark", "Q:Back B:Bench");
        refresh();
        int ch = getch();
        if (ch == 'q' || ch == 'Q' || ch == 27) return;
        if (ch == KEY_RESIZE) continue;
        if (ch == 'b' || ch == 'B') {
            if (bench_pid > 0) {
                kill(bench_pid, SIGTERM);
                waitpid(bench_pid, NULL, 0);
                bench_pid = -1;
            }
            if (bench_rd_fd >= 0) {
                close(bench_rd_fd);
                bench_rd_fd = -1;
            }
            bench_finished = 0;
            bench_buf[0] = 0;
            bench_buf_len = 0;
            bench_start_time = time(NULL);
            for (int i = 0; i < ndisks; i++) {
                disks[i].speed_write = 0;
                disks[i].speed_read = 0;
            }
            auto_bench_start();
            snprintf(status_msg, sizeof(status_msg), "Re-benchmark started...");
        }
    }
}

static void screen_bench() {
    if (!check_terminal_size()) return;
    if (ndisks == 0) { snprintf(status_msg, sizeof(status_msg), "No disks found!"); return; }

    auto_bench_poll();

    if (!bench_finished) {
        while (!bench_finished) {
            auto_bench_poll();
    clear();
    int maxx = getmaxx(stdscr);
            attron(A_BOLD | COLOR_PAIR(2));
            mvprintw(0, (maxx - 28) / 2, "=== Benchmark (Auto) ===");
            attroff(A_BOLD | COLOR_PAIR(2));
            int bw = maxx - 4;
            int nrow = 0;
            for (int i = 0; i < ndisks; i++) if (!disks[i].is_root) nrow++;
            int bh = nrow + 5;
            draw_box(2, 1, bh, bw, "Auto-benchmark in progress...");
            attron(A_BOLD);
            mvprintw(4, 3, "%-10s %-26s %-6s %8s  %s", "DEVICE", "MODEL", "TRAN", "SIZE", "STATUS");
            mvhline(5, 2, ACS_HLINE, bw - 2);
            attroff(A_BOLD);
            int row = 0;
            int elapsed_all = (int)difftime(time(NULL), bench_start_time);
            for (int i = 0; i < ndisks; i++) {
                ui_disk_t *d = &disks[i];
                if (d->is_root) continue;
                if (bench_disk_done(d->disk, bench_buf)) {
                    attron(COLOR_PAIR(1));
                    mvprintw(6 + row, 3, "%-10s %-26s %-6s %6lldG  Write %6.0f  Read %6.0f MB/s",
                             d->disk, d->model, d->tran, d->size_gb,
                             d->speed_write, d->speed_read);
                    attroff(COLOR_PAIR(1));
                } else {
                    char pat_test[64], pat_started[64];
                    snprintf(pat_test, sizeof(pat_test), "Testing /dev/%s", d->disk);
                    snprintf(pat_started, sizeof(pat_started), "Started benchmark for /dev/%s", d->disk);
                    if (strstr(bench_buf, pat_test) || strstr(bench_buf, pat_started)) {
                        mvprintw(6 + row, 3, "%-10s %-26s %-6s %6lldG  Testing... %dm%02ds  %d/%d",
                                 d->disk, d->model, d->tran, d->size_gb,
                                 elapsed_all / 60, elapsed_all % 60, row + 1, nrow);
                    } else {
                        mvprintw(6 + row, 3, "%-10s %-26s %-6s %6lldG  Waiting...           %d/%d",
                                 d->disk, d->model, d->tran, d->size_gb, row + 1, nrow);
                    }
                }
                row++;
            }
            int elapsed = (int)difftime(time(NULL), bench_start_time);
            mvprintw(6 + nrow + 2, 3, "Elapsed: %dm %ds", elapsed / 60, elapsed % 60);
            draw_status_bar("Auto-benchmarking... Q:Back", "Q:Back");
            refresh();
            napms(200);
            int ch = getch();
            if (ch == 'q' || ch == 'Q' || ch == 27) return;
            if (ch == KEY_RESIZE) continue;
        }
    }

    clear();
    int maxy = getmaxy(stdscr);
    int maxx = getmaxx(stdscr);
    attron(A_BOLD | COLOR_PAIR(1));
    mvprintw(0, (maxx - 22) / 2, "=== Benchmark Results ===");
    attroff(A_BOLD | COLOR_PAIR(1));
    int nrow = 0;
    for (int i = 0; i < ndisks; i++) if (!disks[i].is_root) nrow++;
    int rbw = 72;
    int rbh = nrow + 6;
    int rby = 2;
    if (rby + rbh > maxy - 1) rbh = maxy - rby - 1;
    if (rbh < 3) rbh = 3;
    int rbx = (maxx - rbw) / 2;
    draw_box(rby, rbx, rbh, rbw, "Results");
    attron(A_BOLD);
    mvprintw(rby + 2, rbx + 2, "%-10s %-26s %-6s %10s %10s",
             "DEVICE", "MODEL", "TRAN", "Write", "Read");
    mvhline(rby + 3, rbx + 1, ACS_HLINE, rbw - 2);
    attroff(A_BOLD);
    int row = 0;
    double tw = 0, tr = 0;
    for (int i = 0; i < ndisks; i++) {
        ui_disk_t *d = &disks[i];
        if (d->is_root) continue;
        mvprintw(rby + 4 + row, rbx + 2, "%-10s %-26s %-6s %7.0f %7.0f MB/s",
                 d->disk, d->model, d->tran, d->speed_write, d->speed_read);
        tw += d->speed_write;
        tr += d->speed_read;
        row++;
    }
    mvhline(rby + 4 + row, rbx + 1, ACS_HLINE, rbw - 2);
    attron(A_BOLD);
    mvprintw(rby + 5 + row, rbx + 2, "%-43s %7.0f %7.0f MB/s",
             "TOTAL:", tw, tr);
    attroff(A_BOLD);
    draw_status_bar("Press any key to continue", "Any key:Continue");
    refresh();
    getch();
}

static int input_str(int y, int x, int w, const char *prompt, char *buf, int bufsize, const char *def) {
    attron(A_BOLD);
    mvprintw(y, x, "%-20s", prompt);
    attroff(A_BOLD);
    attron(A_UNDERLINE);
    for (int i = 0; i < w; i++) mvaddch(y, x + 20 + i, ' ');
    attroff(A_UNDERLINE);
    if (def && def[0]) {
        mvprintw(y, x + 20, "%s", def);
        strncpy(buf, def, bufsize - 1);
        buf[bufsize - 1] = 0;
    } else {
        buf[0] = 0;
    }
    int len = strlen(buf);
    int pos = len;
    int cx = x + 20 + pos;
    int cy = y;
    curs_set(1);
    move(cy, cx);
    while (1) {
        int ch = getch();
        if (ch == '\n' || ch == KEY_ENTER) { curs_set(0); return 0; }
        if (ch == '\t') { curs_set(0); return 1; }
        if (ch == 27) { curs_set(0); return -1; }
        if (ch == KEY_LEFT) {
            if (pos > 0) { pos--; cx--; move(cy, cx); }
        } else if (ch == KEY_RIGHT) {
            if (pos < len) { pos++; cx++; move(cy, cx); }
        } else if (ch == KEY_HOME || ch == 1) {
            pos = 0; cx = x + 20; move(cy, cx);
        } else if (ch == KEY_END || ch == 5) {
            pos = len; cx = x + 20 + len; move(cy, cx);
        } else if (ch == KEY_BACKSPACE || ch == 127) {
            if (pos > 0) {
                memmove(buf + pos - 1, buf + pos, len - pos + 1);
                pos--;
                len--;
                mvprintw(cy, x + 20, "%-*s", w, buf);
                cx = x + 20 + pos;
                move(cy, cx);
            }
        } else if (ch >= 32 && ch < 127 && len < bufsize - 1) {
            memmove(buf + pos + 1, buf + pos, len - pos + 1);
            buf[pos] = ch;
            pos++;
            len++;
            mvprintw(cy, x + 20, "%-*.*s", w, w, buf);
            cx = x + 20 + pos;
            if (cx > x + 20 + w - 1) cx = x + 20 + w - 1;
            move(cy, cx);
        }
    }
}

static int create_phase_select_disks(int *sel, int *carve, int *cursor, int *sel_count) {
    if (!check_terminal_size()) return -1;
    while (1) {
        clear();
        int maxx = getmaxx(stdscr);
        attron(A_BOLD | COLOR_PAIR(2));
        mvprintw(0, (maxx - 34) / 2, "=== Create Tiered Volume ===");
        attroff(A_BOLD | COLOR_PAIR(2));
        int bw = maxx - 4;
        int bh = ndisks + 5;
        draw_box(2, 1, bh, bw, "Step 1: Select Disks (Space=toggle, Enter=next)");
        for (int i = 0; i < ndisks; i++) {
            ui_disk_t *d = &disks[i];
            if (d->is_root) {
                attron(COLOR_PAIR(3));
                mvprintw(4 + i, 3, "  %-10s %-26s %6lldG  [ROOT]", d->disk, d->model, d->size_gb);
                attroff(COLOR_PAIR(3));
            } else if (d->is_mounted) {
                attron(COLOR_PAIR(4));
                mvprintw(4 + i, 3, "  %-10s %-26s %6lldG  [MOUNTED]", d->disk, d->model, d->size_gb);
                attroff(COLOR_PAIR(4));
            } else if (!bench_finished && !bench_disk_done(d->disk, bench_buf)) {
                attron(COLOR_PAIR(4));
                mvprintw(4 + i, 3, "  %-10s %-26s %6lldG  [BENCH...]", d->disk, d->model, d->size_gb);
                attroff(COLOR_PAIR(4));
            } else {
                if (i == *cursor) attron(A_BOLD);
                mvprintw(4 + i, 3, "%c %-10s %-26s %6lldG  carve:%dG",
                         sel[i] ? '*' : ' ', d->disk, d->model, d->size_gb, carve[i]);
                if (i == *cursor) attroff(A_BOLD);
            }
        }
        *sel_count = 0;
        for (int i = 0; i < ndisks; i++) if (sel[i]) (*sel_count)++;
        mvprintw(4 + ndisks + 1, 3, "Selected: %d disks", *sel_count);
        draw_status_bar("Space:Toggle  Enter:Next  Q:Cancel", "Space/Enter/Q");
        refresh();
        int ch = getch();
        if (ch == 'q' || ch == 'Q') return -1;
        switch (ch) {
            case KEY_UP:
                if (*cursor > 0) {
                    int nc = *cursor - 1;
                    while (nc >= 0 && (disks[nc].is_root || disks[nc].is_mounted)) nc--;
                    if (nc >= 0) *cursor = nc;
                }
                break;
            case KEY_DOWN:
                if (*cursor < ndisks - 1) {
                    int nc = *cursor + 1;
                    while (nc < ndisks && (disks[nc].is_root || disks[nc].is_mounted)) nc++;
                    if (nc < ndisks) *cursor = nc;
                }
                break;
            case ' ':
                if (!disks[*cursor].is_root && !disks[*cursor].is_mounted) {
                    if (!bench_finished && !bench_disk_done(disks[*cursor].disk, bench_buf)) {
                        snprintf(status_msg, sizeof(status_msg), "%s is benchmarking, wait...", disks[*cursor].disk);
                    } else {
                        sel[*cursor] = !sel[*cursor];
                    }
                }
                break;
            case '\n': case KEY_ENTER:
                if (*sel_count >= 2) {
                    for (int i = 0; i < ndisks; i++) if (sel[i]) { *cursor = i; break; }
                    return 0;
                } else {
                    snprintf(status_msg, sizeof(status_msg), "Select at least 2 disks!");
                }
                break;
            case 27: return -1;
            case KEY_RESIZE: continue;
        }
    }
}

static int create_phase_set_carves(int *sel, int *carve, int *cursor) {
    if (!check_terminal_size()) return -1;
    while (1) {
        clear();
        int maxx = getmaxx(stdscr);
        attron(A_BOLD | COLOR_PAIR(2));
        mvprintw(0, (maxx - 34) / 2, "=== Create Tiered Volume ===");
        attroff(A_BOLD | COLOR_PAIR(2));
        int sel_count = 0;
        for (int i = 0; i < ndisks; i++) if (sel[i]) sel_count++;
        int bw = maxx - 4;
        int bh = sel_count + 5;
        draw_box(2, 1, bh, bw, "Step 2: Set Carve Sizes (Enter=next, Esc=back)");
        int row = 0;
        attron(A_BOLD);
        mvprintw(4, 3, "%-10s %-20s %10s %10s", "DEVICE", "MODEL", "AVAIL", "CARVE");
        mvhline(5, 2, ACS_HLINE, bw - 2);
        attroff(A_BOLD);
        for (int i = 0; i < ndisks; i++) {
            if (!sel[i]) continue;
            ui_disk_t *d = &disks[i];
            int active = (i == *cursor);
            if (active) attron(A_REVERSE);
            mvprintw(6 + row, 3, "%-10s %-20s %8lldG %8dG",
                     d->disk, d->model, d->size_gb, carve[i]);
            if (active) attroff(A_REVERSE);
            row++;
        }
        mvprintw(4 + row + 2, 3, "Left/Right:Adjust size  Up/Down:Select disk");
        draw_status_bar("Left/Right:Adjust  Enter:Next  Esc:Back", "Arrow/Enter/Esc");
        refresh();
        int ch = getch();
        switch (ch) {
            case KEY_UP:
                { int found = -1;
                for (int i = *cursor - 1; i >= 0; i--) if (sel[i]) { found = i; break; }
                if (found >= 0) *cursor = found; }
                break;
            case KEY_DOWN:
                { int found = -1;
                for (int i = *cursor + 1; i < ndisks; i++) if (sel[i]) { found = i; break; }
                if (found >= 0) *cursor = found; }
                break;
            case KEY_LEFT:
                if (carve[*cursor] > 50) carve[*cursor] -= 50;
                else carve[*cursor] = 0;
                break;
            case KEY_RIGHT:
                { int maxc = (int)disks[*cursor].size_gb - 1;
                if (maxc < 0) maxc = 0;
                if (carve[*cursor] < maxc) carve[*cursor] += 50;
                if (carve[*cursor] > maxc) carve[*cursor] = maxc; }
                break;
            case '\n': case KEY_ENTER:
                return 0;
            case 27:
                return -1;
            case KEY_RESIZE:
                break;
        }
    }
}

static int create_phase_input_settings(char *input_name, char *input_mount, char *input_fs) {
    if (!check_terminal_size()) return -1;
    clear();
    int maxx = getmaxx(stdscr);
    attron(A_BOLD | COLOR_PAIR(2));
    mvprintw(0, (maxx - 34) / 2, "=== Create Tiered Volume ===");
    attroff(A_BOLD | COLOR_PAIR(2));
    int bw = 60;
    int bh = 12;
    int by = 3;
    int bx = (maxx - bw) / 2;
    draw_box(by, bx, bh, bw, "Step 3: Volume Settings (Tab=next field, Enter=create)");
    int field = 0;
    input_name[0] = 0;
    input_mount[0] = 0;
    input_fs[0] = 0;
    while (1) {
        int ry = by + 2 + field;
        int r;
        if (field == 0)
            r = input_str(ry, bx + 2, 30, "Volume name:", input_name, sizeof(input_name), NULL);
        else if (field == 1)
            r = input_str(ry, bx + 2, 30, "Mount point:", input_mount, sizeof(input_mount), "/mnt/fast");
        else
            r = input_str(ry, bx + 2, 30, "Filesystem:", input_fs, sizeof(input_fs), "ext4");
        if (r == -1) return -1;
        if (r == 1) { field = (field + 1) % 3; continue; }
        if (r == 0) {
            if (field < 2) { field++; continue; }
            break;
        }
    }
    if (!input_name[0]) { snprintf(status_msg, sizeof(status_msg), "Error: name required"); return -1; }
    if (!input_mount[0]) { snprintf(status_msg, sizeof(status_msg), "Error: mount point required"); return -1; }
    if (!input_fs[0]) { snprintf(status_msg, sizeof(status_msg), "Error: filesystem required"); return -1; }
    if (!tiered_is_valid_name(input_name)) {
        snprintf(status_msg, sizeof(status_msg), "Error: name must be alphanumeric (a-z, 0-9, . _ -)");
        return -1;
    }
    if (!tiered_is_valid_mount(input_mount)) {
        snprintf(status_msg, sizeof(status_msg), "Error: mount must start with /");
        return -1;
    }
    if (!tiered_is_valid_fs(input_fs)) {
        snprintf(status_msg, sizeof(status_msg), "Error: invalid filesystem (ext4/ext3/xfs/btrfs/none)");
        return -1;
    }
    mvprintw(by + 7, bx + 2, "Will create: %s", input_name);
    mvprintw(by + 8, bx + 2, "Mount at: %s", input_mount);
    mvprintw(by + 9, bx + 2, "Filesystem: %s", input_fs);
    mvprintw(by + 10, bx + 2, "Press Enter to create...");
    refresh();
    int ch2 = getch();
    if (ch2 == 27) return -1;
    if (ch2 == '\n' || ch2 == KEY_ENTER) return 0;
    return -1;
}

static int screen_create_select_disks() {
    if (ndisks == 0) { snprintf(status_msg, sizeof(status_msg), "No disks found!"); return -1; }

    save_speeds();
    parse_disk_list();
    restore_speeds();

    int sel[MAX_DISKS] = {0};
    int carve[MAX_DISKS] = {0};
    int sel_count = 0;
    int cursor = 0;
    for (int i = 0; i < ndisks; i++) {
        if (!disks[i].is_root && !disks[i].is_mounted) {
            cursor = i;
            break;
        }
    }
    for (int i = 0; i < ndisks; i++) {
        if (!disks[i].is_root) {
            int max_carve = disks[i].size_gb > 50 ? (int)disks[i].size_gb - 1 : 0;
            carve[i] = max_carve > 600 ? 600 : max_carve;
        }
    }

    if (create_phase_select_disks(sel, carve, &cursor, &sel_count) < 0) return -1;
    if (create_phase_set_carves(sel, carve, &cursor) < 0) return -1;

    char input_name[128] = "";
    char input_mount[256] = "";
    char input_fs[32] = "";
    if (create_phase_input_settings(input_name, input_mount, input_fs) < 0) return -1;

    strncpy(vol_name, input_name, sizeof(vol_name) - 1);
    vol_name[sizeof(vol_name) - 1] = 0;
    strncpy(mount_point, input_mount, sizeof(mount_point) - 1);
    mount_point[sizeof(mount_point) - 1] = 0;

    for (int i = 0; i < ndisks; i++) {
        if (sel[i] && carve[i] <= 0) {
            snprintf(status_msg, sizeof(status_msg), "Error: carve size for %s must be > 0", disks[i].disk);
            return -1;
        }
    }

    char disk_spec[1024] = "";
    int offset = 0;
    for (int i = 0; i < ndisks; i++) {
        if (sel[i]) {
            char tmp[64];
            int n = snprintf(tmp, sizeof(tmp), "%s:%d", disks[i].disk, carve[i]);
            if (offset + n + 1 < (int)sizeof(disk_spec)) {
                if (offset > 0) disk_spec[offset++] = ',';
                memcpy(disk_spec + offset, tmp, n);
                offset += n;
                disk_spec[offset] = 0;
            }
        }
    }
    char cmd[PATH_MAX + 2048];
    snprintf(cmd, sizeof(cmd),
        "%s --create --name %s --disks %s --fs %s --mount %s 2>&1",
        tool_path, vol_name, disk_spec, input_fs, mount_point);
    clear();
    int maxy = getmaxy(stdscr);
    int maxx = getmaxx(stdscr);
    attron(A_BOLD | COLOR_PAIR(2));
    mvprintw(2, (maxx - 30) / 2, "=== Creating Volume... ===");
    attroff(A_BOLD | COLOR_PAIR(2));
    mvprintw(4, 3, "Name: %s", vol_name);
    mvprintw(5, 3, "Disks: %s", disk_spec);
    mvprintw(6, 3, "Mount: %s", mount_point);
    mvprintw(7, 3, "Filesystem: %s", input_fs);
    mvprintw(8, 3, "Please wait, this may take a minute...");
    refresh();
    char out[16384] = "";
    run_cmd(cmd, out, sizeof(out));
    clear();
    if (strstr(out, "Complete!")) {
        attron(A_BOLD | COLOR_PAIR(1));
        mvprintw(2, (maxx - 30) / 2, "=== Volume Created! ===");
        attroff(A_BOLD | COLOR_PAIR(1));
        int sy = 4;
        char line_buf[256];
        char *p = strstr(out, "Device:");
        if (p) { char *nl = strchr(p, '\n'); int len = nl ? (int)(nl - p) : (int)strlen(p); snprintf(line_buf, sizeof(line_buf), "%.*s", len, p); mvprintw(sy++, 3, "%s", line_buf); }
        p = strstr(out, "Size:");
        if (p) { char *nl = strchr(p, '\n'); int len = nl ? (int)(nl - p) : (int)strlen(p); snprintf(line_buf, sizeof(line_buf), "%.*s", len, p); mvprintw(sy++, 3, "%s", line_buf); }
        p = strstr(out, "Stripes:");
        if (p) { char *nl = strchr(p, '\n'); int len = nl ? (int)(nl - p) : (int)strlen(p); snprintf(line_buf, sizeof(line_buf), "%.*s", len, p); mvprintw(sy++, 3, "%s", line_buf); }
        p = strstr(out, "Mount:");
        if (p) { char *nl = strchr(p, '\n'); int len = nl ? (int)(nl - p) : (int)strlen(p); snprintf(line_buf, sizeof(line_buf), "%.*s", len, p); mvprintw(sy++, 3, "%s", line_buf); }
        snprintf(status_msg, sizeof(status_msg), "Volume '%s' created!", vol_name);
    } else {
        attron(A_BOLD | COLOR_PAIR(3));
        mvprintw(2, (maxx - 30) / 2, "=== Create Failed ===");
        attroff(A_BOLD | COLOR_PAIR(3));
        int ey = 4;
        char *errline = strtok(out, "\n");
        while (errline && ey < maxy - 3) {
            mvprintw(ey++, 3, "%.70s", errline);
            errline = strtok(NULL, "\n");
        }
        snprintf(status_msg, sizeof(status_msg), "Volume creation failed!");
    }
    draw_status_bar("Press any key to continue", "Any key:Continue");
    refresh();
    getch();
    return 0;
}

static void screen_status() {
    if (!check_terminal_size()) return;
    clear();
    int maxy = getmaxy(stdscr);
    int maxx = getmaxx(stdscr);
    attron(A_BOLD | COLOR_PAIR(2));
    mvprintw(0, (maxx - 20) / 2, "=== Volume Status ===");
    attroff(A_BOLD | COLOR_PAIR(2));
    int bw = maxx - 4;
    int sy = 2;

    if (!vol_name[0]) {
        draw_box(sy, 1, 5, bw, "Volume");
        mvprintw(sy + 2, 3, "No volume detected.");
        sy += 6;
    } else {
        char lv_out[2048] = "";
        char lv_cmd[512];
        snprintf(lv_cmd, sizeof(lv_cmd), "sudo lvs --noheadings -o lv_name,lv_size,stripes,stripesize tv_vg_%s 2>/dev/null", vol_name);
        run_cmd(lv_cmd, lv_out, sizeof(lv_out));
        char df_out[2048] = "";
        if (mount_point[0]) {
            char df_cmd[512];
            snprintf(df_cmd, sizeof(df_cmd), "df -h %s 2>/dev/null", mount_point);
            run_cmd(df_cmd, df_out, sizeof(df_out));
        }

        draw_box(sy, 1, 8, bw, "Volume");
        sy += 2;
        attron(A_BOLD);
        mvprintw(sy, 3, "%-14s %s", "Name:", vol_name);
        sy++;
        attroff(A_BOLD);

        char *lvline = strtok(lv_out, "\n");
        while (lvline) {
            while (*lvline == ' ') lvline++;
            mvprintw(sy, 3, "%-14s %s", "LVM Info:", lvline);
            sy++;
            lvline = strtok(NULL, "\n");
        }

        if (mount_point[0]) {
            mvprintw(sy, 3, "%-14s %s", "Mount:", mount_point);
            sy++;
        }

        char *dfline = strtok(df_out, "\n");
        int df_header_skipped = 0;
        while (dfline) {
            if (!df_header_skipped) { df_header_skipped = 1; dfline = strtok(NULL, "\n"); continue; }
            while (*dfline == ' ') dfline++;
            mvprintw(sy, 3, "%-14s %s", "Disk Usage:", dfline);
            sy++;
            dfline = strtok(NULL, "\n");
        }
        sy++;
    }

    char dm_out[2048] = "";
    run_cmd("sudo dmsetup ls 2>/dev/null", dm_out, sizeof(dm_out));
    int dm_count = 0;
    char *line = strtok(dm_out, "\n");
    while (line) {
        if (strstr(line, "tv_")) dm_count++;
        line = strtok(NULL, "\n");
    }

    if (sy + 4 < maxy) {
        draw_box(sy, 1, 4, bw, "DM Targets");
        sy += 2;
        if (dm_count == 0) {
            mvprintw(sy, 3, "None");
        } else {
            mvprintw(sy, 3, "%d dm-linear target(s) active", dm_count);
        }
        sy += 2;
    }

    if (sy + 3 < maxy) {
        draw_box(sy, 1, 3, bw, "Info");
        sy++;
        mvprintw(sy, 3, "Use 'tiered_setup --status' for full details");
        sy++;
    }

    draw_status_bar("Press any key to go back", "Any key:Back");
    refresh();
    getch();
}

static void screen_destroy() {
    if (!check_terminal_size()) return;
    clear();
    int maxy = getmaxy(stdscr);
    int maxx = getmaxx(stdscr);
    attron(A_BOLD | COLOR_PAIR(3));
    mvprintw(0, (maxx - 24) / 2, "=== Destroy Volume ===");
    attroff(A_BOLD | COLOR_PAIR(3));
    if (!vol_name[0]) {
        int bw = 50;
        draw_box(4, (maxx - bw) / 2, 5, bw, "Warning");
        mvprintw(6, (maxx - 42) / 2, "No volume name set. Create a volume first.");
        draw_status_bar("Press any key to go back", "Any key:Back");
        refresh();
        getch();
        return;
    }
    char dm_out[2048] = "";
    run_cmd("sudo dmsetup ls 2>/dev/null", dm_out, sizeof(dm_out));
    if (!strstr(dm_out, "tv_")) {
        int bw = 50;
        draw_box(4, (maxx - bw) / 2, 5, bw, "Warning");
        mvprintw(6, (maxx - 36) / 2, "No tiered volume found to destroy.");
        draw_status_bar("Press any key to go back", "Any key:Back");
        refresh();
        getch();
        return;
    }
    int bw = 56;
    draw_box(4, (maxx - bw) / 2, 8, bw, "Confirm Destroy");
    attron(COLOR_PAIR(3));
    mvprintw(6, (maxx - 40) / 2, "WARNING: This will permanently delete:");
    attroff(COLOR_PAIR(3));
    mvprintw(8, (maxx - 30) / 2, "- All data on the volume");
    mvprintw(9, (maxx - 30) / 2, "- The striped LVM configuration");
    mvprintw(10, (maxx - 30) / 2, "- All dm-linear targets");
    mvprintw(12, (maxx - 30) / 2, "Are you sure? (y/N)");
    draw_status_bar("Y:Destroy  Any other:Cancel", "Y:Destroy");
    refresh();
    int ch = getch();
    if (ch == 'y' || ch == 'Y') {
        clear();
        attron(A_BOLD | COLOR_PAIR(3));
        mvprintw(2, (maxx - 26) / 2, "=== Destroying Volume... ===");
        attroff(A_BOLD | COLOR_PAIR(3));
        mvprintw(4, 3, "Please wait...");
        refresh();
        char cmd[PATH_MAX + 256];
        snprintf(cmd, sizeof(cmd), "%s --destroy --name %s 2>&1", tool_path, vol_name);
        char out[4096] = "";
        run_cmd(cmd, out, sizeof(out));
        clear();
        if (strstr(out, "Remove Complete")) {
            attron(A_BOLD | COLOR_PAIR(1));
            mvprintw(2, (maxx - 30) / 2, "=== Volume Destroyed! ===");
            attroff(A_BOLD | COLOR_PAIR(1));
            snprintf(status_msg, sizeof(status_msg), "Volume '%s' destroyed!", vol_name);
        } else {
            attron(A_BOLD | COLOR_PAIR(3));
            mvprintw(2, (maxx - 28) / 2, "=== Destroy Complete ===");
            attroff(A_BOLD | COLOR_PAIR(3));
            snprintf(status_msg, sizeof(status_msg), "Volume destroyed.");
        }
        vol_name[0] = 0;
        mount_point[0] = 0;
        int ey = 4;
        char *line = strtok(out, "\n");
        while (line && ey < maxy - 3) {
            mvprintw(ey++, 3, "%.70s", line);
            line = strtok(NULL, "\n");
        }
        draw_status_bar("Press any key to continue", "Any key:Continue");
        refresh();
        getch();
    }
}

int main(int argc, char *argv[]) {
    (void)argc;
    if (geteuid() != 0) {
        fprintf(stderr, "Error: tiered_ui requires root privileges.\n");
        fprintf(stderr, "Please run: sudo %s\n", argv[0]);
        return 1;
    }
    detect_tool_path();
    if (!initscr()) {
        fprintf(stderr, "Failed to initialize ncurses\n");
        return 1;
    }
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_CYAN, COLOR_BLACK);
        init_pair(2, COLOR_GREEN, COLOR_BLACK);
        init_pair(3, COLOR_RED, COLOR_BLACK);
        init_pair(4, COLOR_YELLOW, COLOR_BLACK);
    }

    int maxy = getmaxy(stdscr);
    int maxx = getmaxx(stdscr);
    if (maxy < MIN_ROWS || maxx < MIN_COLS) {
        endwin();
        fprintf(stderr, "Error: terminal too small (%dx%d). Minimum: %dx%d\n", maxx, maxy, MIN_COLS, MIN_ROWS);
        return 1;
    }

    system_dirty_ratio = read_sysctl_int("vm.dirty_ratio");
    system_dirty_bg_ratio = read_sysctl_int("vm.dirty_background_ratio");

    parse_disk_list();
    detect_existing_volume();
    auto_bench_start();
    while (1) {
        int choice = screen_main();
        switch (choice) {
            case 0: screen_disk_list(); break;
            case 1: screen_bench(); break;
            case 2: screen_create_select_disks(); break;
            case 3: screen_status(); break;
            case 4: screen_ram_cache(); break;
            case 5: screen_destroy(); break;
            case 6: goto exit;
        }
    }
exit:
    if (bench_pid > 0) {
        kill(-bench_pid, SIGTERM);
        for (int i = 0; i < 20; i++) {
            pid_t ret = waitpid(-1, NULL, WNOHANG);
            if (ret > 0) break;
            usleep(100000);
        }
        kill(-bench_pid, SIGKILL);
        waitpid(-1, NULL, 0);
    }
    if (bench_rd_fd >= 0) close(bench_rd_fd);
    endwin();
    return 0;
}

#pragma GCC diagnostic pop
