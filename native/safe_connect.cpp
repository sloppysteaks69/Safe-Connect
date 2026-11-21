// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Safe Connect: keeps a jailbroken PS5 from downloading system updates.
// Native Win32 + GDI+ app: one process, no runtime to unpack.
//
// The PS5 saves a downloaded update in /update, as PS5UPDATE.PUP.net.temp
// and then PS5UPDATE.PUP. Over the console's FTP server this app deletes a
// downloaded update there, makes a folder at each of those names (with a
// read-only file inside, so it is never empty) and makes the folders
// read-only, so a download has nowhere to go.
//
// safe-connect.exe --check|--block|--unblock HOST [PORT] runs once without a
// window (exit code 0 = blocked).
#define _USE_MATH_DEFINES
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>
#include <initializer_list>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

using namespace Gdiplus;

// No C++ runtime beyond this: keeps the exe small.
void *operator new(size_t n) { return malloc(n); }
void *operator new[](size_t n) { return malloc(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

#define APP_NAME L"Safe Connect"
#define APP_VERSION L"1.1"
#define IDI_APP 1

// ---------------------------------------------------------------------------
// FTP client (just what the blocker needs: CWD, MLSD, MKD, RMD, DELE, STOR,
// SITE CHMOD). Errors: -1 = the connection failed, -2 = the server refused.
// ---------------------------------------------------------------------------
struct Ftp {
    SOCKET s = INVALID_SOCKET;
    char host[256];
    int port;
    char in[16384];
    int in_len = 0;
    char reply[1024];
};

static SOCKET tcp_connect(const char *host, int port, int timeout_ms) {
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%d", port);
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_text, &hints, &res) != 0 || !res) return INVALID_SOCKET;
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return s; }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int rc = connect(s, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0 && WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(s); return INVALID_SOCKET; }
    if (rc != 0) {
        fd_set w, e;
        FD_ZERO(&w); FD_SET(s, &w); FD_ZERO(&e); FD_SET(s, &e);
        timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        if (select(0, nullptr, &w, &e, &tv) <= 0 || FD_ISSET(s, &e)) { closesocket(s); return INVALID_SOCKET; }
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    DWORD t = (DWORD)timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof(t));
    return s;
}

static bool send_all(SOCKET s, const char *data, int len) {
    while (len > 0) {
        int n = send(s, data, len, 0);
        if (n <= 0) return false;
        data += n; len -= n;
    }
    return true;
}

static int read_line(Ftp &f, char *line, int size) {
    for (;;) {
        char *nl = (char *)memchr(f.in, '\n', f.in_len);
        if (nl) {
            int n = (int)(nl - f.in) + 1;
            int copy = min(n, size - 1);
            memcpy(line, f.in, copy);
            line[copy] = 0;
            while (copy > 0 && (line[copy - 1] == '\n' || line[copy - 1] == '\r')) line[--copy] = 0;
            memmove(f.in, f.in + n, f.in_len - n);
            f.in_len -= n;
            return 0;
        }
        if (f.in_len >= (int)sizeof(f.in) - 1) f.in_len = 0;
        int got = recv(f.s, f.in + f.in_len, (int)sizeof(f.in) - 1 - f.in_len, 0);
        if (got <= 0) return -1;
        f.in_len += got;
    }
}

static int read_reply(Ftp &f) {
    char line[1024];
    if (read_line(f, line, sizeof(line)) < 0) return -1;
    snprintf(f.reply, sizeof(f.reply), "%s", line);
    if (strlen(line) < 3) return -1;
    int code = atoi(line);
    if (line[3] == '-') {
        char end[5];
        snprintf(end, sizeof(end), "%.3s ", line);
        do {
            if (read_line(f, line, sizeof(line)) < 0) return -1;
        } while (strncmp(line, end, 4) != 0);
    }
    return code;
}

static int ftp_cmd(Ftp &f, const char *fmt, ...) {
    char text[1100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text) - 2, fmt, ap);
    va_end(ap);
    strcat(text, "\r\n");
    if (!send_all(f.s, text, (int)strlen(text))) return -1;
    return read_reply(f);
}

static void ftp_close(Ftp &f) {
    if (f.s != INVALID_SOCKET) {
        ftp_cmd(f, "QUIT");
        closesocket(f.s);
        f.s = INVALID_SOCKET;
    }
}

static bool ftp_open(Ftp &f, const char *host, int port, char *error, int error_size) {
    snprintf(f.host, sizeof(f.host), "%s", host);
    f.port = port;
    f.in_len = 0;
    f.s = tcp_connect(host, port, 8000);
    if (f.s == INVALID_SOCKET) {
        snprintf(error, error_size, "no FTP server answers at %s:%d. Check the address and that the "
                                    "PS5's FTP payload is running.", host, port);
        return false;
    }
    int code = read_reply(f);
    if (code / 100 != 2) goto fail;
    code = ftp_cmd(f, "USER anonymous");
    if (code == 331) code = ftp_cmd(f, "PASS anonymous@");
    if (code / 100 != 2) goto fail;
    ftp_cmd(f, "TYPE I");
    return true;
fail:
    snprintf(error, error_size, "the FTP server at %s:%d didn't let us in (%s)", host, port,
             code < 0 ? "no answer" : f.reply);
    closesocket(f.s);
    f.s = INVALID_SOCKET;
    return false;
}

static SOCKET ftp_pasv(Ftp &f) {
    int code = ftp_cmd(f, "PASV");
    if (code != 227) return INVALID_SOCKET;
    const char *p = strchr(f.reply, '(');
    int a[6];
    if (!p || sscanf(p + 1, "%d,%d,%d,%d,%d,%d", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6)
        return INVALID_SOCKET;
    // like Python's ftplib: the control connection's address, not the one in the reply
    return tcp_connect(f.host, a[4] * 256 + a[5], 8000);
}

struct Entry {
    char name[256];
    bool dir;
    long long size;
};

// Lists path (MLSD after CWD, the PS5 server's way). Returns the count, -1, -2.
static int ftp_list(Ftp &f, const char *path, Entry *out, int max_out) {
    int code = ftp_cmd(f, "CWD %s", path);
    if (code < 0) return -1;
    if (code / 100 != 2) return -2;
    SOCKET d = ftp_pasv(f);
    if (d == INVALID_SOCKET) return -1;
    code = ftp_cmd(f, "MLSD");
    if (code < 0) { closesocket(d); return -1; }
    if (code / 100 != 1 && code / 100 != 2) { closesocket(d); return -2; }
    static char *data;
    if (!data) data = (char *)malloc(65536);
    int len = 0;
    for (;;) {
        int got = recv(d, data + len, 65536 - 1 - len, 0);
        if (got <= 0) break;
        len += got;
        if (len >= 65536 - 1) break;
    }
    data[len] = 0;
    closesocket(d);
    if (code / 100 == 1 && read_reply(f) < 0) return -1;
    int count = 0;
    char *outer = nullptr;
    for (char *line = strtok_s(data, "\r\n", &outer); line && count < max_out; line = strtok_s(nullptr, "\r\n", &outer)) {
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        const char *name = sp + 1;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        Entry &e = out[count];
        e.dir = false;
        e.size = 0;
        bool skip = false;
        for (char *fact = strtok_s(line, ";", &sp); fact; fact = strtok_s(nullptr, ";", &sp)) {
            if (!_strnicmp(fact, "type=", 5)) {
                const char *t = fact + 5;
                if (!_stricmp(t, "cdir") || !_stricmp(t, "pdir")) skip = true;
                e.dir = !_stricmp(t, "dir");
            } else if (!_strnicmp(fact, "size=", 5)) {
                e.size = _atoi64(fact + 5);
            }
        }
        if (skip) continue;
        snprintf(e.name, sizeof(e.name), "%s", name);
        count++;
    }
    return count;
}

static int ftp_stor(Ftp &f, const char *path, const char *data, int len) {
    SOCKET d = ftp_pasv(f);
    if (d == INVALID_SOCKET) return -1;
    int code = ftp_cmd(f, "STOR %s", path);
    if (code < 0) { closesocket(d); return -1; }
    if (code / 100 != 1) { closesocket(d); return code / 100 == 2 ? 0 : -2; }
    send_all(d, data, len);
    shutdown(d, SD_SEND);
    closesocket(d);
    code = read_reply(f);
    return code < 0 ? -1 : code / 100 == 2 ? 0 : -2;
}

// ---------------------------------------------------------------------------
// The blocker
// ---------------------------------------------------------------------------
static const char *UPDATE_DIR = "/update";
static const char *UPDATE_NAMES[] = {"PS5UPDATE.PUP", "PS5UPDATE.PUP.net.temp"};
static const char *UPDATE_DUMMY = "prospero-win-update-blocker";
static const char UPDATE_DUMMY_TEXT[] =
    "Made by Safe Connect so the PS5 can't save a system update here.\n"
    "Undo it in the app: Remove blocker.\n";

enum State { SC_BLOCKED, SC_OPEN, SC_FILE, SC_NOFOLDER };
#define MAX_STATES 16
struct States {
    int count = 0;
    char path[MAX_STATES][300];
    State state[MAX_STATES];
};

typedef void (*SayFn)(const char *line, void *ctx);

static Entry *g_entries;          // 512, allocated at start (kept out of the exe file)

static bool is_update_name(const char *name) { return !_strnicmp(name, "PS5UPDATE", 9); }

static void add_name(char names[][256], int &count, const char *name) {
    for (int i = 0; i < count; i++)
        if (!strcmp(names[i], name)) return;
    if (count < MAX_STATES) snprintf(names[count++], 256, "%s", name);
}

// 'd', 'f' or 0 for name in folder; -1 connection failed
static int kind(Ftp &f, const char *folder, const char *name) {
    int n = ftp_list(f, folder, g_entries, 512);
    if (n == -1) return -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(g_entries[i].name, name)) return g_entries[i].dir ? 'd' : 'f';
    return 0;
}

