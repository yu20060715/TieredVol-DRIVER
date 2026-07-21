#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include "tiered_common.h"
#include "tiered_types.h"
#include "tiered_sched.h"
#include "version.h"
#include "setup_discover.h"
#include "setup_bench.h"
#include "exec_helper.h"
#include "cmd_create.h"

void make_target(char *out, size_t sz, const char *disk) {
    snprintf(out, sz, "tv_%s_carve", disk);
}

void cleanup_create(const char *name, disk_t *valid, int valid_disks) {
    fprintf(stderr, "  Rolling back...\n");
    {
        char umount_lv[256];
        snprintf(umount_lv, sizeof(umount_lv), "/dev/mapper/tv_vg_%s-tv_lv_%s", name, name);
        char *umount_argv[] = {"sudo", "umount", umount_lv, NULL};
        (void)tv_exec_quiet("sudo", umount_argv);
    }
    {
        char full_lv[256];
        snprintf(full_lv, sizeof(full_lv), "tv_vg_%s/tv_lv_%s", name, name);
        char *const lv_argv[] = {"lvremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", full_lv, NULL};
        (void)tv_exec_run("lvremove", lv_argv);
    }
    {
        char vg_name[128];
        snprintf(vg_name, sizeof(vg_name), "tv_vg_%s", name);
        char *const vg_argv2[] = {"vgremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", vg_name, NULL};
        (void)tv_exec_run("vgremove", vg_argv2);
    }
    for (int i = 0; i < valid_disks; i++) {
        char target[64];
        make_target(target, sizeof(target), valid[i].disk);
        {
            char devpath[128];
            snprintf(devpath, sizeof(devpath), "/dev/mapper/%s", target);
            char *const pv_argv[] = {"pvremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-ff", "-y", devpath, NULL};
            (void)tv_exec_run("pvremove", pv_argv);
        }
        {
            char *const dm_argv[] = {"sudo", "dmsetup", "remove", target, NULL};
            (void)tv_exec_sudo(dm_argv, 0);
        }
    }
    fprintf(stderr, "  Rollback complete.\n");
}

