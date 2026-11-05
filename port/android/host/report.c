/* Bug reports from the in-game menu (vk/overlay.h report): /sdcard/tok/reports/<date-time>/ with what is needed to see
   and replay the moment without adb:
     video/seg-*.rec   the last 30 seconds (recorder.c, when recording is on); the startup screen makes replay.mp4
     frame.ppm         the picture when the menu opened (the backend's backdrop capture); the startup screen makes frame.png
     totk.log          this run's log
     launch.env, debug.env, actions.txt, overlay.env   the settings in use
     save/             a copy of the save folder (the game's own files are only read)
     device.txt        build, GPU and driver, frame rate, device modes (GameActivity.deviceInfo), temperatures
     capture/          with a frame capture: the freeze-frame probe of the next frame (vk/nvn_vulkan_probe.inc:
                       pipeline.tsv with every draw's state and inputs, the images it reads), the game held meanwhile
   The copy runs on a thread of its own; dm_android_report says how far it is. Files are named one by one: nothing may come
   from the app's no-backup folder, which holds the player's prod.keys (Setup.java). */
#include "files.h"
#include "recorder.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void *dm_android_asset(const char *name, size_t *size);
/* app.c: GameActivity.deviceInfo() */
char *dm_android_device_info(void);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char status[300], info[1024];
static int busy, capture;

static void say(const char *fmt, const char *arg) { pthread_mutex_lock(&lock); snprintf(status, sizeof status, fmt, arg); pthread_mutex_unlock(&lock); }

/* files and folders under from; returns the files copied */
static int copy_tree(const char *from, const char *to) {
    DIR *d = opendir(from); if (!d) return 0;
    mkdir(to, 0755); int n = 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[1200], b[1200]; struct stat st;
        snprintf(a, sizeof a, "%s/%s", from, e->d_name); snprintf(b, sizeof b, "%s/%s", to, e->d_name);
        if (stat(a, &st)) continue;
        if (S_ISDIR(st.st_mode)) n += copy_tree(a, b); else n += copy_file(a, b);
    }
    closedir(d); return n;
}
static void remove_tree(const char *path) {
    DIR *d = opendir(path); struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[1200]; struct stat st; snprintf(a, sizeof a, "%s/%s", path, e->d_name);
        if (!lstat(a, &st) && S_ISDIR(st.st_mode)) remove_tree(a); else unlink(a);
    }
    if (d) closedir(d);
    rmdir(path);
}

/* the thermal zones the app may read */
static void temperatures(FILE *f) {
    for (int i = 0; i < 128; i++) {
        char p[96], type[64] = "", temp[32] = ""; FILE *t;
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/type", i); if (!(t = fopen(p, "r"))) continue;
        if (!fgets(type, sizeof type, t)) type[0] = 0; fclose(t);
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/temp", i); if (!(t = fopen(p, "r"))) continue;
        if (!fgets(temp, sizeof temp, t)) temp[0] = 0; fclose(t);
        type[strcspn(type, "\n")] = 0; double c = atol(temp) / 1000.0;
        /* not the power ICs */
        static const char *const wanted[] = {"cpu-", "cpuss-", "gpuss-", "ddr", "battery", "sys-therm-", "video"};
        int keep = 0; for (unsigned k = 0; k < sizeof wanted / sizeof *wanted; k++) keep |= !strncmp(type, wanted[k], strlen(wanted[k]));
        if (keep && c > 0 && c < 150) fprintf(f, "  %-14s %5.1f °C\n", type, c);
    }
}

