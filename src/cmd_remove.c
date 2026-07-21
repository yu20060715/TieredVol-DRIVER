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
#include "cmd_remove.h"

int cmd_remove(int argc, char *argv[]) {
    char *name = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) name = argv[++i];
    }

    if (!name) {
        fprintf(stderr, "Usage: tiered_setup --remove --name NAME\n");
        return TV_ERR;
    }

    if (!tiered_is_valid_name(name)) {
        fprintf(stderr, "Error: invalid name '%s'\n", name);
        return TV_ERR;
    }

    printf("=== TieredVol: Removing '%s' ===\n", name);

    /* Check for scheduler volume first */
    char sched_path[256];
    snprintf(sched_path, sizeof(sched_path), TV_CONFIG_DIR "%s.scheduler", name);
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
                if (tv_exec_sudo(dm_argv, 0) == 0) {
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
            (void)tv_exec_sudo(rm_argv, 0);
        }
        {
            char conf_path_cleanup[256];
            snprintf(conf_path_cleanup, sizeof(conf_path_cleanup), TV_CONFIG_DIR "%s.conf", name);
            char *rm_argv[] = {"sudo", "rm", "-f", conf_path_cleanup, NULL};
            (void)tv_exec_sudo(rm_argv, 0);
        }
        {
            char *rmdir_argv[] = {"sudo", "rmdir", TV_CONFIG_DIR, NULL};
            (void)tv_exec_sudo(rmdir_argv, 0);
        }

        printf("\n=== Remove Complete ===\n");
        return TV_OK;
    }

    /* LVM volume path */

    char targets[MAX_DISKS][64];
    int ntargets = 0;

    char conf_path[256];
    snprintf(conf_path, sizeof(conf_path), TV_CONFIG_DIR "%s.conf", name);
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

        if (ntargets == 0) {
            fprintf(stderr, "Error: no devices found in config %s\n", conf_path);
            return TV_ERR;
        }
    } else {
        fprintf(stderr, "Error: no config found for '%s' at %s\n", name, conf_path);
        fprintf(stderr, "  Run: sudo tiered_setup --remove --name %s\n", name);
        return TV_ERR;
    }

    printf("Checking mounts...\n");
    for (int i = 0; i < ntargets; i++) {
        char target_dev[128];
        snprintf(target_dev, sizeof(target_dev), "/dev/mapper/%s", targets[i]);
        char mp[512];
        find_mount_for_disk(target_dev, mp, sizeof(mp));
        if (mp[0]) {
            char *umount_argv[] = {"sudo", "umount", mp, NULL};
            int umount_ret = tv_exec_sudo(umount_argv, 0);
            if (umount_ret != 0) {
                fprintf(stderr, "Warning: umount %s returned %d\n", mp, umount_ret);
            } else {
                printf("  Unmounted %s\n", mp);
            }
        }
    }

    char lv_path[256];
    snprintf(lv_path, sizeof(lv_path), "/dev/mapper/tv_vg_%s-tv_lv_%s", name, name);
    {
        struct stat st;
        if (stat(lv_path, &st) == 0) {
            char mp[512];
            find_mount_for_disk(lv_path, mp, sizeof(mp));
            if (mp[0]) {
                char *umount_argv[] = {"sudo", "umount", mp, NULL};
                int umount_ret = tv_exec_sudo(umount_argv, 0);
                if (umount_ret != 0) {
                    fprintf(stderr, "Warning: umount %s returned %d\n", mp, umount_ret);
                } else {
                    printf("  Unmounted %s\n", mp);
                }
            }
        }
    }

    printf("Removing LVM logical volume...\n");
    {
        char full_lv[256];
        snprintf(full_lv, sizeof(full_lv), "tv_vg_%s/tv_lv_%s", name, name);
        char *const lv_argv[] = {"lvremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", full_lv, NULL};
        (void)tv_exec_run("lvremove", lv_argv);
    }

    printf("Removing volume group...\n");
    {
        char vg_name[128];
        snprintf(vg_name, sizeof(vg_name), "tv_vg_%s", name);
        char *const vg_argv[] = {"vgremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", vg_name, NULL};
        (void)tv_exec_run("vgremove", vg_argv);
    }

    printf("Removing dm-linear carve targets...\n");
    for (int i = 0; i < ntargets; i++) {
        {
            char devpath[128];
            snprintf(devpath, sizeof(devpath), "/dev/mapper/%s", targets[i]);
            char *const pv_argv[] = {"pvremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-ff", "-y", devpath, NULL};
            (void)tv_exec_run("pvremove", pv_argv);
        }
        {
            char *const dm_argv[] = {"sudo", "dmsetup", "remove", targets[i], NULL};
            (void)tv_exec_sudo(dm_argv, 0);
        }
    }

    printf("Removing config...\n");
    {
        char *rm_argv[] = {"sudo", "rm", "-f", conf_path, NULL};
        (void)tv_exec_sudo(rm_argv, 0);
        char *rmdir_argv[] = {"sudo", "rmdir", TV_CONFIG_DIR, NULL};
        (void)tv_exec_sudo(rmdir_argv, 0);
    }

    printf("\n=== Remove Complete ===\n");
    return TV_OK;
}

int cmd_status(void) {
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
        DIR *d = opendir(TV_CONFIG_DIR);
        if (d) {
            struct dirent *ent;
            int found = 0;
            while ((ent = readdir(d))) {
                if (strstr(ent->d_name, ".conf") || strstr(ent->d_name, ".scheduler")) {
                    printf("  " TV_CONFIG_DIR "%s\n", ent->d_name);
                    found = 1;
                }
            }
            closedir(d);
            if (!found) printf("  None\n");
        } else {
            printf("  None\n");
        }
    }

    return TV_OK;
}