int cmd_create(int argc, char *argv[]) {
    char *name = NULL;
    char *disk_spec = NULL;
    char *fs = "ext4";
    char *mount_point = NULL;
    int stripe_size_kb = DEFAULT_STRIPE_SIZE_KB;
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
                return TV_ERR;
            }
            stripe_size_kb = (int)val;
            user_stripesize = 1;
        }
    }

    if (!name || !disk_spec) {
        fprintf(stderr, "Usage: tiered_setup --create --name NAME --disks sdb:300,sdc:200 [--fs ext4] [--mount /mnt/fast]\n");
        return TV_ERR;
    }

    if (!tiered_is_valid_name(name)) {
        fprintf(stderr, "Error: invalid name '%s' (only a-z, A-Z, 0-9, ., _, - allowed)\n", name);
        return TV_ERR;
    }

    if (!tiered_is_valid_fs(fs)) {
        fprintf(stderr, "Error: invalid filesystem '%s' (ext4/ext3/xfs/btrfs/none)\n", fs);
        return TV_ERR;
    }

    if (mount_point && !tiered_is_valid_mount(mount_point)) {
        fprintf(stderr, "Error: mount point invalid (only / a-z 0-9 . _ - allowed)\n");
        return TV_ERR;
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
                return TV_ERR;
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
        return TV_ERR;
    }

    printf("=== TieredVol: Creating '%s' ===\n", name);

    disk_t dinfo[MAX_DISKS];
    int ninfo = load_all_disk_info(dinfo, MAX_DISKS);

    int valid_disks = 0;
    disk_t valid[MAX_DISKS];

    for (int i = 0; i < nd; i++) {
        sysfs_model(disks_arr[i].disk, disks_arr[i].model, sizeof(disks_arr[i].model));
        disks_arr[i].size_gb = sysfs_size_gb(disks_arr[i].disk);

        disks_arr[i].is_root = 0;
        disks_arr[i].is_mounted = 0;
        for (int j = 0; j < ninfo; j++) {
            if (strcmp(disks_arr[i].disk, dinfo[j].disk) == 0) {
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
                return TV_ERR;
            }
        }

        if (disks_arr[i].size_gb <= 1) {
            fprintf(stderr, "Error: /dev/%s size not detected or too small\n", disks_arr[i].disk);
            return TV_ERR;
        }

        if (disks_arr[i].carve_gb <= 0) {
            disks_arr[i].carve_gb = disks_arr[i].size_gb - 1;
        }
        if (disks_arr[i].carve_gb > disks_arr[i].size_gb - 1) {
            fprintf(stderr, "Error: /dev/%s has %lldGB, cannot carve %lldGB\n",
                    disks_arr[i].disk, disks_arr[i].size_gb, disks_arr[i].carve_gb);
            return TV_ERR;
        }

        valid[valid_disks++] = disks_arr[i];
    }

    if (valid_disks == 0) {
        fprintf(stderr, "Error: no usable disks (all are system disks)\n");
        return TV_ERR;
    }

    printf("  Benchmarking %d disks in parallel...\n", valid_disks);
    if (run_parallel_bench(valid, valid_disks, 1, NULL, NULL) != 0) {
        cleanup_create(name, valid, valid_disks);
        return TV_ERR;
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
    printf("  Estimated actual: ~%.0f MB/s\n", total_speed * TV_ESTIMATED_EFFICIENCY);
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
        return TV_ERR;
    }
    printf("\n");

    printf("Step 1: Cleaning up old targets...\n");
    for (int i = 0; i < valid_disks; i++) {
        char mp[512];
        find_mount_for_disk(valid[i].disk, mp, sizeof(mp));
        if (mp[0]) {
            char *umount_argv[] = {"sudo", "umount", mp, NULL};
            (void)tv_exec_sudo(umount_argv, 0);
        }
        char target[64];
        make_target(target, sizeof(target), valid[i].disk);
        {
            char *const dm_argv[] = {"sudo", "dmsetup", "remove", target, NULL};
            (void)tv_exec_sudo(dm_argv, 1);
        }
    }
    {
        char full_lv[256];
        snprintf(full_lv, sizeof(full_lv), "tv_vg_%s/tv_lv_%s", name, name);
        char *const lv_argv[] = {"lvremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", full_lv, NULL};
        (void)tv_exec_quiet("lvremove", lv_argv);
    }
    {
        char vg_name[128];
        snprintf(vg_name, sizeof(vg_name), "tv_vg_%s", name);
        char *const vg_argv[] = {"vgremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", vg_name, NULL};
        (void)tv_exec_quiet("vgremove", vg_argv);
    }
    for (int i = 0; i < valid_disks; i++) {
        char target[64];
        make_target(target, sizeof(target), valid[i].disk);
        char devpath[128];
        snprintf(devpath, sizeof(devpath), "/dev/mapper/%s", target);
        char *const pv_argv[] = {"pvremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-ff", "-y", devpath, NULL};
        (void)tv_exec_quiet("pvremove", pv_argv);
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
            return TV_ERR;
        }
        char *dm_argv[] = {"sudo", "dmsetup", "create", target, NULL};
        int dm_ret = tv_exec_with_stdin("sudo", dm_argv, table);
        if (dm_ret != 0) {
            fprintf(stderr, "Error: dmsetup create failed for %s (exit=%d)\n", valid[i].disk, dm_ret);
            cleanup_create(name, valid, i);
            return TV_ERR;
        }
        char devpath[128];
        snprintf(devpath, sizeof(devpath), "/dev/mapper/%s", target);
        struct stat st;
        if (stat(devpath, &st) != 0) {
            fprintf(stderr, "Error: failed to create %s\n", target);
            cleanup_create(name, valid, i);
            return TV_ERR;
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
                return TV_ERR;
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
            return TV_ERR;
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

        mkdir(TV_CONFIG_DIR, 0755);

        char config_path[256];
        snprintf(config_path, sizeof(config_path), TV_CONFIG_DIR "%s.scheduler", name);

        if (tv_metadata_save(&meta, config_path) < 0) {
            fprintf(stderr, "Error: failed to save metadata to %s\n", config_path);
            for (int i = 0; i < n_tv_disks; i++) close(tv_disks[i].fd);
            cleanup_create(name, valid, valid_disks);
            return TV_ERR;
        }
        printf("  Metadata saved to %s\n", config_path);

        for (int i = 0; i < n_tv_disks; i++) close(tv_disks[i].fd);

        printf("\n=== Weighted I/O Scheduler volume '%s' created ===\n", name);
        printf("  Use tiered_io to manage this volume.\n");
        printf("  NOTE: This volume uses the weighted I/O scheduler, not LVM striping.\n");
        return TV_OK;
    }

    printf("Step 3: Creating LVM physical volumes...\n");
    for (int i = 0; i < valid_disks; i++) {
        char target[64];
        make_target(target, sizeof(target), valid[i].disk);
        char devpath[128];
        snprintf(devpath, sizeof(devpath), "/dev/mapper/%s", target);
        char *const pvcreate_argv[] = {"pvcreate", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", devpath, NULL};
        if (tv_exec_run("pvcreate", pvcreate_argv) != 0) {
            fprintf(stderr, "Error: pvcreate failed for %s\n", target);
            cleanup_create(name, valid, valid_disks);
            return TV_ERR;
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
                return TV_ERR;
            }
            argc_vg++;
        }
        args[argc_vg] = NULL;
        int ret = tv_exec_run("vgcreate", args);
        for (int i = 5; i < argc_vg; i++) free(args[i]);
        if (ret != 0) {
            fprintf(stderr, "Error: vgcreate failed for tv_vg_%s\n", name);
            cleanup_create(name, valid, valid_disks);
            return TV_ERR;
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
        if (tv_exec_run("lvcreate", lv_argv) != 0) {
            fprintf(stderr, "Error: lvcreate failed\n");
            cleanup_create(name, valid, valid_disks);
            return TV_ERR;
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
        if (tv_exec_run(mkfs_name, mkfs_argv) != 0) {
            fprintf(stderr, "Error: mkfs.%s failed\n", fs);
            cleanup_create(name, valid, valid_disks);
            return TV_ERR;
        }
    } else {
        printf("Step 6: Skipped formatting (raw)\n");
    }

    if (mount_point && strcmp(fs, "none") != 0) {
        printf("Step 7: Mounting...\n");
        char *mkdir_argv[] = {"sudo", "mkdir", "-p", mount_point, NULL};
        (void)tv_exec_sudo(mkdir_argv, 0);
        char *mount_argv[] = {"sudo", "mount", lv_path, mount_point, NULL};
        if (tv_exec_sudo(mount_argv, 0) != 0) {
            fprintf(stderr, "Error: mount failed\n");
            cleanup_create(name, valid, valid_disks);
            return TV_ERR;
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
                char *mkdir_argv[] = {"sudo", "mkdir", "-p", TV_CONFIG_DIR, NULL};
                (void)tv_exec_sudo(mkdir_argv, 0);
                char dest[512];
                snprintf(dest, sizeof(dest), TV_CONFIG_DIR "%s.conf", name);
                char *mv_argv[] = {"sudo", "mv", "-f", conf_path, dest, NULL};
                int mv_ret = tv_exec_sudo(mv_argv, 0);
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

    return TV_OK;
}