static void *report_main(void *arg) {
    char *dir = arg, path[900], from[900];
    const char *files = getenv("TOTK_FILES_DIR"), *profile = getenv("TOTK_PROFILE_DIR");
    say("Saving the bug report: %s", "video");
    snprintf(path, sizeof path, "%s/video", dir);
    int segments = recorder_snapshot(path, 30);
    if (!segments) rmdir(path);
    say("Saving the bug report: %s", "log and settings");
    if (files) {
        static const char *const copies[][2] = {{"profile/totk.log", "totk.log"}, {"backdrop.ppm", "frame.ppm"}, {"launch.env", "launch.env"},
            {"debug.env", "debug.env"}, {"actions.txt", "actions.txt"}, {"overlay.env", "overlay.env"}};
        for (unsigned i = 0; i < sizeof copies / sizeof *copies; i++) {
            snprintf(from, sizeof from, "%s/%s", files, copies[i][0]); snprintf(path, sizeof path, "%s/%s", dir, copies[i][1]); copy_file(from, path); }
    }
    say("Saving the bug report: %s", "save data");
    int saves = 0;
    if (profile) { snprintf(from, sizeof from, "%s/save", profile); snprintf(path, sizeof path, "%s/save", dir); saves = copy_tree(from, path); }
    snprintf(path, sizeof path, "%s/device.txt", dir);
    FILE *f = fopen(path, "w");
    if (f) {
        time_t now = time(NULL); char when[64]; strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S %z", localtime(&now));
        size_t n = 0; char *build = dm_android_asset("build.txt", &n), *device = dm_android_device_info();
        fprintf(f, "Bug report %s\nBuild: %s\n%s\n", when, build ? build : "unknown\n", info);
        if (device) fprintf(f, "%s\n", device);
        fprintf(f, "Video: %d segment(s) of the last 30 s%s\nSave files copied: %d\nTemperatures:\n", segments, segments ? "" : " (recording was off)", saves);
        temperatures(f);
        fclose(f); free(build); free(device);
    }
    /* freeze the next frame (hle_nvn.c freeze-frame), probe a replay of it, copy the probe, resume */
    if (capture && files) {
        say("Saving the bug report: %s", "frame capture");
        char freeze[700], trigger[700], probes[700]; int found = 0; time_t started = time(NULL);
        snprintf(freeze, sizeof freeze, "%s/freeze", files); snprintf(trigger, sizeof trigger, "%s/probe.trigger", files); snprintf(probes, sizeof probes, "%s/liveprobe", files);
        FILE *t = fopen(freeze, "w"); if (t) fclose(t);
        /* armed, then a frame recorded, then replaying */
        sleep(2);
        t = fopen(trigger, "w"); if (t) { fputs("\n", t); fclose(t); }
        /* up to a minute; the probe's numbers restart with each run */
        for (int wait = 0; wait < 600 && !found; wait++) {
            usleep(100000);
            DIR *d = opendir(probes); struct dirent *e; struct stat st;
            while (d && !found && (e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                snprintf(from, sizeof from, "%s/%s/done", probes, e->d_name);
                if (!stat(from, &st) && st.st_mtime >= started) {
                    snprintf(from, sizeof from, "%s/%s", probes, e->d_name); snprintf(path, sizeof path, "%s/capture", dir); copy_tree(from, path); remove_tree(from); found = 1;
                }
            }
            if (d) closedir(d);
        }
        unlink(freeze); capture = found;
        if (!found) say("Bug report saved without the frame capture, which did not finish in time: %s", dir), sleep(1);
        /* with the capture's lines */
        snprintf(from, sizeof from, "%s/profile/totk.log", files); snprintf(path, sizeof path, "%s/totk.log", dir); copy_file(from, path);
    }
    say("Bug report saved: %s", dir);
    fprintf(stderr, "[report] saved %s (%d video segments, %d save files%s)\n", dir, segments, saves, capture ? ", frame capture" : "");
    pthread_mutex_lock(&lock); busy = 0; pthread_mutex_unlock(&lock);
    free(dir);
    return NULL;
}

/* start = 1: a new report (unless one is being written), 2: one with a frame capture (the game freezes for a few
   seconds; close the menu first), with details (GPU, driver, frame rate) for device.txt. out gets what the menu
   shows: "" before the first report, then its progress, then where it went. */
void dm_android_report(int start, const char *details, char *out, size_t size) {
    pthread_mutex_lock(&lock);
    if (start && !busy) {
        /* the launcher's choice (ReportStore.ROOT) */
        const char *root = getenv("TOTK_DATA_ROOT"), *reports = getenv("TOTK_REPORTS_DIR"); char base[300], *dir = malloc(400);
        if (reports && *reports) snprintf(base, sizeof base, "%s", reports); else snprintf(base, sizeof base, "%s/reports", root ? root : "/sdcard/tok");
        mkdir(base, 0755);
        time_t now = time(NULL); char stamp[32]; strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
        if (dir) {
            snprintf(dir, 400, "%s/%s", base, stamp);
            if (mkdir(dir, 0755) && errno != EEXIST) { snprintf(status, sizeof status, "Could not create the folder %s: %s", dir, strerror(errno)); free(dir); }
            else {
                snprintf(info, sizeof info, "%s", details ? details : ""); busy = 1; capture = start == 2; snprintf(status, sizeof status, "Saving the bug report");
                pthread_t t; if (pthread_create(&t, NULL, report_main, dir)) { busy = 0; free(dir); snprintf(status, sizeof status, "Could not start saving the bug report."); } else pthread_detach(t);
            }
        }
    }
    if (out && size) snprintf(out, size, "%s", status);
    pthread_mutex_unlock(&lock);
}