static int update_names(Ftp &f, char names[][256]) {
    int count = 0;
    for (const char *n : UPDATE_NAMES) add_name(names, count, n);
    int n = ftp_list(f, UPDATE_DIR, g_entries, 512);
    for (int i = 0; i < n; i++)
        if (g_entries[i].dir && is_update_name(g_entries[i].name)) add_name(names, count, g_entries[i].name);
    return count;
}

static bool blocker_state(Ftp &f, States &st) {
    st.count = 0;
    int n = ftp_list(f, UPDATE_DIR, g_entries, 512);
    if (n == -1) return false;
    if (n == -2) {
        for (const char *name : UPDATE_NAMES) {
            snprintf(st.path[st.count], 300, "%s/%s", UPDATE_DIR, name);
            st.state[st.count++] = SC_NOFOLDER;
        }
        return true;
    }
    char names[MAX_STATES][256];
    int count = 0;
    for (const char *name : UPDATE_NAMES) add_name(names, count, name);
    for (int i = 0; i < n; i++)
        if (g_entries[i].dir && is_update_name(g_entries[i].name)) add_name(names, count, g_entries[i].name);
    for (int i = 0; i < count; i++) {
        State s = SC_OPEN;
        for (int j = 0; j < n; j++)
            if (!strcmp(g_entries[j].name, names[i])) s = g_entries[j].dir ? SC_BLOCKED : SC_FILE;
        snprintf(st.path[st.count], 300, "%s/%s", UPDATE_DIR, names[i]);
        st.state[st.count++] = s;
    }
    return true;
}

static bool block_updates(Ftp &f, States &st, SayFn say, void *ctx) {
    char line[400], path[300], dummy[400];
    int n = ftp_list(f, UPDATE_DIR, g_entries, 512);
    if (n == -1) return false;
    if (n == -2) {
        snprintf(line, sizeof(line), "%s can't be opened over FTP", UPDATE_DIR);
        say(line, ctx);
    } else {
        char names[MAX_STATES][256];
        int count = 0;
        for (const char *name : UPDATE_NAMES) add_name(names, count, name);
        // a finished or half-finished download: gone, so it can't install; its name gets blocked too
        static Entry *found;
        if (!found) found = (Entry *)malloc(sizeof(Entry) * 256);
        int found_n = min(n, 256);
        memcpy(found, g_entries, sizeof(Entry) * found_n);
        for (int i = 0; i < found_n; i++) {
            if (found[i].dir || !is_update_name(found[i].name)) continue;
            snprintf(line, sizeof(line), "Deleting the downloaded update %s/%s (%lld bytes)", UPDATE_DIR,
                     found[i].name, found[i].size);
            say(line, ctx);
            snprintf(path, sizeof(path), "%s/%s", UPDATE_DIR, found[i].name);
            if (ftp_cmd(f, "DELE %s", path) < 0) return false;
            add_name(names, count, found[i].name);
        }
        for (int i = 0; i < count; i++) {
            snprintf(path, sizeof(path), "%s/%s", UPDATE_DIR, names[i]);
            snprintf(line, sizeof(line), "Blocking %s", path);
            say(line, ctx);
            int k = kind(f, UPDATE_DIR, names[i]);
            if (k < 0) return false;
            if (k != 'd' && ftp_cmd(f, "MKD %s", path) < 0) return false;
            snprintf(dummy, sizeof(dummy), "%s/%s", path, UPDATE_DUMMY);
            if (ftp_stor(f, dummy, UPDATE_DUMMY_TEXT, (int)sizeof(UPDATE_DUMMY_TEXT) - 1) == -1) return false;
            if (ftp_cmd(f, "SITE CHMOD 444 %s", dummy) < 0) return false;
            if (ftp_cmd(f, "SITE CHMOD 555 %s", path) < 0) return false;
        }
    }
    ftp_cmd(f, "CWD /");
    return blocker_state(f, st);
}

static bool unblock_updates(Ftp &f, States &st, SayFn say, void *ctx) {
    char line[400], path[300], dummy[400], names[MAX_STATES][256];
    int count = update_names(f, names);
    for (int i = 0; i < count; i++) {
        int k = kind(f, UPDATE_DIR, names[i]);
        if (k < 0) return false;
        if (k != 'd') continue;
        snprintf(path, sizeof(path), "%s/%s", UPDATE_DIR, names[i]);
        snprintf(line, sizeof(line), "Removing %s", path);
        say(line, ctx);
        snprintf(dummy, sizeof(dummy), "%s/%s", path, UPDATE_DUMMY);
        ftp_cmd(f, "SITE CHMOD 777 %s", path);
        ftp_cmd(f, "SITE CHMOD 666 %s", dummy);
        ftp_cmd(f, "DELE %s", dummy);
        ftp_cmd(f, "CWD /");
        int code = ftp_cmd(f, "RMD %s", path);
        if (code < 0) return false;
        if (code / 100 != 2) {
            snprintf(line, sizeof(line), "%s was left (not empty, or the server refused)", path);
            say(line, ctx);
        }
    }
    return blocker_state(f, st);
}

static bool all_blocked(const States &st) {
    if (st.count == 0) return false;
    for (int i = 0; i < st.count; i++)
        if (st.state[i] != SC_BLOCKED) return false;
    return true;
}

static bool needs_block(const States &st) {
    for (int i = 0; i < st.count; i++)
        if (st.state[i] == SC_OPEN || st.state[i] == SC_FILE) return true;
    return false;
}

static const char *state_text(State s) {
    switch (s) {
    case SC_BLOCKED: return "blocked";
    case SC_OPEN: return "open";
    case SC_FILE: return "update file";
    default: return "no folder";
    }
}

