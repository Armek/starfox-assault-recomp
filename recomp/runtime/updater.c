/* updater.c - updates the program from its GitHub releases (the start screen offers it).
 *
 * At start (unless check_updates=0 in user/launcher.cfg or SFA_NO_UPDATE_CHECK is set), a background
 * thread asks GitHub's public API for the repository's releases and looks for a newer version than
 * this build's (SFA_VERSION, from recomp/VERSION) with the package for this platform. The start
 * screen shows it; when the player chooses to update, the package is downloaded into user/update/,
 * checked against the size GitHub reports, unpacked, and its files replace the program's (on Windows
 * the running program and SDL2.dll are renamed to *.old first, and removed at the next start). The
 * program then restarts itself. Saves and settings (user/) are never touched: packages don't contain
 * them.
 *
 * Downloading and unpacking use the curl (or, where there is none, wget) and tar programs that come with
 * Windows 10/11, SteamOS and most Linux systems,
 * so there are no new libraries to ship. On Linux they run without the Steam runtime's library
 * overrides (LD_PRELOAD, LD_LIBRARY_PATH), as the file dialogs do.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "runtime.h"
#include "updater.h"
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define PATH_SEP "\\"
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#define PATH_SEP "/"
#endif

#ifndef SFA_VERSION
#define SFA_VERSION "0.0.0"
#endif

#define REPO "Elroy1995/starfox-assault-recomp"
#ifdef _WIN32
#define ASSET "sfa-recomp-windows-x64.zip"
#define PKG_DIR "sfa-recomp-windows-x64"
#else
#define ASSET "sfa-recomp-linux-x86_64.tar.gz"
#define PKG_DIR "sfa-recomp-linux-x86_64"
#endif

static SDL_atomic_t g_state;          /* UPD_* */
static char g_msg[256];               /* what to show (set before g_state changes) */
static char g_new_tag[32], g_new_url[512];
static long long g_new_size;
static char g_dir[1024];              /* the program's folder */
static char g_exe[1100];              /* the program itself */
static int g_argc;
static char **g_argv;

const char *updater_version(void) {
    const char *o = getenv("SFA_VERSION_OVERRIDE"); /* testing: pretend to be an older version */
    return o && *o ? o : SFA_VERSION;
}
int updater_state(void) { return SDL_AtomicGet(&g_state); }
const char *updater_message(void) { return g_msg; }
const char *updater_new_version(void) { return g_new_tag[0] == 'v' ? g_new_tag + 1 : g_new_tag; }

/* "v0.2.10" or "0.2.10" -> comparable number (each part up to 999) */
static long long version_num(const char *s) {
    if (*s == 'v' || *s == 'V') s++;
    long long n = 0;
    for (int part = 0; part < 4; part++) {
        n *= 1000;
        if (*s >= '0' && *s <= '9') {
            n += strtol(s, (char **)&s, 10) % 1000;
            if (*s == '.') s++;
        }
    }
    return n;
}

static void set_state(int st, const char *fmt, const char *arg) {
    snprintf(g_msg, sizeof g_msg, fmt, arg ? arg : "");
    SDL_AtomicSet(&g_state, st);
    printf("[update] %s\n", g_msg);
    fflush(stdout);
}

/* Runs a program (curl or tar) with these arguments and waits; returns its exit code (-1 if it couldn't
 * be started). No console window on Windows; no Steam runtime overrides on Linux. */
static int run(const char *const *args) {
#ifdef _WIN32
    char cmd[4096] = "";
    for (int i = 0; args[i]; i++) {
        size_t n = strlen(cmd);
        snprintf(cmd + n, sizeof cmd - n, "%s\"%s\"", i ? " " : "", args[i]);
    }
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        unsetenv("LD_PRELOAD");
        unsetenv("LD_LIBRARY_PATH");
        execvp(args[0], (char *const *)args);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

/* Downloads url to out with curl, or with wget where there's no curl (some Linux systems); api: GitHub's
 * JSON API (its media type, a short timeout). Returns 0 on success. */
static int download(const char *url, const char *out, int api) {
    const char *curl[16];
    int n = 0;
    curl[n++] = "curl"; curl[n++] = "-fsSL"; curl[n++] = "--retry"; curl[n++] = "2";
    curl[n++] = "--max-time"; curl[n++] = api ? "20" : "600";
    if (api) { curl[n++] = "-H"; curl[n++] = "Accept: application/vnd.github+json"; }
    curl[n++] = "-o"; curl[n++] = out; curl[n++] = url; curl[n] = NULL;
    int rc = run(curl);
#ifndef _WIN32
    if (rc == 127 || rc == -1) { /* no curl */
        const char *wget[12];
        n = 0;
        wget[n++] = "wget"; wget[n++] = "-q"; wget[n++] = "--tries=3"; wget[n++] = api ? "--timeout=20" : "--timeout=60";
        if (api) wget[n++] = "--header=Accept: application/vnd.github+json";
        wget[n++] = "-O"; wget[n++] = out; wget[n++] = url; wget[n] = NULL;
        rc = run(wget);
    }
#endif
    return rc;
}

static long long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long long n = ftell(f);
    fclose(f);
    return n;
}

