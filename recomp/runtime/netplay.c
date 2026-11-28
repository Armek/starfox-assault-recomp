/* netplay.c - play over the network in lockstep, the way Dolphin's netplay does.
 *
 * Every player runs the whole game from their own disc image; the players only exchange controller
 * input. That works because with deterministic time (threads.c: g_det) the same input on the same
 * frame gives exactly the same game, so all copies stay in step. Each PADRead is a frame: a player's
 * own controller (port 1 on their machine) is read, sent to the others and used DELAY frames later,
 * and PADRead waits until it has every player's input for the current frame. Players get ports in
 * the order they joined: the host port 1, the first to join port 2, and so on.
 *
 * Star layout over TCP: the host listens, players join it, and the host relays each player's input to
 * the others. Before the game starts the host sends its settings (60 fps, input delay, player count)
 * and its memory card saves; a joining player plays on a copy of those (user/netplay/card_a), so menus
 * and unlocks match and their own saves aren't touched. About once a second every copy reports a
 * sampled hash of main RAM for the same frame; a mismatch means the games drifted apart (logged).
 *
 * Started from the start screen's Online page (launcher.c: netplay_host_start / netplay_join_start run
 * the connecting on a thread, netplay_status reports it), or for testing with
 * SFA_NETPLAY_HOST=<players>[:<port>] / SFA_NETPLAY_JOIN=<address>[:<port>] (default port 47820).
 * SFA_NETPLAY_DELAY=<frames> (default 3) on the host.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <SDL.h>
#include "ppc.h"
#include "runtime.h"
#include "gx.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define closesock closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define closesock close
#endif

#define NET_PORT 47820
#define MAGIC "SFANET01"
#define RING 512 /* frames of input kept */

enum { MSG_INPUT = 1, MSG_HASH = 2, MSG_SETTINGS = 3, MSG_FILE = 4, MSG_START = 5 };

int g_netplay;           /* 1 while a netplay session runs */
static int g_host, g_me, g_players, g_delay = 3;
static sock_t g_peer[4] = {BAD_SOCK, BAD_SOCK, BAD_SOCK, BAD_SOCK}; /* host: by port; joiner: [0] = host */
static uint8_t g_in[RING][4][12];
static uint32_t g_have[RING][4]; /* frame number + 1 when g_in holds that frame's input */
static uint32_t g_frame;          /* PADRead calls so far */
#define HSLOTS 8
static uint64_t g_hash_mine[HSLOTS], g_hash_theirs[HSLOTS][4];
static uint32_t g_hash_frame[HSLOTS], g_theirs_frame[HSLOTS][4]; /* frame + 1 */
static unsigned g_desyncs;
static int g_net_fps60 = -1; /* a joining player: the host's 60 fps setting */

/* connecting, for the start screen: 0 idle, 1 under way, 2 connected (start the game), -1 failed */
static SDL_mutex *g_ns_lock;
static char g_ns_msg[200];
static volatile int g_ns_state, g_cancel;
static void ns_set(int state, const char *fmt, ...) {
    if (!g_ns_lock) g_ns_lock = SDL_CreateMutex();
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    SDL_LockMutex(g_ns_lock);
    snprintf(g_ns_msg, sizeof g_ns_msg, "%s", buf);
    SDL_UnlockMutex(g_ns_lock);
    g_ns_state = state;
    printf("[netplay] %s\n", buf);
    fflush(stdout);
}
int netplay_status(char *msg, size_t n) {
    if (!g_ns_lock) g_ns_lock = SDL_CreateMutex();
    SDL_LockMutex(g_ns_lock);
    snprintf(msg, n, "%s", g_ns_msg);
    SDL_UnlockMutex(g_ns_lock);
    return g_ns_state;
}

/* ---- sockets ----------------------------------------------------------------------------------------- */
static void net_fail(const char *what) {
#ifdef _WIN32
    fprintf(stderr, "[netplay] %s failed (error %d)\n", what, WSAGetLastError());
#else
    fprintf(stderr, "[netplay] %s failed: %s\n", what, strerror(errno));
#endif
}