// ---------------------------------------------------------------------------
// Settings: %LOCALAPPDATA%\safe-connect\settings.ini
// ---------------------------------------------------------------------------
struct Settings {
    wchar_t host[128] = L"0.0.0.0";
    wchar_t port[16] = L"2121";
    bool watch = true;
    bool auto_block = true;
    int interval = 60;
};
static Settings g_set;
static wchar_t g_ini[MAX_PATH];

static void settings_path() {
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) wcscpy(base, L".");
    swprintf(g_ini, MAX_PATH, L"%ls\\safe-connect", base);
    CreateDirectoryW(g_ini, nullptr);
    wcscat(g_ini, L"\\settings.ini");
}

static void load_settings() {
    settings_path();
    GetPrivateProfileStringW(L"safe-connect", L"host", L"0.0.0.0", g_set.host, 128, g_ini);
    GetPrivateProfileStringW(L"safe-connect", L"port", L"2121", g_set.port, 16, g_ini);
    if (!g_set.host[0]) wcscpy(g_set.host, L"0.0.0.0");
    if (!g_set.port[0]) wcscpy(g_set.port, L"2121");
    g_set.watch = GetPrivateProfileIntW(L"safe-connect", L"watch", 1, g_ini) != 0;
    g_set.auto_block = GetPrivateProfileIntW(L"safe-connect", L"auto_block", 1, g_ini) != 0;
    g_set.interval = GetPrivateProfileIntW(L"safe-connect", L"interval", 60, g_ini);
    if (g_set.interval != 30 && g_set.interval != 60 && g_set.interval != 300 && g_set.interval != 900)
        g_set.interval = 60;
}

static void save_settings() {
    wchar_t num[16];
    WritePrivateProfileStringW(L"safe-connect", L"host", g_set.host, g_ini);
    WritePrivateProfileStringW(L"safe-connect", L"port", g_set.port, g_ini);
    WritePrivateProfileStringW(L"safe-connect", L"watch", g_set.watch ? L"1" : L"0", g_ini);
    WritePrivateProfileStringW(L"safe-connect", L"auto_block", g_set.auto_block ? L"1" : L"0", g_ini);
    swprintf(num, 16, L"%d", g_set.interval);
    WritePrivateProfileStringW(L"safe-connect", L"interval", num, g_ini);
}

// ---------------------------------------------------------------------------
// Window state
// ---------------------------------------------------------------------------
struct Rgb { int r, g, b; };
static const Rgb BG = {0x07, 0x0B, 0x14}, PANEL = {0x0E, 0x15, 0x26}, EDGE = {0x1C, 0x2A, 0x48},
                 TEXT = {0xDC, 0xE6, 0xFF}, DIM = {0x70, 0x83, 0xAD}, CYAN = {0x29, 0xD3, 0xFF},
                 GREEN = {0x2B, 0xFF, 0x88}, RED = {0xFF, 0x3B, 0x5C}, AMBER = {0xFF, 0xB0, 0x20};

static Rgb hex(unsigned v) { return {(int)(v >> 16) & 255, (int)(v >> 8) & 255, (int)v & 255}; }
static Rgb mix(Rgb a, Rgb b, double t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return {(int)lround(a.r + (b.r - a.r) * t), (int)lround(a.g + (b.g - a.g) * t),
            (int)lround(a.b + (b.b - a.b) * t)};
}
static Color col(Rgb c, int alpha = 255) { return Color((BYTE)alpha, (BYTE)c.r, (BYTE)c.g, (BYTE)c.b); }

enum Mode { M_IDLE, M_BUSY, M_BLOCKED, M_OPEN, M_ERROR };
enum Action { A_CHECK, A_BLOCK, A_UNBLOCK, A_AUTO, A_REBLOCK };

#define WM_APP_LOG (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)
#define WM_APP_FAIL (WM_APP + 3)
#define TIMER_ANIM 1
#define TIMER_TICK 2

static HINSTANCE g_inst;
static HWND g_wnd, g_host_edit, g_port_edit;
static HFONT g_edit_font;
static HBRUSH g_edit_brush;
static double g_scale = 1.0;
static Mode g_mode = M_IDLE;
static int g_shown = -1;                // -1 none, M_BLOCKED, M_OPEN
static double g_anim_start = -1;        // seconds; -1 = no animation running
static double g_phase = 0;
static bool g_busy = false;
static double g_next_check = 0;
static States g_states;
static int g_hover = -1;
static wchar_t g_countdown[96];
static const double ANIM_LENGTH = 5.0;