static void make_dir(const char *path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

/* Removes a folder and everything in it (the unpacked package; depth 2 at most). */
static void remove_tree(const char *path) {
#ifdef _WIN32
    char pat[1200];
    snprintf(pat, sizeof pat, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char p[1200];
            snprintf(p, sizeof p, "%s\\%s", path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(p);
            else DeleteFileA(p);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(path);
#else
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char p[1200];
            snprintf(p, sizeof p, "%s/%s", path, e->d_name);
            struct stat st;
            if (!lstat(p, &st) && S_ISDIR(st.st_mode)) remove_tree(p);
            else unlink(p);
        }
        closedir(d);
    }
    rmdir(path);
#endif
}

/* The value after "key": in text at or after from (a string's contents, or a number's digits). */
static const char *json_value(const char *from, const char *key, char *out, size_t n) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(from, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    size_t k = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && k + 1 < n) out[k++] = *p++;
    } else {
        while (*p && ((*p >= '0' && *p <= '9') || *p == '-') && k + 1 < n) out[k++] = *p++;
    }
    out[k] = 0;
    return p;
}

/* Finds the newest release with this platform's package in GitHub's release list. */
static int pick_release(const char *json) {
    long long best = version_num(updater_version());
    int found = 0;
    const char *p = json;
    char tag[32];
    while ((p = json_value(p, "tag_name", tag, sizeof tag))) {
        const char *next_rel = strstr(p, "\"tag_name\"");
        const char *assets = strstr(p, "\"assets\"");
        if (!assets || (next_rel && assets > next_rel)) continue;
        /* this release's assets: find ours */
        const char *a = assets;
        char name[128];
        while ((a = json_value(a, "name", name, sizeof name)) && (!next_rel || a < next_rel)) {
            if (strcmp(name, ASSET)) continue;
            char size[32], url[512];
            const char *s = json_value(a, "size", size, sizeof size);
            const char *u = json_value(a, "browser_download_url", url, sizeof url);
            if (!s || !u || (next_rel && (s > next_rel || u > next_rel))) break;
            long long v = version_num(tag);
            if (v > best && !strncmp(url, "https://github.com/" REPO "/releases/download/", 19 + strlen(REPO) + 19)) {
                best = v;
                snprintf(g_new_tag, sizeof g_new_tag, "%s", tag);
                snprintf(g_new_url, sizeof g_new_url, "%s", url);
                g_new_size = atoll(size);
                found = 1;
            }
            break;
        }
    }
    return found;
}

static int check_thread(void *arg) {
    (void)arg;
    char path[1200], updir[1100];
    snprintf(updir, sizeof updir, "%s" PATH_SEP "user" PATH_SEP "update", g_dir);
    snprintf(path, sizeof path, "%s" PATH_SEP "releases.json", updir);
    char user[1100];
    snprintf(user, sizeof user, "%s" PATH_SEP "user", g_dir);
    make_dir(user);
    make_dir(updir);
    remove(path);
    int rc = download("https://api.github.com/repos/" REPO "/releases?per_page=20", path, 1);
    long long n = rc == 0 ? file_size(path) : -1;
    if (n <= 0 || n > (8 << 20)) {
        set_state(UPD_NONE, "Couldn't check for updates (offline?)", NULL);
        return 0;
    }
    char *json = malloc((size_t)n + 1);
    FILE *f = fopen(path, "rb");
    size_t got = f && json ? fread(json, 1, (size_t)n, f) : 0;
    if (f) fclose(f);
    if (!json || got != (size_t)n) { free(json); set_state(UPD_NONE, "Couldn't check for updates", NULL); return 0; }
    json[n] = 0;
    int found = pick_release(json);
    free(json);
    remove(path);
    if (found) set_state(UPD_AVAILABLE, "Update available: %s", updater_new_version());
    else set_state(UPD_NONE, "Up to date (v%s)", updater_version());
    return 0;
}

