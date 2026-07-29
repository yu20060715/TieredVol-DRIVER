#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include "tiered_common.h"
#include "tiered_types.h"
#include "setup_discover.h"
#include "exec_helper.h"
#include "cmd_create.h"
#include "cmd_remove.h"

#pragma GCC diagnostic ignored "-Wformat-truncation="

static int is_kernel_target(const char *name) {
    char path[256];
    snprintf(path, sizeof(path), "/dev/mapper/%s", name);
    struct stat st;
    if (stat(path, &st) != 0) return 0;

    char output[4096];
    char *dm_argv[] = {"dmsetup", "table", (char *)name, NULL};
    if (tv_exec_capture("dmsetup", dm_argv, output, sizeof(output)) != 0)
        return 0;
    return strstr(output, "tieredvol") != NULL;
}

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

    if (is_kernel_target(name)) {
        printf("  Detected kernel dm target (tieredvol)\n");

        printf("  Removing dm target...\n");
        {
            char *dm_argv[] = {"sudo", "dmsetup", "remove", name, NULL};
            int ret = tv_exec_sudo(dm_argv, 0);
            if (ret != 0) {
                fprintf(stderr, "  dmsetup remove failed (retrying)...\n");
                sleep(1);
                ret = tv_exec_sudo(dm_argv, 0);
            }
            if (ret == 0)
                printf("  Removed /dev/mapper/%s\n", name);
            else
                fprintf(stderr, "  Failed to remove /dev/mapper/%s\n", name);
        }

        printf("  Removing metadata...\n");
        {
            char conf_path[256];
            snprintf(conf_path, sizeof(conf_path), TV_CONFIG_DIR "%s.conf", name);
            char *rm_argv[] = {"sudo", "rm", "-f", conf_path, NULL};
            (void)tv_exec_sudo(rm_argv, 0);
        }
        {
            char *rmdir_argv[] = {"sudo", "rmdir", TV_CONFIG_DIR, NULL};
            (void)tv_exec_sudo(rmdir_argv, 0);
        }

        printf("\n=== Remove Complete ===\n");
        return TV_OK;
    }

    /* LVM volume path (legacy) */
    char targets[TV_MAX_DISKS][64];
    int ntargets = 0;

    char conf_path[512];
    snprintf(conf_path, sizeof(conf_path), TV_CONFIG_DIR "%s.conf", name);
    FILE *cf = fopen(conf_path, "r");
    if (cf) {
        char line[256];
        while (fgets(line, sizeof(line), cf) && ntargets < TV_MAX_DISKS) {
            if (strncmp(line, "device=", 7) == 0) {
                char disk[32];
                sscanf(line + 7, "%31s", disk);
                make_target(targets[ntargets], sizeof(targets[0]), disk);
                ntargets++;
            }
        }
        fclose(cf);
        if (ntargets == 0) { fprintf(stderr, "Error: no devices in config\n"); return TV_ERR; }
    } else {
        fprintf(stderr, "Error: no config for '%s'\n", name);
        return TV_ERR;
    }

    printf("Checking mounts...\n");
    char lv_path[256];
    snprintf(lv_path, sizeof(lv_path), "/dev/mapper/tv_vg_%s-tv_lv_%s", name, name);
    { struct stat st; if (stat(lv_path, &st) == 0) { char mp[512]; find_mount_for_disk(lv_path, mp, sizeof(mp)); if (mp[0]) { char *ua[] = {"sudo", "umount", mp, NULL}; (void)tv_exec_sudo(ua, 0); printf("  Unmounted %s\n", mp); } } }

    printf("Removing LVM logical volume...\n");
    { char fl[256]; snprintf(fl, sizeof(fl), "tv_vg_%s/tv_lv_%s", name, name); char *la[] = {"lvremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", fl, NULL}; (void)tv_exec_run("lvremove", la); }

    printf("Removing volume group...\n");
    { char vn[128]; snprintf(vn, sizeof(vn), "tv_vg_%s", name); char *va[] = {"vgremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", vn, NULL}; (void)tv_exec_run("vgremove", va); }

    printf("Removing dm-linear carve targets...\n");
    for (int i = 0; i < ntargets; i++) {
        { char dp[192]; snprintf(dp, sizeof(dp), "/dev/mapper/%s", targets[i]); char *pa[] = {"pvremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-ff", "-y", dp, NULL}; (void)tv_exec_run("pvremove", pa); }
        { char *da[] = {"sudo", "dmsetup", "remove", targets[i], NULL}; (void)tv_exec_sudo(da, 0); }
    }

    printf("Removing config...\n");
    { char *ra[] = {"sudo", "rm", "-f", conf_path, NULL}; (void)tv_exec_sudo(ra, 0); char *rda[] = {"sudo", "rmdir", TV_CONFIG_DIR, NULL}; (void)tv_exec_sudo(rda, 0); }

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
                if (ent->d_name[0] == '.') continue;
                if (strncmp(ent->d_name, "tv_", 3) == 0) {
                    printf("  /dev/mapper/%s", ent->d_name);
                    if (is_kernel_target(ent->d_name))
                        printf(" [tieredvol]");
                    printf("\n");
                    found = 1;
                }
            }
            closedir(d);
            if (!found) printf("  None\n");
        }
    }

    printf("\nKernel Module:\n");
    {
        FILE *f = fopen("/proc/modules", "r");
        if (f) {
            char line[256];
            int found = 0;
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "tieredvol ", 10) == 0) {
                    printf("  tieredvol loaded\n");
                    found = 1;
                    break;
                }
            }
            fclose(f);
            if (!found) printf("  tieredvol not loaded\n");
        }
    }

    printf("\nSaved Configs:\n");
    {
        DIR *d = opendir(TV_CONFIG_DIR);
        if (d) {
            struct dirent *ent;
            int found = 0;
            while ((ent = readdir(d))) {
                if (strstr(ent->d_name, ".conf")) {
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