static int send_all(sock_t s, const void *buf, int len) {
    const char *p = buf;
    while (len > 0) {
        int n = send(s, p, len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static int recv_all(sock_t s, void *buf, int len) {
    char *p = buf;
    while (len > 0) {
        int n = recv(s, p, len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static void tune(sock_t s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* messages: type (1), length of the rest (4), the rest */
static int send_msg(sock_t s, int type, const void *body, uint32_t len) {
    uint8_t h[5];
    h[0] = (uint8_t)type;
    put32(h + 1, len);
    if (send_all(s, h, 5) < 0) return -1;
    return len ? send_all(s, body, (int)len) : 0;
}

static uint8_t *recv_msg(sock_t s, int *type, uint32_t *len) {
    uint8_t h[5];
    if (recv_all(s, h, 5) < 0) return NULL;
    *type = h[0];
    *len = get32(h + 1);
    if (*len > 64u << 20) return NULL;
    uint8_t *b = malloc(*len ? *len : 1);
    if (!b) return NULL;
    if (*len && recv_all(s, b, (int)*len) < 0) { free(b); return NULL; }
    return b;
}

/* ---- handshake ------------------------------------------------------------------------------------------ */
extern int g_force_60fps;
extern int g_det;
extern const char *g_card_dir_override;

static int list_card_files(char names[][260], int max) {
    int n = 0;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("user\\GF7E01\\card_a\\*.gci", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (n < max) snprintf(names[n++], 260, "%s", fd.cFileName); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    FILE *p = popen("ls user/GF7E01/card_a 2>/dev/null", "r");
    char line[300];
    while (p && fgets(line, sizeof line, p) && n < max) {
        line[strcspn(line, "\r\n")] = 0;
        size_t L = strlen(line);
        if (L > 4 && !strcmp(line + L - 4, ".gci")) snprintf(names[n++], 260, "%s", line);
    }
    if (p) pclose(p);
#endif
    return n;
}

static void mkdir_one(const char *d) {
#ifdef _WIN32
    CreateDirectoryA(d, NULL);
#else
    char cmd[300];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", d);
    if (system(cmd)) {}
#endif
}

static void clear_dir_gci(const char *d) {
#ifdef _WIN32
    char pat[300];
    snprintf(pat, sizeof pat, "%s\\*.gci", d);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { char p[600]; snprintf(p, sizeof p, "%s\\%s", d, fd.cFileName); DeleteFileA(p); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -f '%s'/*.gci", d);
    if (system(cmd)) {}
#endif
}

static sock_t g_listen = BAD_SOCK;
static int g_net_ready_init; /* sockets set up (WSAStartup) */

static int net_startup(void) {
    if (g_net_ready_init) return 0;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { net_fail("WSAStartup"); return -1; }
#endif
    g_net_ready_init = 1;
    return 0;
}

/* This machine's IPv4 addresses on the network (not loopback), comma-separated, for the host to tell the
 * others. Returns how many. */
int netplay_local_addresses(char *out, size_t n) {
    out[0] = 0;
    if (net_startup() < 0) return 0;
    char name[256];
    if (gethostname(name, sizeof name)) return 0;
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    if (getaddrinfo(name, NULL, &hints, &res) || !res) return 0;
    int count = 0;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        char ip[64];
        inet_ntop(AF_INET, &((struct sockaddr_in *)a->ai_addr)->sin_addr, ip, sizeof ip);
        if (!strncmp(ip, "127.", 4) || !strncmp(ip, "169.254.", 8) || strstr(out, ip)) continue; /* loopback, self-assigned */
        size_t used = strlen(out);
        snprintf(out + used, n - used, "%s%s", count ? ", " : "", ip);
        count++;
    }
    freeaddrinfo(res);
    return count;
}

extern int launcher_fps60(void);

static int host_session(int players, int port) {
    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == BAD_SOCK) { net_fail("socket"); ns_set(-1, "Couldn't open a network connection"); return -1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 4) < 0) {
        net_fail("bind/listen");
        closesock(ls);
        ns_set(-1, "Port %d is in use (is another copy hosting?)", port);
        return -1;
    }
    g_listen = ls;
    int fps60 = launcher_fps60(); /* the host's own setting goes to everyone */
    for (int p = 1; p < players; p++) {
        ns_set(1, "Waiting for players: %d of %d joined", p - 1, players - 1);
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        sock_t s = BAD_SOCK;
        while (s == BAD_SOCK) { /* wait, checking now and then whether hosting was cancelled */
            if (g_cancel) { closesock(ls); g_listen = BAD_SOCK; ns_set(0, "Cancelled"); return -1; }
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(ls, &fds);
            struct timeval tv = {0, 250000};
            if (select((int)ls + 1, &fds, NULL, NULL, &tv) > 0) s = accept(ls, (struct sockaddr *)&from, &fl);
        }
        tune(s);
        char hello[64] = {0};
        if (recv_all(s, hello, sizeof hello) < 0 || memcmp(hello, MAGIC, 8) || strcmp(hello + 8, SFA_VERSION)) {
            printf("[netplay] a player with another version (%.40s) tried to join; turned away\n", hello + 8);
            closesock(s);
            p--;
            continue;
        }
        g_peer[p] = s;
        char ip[64];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        printf("[netplay] player %d joined from %s\n", p + 1, ip);
    }
    closesock(ls);
    g_listen = BAD_SOCK;
    /* settings, saves, start */
    char names[16][260];
    int nf = list_card_files(names, 16);
    for (int p = 1; p < players; p++) {
        uint8_t set[8] = {(uint8_t)(fps60 > 0), (uint8_t)g_delay, (uint8_t)players, (uint8_t)p};
        if (send_msg(g_peer[p], MSG_SETTINGS, set, sizeof set) < 0) { ns_set(-1, "Player %d left", p + 1); return -1; }
        for (int i = 0; i < nf; i++) {
            char path[600];
            snprintf(path, sizeof path, "user/GF7E01/card_a/%s", names[i]);
            FILE *f = fopen(path, "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint32_t nl = (uint32_t)strlen(names[i]);
            uint8_t *body = malloc(4 + nl + (size_t)(sz > 0 ? sz : 0));
            put32(body, nl);
            memcpy(body + 4, names[i], nl);
            size_t got = sz > 0 ? fread(body + 4 + nl, 1, (size_t)sz, f) : 0;
            fclose(f);
            int bad = send_msg(g_peer[p], MSG_FILE, body, 4 + nl + (uint32_t)got) < 0;
            free(body);
            if (bad) { ns_set(-1, "Player %d left", p + 1); return -1; }
        }
        if (send_msg(g_peer[p], MSG_START, NULL, 0) < 0) { ns_set(-1, "Player %d left", p + 1); return -1; }
    }
    g_host = 1;
    g_me = 0;
    g_players = players;
    ns_set(2, "Everyone's here: starting (%d players, input delay %d frames)", players, g_delay);
    return 0;
}

static int join_session(const char *where) {
    char hostname[256];
    int port = NET_PORT;
    snprintf(hostname, sizeof hostname, "%s", where);
    char *colon = strrchr(hostname, ':');
    if (colon) { port = atoi(colon + 1); *colon = 0; }
    ns_set(1, "Looking for %s", hostname);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(hostname, ps, &hints, &res) || !res) { ns_set(-1, "Couldn't find %s", hostname); return -1; }
    sock_t s = BAD_SOCK;
    for (int tries = 0; tries < 60 && s == BAD_SOCK; tries++) { /* the host may still be setting up: a minute */
        if (g_cancel) { freeaddrinfo(res); ns_set(0, "Cancelled"); return -1; }
        ns_set(1, "Connecting to %s%s", hostname, tries ? " (is it hosting?)" : "");
        s = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(s, res->ai_addr, (int)res->ai_addrlen) < 0) {
            closesock(s);
            s = BAD_SOCK;
            for (int k = 0; k < 10 && !g_cancel; k++) rt_sleep_us(100000);
        }
    }
    freeaddrinfo(res);
    if (s == BAD_SOCK) { if (!g_cancel) ns_set(-1, "Couldn't reach %s: is it hosting, on the same network?", hostname); return -1; }
    tune(s);
    char hello[64] = {0};
    memcpy(hello, MAGIC, 8);
    snprintf(hello + 8, sizeof hello - 8, "%s", SFA_VERSION);
    if (send_all(s, hello, sizeof hello) < 0) { closesock(s); ns_set(-1, "The host closed the connection"); return -1; }
    g_peer[0] = s;
    ns_set(1, "Connected: waiting for the host to start");
    const char *dir = "user/netplay/card_a";
    mkdir_one("user");
    mkdir_one("user/netplay");
    mkdir_one(dir);
    clear_dir_gci(dir);
    int files = 0;
    for (;;) {
        int type;
        uint32_t len;
        uint8_t *b = recv_msg(s, &type, &len); /* netplay_cancel closes the socket to stop this */
        if (!b) {
            closesock(s);
            g_peer[0] = BAD_SOCK;
            if (g_cancel) ns_set(0, "Cancelled");
            else ns_set(-1, "The host closed the connection (another version, or it stopped hosting)");
            return -1;
        }
        if (type == MSG_SETTINGS && len >= 4) {
            g_net_fps60 = b[0];
            g_delay = b[1];
            g_players = b[2];
            g_me = b[3];
        } else if (type == MSG_FILE && len >= 4) {
            uint32_t nl = get32(b);
            if (nl < 200 && 4 + nl <= len) {
                char name[256], path[400];
                memcpy(name, b + 4, nl);
                name[nl] = 0;
                if (!strchr(name, '/') && !strchr(name, '\\') && !strstr(name, "..")) {
                    snprintf(path, sizeof path, "%s/%s", dir, name);
                    FILE *f = fopen(path, "wb");
                    if (f) { fwrite(b + 4 + nl, 1, len - 4 - nl, f); fclose(f); files++; }
                }
            }
        } else if (type == MSG_START) {
            free(b);
            break;
        }
        free(b);
    }
    g_card_dir_override = dir;
    g_host = 0;
    ns_set(2, "Joined as player %d of %d: starting", g_me + 1, g_players);
    return 0;
}

/* connecting runs on a thread so the start screen keeps drawing */
static int g_start_players;
static char g_start_addr[256];
static int SDLCALL session_thread(void *arg) {
    (void)arg;
    if (g_start_players) host_session(g_start_players, NET_PORT);
    else join_session(g_start_addr);
    return 0;
}
static void start_thread(void) {
    g_cancel = 0;
    if (getenv("SFA_NETPLAY_DELAY")) g_delay = atoi(getenv("SFA_NETPLAY_DELAY"));
    if (g_delay < 1) g_delay = 1;
    if (g_delay > 30) g_delay = 30;
    SDL_Thread *t = SDL_CreateThread(session_thread, "netplay-connect", NULL);
    if (t) SDL_DetachThread(t);
    else ns_set(-1, "Couldn't start connecting");
}
int netplay_host_start(int players) {
    if (net_startup() < 0) { ns_set(-1, "Networking isn't available"); return -1; }
    g_start_players = players < 2 ? 2 : players > 4 ? 4 : players;
    ns_set(1, "Starting to host");
    start_thread();
    return 0;
}
int netplay_join_start(const char *addr) {
    if (net_startup() < 0) { ns_set(-1, "Networking isn't available"); return -1; }
    g_start_players = 0;
    snprintf(g_start_addr, sizeof g_start_addr, "%s", addr);
    ns_set(1, "Looking for %s", addr);
    start_thread();
    return 0;
}
/* stop hosting or joining (the thread notices and ends) */
void netplay_cancel(void) {
    g_cancel = 1;
    if (!g_host && g_peer[0] != BAD_SOCK) { closesock(g_peer[0]); g_peer[0] = BAD_SOCK; }
}

/* Before the game boots: finishes a session the start screen connected, or (testing) connects one from
 * SFA_NETPLAY_HOST / SFA_NETPLAY_JOIN, waiting. Returns 0 when there's no netplay or it's set up, -1 when
 * connecting failed. */
int netplay_init(void) {
    if (g_ns_state != 2) {
        const char *h = getenv("SFA_NETPLAY_HOST"), *j = getenv("SFA_NETPLAY_JOIN");
        if (!h && !j) return 0;
        if (h) {
            const char *c = strchr(h, ':');
            netplay_host_start(atoi(h));
            (void)c; /* (the port is always NET_PORT from here) */
        } else {
            netplay_join_start(j);
        }
        while (g_ns_state == 1) rt_sleep_us(20000);
        if (g_ns_state != 2) return -1;
    }
    if (!g_host && g_net_fps60 >= 0) g_force_60fps = g_net_fps60; /* the host's setting */
    g_netplay = 1;
    g_det = 1;
    printf("[netplay] playing as player %d of %d, input delay %d frames\n", g_me + 1, g_players, g_delay);
    fflush(stdout);
    char t[120];
    snprintf(t, sizeof t, "Network game: you're player %d of %d", g_me + 1, g_players);
    video_toast(t);
    return 0;
}

/* ---- the frame loop ---------------------------------------------------------------------------------- */
static void store(uint32_t frame, int port, const uint8_t *pad) {
    memcpy(g_in[frame % RING][port], pad, 12);
    g_have[frame % RING][port] = frame + 1;
}

static void broadcast_input(uint32_t frame, int port, const uint8_t *pad, int except) {
    uint8_t m[17];
    put32(m, frame);
    m[4] = (uint8_t)port;
    memcpy(m + 5, pad, 12);
    if (g_host) {
        for (int p = 1; p < g_players; p++)
            if (p != except && g_peer[p] != BAD_SOCK) send_msg(g_peer[p], MSG_INPUT, m, sizeof m);
    } else {
        send_msg(g_peer[0], MSG_INPUT, m, sizeof m);
    }
}

/* hashes are kept by slot (a second of frames each) until both sides of a comparison are here */
static void compare(unsigned slot, int from) {
    if (g_hash_frame[slot] && g_hash_frame[slot] == g_theirs_frame[slot][from] && g_hash_mine[slot] != g_hash_theirs[slot][from]) {
        if (!g_desyncs) video_toast("Network game: the games drifted apart. Restart the session to play in step again.");
        if (g_desyncs++ < 20)
            printf("[netplay] out of step with player %d at frame %u (hash %016llx here, %016llx there)\n", from + 1,
                   g_hash_frame[slot] - 1, (unsigned long long)g_hash_mine[slot], (unsigned long long)g_hash_theirs[slot][from]);
        fflush(stdout);
    }
}
static void check_hash(uint32_t frame, uint64_t theirs, int from) {
    unsigned slot = (frame / 60) % HSLOTS;
    g_hash_theirs[slot][from] = theirs;
    g_theirs_frame[slot][from] = frame + 1;
    compare(slot, from);
}

static int g_lost;
static void lost(const char *why) {
    if (g_lost) return;
    g_lost = 1;
    printf("[netplay] %s: playing on alone (the other players' controllers stay idle)\n", why);
    char t[160];
    snprintf(t, sizeof t, "Network game: %s. You're playing on alone.", why);
    video_toast(t);
    fflush(stdout);
}

/* Read and handle one message from socket s (index i). Returns -1 when the connection is gone. */
static int pump_one(int i) {
    int type;
    uint32_t len;
    uint8_t *b = recv_msg(g_peer[i], &type, &len);
    if (!b) return -1;
    if (type == MSG_INPUT && len == 17) {
        uint32_t frame = get32(b);
        int port = b[4] & 3;
        store(frame, port, b + 5);
        if (g_host) broadcast_input(frame, port, b + 5, i); /* relay to the other joiners */
    } else if (type == MSG_HASH && len == 13) {
        uint32_t frame = get32(b);
        uint64_t h = (uint64_t)get32(b + 4) << 32 | get32(b + 8);
        int from = b[12] & 3;
        check_hash(frame, h, from);
        if (g_host) { /* relay */
            for (int p = 1; p < g_players; p++)
                if (p != i && g_peer[p] != BAD_SOCK) send_msg(g_peer[p], MSG_HASH, b, len);
        }
    }
    free(b);
    return 0;
}

/* wait until every player's input for `frame` is here (up to 20 s), reading whatever arrives */
static void wait_for(uint32_t frame) {
    for (;;) {
        int missing = 0;
        for (int p = 0; p < g_players; p++)
            if (g_have[frame % RING][p] != frame + 1) missing = 1;
        if (!missing || g_lost) return;
        fd_set fds;
        FD_ZERO(&fds);
        sock_t maxs = 0;
        for (int i = 0; i < 4; i++)
            if (g_peer[i] != BAD_SOCK) { FD_SET(g_peer[i], &fds); if (g_peer[i] > maxs) maxs = g_peer[i]; }
        struct timeval tv = {20, 0};
        int n = select((int)maxs + 1, &fds, NULL, NULL, &tv);
        if (n <= 0) { lost("no input from the other players for 20 s"); return; }
        for (int i = 0; i < 4; i++)
            if (g_peer[i] != BAD_SOCK && FD_ISSET(g_peer[i], &fds) && pump_one(i) < 0) {
                closesock(g_peer[i]);
                g_peer[i] = BAD_SOCK;
                lost("a player disconnected");
                return;
            }
    }
}

/* a sampled hash of main RAM: every 64th 8-byte word (enough to notice two games drifting apart) */
static uint64_t ram_hash(void) {
    const uint64_t *w = (const uint64_t *)g_mem;
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < PPC_MEM_SIZE / 8; i += 64) h = (h ^ w[i]) * 1099511628211ull;
    return h;
}

/* PADRead (hle.c), after the live input is in status[]: send this machine's controller for a later frame
 * and replace all four ports with the players' inputs for this frame. */
void netplay_pads(uint32_t status) {
    if (!g_netplay) return;
    uint8_t *st = ppc_ram(status);
    uint8_t local[12];
    memcpy(local, st, 12); /* this machine's controller is its port 1 */
    uint32_t f = g_frame++;
    if (!g_lost) {
        store(f + (uint32_t)g_delay, g_me, local);
        broadcast_input(f + (uint32_t)g_delay, g_me, local, -1);
    }
    if (f < (uint32_t)g_delay) { /* the first frames: nobody has pressed anything yet */
        uint8_t idle[12] = {0};
        for (int p = 0; p < g_players; p++) store(f, p, idle);
    }
    wait_for(f);
    for (int port = 0; port < 4; port++) {
        uint8_t *dst = st + port * 12;
        if (port < g_players && g_have[f % RING][port] == f + 1) {
            memcpy(dst, g_in[f % RING][port], 12);
            dst[10] = 0; /* connected */
        } else {
            memset(dst, 0, 12);
            dst[10] = (uint8_t)(port < g_players ? 0 : 0xFF); /* players who left read as idle; others unplugged */
        }
    }
    if (f % 60 == 30 && !g_lost) { /* compare hashes about once a second */
        uint64_t h = ram_hash();
        unsigned slot = (f / 60) % HSLOTS;
        g_hash_mine[slot] = h;
        g_hash_frame[slot] = f + 1;
        for (int p = 0; p < g_players; p++) if (p != g_me) compare(slot, p);
        uint8_t m[13];
        put32(m, f);
        put32(m + 4, (uint32_t)(h >> 32));
        put32(m + 8, (uint32_t)h);
        m[12] = (uint8_t)g_me;
        if (g_host) { for (int p = 1; p < g_players; p++) if (g_peer[p] != BAD_SOCK) send_msg(g_peer[p], MSG_HASH, m, sizeof m); }
        else send_msg(g_peer[0], MSG_HASH, m, sizeof m);
    }
}