static double now_s() {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

#define LOG_MAX 60
struct LogLine { wchar_t text[300]; Rgb colour; };
static LogLine *g_log;             // LOG_MAX, allocated at start
static int g_log_count;

static void log_line(const wchar_t *text, Rgb colour) {
    if (g_log_count == LOG_MAX) {
        memmove(g_log, g_log + 1, sizeof(LogLine) * (LOG_MAX - 1));
        g_log_count--;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf(g_log[g_log_count].text, 300, L"%02d:%02d:%02d  %ls", t.wHour, t.wMinute, t.wSecond, text);
    g_log[g_log_count].colour = colour;
    g_log_count++;
    if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
}

// Layout, in 96-dpi units
static const int W = 760, H = 760;
static const RectF R_CONN(18, 58, 724, 52), R_SCENE(18, 118, 724, 280), R_AUTO(18, 456, 724, 62);
struct Button { RectF r; int id; };
enum { B_CHECK, B_BLOCK, B_REMOVE, B_WATCH, B_INTERVAL, B_AUTO, B_COUNT };
static Button g_buttons[B_COUNT] = {
    {RectF(452, 68, 92, 32), B_CHECK},     {RectF(186, 406, 182, 42), B_BLOCK},
    {RectF(388, 406, 182, 42), B_REMOVE},  {RectF(28, 464, 230, 20), B_WATCH},
    {RectF(262, 462, 74, 24), B_INTERVAL}, {RectF(28, 490, 640, 20), B_AUTO},
};

static bool host_set(const wchar_t *host) { return host[0] && wcscmp(host, L"0.0.0.0") != 0; }

static void read_fields() {
    GetWindowTextW(g_host_edit, g_set.host, 128);
    GetWindowTextW(g_port_edit, g_set.port, 16);
    // trim
    for (wchar_t *s : {g_set.host, g_set.port}) {
        int n = (int)wcslen(s);
        while (n && iswspace(s[n - 1])) s[--n] = 0;
        int i = 0;
        while (s[i] && iswspace(s[i])) i++;
        if (i) memmove(s, s + i, (wcslen(s + i) + 1) * sizeof(wchar_t));
    }
}

static void saved() {
    read_fields();
    save_settings();
    g_next_check = now_s() + g_set.interval;
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------
struct Job {
    Action action;
    char host[128];
    int port;
    bool auto_block;
};
struct Result {
    Action action;
    States states;
    char error[300];
};

static void post_log(const char *line, Rgb colour) {
    wchar_t *w = (wchar_t *)malloc(600);
    MultiByteToWideChar(CP_UTF8, 0, line, -1, w, 300);
    PostMessageW(g_wnd, WM_APP_LOG, (WPARAM)((colour.r << 16) | (colour.g << 8) | colour.b), (LPARAM)w);
}
static void say_gui(const char *line, void *) { post_log(line, TEXT); }

static DWORD WINAPI worker(void *arg) {
    Job *job = (Job *)arg;
    Result *res = (Result *)calloc(1, sizeof(Result));
    res->action = job->action;
    Ftp *f = new Ftp();
    bool ok = ftp_open(*f, job->host, job->port, res->error, sizeof(res->error));
    if (ok) {
        if (job->action == A_BLOCK) ok = block_updates(*f, res->states, say_gui, nullptr);
        else if (job->action == A_UNBLOCK) ok = unblock_updates(*f, res->states, say_gui, nullptr);
        else {
            ok = blocker_state(*f, res->states);
            if (ok && job->action == A_AUTO && job->auto_block && needs_block(res->states)) {
                post_log("The PS5 cleared the blocker. Putting it back...", AMBER);
                ok = block_updates(*f, res->states, say_gui, nullptr);
                res->action = A_REBLOCK;
            }
        }
        if (!ok) snprintf(res->error, sizeof(res->error), "the FTP connection broke");
        ftp_close(*f);
    }
    delete f;
    PostMessageW(g_wnd, ok ? WM_APP_DONE : WM_APP_FAIL, 0, (LPARAM)res);
    free(job);
    return 0;
}

static void run(Action action, bool quiet) {
    if (g_busy) return;
    read_fields();
    if (!host_set(g_set.host)) {
        if (!quiet)
            MessageBoxW(g_wnd, L"Type the PS5's IP address first (0.0.0.0 is only a placeholder).", APP_NAME,
                        MB_ICONERROR);
        return;
    }
    wchar_t *end;
    long port = g_set.port[0] ? wcstol(g_set.port, &end, 10) : 2121;
    if (g_set.port[0] && (*end || port <= 0 || port > 65535)) {
        if (!quiet) {
            wchar_t text[200];
            swprintf(text, 200, L"'%ls' is not a port number (the PS5's FTP payload usually uses 2121).", g_set.port);
            MessageBoxW(g_wnd, text, APP_NAME, MB_ICONERROR);
        }
        return;
    }
    saved();
    g_busy = true;
    g_mode = M_BUSY;
    const wchar_t *verb = action == A_BLOCK ? L"Blocking updates on" : action == A_UNBLOCK ? L"Removing the blocker on"
                                                                                            : L"Checking";
    wchar_t text[300];
    swprintf(text, 300, L"%ls %ls:%ld...", verb, g_set.host, port);
    log_line(text, CYAN);
    Job *job = (Job *)calloc(1, sizeof(Job));
    job->action = action;
    WideCharToMultiByte(CP_UTF8, 0, g_set.host, -1, job->host, sizeof(job->host), nullptr, nullptr);
    job->port = (int)port;
    job->auto_block = g_set.auto_block;
    HANDLE t = CreateThread(nullptr, 0, worker, job, 0, nullptr);
    if (t) CloseHandle(t);
    InvalidateRect(g_wnd, nullptr, FALSE);
}

static void ready() {
    g_busy = false;
    g_next_check = now_s() + g_set.interval;
}

static void finished(Result *r) {
    ready();
    g_states = r->states;
    bool blocked = all_blocked(g_states);
    g_mode = blocked ? M_BLOCKED : M_OPEN;
    if (blocked && g_shown != M_BLOCKED) g_anim_start = now_s();     // the protection animation
    else if (!blocked) g_anim_start = -1;
    g_shown = g_mode;
    if (r->action == A_BLOCK || r->action == A_REBLOCK) {
        if (blocked) { log_line(L"Blocker ON. The PS5 can't download system updates.", GREEN); }
        else { log_line(L"Some places could not be blocked (see the list).", RED); }
    } else if (r->action == A_UNBLOCK) {
        log_line(L"Blocker removed. The PS5 can download system updates again.", AMBER);
    } else if (blocked) {
        log_line(L"Blocker is on.", GREEN);
    } else {
        bool file = false, none = g_states.count > 0;
        for (int i = 0; i < g_states.count; i++) {
            if (g_states.state[i] == SC_FILE) file = true;
            if (g_states.state[i] != SC_NOFOLDER) none = false;
        }
        if (file) {
            log_line(L"An update is already DOWNLOADED on the PS5. Press BLOCK UPDATES to delete it and block.", RED);
        } else if (none) {
            log_line(L"/update can't be opened over FTP. The FTP payload may not have full access.", AMBER);
        } else {
            log_line(L"Blocker is OFF. The PS5 can download system updates. Press BLOCK UPDATES.", RED);
        }
    }
    InvalidateRect(g_wnd, nullptr, FALSE);
}

static void failed(Result *r) {
    ready();
    g_mode = M_ERROR;
    wchar_t text[400];
    swprintf(text, 400, L"Couldn't reach the PS5: %hs", r->error);
    log_line(text, AMBER);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static FontFamily *g_ui, *g_ui_bold_family, *g_mono, *g_symbol;

static void text(Graphics &g, const wchar_t *s, float x, float y, float px, Rgb c, bool bold = false,
                 int align = 0, FontFamily *family = nullptr, float wrap_width = 0) {
    Font font(family ? family : g_ui, px, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    SolidBrush brush(col(c));
    StringFormat fmt;
    fmt.SetAlignment(align == 0 ? StringAlignmentNear : align == 1 ? StringAlignmentCenter : StringAlignmentFar);
    if (wrap_width > 0) {
        g.DrawString(s, -1, &font, RectF(x, y, wrap_width, 200), &fmt, &brush);
    } else {
        fmt.SetFormatFlags(StringFormatFlagsNoWrap);
        g.DrawString(s, -1, &font, PointF(x, y), &fmt, &brush);
    }
}

static void panel(Graphics &g, RectF r) {
    SolidBrush fill(col(PANEL));
    g.FillRectangle(&fill, r);
    Pen pen(col(EDGE), 1);
    g.DrawRectangle(&pen, r);
}

static PointF mid(PointF a, PointF b) { return PointF((a.X + b.X) / 2, (a.Y + b.Y) / 2); }

// Tk's "smooth" curves: quadratic B-splines through the midpoints.
static void smooth_closed(GraphicsPath &path, const PointF *p, int n) {
    for (int i = 0; i < n; i++) {
        PointF a = mid(p[(i + n - 1) % n], p[i]), b = mid(p[i], p[(i + 1) % n]), c = p[i];
        PointF c1(a.X + 2.0f / 3 * (c.X - a.X), a.Y + 2.0f / 3 * (c.Y - a.Y));
        PointF c2(b.X + 2.0f / 3 * (c.X - b.X), b.Y + 2.0f / 3 * (c.Y - b.Y));
        path.AddBezier(a, c1, c2, b);
    }
    path.CloseFigure();
}

static void smooth_open(GraphicsPath &path, const PointF *p, int n) {
    if (n < 3) { path.AddLines(p, n); return; }
    for (int i = 1; i < n - 1; i++) {
        PointF a = i == 1 ? p[0] : mid(p[i - 1], p[i]);
        PointF b = i == n - 2 ? p[n - 1] : mid(p[i], p[i + 1]);
        PointF c = p[i];
        PointF c1(a.X + 2.0f / 3 * (c.X - a.X), a.Y + 2.0f / 3 * (c.Y - a.Y));
        PointF c2(b.X + 2.0f / 3 * (c.X - b.X), b.Y + 2.0f / 3 * (c.Y - b.Y));
        path.AddBezier(a, c1, c2, b);
    }
}

static double ease(double x) {
    x = x < 0 ? 0 : x > 1 ? 1 : x;
    return x * x * (3 - 2 * x);
}

static void ellipse(Graphics &g, float x0, float y0, float x1, float y1, Rgb *fill, int fill_alpha, Rgb *line,
                    float width) {
    if (fill) { SolidBrush b(col(*fill, fill_alpha)); g.FillEllipse(&b, x0, y0, x1 - x0, y1 - y0); }
    if (line) { Pen p(col(*line), width); g.DrawEllipse(&p, x0, y0, x1 - x0, y1 - y0); }
}

static void rect(Graphics &g, float x0, float y0, float x1, float y1, Rgb *fill, int fill_alpha, Rgb *line,
                 float width) {
    if (fill) { SolidBrush b(col(*fill, fill_alpha)); g.FillRectangle(&b, x0, y0, x1 - x0, y1 - y0); }
    if (line) { Pen p(col(*line), width); g.DrawRectangle(&p, x0, y0, x1 - x0, y1 - y0); }
}

static void line(Graphics &g, Rgb c, float width, std::initializer_list<PointF> pts, bool smooth = false,
                 bool round = false) {
    Pen pen(col(c), width);
    if (round) { pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound); pen.SetLineJoin(LineJoinRound); }
    PointF p[16];
    int n = 0;
    for (const PointF &q : pts) p[n++] = q;
    if (smooth) {
        GraphicsPath path;
        smooth_open(path, p, n);
        g.DrawPath(&pen, &path);
    } else {
        g.DrawLines(&pen, p, n);
    }
}

static void draw_scene(Graphics &g) {
    const float w = R_SCENE.Width, h = R_SCENE.Height, cx = floorf(w / 2);
    GraphicsState saved_state = g.Save();
    g.TranslateTransform(R_SCENE.X, R_SCENE.Y);
    g.SetClip(RectF(0, 0, w, h));
    double t = g_anim_start < 0 ? -1 : now_s() - g_anim_start;
    double dx = 0, gr = 0, roll = 0, ring = -1, wrapper = -1, tear = 0, drop = 0;
    if (g_shown == M_BLOCKED) {
        if (t < 0 || t > ANIM_LENGTH) {
            gr = 1; roll = 1;
        } else {
            dx = t < 3.4 ? 70 * ease(t / 0.8) : 70 * (1 - ease((t - 3.4) / 0.8));
            gr = ease((t - 4.2) / 0.8);
            if (t >= 0.8 && t < 3.4) {
                wrapper = ease((t - 0.8) / 0.4);
                tear = ease((t - 1.3) / 0.5);
                drop = ease((t - 2.6) / 0.8);
            }
            if (t >= 1.8 && t < 2.6) ring = ease((t - 1.8) / 0.8);
            roll = ease((t - 2.6) / 0.8);
        }
    }
    double pulse = (sin(g_phase * (gr < 0.5 ? 2.4 : 1.0)) + 1) / 2;
    Rgb light = mix(hex(0xFF8A9A), hex(0x9CFFC4), gr), mid_c = mix(hex(0xC0263E), hex(0x1FA35C), gr),
        dark = mix(hex(0x3A0A12), hex(0x08361E), gr), edge = mix(RED, GREEN, gr),
        grid = mix(hex(0x1A0A10), hex(0x0A1A12), gr);
    SolidBrush bg(col(BG));
    g.FillRectangle(&bg, 0.0f, 0.0f, w, h);
    Pen grid_pen(col(grid), 1);
    for (float x = 0; x < w; x += 28) g.DrawLine(&grid_pen, x, 0.0f, x, h);
    for (float y = 0; y < h; y += 28) g.DrawLine(&grid_pen, 0.0f, y, w, y);

    const float x0 = cx - 100, top = 8, k = 0.25f;
    auto P = [&](float x, float y) { return PointF(x0 + (x - 855) * k, top + (y - 60) * k); };
    // glow
    for (int i = 0; i < 3; i++) {
        float r = (float)(104 + i * 14 + pulse * 5), gx = x0, gy = top + 440 * k;
        Rgb c = mix(edge, BG, 0.35 + i * 0.22);
        ellipse(g, gx - r * 0.7f, gy - r, gx + r * 0.7f, gy + r, nullptr, 0, &c, 2);
    }
    // stand
    {
        PointF p[] = {P(700, 900), P(1010, 900), P(1022, 958), P(690, 958)};
        GraphicsPath path;
        smooth_closed(path, p, 4);
        SolidBrush b(col(mix(dark, {0, 0, 0}, 0.3)));
        Pen pen(col(mix(edge, BG, 0.5)), 1);
        g.FillPath(&b, &path);
        g.DrawPath(&pen, &path);
    }
    // black core
    {
        PointF p[] = {P(780, 95), P(910, 95), P(920, 250), P(916, 450), P(898, 700),
                      P(882, 940), P(786, 940), P(790, 700), P(806, 450), P(792, 250)};
        SolidBrush b(col(dark));
        Pen pen(col(mix(edge, BG, 0.4)), 1);
        g.FillPolygon(&b, p, 10);
        g.DrawPolygon(&pen, p, 10);
        Rgb side = mix(hex(0x7A1A34), hex(0x1A7A4A), gr);
        line(g, side, 2, {P(792, 250), P(806, 450), P(790, 700), P(786, 900)}, true);
        line(g, side, 2, {P(920, 250), P(916, 450), P(898, 700), P(884, 900)}, true);
    }
    // front USB ports
    {
        Rgb black = hex(0x05070C);
        PointF a = P(843, 522), b = P(857, 552), c = P(845, 575), d = P(855, 598);
        rect(g, a.X, a.Y, b.X, b.Y, &black, 255, &mid_c, 1);
        ellipse(g, c.X, c.Y, d.X, d.Y, &black, 255, &mid_c, 1);
    }
    // the two white side plates
    {
        PointF left[] = {P(730, 56), P(752, 150), P(774, 300), P(786, 450), P(770, 600), P(746, 750), P(728, 880),
                         P(730, 942), P(794, 942), P(798, 700), P(814, 450), P(800, 250), P(790, 140), P(788, 104),
                         P(768, 78)};
        PointF right[] = {P(952, 56), P(936, 150), P(926, 300), P(934, 450), P(958, 600), P(990, 750),
                          P(1015, 880), P(1018, 942), P(874, 942), P(890, 700), P(908, 450), P(912, 250),
                          P(904, 130), P(900, 104), P(922, 78)};
        for (PointF *plate : {left, right}) {
            GraphicsPath path;
            smooth_closed(path, plate, 15);
            SolidBrush b(col(light));
            Pen pen(col(edge), 2);
            g.FillPath(&b, &path);
            g.DrawPath(&pen, &path);
        }
    }
    line(g, mid_c, 3, {P(924, 612), P(936, 905)});
    // power light, blinks while talking to the PS5
    bool blink = g_mode == M_BUSY && ((int)(g_phase * 4)) % 2 == 0;
    line(g, blink ? CYAN : mix(edge, {255, 255, 255}, 0.3), 2, {P(800, 470), P(793, 690)});
    // the network port, at the back: the cable comes out past the right plate
    const float port_x = x0 + (968 - 855) * k, port_y = top + (640 - 60) * k;
    {
        Rgb black = hex(0x05070C);
        rect(g, port_x - 4, port_y - 9, port_x + 4, port_y + 9, &black, 255, &mid_c, 1);
    }
    // the cable and its RJ45 plug
    const float px = (float)(port_x + 2 + dx);
    Rgb cable = mix(hex(0xD0304A), hex(0x22C46E), gr);
    line(g, mix(cable, BG, 0.45), 10,
         {PointF(px + 30, port_y), PointF(px + 70, port_y), PointF(cx + 90, port_y + 20), PointF(cx + 140, h - 20),
          PointF(w + 10, h - 6)}, true, true);
    line(g, cable, 6,
         {PointF(px + 30, port_y), PointF(px + 70, port_y), PointF(cx + 90, port_y + 20), PointF(cx + 140, h - 20),
          PointF(w + 10, h - 6)}, true, true);
    Rgb plug = mix(hex(0xFFC2CB), hex(0xC8FFE0), gr), gold = hex(0xD4A017);
    rect(g, px, port_y - 8, px + 30, port_y + 8, &plug, 255, &edge, 2);
    rect(g, px + 6, port_y - 13, px + 20, port_y - 8, &plug, 255, &edge, 1);
    for (int i = 0; i < 4; i++)
        line(g, gold, 1, {PointF(px + 3, port_y - 5 + i * 3.4f), PointF(px + 9, port_y - 5 + i * 3.4f)});
    // the protection, rolled onto the plug
    Rgb latex = mix(hex(0xFFB3C8), hex(0x7DFFB0), gr);
    if (roll > 0) {
        float length = (float)(46 * roll), tip = px - 4;
        rect(g, tip, port_y - 11, tip + length, port_y + 11, &latex, 128, &latex, 2);
        ellipse(g, tip - 8, port_y - 5, tip + 2, port_y + 5, &latex, 128, &latex, 2);
        ellipse(g, tip + length - 4, port_y - 13, tip + length + 4, port_y + 13, nullptr, 0, &latex, 3);
    }
    // the wrapper, torn open, then dropped
    if (wrapper >= 0) {
        float size = (float)(66 * (0.4 + 0.6 * wrapper)), wx = cx + 110, wy = (float)(46 + drop * 220);
        Rgb foil = mix(hex(0xE8C860), hex(0x9AA4B8), 0.3), foil_edge = hex(0x8A6D1A);
        PointF pts[16];
        int n = 0;
        for (int i = 0; i <= 7; i++) pts[n++] = PointF(wx - size / 2 + size * i / 7, wy - size / 2 + (i % 2 ? 4 : 0) + 12);
        for (int i = 0; i <= 7; i++) pts[n++] = PointF(wx + size / 2 - size * i / 7, wy + size / 2 - (i % 2 ? 4 : 0));
        SolidBrush fb(col(foil));
        Pen fp(col(foil_edge), 2);
        g.FillPolygon(&fb, pts, n);
        g.DrawPolygon(&fp, pts, n);
        if (ring < 0 && drop < 0.05)
            ellipse(g, wx - size / 3, wy - size / 3 + 8, wx + size / 3, wy + size / 3 + 4, nullptr, 0, &foil_edge, 2);
        float lift = (float)(tear * 60);
        PointF strip[] = {PointF(wx - size / 2 + lift * 1.2f, wy - size / 2 - lift),
                          PointF(wx + size / 2 + lift * 1.2f, wy - size / 2 - lift),
                          PointF(wx + size / 2 + lift * 1.2f, wy - size / 2 + 12 - lift),
                          PointF(wx - size / 2 + lift * 1.2f, wy - size / 2 + 12 - lift)};
        g.FillPolygon(&fb, strip, 4);
        g.DrawPolygon(&fp, strip, 4);
        Font font(g_ui, 7 * 4.0f / 3, FontStyleBold, UnitPixel);
        SolidBrush tb(col(hex(0x5A4210)));
        StringFormat fmt;
        fmt.SetAlignment(StringAlignmentCenter);
        fmt.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(L"SAFE\nCONNECT", -1, &font, RectF(wx - 40, wy + 14 - 20, 80, 40), &fmt, &tb);
    }
    // the ring travelling from the wrapper to the plug
    if (ring >= 0) {
        float sx = cx + 110, sy = 52, ex = px - 4, ey = port_y;
        float rx = (float)(sx + (ex - sx) * ring), ry = (float)(sy + (ey - sy) * ring - sin(ring * M_PI) * 40);
        float hw = (float)(22 + (5 - 22) * ring), hh = (float)(8 + (14 - 8) * ring);
        ellipse(g, rx - hw, ry - hh, rx + hw, ry + hh, nullptr, 0, &latex, 4);
    }
    // sparkles once it's on
    if (g_shown == M_BLOCKED && t >= 0 && t > ANIM_LENGTH - 1.0 && t < ANIM_LENGTH + 2.0) {
        double kk = t - (ANIM_LENGTH - 1.0);
        for (int i = 0; i < 7; i++) {
            double a = i * 0.9 + kk;
            float sx = (float)(port_x + 30 + cos(a) * (30 + kk * 18)), sy = (float)(port_y - 10 - kk * 26 - i * 6);
            float size = (float)((10 + (i % 3) * 2) * 4.0 / 3);
            text(g, i % 2 ? L"✦" : L"♥", sx, sy - size * 0.6f, size, GREEN, false, 1, g_symbol);
        }
    }
    const wchar_t *title;
    Rgb tc;
    if (g_shown == M_BLOCKED) {
        bool done = t < 0 || t > 4.2;
        title = done ? L"PROTECTED: SYSTEM UPDATES BLOCKED" : L"PUTTING PROTECTION ON…";
        tc = done ? GREEN : CYAN;
    } else if (g_shown == M_OPEN) {
        title = L"UNPROTECTED: SYSTEM UPDATES CAN DOWNLOAD"; tc = RED;
    } else {
        title = L"NOT CHECKED YET"; tc = DIM;
    }
    if (g_mode == M_BUSY) { title = L"TALKING TO THE PS5…"; tc = CYAN; }
    else if (g_mode == M_ERROR) { title = L"PS5 NOT REACHABLE"; tc = AMBER; }
    text(g, title, cx, h - 14 - 13, 15 * 4.0f / 3, tc, true, 1);
    g.Restore(saved_state);
}

static void draw_button(Graphics &g, const Button &b, const wchar_t *label, Rgb c, float px) {
    bool hover = g_hover == b.id && !g_busy;
    if (g_busy) c = mix(c, BG, 0.5);
    SolidBrush fill(col(hover ? c : BG));
    g.FillRectangle(&fill, b.r);
    Pen pen(col(c), 2);
    g.DrawRectangle(&pen, RectF(b.r.X + 1, b.r.Y + 1, b.r.Width - 2, b.r.Height - 2));
    Font font(g_ui, px, FontStyleBold, UnitPixel);
    SolidBrush tb(col(hover ? BG : c));
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(label, -1, &font, b.r, &fmt, &tb);
}

static void draw_toggle(Graphics &g, const Button &b, const wchar_t *label, bool on) {
    RectF box(b.r.X + 2, b.r.Y + 3, 13, 13);
    SolidBrush fill(col(BG));
    g.FillRectangle(&fill, box);
    Pen pen(col(g_hover == b.id ? CYAN : DIM), 1);
    g.DrawRectangle(&pen, box);
    if (on) line(g, CYAN, 2, {PointF(box.X + 3, box.Y + 7), PointF(box.X + 5.5f, box.Y + 10), PointF(box.X + 10.5f, box.Y + 3)});
    text(g, label, b.r.X + 21, b.r.Y + 2, 9 * 4.0f / 3, g_hover == b.id ? CYAN : TEXT);
}

static float rows_height() { return max(40.0f, 14.0f + 26.0f * g_states.count); }

static void paint(HDC dc, RECT client) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    SolidBrush bg(col(BG));
    g.FillRectangle(&bg, 0, 0, (INT)client.right, (INT)client.bottom);
    g.ScaleTransform((REAL)g_scale, (REAL)g_scale);
    const float logical_h = (float)(client.bottom / g_scale);

    text(g, L"SAFE CONNECT", 18, 12, 20 * 4.0f / 3, CYAN, true);
    {
        Font font(g_ui, 20 * 4.0f / 3, FontStyleBold, UnitPixel);
        RectF box;
        g.MeasureString(L"SAFE CONNECT", -1, &font, PointF(18, 12), &box);
        text(g, L"PS5 update blocker   v" APP_VERSION, box.X + box.Width + 8, 25, 10 * 4.0f / 3, DIM);
    }

    panel(g, R_CONN);
    text(g, L"PS5 IP", 30, 76, 10 * 4.0f / 3, DIM, true);
    text(g, L"FTP PORT", 296, 76, 10 * 4.0f / 3, DIM, true);
    Pen field(col(EDGE), 1);
    SolidBrush field_bg(col(BG));
    g.FillRectangle(&field_bg, RectF(84, 68, 186, 32));
    g.DrawRectangle(&field, RectF(84, 68, 186, 32));
    g.FillRectangle(&field_bg, RectF(370, 68, 66, 32));
    g.DrawRectangle(&field, RectF(370, 68, 66, 32));
    draw_button(g, g_buttons[B_CHECK], L"CHECK", CYAN, 10 * 4.0f / 3);

    draw_scene(g);
    draw_button(g, g_buttons[B_BLOCK], L"BLOCK UPDATES", GREEN, 12 * 4.0f / 3);
    draw_button(g, g_buttons[B_REMOVE], L"REMOVE BLOCKER", RED, 12 * 4.0f / 3);

    panel(g, R_AUTO);
    draw_toggle(g, g_buttons[B_WATCH], L"Keep checking the PS5 every", g_set.watch);
    {
        const Button &b = g_buttons[B_INTERVAL];
        SolidBrush f(col(g_hover == B_INTERVAL ? EDGE : BG));
        g.FillRectangle(&f, b.r);
        const wchar_t *label = g_set.interval == 30 ? L"30 s  ▾" : g_set.interval == 60 ? L"1 min  ▾"
                             : g_set.interval == 300 ? L"5 min  ▾" : L"15 min  ▾";
        Font font(g_ui, 9 * 4.0f / 3, FontStyleRegular, UnitPixel);
        SolidBrush tb(col(TEXT));
        StringFormat fmt;
        fmt.SetAlignment(StringAlignmentCenter);
        fmt.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(label, -1, &font, b.r, &fmt, &tb);
    }
    text(g, g_countdown, 350, 466, 9 * 4.0f / 3, DIM);
    draw_toggle(g, g_buttons[B_AUTO], L"Put the blocker back automatically if the PS5 clears it "
                                      L"(a hard crash or a factory reset empties /update)", g_set.auto_block);

    RectF rows(18, 526, 724, rows_height());
    panel(g, rows);
    if (g_states.count == 0) {
        text(g, L"Not checked yet.", 30, rows.Y + 11, 10 * 4.0f / 3, DIM);
    }
    for (int i = 0; i < g_states.count; i++) {
        Rgb c = g_states.state[i] == SC_BLOCKED ? GREEN : g_states.state[i] == SC_NOFOLDER ? AMBER : RED;
        const wchar_t *label = g_states.state[i] == SC_BLOCKED ? L"BLOCKED"
                             : g_states.state[i] == SC_OPEN    ? L"OPEN: an update can download"
                             : g_states.state[i] == SC_FILE    ? L"AN UPDATE IS DOWNLOADED HERE"
                                                               : L"folder not reachable over FTP";
        float y = rows.Y + 8 + i * 26;
        ellipse(g, 30, y + 5, 40, y + 15, &c, 255, nullptr, 0);
        wchar_t path[300];
        swprintf(path, 300, L"%hs", g_states.path[i]);
        text(g, path, 52, y + 1, 10 * 4.0f / 3, TEXT, false, 0, g_mono);
        text(g, label, 320, y + 1, 10 * 4.0f / 3, c, true);
    }

    float log_top = rows.Y + rows.Height + 8, log_bottom = logical_h - 52;
    RectF log_r(18, log_top, 724, max(40.0f, log_bottom - log_top));
    panel(g, log_r);
    {
        const float lh = 15;
        int fit = (int)((log_r.Height - 12) / lh);
        int first = max(0, g_log_count - fit);
        g.SetClip(log_r);
        for (int i = first; i < g_log_count; i++)
            text(g, g_log[i].text, 28, log_r.Y + 6 + (i - first) * lh, 9 * 4.0f / 3, g_log[i].colour, false, 0, g_mono);
        g.ResetClip();
    }
    text(g, L"Also turn off Settings > System > System Software > System Software Update and Settings > "
            L"Download Update Files Automatically / Install Update Files Automatically. A manual USB or "
            L"recovery-mode update ignores the blocker.",
         18, logical_h - 44, 8 * 4.0f / 3, DIM, false, 0, nullptr, 724);
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------
static WNDPROC g_edit_proc;
static LRESULT CALLBACK edit_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_KEYDOWN && wp == VK_RETURN) { run(A_CHECK, false); return 0; }
    if (m == WM_CHAR && (wp == '\r' || wp == '\n')) return 0;
    if (m == WM_KEYDOWN && wp == VK_TAB) {
        SetFocus(w == g_host_edit ? g_port_edit : g_host_edit);
        return 0;
    }
    return CallWindowProcW(g_edit_proc, w, m, wp, lp);
}

static void layout() {
    if (g_edit_font) DeleteObject(g_edit_font);
    g_edit_font = CreateFontW(-(int)lround(13 * 4.0 / 3 * g_scale), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                              CLEARTYPE_QUALITY, 0, L"Consolas");
    auto place = [](HWND e, float x, float y, float w, float h) {
        int fh = (int)lround(13 * 4.0 / 3 * g_scale * 1.2);
        SetWindowPos(e, nullptr, (int)lround(x * g_scale), (int)lround((y + h / 2) * g_scale) - fh / 2,
                     (int)lround(w * g_scale), fh, SWP_NOZORDER);
        SendMessageW(e, WM_SETFONT, (WPARAM)g_edit_font, TRUE);
    };
    place(g_host_edit, 90, 68, 176, 32);
    place(g_port_edit, 376, 68, 56, 32);
}

static int hit(LPARAM lp) {
    float x = (float)(GET_X_LPARAM(lp) / g_scale), y = (float)(GET_Y_LPARAM(lp) / g_scale);
    for (const Button &b : g_buttons)
        if (b.r.Contains(x, y)) return b.id;
    return -1;
}

static void click(int id) {
    switch (id) {
    case B_CHECK: run(A_CHECK, false); break;
    case B_BLOCK: run(A_BLOCK, false); break;
    case B_REMOVE:
        if (g_busy) break;
        if (MessageBoxW(g_wnd, L"Let the PS5 download system updates again?\n\nAn update can end the jailbreak for good.",
                        APP_NAME, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
            if (g_set.auto_block) {
                g_set.auto_block = false;    // or the next check would put it straight back
                log_line(L"Turned off automatic put-back, so the blocker stays removed.", AMBER);
            }
            run(A_UNBLOCK, false);
        }
        break;
    case B_WATCH: g_set.watch = !g_set.watch; saved(); break;
    case B_AUTO: g_set.auto_block = !g_set.auto_block; saved(); break;
    case B_INTERVAL:
        g_set.interval = g_set.interval == 30 ? 60 : g_set.interval == 60 ? 300 : g_set.interval == 300 ? 900 : 30;
        saved();
        break;
    }
    InvalidateRect(g_wnd, nullptr, FALSE);
}

static void tick() {
    wchar_t old[96];
    wcscpy(old, g_countdown);
    wchar_t host[128];
    GetWindowTextW(g_host_edit, host, 128);
    if (g_set.watch && !g_busy && host_set(host)) {
        int left = (int)(g_next_check - now_s());
        if (left <= 0) {
            run(A_AUTO, true);
            wcscpy(g_countdown, L"checking…");
        } else {
            swprintf(g_countdown, 96, L"next check in %d:%02d%ls", left / 60, left % 60,
                     g_set.auto_block ? L"  ·  auto put-back on" : L"");
        }
    } else if (!g_set.watch) {
        wcscpy(g_countdown, L"(watching is off)");
    }
    if (wcscmp(old, g_countdown)) {
        RECT r = {(LONG)(340 * g_scale), (LONG)(460 * g_scale), (LONG)(742 * g_scale), (LONG)(486 * g_scale)};
        InvalidateRect(g_wnd, &r, FALSE);
    }
}

typedef UINT(WINAPI *GetDpiForWindowFn)(HWND);
static double window_scale(HWND w) {
    static GetDpiForWindowFn fn = (GetDpiForWindowFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn) return fn(w) / 96.0;
    HDC dc = GetDC(w);
    double s = GetDeviceCaps(dc, LOGPIXELSX) / 96.0;
    ReleaseDC(w, dc);
    return s;
}

static LRESULT CALLBACK wnd_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        g_wnd = w;
        g_scale = window_scale(w);
        g_edit_brush = CreateSolidBrush(RGB(BG.r, BG.g, BG.b));
        g_host_edit = CreateWindowExW(0, L"EDIT", g_set.host, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, w,
                                      nullptr, g_inst, nullptr);
        g_port_edit = CreateWindowExW(0, L"EDIT", g_set.port, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, w,
                                      nullptr, g_inst, nullptr);
        SendMessageW(g_host_edit, EM_SETLIMITTEXT, 120, 0);
        SendMessageW(g_port_edit, EM_SETLIMITTEXT, 6, 0);
        g_edit_proc = (WNDPROC)SetWindowLongPtrW(g_host_edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);
        SetWindowLongPtrW(g_port_edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);
        layout();
        SetTimer(w, TIMER_ANIM, 30, nullptr);
        SetTimer(w, TIMER_TICK, 500, nullptr);
        if (host_set(g_set.host)) PostMessageW(w, WM_APP + 9, 0, 0);
        else log_line(L"Type the PS5's IP address and press CHECK.", DIM);
        return 0;
    }
    case WM_APP + 9: run(A_CHECK, true); return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, RGB(TEXT.r, TEXT.g, TEXT.b));
        SetBkColor((HDC)wp, RGB(BG.r, BG.g, BG.b));
        return (LRESULT)g_edit_brush;
    case WM_TIMER:
        if (wp == TIMER_ANIM) {
            g_phase += 0.06;
            // full rate only while something moves fast; the idle glow is fine at 15 fps
            static int skip;
            bool fast = g_busy || (g_anim_start >= 0 && now_s() - g_anim_start < ANIM_LENGTH + 2.5);
            if (IsIconic(w) || (!fast && (++skip & 1))) return 0;
            RECT r = {(LONG)(R_SCENE.X * g_scale), (LONG)(R_SCENE.Y * g_scale),
                      (LONG)((R_SCENE.X + R_SCENE.Width) * g_scale + 1), (LONG)((R_SCENE.Y + R_SCENE.Height) * g_scale + 1)};
            InvalidateRect(w, &r, FALSE);
        } else {
            tick();
        }
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT c;
        GetClientRect(w, &c);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint(mem, c);
        BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
               ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int h = hit(lp);
        if (h != g_hover) {
            g_hover = h;
            InvalidateRect(w, nullptr, FALSE);
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, w, 0};
            TrackMouseEvent(&tme);
        }
        SetCursor(LoadCursorW(nullptr, h >= 0 ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE: g_hover = -1; InvalidateRect(w, nullptr, FALSE); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(w, &p);
            SetCursor(LoadCursorW(nullptr, hit(MAKELPARAM(p.x, p.y)) >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        int h = hit(lp);
        if (h >= 0) click(h);
        else SetFocus(w);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        RECT r = {0, 0, (LONG)(W * g_scale), (LONG)(680 * g_scale)};
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
        mm->ptMinTrackSize.x = r.right - r.left;
        mm->ptMinTrackSize.y = r.bottom - r.top;
        RECT r2 = {0, 0, (LONG)(W * g_scale), 4000};
        AdjustWindowRectEx(&r2, WS_OVERLAPPEDWINDOW, FALSE, 0);
        mm->ptMaxTrackSize.x = r2.right - r2.left;
        return 0;
    }
    case WM_SIZE: InvalidateRect(w, nullptr, FALSE); return 0;
    case 0x02E0: {   // WM_DPICHANGED
        g_scale = HIWORD(wp) / 96.0;
        RECT *r = (RECT *)lp;
        SetWindowPos(w, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        layout();
        InvalidateRect(w, nullptr, FALSE);
        return 0;
    }
    case WM_APP_LOG: {
        Rgb c = {(int)(wp >> 16) & 255, (int)(wp >> 8) & 255, (int)wp & 255};
        log_line((wchar_t *)lp, c);
        free((void *)lp);
        return 0;
    }
    case WM_APP_DONE: finished((Result *)lp); free((void *)lp); return 0;
    case WM_APP_FAIL: failed((Result *)lp); free((void *)lp); return 0;
    case WM_CLOSE: saved(); DestroyWindow(w); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

// ---------------------------------------------------------------------------
// Command line: --check|--block|--unblock HOST [PORT]
// ---------------------------------------------------------------------------
static void say_cli(const char *line, void *) { printf("%s\n", line); }

static int cli(int argc, wchar_t **argv) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        printf("\n");
    }
    char host[128];
    WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, host, sizeof(host), nullptr, nullptr);
    int port = argc > 3 ? _wtoi(argv[3]) : 2121;
    Ftp *f = new Ftp();
    char error[300];
    if (!ftp_open(*f, host, port, error, sizeof(error))) { printf("%s\n", error); return 2; }
    States st;
    bool ok;
    if (!wcscmp(argv[1], L"--block")) ok = block_updates(*f, st, say_cli, nullptr);
    else if (!wcscmp(argv[1], L"--unblock")) ok = unblock_updates(*f, st, say_cli, nullptr);
    else ok = blocker_state(*f, st);
    ftp_close(*f);
    if (!ok) { printf("the FTP connection broke\n"); return 2; }
    for (int i = 0; i < st.count; i++) printf("%s: %s\n", st.path[i], state_text(st.state[i]));
    bool blocked = all_blocked(st);
    printf("%s\n", blocked ? "BLOCKED" : "NOT BLOCKED");
    fflush(stdout);
    return blocked ? 0 : 1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    g_inst = inst;
    g_entries = (Entry *)malloc(sizeof(Entry) * 512);
    g_log = (LogLine *)calloc(LOG_MAX, sizeof(LogLine));
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 3 && (!wcscmp(argv[1], L"--check") || !wcscmp(argv[1], L"--block") || !wcscmp(argv[1], L"--unblock")))
        return cli(argc, argv);

    GdiplusStartupInput gsi;
    ULONG_PTR token;
    GdiplusStartup(&token, &gsi, nullptr);
    auto family = [](std::initializer_list<const wchar_t *> names) -> FontFamily * {
        for (const wchar_t *name : names) {
            FontFamily *f = new FontFamily(name);
            if (f->GetLastStatus() == Ok) return f;
            delete f;
        }
        return FontFamily::GenericSansSerif()->Clone();
    };
    g_ui = family({L"Segoe UI", L"Tahoma", L"Arial"});
    g_mono = family({L"Consolas", L"Lucida Console", L"Courier New"});
    g_symbol = family({L"Segoe UI Symbol", L"Segoe UI", L"Arial"});
    load_settings();

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SafeConnect";
    RegisterClassExW(&wc);

    // size for the monitor's DPI before the window exists
    HDC screen = GetDC(nullptr);
    double s = GetDeviceCaps(screen, LOGPIXELSX) / 96.0;
    ReleaseDC(nullptr, screen);
    RECT r = {0, 0, (LONG)(W * s), (LONG)(H * s)};
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    HWND w = CreateWindowExW(0, L"SafeConnect", APP_NAME L" " APP_VERSION, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst,
                             nullptr);
    // the dark title bar on Windows 10/11
    typedef HRESULT(WINAPI * DwmSet)(HWND, DWORD, LPCVOID, DWORD);
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        if (DwmSet set = (DwmSet)GetProcAddress(dwm, "DwmSetWindowAttribute")) {
            BOOL dark = TRUE;
            if (set(w, 20, &dark, sizeof(dark)) != S_OK) set(w, 19, &dark, sizeof(dark));
        }
    }
    ShowWindow(w, show);
    UpdateWindow(w);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    GdiplusShutdown(token);
    return 0;
}