/* Replaces the program's files with the unpacked package's. */
static int replace_files(const char *src) {
    static const char *const files[] = {
#ifdef _WIN32
        "sfa.exe", "SDL2.dll",
#else
        "sfa", "sfa.sh",
#endif
        "PLAY.txt", "font-licenses.txt", NULL};
    for (int i = 0; files[i]; i++) {
        char from[1300], to[1300];
        snprintf(from, sizeof from, "%s" PATH_SEP "%s", src, files[i]);
        snprintf(to, sizeof to, "%s" PATH_SEP "%s", g_dir, files[i]);
        if (file_size(from) < 0) {
            if (i < 1) return 0; /* the program itself must be there */
            continue;
        }
#ifdef _WIN32
        if (i < 2) { /* the running program and its loaded DLL can't be overwritten, but they can be renamed */
            char old[1310];
            snprintf(old, sizeof old, "%s.old", to);
            DeleteFileA(old);
            if (file_size(to) >= 0 && !MoveFileA(to, old)) return 0;
            if (!MoveFileA(from, to)) { MoveFileA(old, to); return 0; }
        } else if (!MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING)) {
            return 0;
        }
#else
        unlink(to); /* the running program keeps its open copy */
        if (rename(from, to) != 0) return 0;
        if (!strcmp(files[i], "sfa") || !strcmp(files[i], "sfa.sh")) chmod(to, 0755);
#endif
    }
    return 1;
}

static int install_thread(void *arg) {
    (void)arg;
    char updir[1100], pkg[1200], xdir[1200], src[1300];
    snprintf(updir, sizeof updir, "%s" PATH_SEP "user" PATH_SEP "update", g_dir);
    snprintf(pkg, sizeof pkg, "%s" PATH_SEP "%s", updir, ASSET);
    snprintf(xdir, sizeof xdir, "%s" PATH_SEP "unpacked", updir);
    make_dir(updir);
    remove(pkg);
    set_state(UPD_DOWNLOADING, "Downloading %s...", updater_new_version());
    if (download(g_new_url, pkg, 0) != 0 || file_size(pkg) != g_new_size) {
        remove(pkg);
        set_state(UPD_FAILED, "Download failed%s", ": please try again later");
        return 0;
    }
    set_state(UPD_INSTALLING, "Installing %s...", updater_new_version());
    remove_tree(xdir);
    make_dir(xdir);
    const char *un[] = {"tar", "-xf", pkg, "-C", xdir, NULL};
    if (run(un) != 0) {
        set_state(UPD_FAILED, "Couldn't unpack the update%s", "");
        return 0;
    }
    snprintf(src, sizeof src, "%s" PATH_SEP PKG_DIR, xdir);
    if (!replace_files(src)) {
        set_state(UPD_FAILED, "Couldn't replace the program's files%s", " (is its folder writable?)");
        return 0;
    }
    remove_tree(xdir);
    remove(pkg);
    set_state(UPD_DONE, "Updated to %s: restarting...", updater_new_version());
    return 0;
}

void updater_init(int argc, char **argv) {
    g_argc = argc;
    g_argv = argv;
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, g_exe, sizeof g_exe);
    if (n == 0 || n >= sizeof g_exe) g_exe[0] = 0;
#else
    ssize_t n = readlink("/proc/self/exe", g_exe, sizeof g_exe - 1);
    g_exe[n > 0 ? n : 0] = 0;
#endif
    snprintf(g_dir, sizeof g_dir, "%s", g_exe);
    char *slash = strrchr(g_dir, '/'), *bslash = strrchr(g_dir, '\\');
    if (bslash > slash) slash = bslash;
    if (slash) *slash = 0;
    else snprintf(g_dir, sizeof g_dir, ".");
#ifdef _WIN32
    /* files the last update renamed aside */
    char old[1200];
    snprintf(old, sizeof old, "%s\\sfa.exe.old", g_dir);
    DeleteFileA(old);
    snprintf(old, sizeof old, "%s\\SDL2.dll.old", g_dir);
    DeleteFileA(old);
#endif
}

void updater_check_start(void) {
    if (SDL_AtomicGet(&g_state) != UPD_IDLE || !g_exe[0]) return;
    SDL_AtomicSet(&g_state, UPD_CHECKING);
    SDL_Thread *t = SDL_CreateThread(check_thread, "update-check", NULL);
    if (t) SDL_DetachThread(t);
    else SDL_AtomicSet(&g_state, UPD_NONE);
}

void updater_install_start(void) {
    if (SDL_AtomicGet(&g_state) != UPD_AVAILABLE && SDL_AtomicGet(&g_state) != UPD_FAILED) return;
    if (!g_new_url[0]) return;
    SDL_AtomicSet(&g_state, UPD_DOWNLOADING);
    SDL_Thread *t = SDL_CreateThread(install_thread, "update-install", NULL);
    if (t) SDL_DetachThread(t);
    else set_state(UPD_FAILED, "Couldn't start the update%s", "");
}

/* Starts the new program with the same arguments and ends this one. */
void updater_restart(void) {
    printf("[update] restarting %s\n", g_exe);
    fflush(stdout);
#ifdef _WIN32
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "\"%s\"", g_exe);
    for (int i = 1; i < g_argc; i++) {
        size_t n = strlen(cmd);
        snprintf(cmd + n, sizeof cmd - n, " \"%s\"", g_argv[i]);
    }
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, g_dir, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    exit(0);
#else
    execv(g_exe, g_argv);
    exit(0); /* couldn't start it: the player starts it again */
#endif
}
