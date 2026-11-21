/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * safe_connect.elf: Safe Connect as a PS5 payload. Send it to the payload
 * loader (port 9021) and it blocks system updates on the console itself,
 * the same way the Safe Connect app does over FTP:
 *   - deletes a downloaded update (any PS5UPDATE* file) in /update,
 *   - makes a folder at PS5UPDATE.PUP and PS5UPDATE.PUP.net.temp (and at the
 *     name of any update it deleted), so a download has nowhere to go,
 *   - puts a read-only file "prospero-win-update-blocker" in each folder and
 *     makes the folders read-only.
 * Then it shows a notification with the result and exits.
 *
 * Built three ways from this file (see Makefile):
 *   safe_connect.elf          SC_MODE=1  block
 *   safe_connect_unblock.elf  SC_MODE=2  remove the block
 *   safe_connect_check.elf    SC_MODE=0  only report
 * Each one also sends a one-line report to 127.0.0.1:9077 when something
 * listens there (the Safe Connect pkg app does), e.g.
 *   "SAFECONNECT 1 BLOCKED /update/PS5UPDATE.PUP=blocked /update/PS5UPDATE.PUP.net.temp=blocked\n"
 *
 * On Linux (-DSC_LINUX_TEST) it works under ./test-root/update and prints
 * the notification instead.
 */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef SC_MODE
#define SC_MODE 1
#endif

#ifdef SC_LINUX_TEST
#define UPDATE_DIR "./test-root/update"
#else
#define UPDATE_DIR "/update"
#endif
#define DUMMY "prospero-win-update-blocker"
#define DUMMY_TEXT "Made by Safe Connect so the PS5 can't save a system update here.\n" \
                   "Undo it with safe_connect_unblock.elf or the Safe Connect app.\n"
#define REPORT_PORT 9077
#define MAX_NAMES 16

#ifndef SC_LINUX_TEST
typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
#endif

static void notify(const char *text) {
#ifdef SC_LINUX_TEST
    printf("[notification] %s\n", text);
#else
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message), "%s", text);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
#endif
}

static char names[MAX_NAMES][256];
static int name_count;

static void add_name(const char *name) {
    for (int i = 0; i < name_count; i++)
        if (!strcmp(names[i], name)) return;
    if (name_count < MAX_NAMES) snprintf(names[name_count++], sizeof(names[0]), "%s", name);
}

static int is_update_name(const char *name) {
    return !strncasecmp(name, "PS5UPDATE", 9);
}

/* 'd' folder, 'f' anything else, 0 nothing there */
static char kind(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 'd' : 'f';
}

/* The names an update downloads to, plus any other PS5UPDATE* folder there. */
static int collect_names(int delete_files, char *log, size_t log_size) {
    name_count = 0;
    add_name("PS5UPDATE.PUP");
    add_name("PS5UPDATE.PUP.net.temp");
    DIR *dir = opendir(UPDATE_DIR);
    if (!dir) return -1;
    struct dirent *entry;
    char path[512];
    while ((entry = readdir(dir)) != NULL) {
        if (!is_update_name(entry->d_name)) continue;
        snprintf(path, sizeof(path), "%s/%s", UPDATE_DIR, entry->d_name);
        char k = kind(path);
        if (k == 'f' && delete_files) {
            struct stat st;
            long long size = lstat(path, &st) == 0 ? (long long)st.st_size : 0;
            if (unlink(path) == 0) {
                size_t used = strlen(log);
                snprintf(log + used, log_size - used, "Deleted a downloaded update (%lld MB). ",
                         size / (1024 * 1024));
            }
        }
        if (k == 'd' || (k == 'f' && delete_files)) add_name(entry->d_name);
    }
    closedir(dir);
    return 0;
}

static void block(void) {
    char path[512], dummy[600];
    for (int i = 0; i < name_count; i++) {
        snprintf(path, sizeof(path), "%s/%s", UPDATE_DIR, names[i]);
        if (kind(path) != 'd') mkdir(path, 0755);
        chmod(path, 0755);                     /* writable again for a moment, if it was ours */
        snprintf(dummy, sizeof(dummy), "%s/%s", path, DUMMY);
        chmod(dummy, 0644);
        int fd = open(dummy, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            ssize_t ignored = write(fd, DUMMY_TEXT, sizeof(DUMMY_TEXT) - 1);
            (void)ignored;
            close(fd);
        }
        chmod(dummy, 0444);
        chmod(path, 0555);
    }
}

static void unblock(void) {
    char path[512], dummy[600];
    for (int i = 0; i < name_count; i++) {
        snprintf(path, sizeof(path), "%s/%s", UPDATE_DIR, names[i]);
        if (kind(path) != 'd') continue;
        chmod(path, 0777);
        snprintf(dummy, sizeof(dummy), "%s/%s", path, DUMMY);
        chmod(dummy, 0666);
        unlink(dummy);
        rmdir(path);                           /* a folder holding anything else stays */
    }
}

/* "blocked" / "update file" / "open" for each name; returns 1 if all blocked. */
static int state(char *out, size_t out_size) {
    char path[512];
    int all = name_count > 0;
    out[0] = 0;
    for (int i = 0; i < name_count; i++) {
        snprintf(path, sizeof(path), "%s/%s", UPDATE_DIR, names[i]);
        char k = kind(path);
        const char *s = k == 'd' ? "blocked" : k == 'f' ? "update-file" : "open";
        if (k != 'd') all = 0;
        size_t used = strlen(out);
        snprintf(out + used, out_size - used, " /update/%s=%s", names[i], s);
    }
    return all;
}

static void report(const char *line) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(REPORT_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        ssize_t ignored = write(s, line, strlen(line));
        (void)ignored;
    }
    close(s);
}

int main(void) {
    char log[512] = "", states[2048], line[2300], text[1024];
    if (collect_names(SC_MODE == 1, log, sizeof(log)) != 0) {
        snprintf(line, sizeof(line), "SAFECONNECT %d NOFOLDER %s can't be opened (errno %d)\n",
                 SC_MODE, UPDATE_DIR, errno);
        report(line);
        notify("Safe Connect: /update can't be opened, nothing was changed.");
        return 1;
    }
    if (SC_MODE == 1) block();
    if (SC_MODE == 2) unblock();
    if (SC_MODE == 2) collect_names(0, log, sizeof(log));
    int blocked = state(states, sizeof(states));
    snprintf(line, sizeof(line), "SAFECONNECT %d %s%s\n", SC_MODE, blocked ? "BLOCKED" : "OPEN", states);
    report(line);
    if (SC_MODE == 1)
        snprintf(text, sizeof(text), blocked ? "Safe Connect: protected. %sSystem updates are BLOCKED."
                                             : "Safe Connect: %scould not block every place, updates may still download.",
                 log);
    else if (SC_MODE == 2)
        snprintf(text, sizeof(text), blocked ? "Safe Connect: the block could not be removed."
                                             : "Safe Connect: block removed. The PS5 can download system updates again.");
    else
        snprintf(text, sizeof(text), blocked ? "Safe Connect: system updates are BLOCKED."
                                             : "Safe Connect: system updates are NOT blocked.");
    notify(text);
#ifdef SC_LINUX_TEST
    printf("%s", line);
#endif
    return blocked ? 0 : (SC_MODE == 2 ? 0 : 1);
}
