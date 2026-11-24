/* bots.c - computer players for VS mode.
 *
 * The game has no computer opponents in VS (the attract-mode matches are a pre-rendered movie), so a
 * bot is a stand-in controller: it reads the players' state from the game's memory and works the
 * stick and buttons of an empty port, the way a person would.
 *
 * What the game keeps per VS player (found from RAM snapshots of a match; see recomp/README.md):
 *   - the player object is the first argument of fn_8007FE88, called once per player per frame
 *     (the entry hook below records it); its controller port is the byte at +0x1FA
 *   - +0x60: facing direction (unit x, y, z); +0x70: position (x, y, z), y up
 * Controls on foot: the stick's x turns (right = counter-clockwise seen from above, i.e. from +x
 * towards +z), its y walks forward/back along the facing; A fires.
 *
 * Bots find their way with a map they learn as the match goes (see "the map, learned from where
 * players walk" below). SFA_BOTS=<ports> (e.g. "234") makes those ports bots during VS matches.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "funcs.h"
#include "runtime.h"
#include "gx.h"

#define NPLAYERS 4
extern uint32_t g_game_state; /* hle.c: 22 = a VS match */

static uint32_t g_obj[NPLAYERS];   /* the player objects of the match in progress, by port */
static uint32_t g_obj_frame[NPLAYERS];
static uint32_t g_frame;            /* counts calls of the frame hook below */

static float rdf(uint32_t a) {
    uint32_t w = rd32(a);
    float f;
    memcpy(&f, &w, 4);
    return f;
}

/* ---- the map, learned from where players walk -----------------------------------------------------
 * Space is cut into cells (8 x 6 x 8 units, y up). Every 200 ms each player's cell is noted; moving
 * from one cell to a neighbouring one (less than 25 units, so not a respawn) proves that step can be
 * walked, and becomes a one-way link (one way because drops can't be climbed back). A bot finds its way
 * to a target by a breadth-first search over these links. The graph starts empty with each map and
 * grows as everyone (bots included) moves around. */
#define NAV_MAX 4096
#define NAV_LINKS 8
#define NAV_HASH 8192
typedef struct {
    int16_t cx, cy, cz;
    uint16_t nlinks;
    float x, y, z; /* where players actually stood in this cell (running average) */
    uint32_t samples;
    uint16_t link[NAV_LINKS];
} NavNode;
static NavNode g_nav[NAV_MAX];
static int g_nnav;
static int16_t g_nav_hash[NAV_HASH];
static int g_nav_stage = -1;
static int g_last_node[NPLAYERS] = {-1, -1, -1, -1};
static float g_last_pos[NPLAYERS][3];

/* Drops: where a bot walked off and fell to its death. Learned like the map (a bot dies after its height
 * fell more than 8 units in its last second; the spot it fell from is kept), and avoided from then on. */
#define HAZ_MAX 64
static float g_haz[HAZ_MAX][3];
static int g_nhaz;

static void haz_add(float x, float y, float z) {
    for (int i = 0; i < g_nhaz; i++)
        if (fabsf(g_haz[i][0] - x) < 4 && fabsf(g_haz[i][2] - z) < 4 && fabsf(g_haz[i][1] - y) < 3) return;
    int i = g_nhaz < HAZ_MAX ? g_nhaz++ : (int)(x * 7 + z * 13) & (HAZ_MAX - 1);
    g_haz[i][0] = x; g_haz[i][1] = y; g_haz[i][2] = z;
}

/* a known drop within r of (x, z) at about height y */
static int haz_near(float x, float y, float z, float r) {
    for (int i = 0; i < g_nhaz; i++)
        if (fabsf(g_haz[i][1] - y) < 4 && hypotf(g_haz[i][0] - x, g_haz[i][2] - z) < r) return 1;
    return 0;
}

static void nav_reset(void) {
    g_nhaz = 0;
    g_nnav = 0;
    memset(g_nav_hash, 0xFF, sizeof g_nav_hash);
    for (int p = 0; p < NPLAYERS; p++) g_last_node[p] = -1;
}

static int nav_cell(float x, float y, float z, int16_t c[3]) {
    c[0] = (int16_t)floorf(x / 8.0f);
    c[1] = (int16_t)floorf(y / 6.0f);
    c[2] = (int16_t)floorf(z / 8.0f);
    uint32_t h = ((uint32_t)c[0] * 73856093u) ^ ((uint32_t)c[1] * 19349663u) ^ ((uint32_t)c[2] * 83492791u);
    return (int)(h & (NAV_HASH - 1));
}

static int nav_find(float x, float y, float z, int add) {
    int16_t c[3];
    int h = nav_cell(x, y, z, c);
    for (int i = 0; i < NAV_HASH; i++, h = (h + 1) & (NAV_HASH - 1)) {
        int n = g_nav_hash[h];
        if (n < 0) {
            if (!add || g_nnav == NAV_MAX) return -1;
            n = g_nnav++;
            NavNode *nd = &g_nav[n];
            memset(nd, 0, sizeof *nd);
            nd->cx = c[0]; nd->cy = c[1]; nd->cz = c[2];
            g_nav_hash[h] = (int16_t)n;
            return n;
        }
        if (g_nav[n].cx == c[0] && g_nav[n].cy == c[1] && g_nav[n].cz == c[2]) return n;
    }
    return -1;
}

static void nav_link(int a, int b) {
    NavNode *n = &g_nav[a];
    for (int i = 0; i < n->nlinks; i++) if (n->link[i] == b) return;
    if (n->nlinks < NAV_LINKS) n->link[n->nlinks++] = (uint16_t)b;
}

/* the nearest known node to a point (its own cell first), within 20 units */
static int nav_nearest(float x, float y, float z) {
    int n = nav_find(x, y, z, 0);
    if (n >= 0) return n;
    float best = 20.0f * 20.0f;
    for (int i = 0; i < g_nnav; i++) {
        float dx = g_nav[i].x - x, dy = (g_nav[i].y - y) * 2.0f, dz = g_nav[i].z - z;
        float d = dx * dx + dy * dy + dz * dz;
        if (d < best) { best = d; n = i; }
    }
    return n;
}

/* record where player p is now (called every 200 ms) */
static void nav_note(int p, float x, float y, float z) {
    int n = nav_find(x, y, z, 1);
    if (n < 0) return;
    NavNode *nd = &g_nav[n];
    nd->samples++;
    float k = 1.0f / (float)(nd->samples < 16 ? nd->samples : 16);
    nd->x += (x - nd->x) * k; nd->y += (y - nd->y) * k; nd->z += (z - nd->z) * k;
    int last = g_last_node[p];
    float dx = x - g_last_pos[p][0], dy = y - g_last_pos[p][1], dz = z - g_last_pos[p][2];
    if (last >= 0 && last != n && dx * dx + dy * dy + dz * dz < 25.0f * 25.0f) nav_link(last, n);
    g_last_node[p] = n;
    g_last_pos[p][0] = x; g_last_pos[p][1] = y; g_last_pos[p][2] = z;
}

/* breadth-first search from a to b; returns the node to head for next (a few steps along the path,
 * for smoother walking), or -1 when b can't be reached over known links */
static int nav_next(int a, int b) {
    static int16_t prev[NAV_MAX];
    static uint16_t queue[NAV_MAX];
    if (a < 0 || b < 0) return -1;
    if (a == b) return b;
    for (int i = 0; i < g_nnav; i++) prev[i] = -1;
    int head = 0, tail = 0;
    queue[tail++] = (uint16_t)a;
    prev[a] = (int16_t)a;
    while (head < tail) {
        int n = queue[head++];
        if (n == b) break;
        for (int i = 0; i < g_nav[n].nlinks; i++) {
            int m = g_nav[n].link[i];
            if (prev[m] >= 0) continue;
            prev[m] = (int16_t)n;
            queue[tail++] = (uint16_t)m;
        }
    }
    if (prev[b] < 0) return -1;
    int path[NAV_MAX], len = 0;
    for (int n = b; n != a; n = prev[n]) path[len++] = n;
    return path[len - 1 - (len > 2 ? 1 : 0)]; /* the second step when there is one */
}

/* m2:fn_6_2BE4: the VS character-select screen's update, once a frame while it's open. */
static uint64_t g_charsel_tb;
static uint32_t g_charsel_session; /* counts openings of the screen */
void rt_hook_90C02BE4(Ctx *c) {
    (void)c;
    uint64_t t = rt_timebase();
    if (!g_charsel_tb || t - g_charsel_tb > 40500000ull) { /* opened (closed for over a second before) */
        g_charsel_session++;
    }
    g_charsel_tb = t;
}

static int charsel_open(void) { return g_charsel_tb && rt_timebase() - g_charsel_tb < 40500000ull / 5; }

/* fn_8007FE88(player): the per-player update in VS (and elsewhere). */
void rt_hook_8007FE88(Ctx *c) {
    uint32_t obj = c->r[3];
    if (!ppc_is_ram(obj)) return;
    uint32_t port = rd8(obj + 0x1FA);
    if (port >= NPLAYERS) return;
    if (port == 0) {
        g_frame++;
        static uint32_t next_report; /* with SFA_BOTS: the score table (0x802A2E63, one byte per port) */
        if (getenv("SFA_BOTS_DEBUG") && g_frame >= next_report) {
            next_report = g_frame + 300;
            printf("[bots] frame %u: scores %u %u %u %u; map %d cells, %d drops\n", g_frame, rd8(0x802A2E63u),
                   rd8(0x802A2E64u), rd8(0x802A2E65u), rd8(0x802A2E66u), g_nnav, g_nhaz);
            fflush(stdout);
        }
    }
    g_obj[port] = obj;
    g_obj_frame[port] = g_frame;
    if (g_game_state != 22) return;
    int stage = rd8(0x802A2B60u + 0x13C);
    if (stage != g_nav_stage) { g_nav_stage = stage; nav_reset(); } /* a new map: learn it from scratch */
    if ((g_frame + (uint32_t)port) % 6 == 0) { /* every 200 ms at 30 fps */
        float x = rdf(obj + 0x70), y = rdf(obj + 0x74), z = rdf(obj + 0x78);
        if (isfinite(x) && isfinite(y) && isfinite(z)) nav_note((int)port, x, y, z);
    }
}


typedef struct { float x, y, z, fx, fz; } Pose;

static int pose(int port, Pose *p) {
    uint32_t o = g_obj[port];
    if (!o || g_frame - g_obj_frame[port] > 30) return 0; /* not updated lately: gone */
    p->x = rdf(o + 0x70);
    p->y = rdf(o + 0x74);
    p->z = rdf(o + 0x78);
    p->fx = rdf(o + 0x60);
    p->fz = rdf(o + 0x68);
    return isfinite(p->x) && isfinite(p->z) && isfinite(p->fx) && isfinite(p->fz);
}

typedef struct {
    float last_x, last_z;
    uint32_t still_since, last_check;
    uint32_t wander_until; /* walking off on its own heading after getting stuck */
    uint32_t ignore_until[NPLAYERS]; /* targets it couldn't get at (behind a wall, another floor) */
    uint32_t idle_since;             /* when it last moved, walking or not */
    float hist[4][3];                /* its position over the last second (every 250 ms), oldest first */
    int alive;
    float wander_x, wander_z;
    uint32_t rng;
} BotMind;

static uint32_t next_rand(BotMind *m) {
    m->rng = m->rng * 1664525u + 1013904223u;
    return m->rng >> 8;
}
static BotMind g_mind[NPLAYERS];

/* Which ports are bots. On the character-select screen, Z on any controller (or the keyboard) puts a
 * bot in the next free slot: a port with no controller that isn't a bot yet. A bot joins, steps through
 * the characters a random number of times, picks one and gets ready. Bots stay for the next matches;
 * backing out of the character screen (to stage select) takes them all out, so the next visit starts
 * with only the bots added then. Bots never press B (on some slot states B backs the whole screen out)
 * and, after picking, only A (the stick would change the handicap). SFA_BOTS=<ports> (testing, e.g. "234") starts with those ports as bots. */
static int g_bots = -1;                 /* bitmask of bot ports */
static uint32_t g_join_session[NPLAYERS], g_join_ms[NPLAYERS];
static int g_steps[NPLAYERS];           /* right-presses before picking: the random character */
static uint32_t g_human_ms[NPLAYERS];   /* when a real controller last reported on each port */
static uint16_t g_human_buttons[NPLAYERS];
static uint32_t g_rand = 0x2545F491u;

static uint32_t bots_rand(void) {
    g_rand ^= g_rand << 13; g_rand ^= g_rand >> 17; g_rand ^= g_rand << 5;
    return g_rand;
}

static int bot_mask(void) {
    if (g_bots < 0) {
        g_bots = 0;
        const char *e = getenv("SFA_BOTS");
        for (; e && *e; e++)
            if (*e >= '1' && *e <= '4') g_bots |= 1 << (*e - '1');
    }
    return g_bots;
}

/* Backing out of the character screen takes the bots out. The screen closes either way: after a
 * person's START (the match starts) or after their B (back to stage select). So when it has closed, the
 * last of those two pressed on it decides, if it came within 1.5 s of the close. */
static uint32_t g_charsel_last_b, g_charsel_last_start; /* ms of a person's last B / START on the screen */
static uint32_t g_closed_session;

static void check_backed_out(uint32_t now_ms) {
    if (!g_charsel_session || charsel_open() || g_closed_session == g_charsel_session) return;
    g_closed_session = g_charsel_session;
    int backed = g_charsel_last_b > g_charsel_last_start && now_ms - g_charsel_last_b < 1500 + 400;
    if (backed && g_bots > 0) {
        g_bots = 0;
        printf("[bots] left the character screen: computer players removed\n");
        fflush(stdout);
    }
}

int bots_active(int port) { return (bot_mask() >> port) & 1; }

/* video.c: what a real controller (or the keyboard) on a port pressed this frame. */
void bots_human_input(int port, uint16_t buttons, uint32_t now_ms) {
    if (port < 0 || port >= NPLAYERS) return;
    uint16_t pressed = buttons & ~g_human_buttons[port];
    g_human_buttons[port] = buttons;
    g_human_ms[port] = now_ms;
    if (charsel_open()) {
        if (pressed & 0x200) g_charsel_last_b = now_ms;
        if (pressed & 0x1000) g_charsel_last_start = now_ms;
    } else if (port == 0)
        check_backed_out(now_ms); /* port 1 always reports, every frame */
    if (pressed & 0x10) {
        printf("[bots] Z on port %d (character screen %s)\n", port + 1, charsel_open() ? "open" : "closed");
        fflush(stdout);
    }
    if (!(pressed & 0x10) || !charsel_open()) return; /* Z, on the character-select screen */
    g_rand ^= now_ms * 2654435761u;
    int mask = bot_mask();
    for (int p = 1; p < NPLAYERS; p++) {
        int has_controller = g_human_ms[p] && now_ms - g_human_ms[p] < 1000;
        if (has_controller || (mask >> p & 1)) continue;
        g_bots |= 1 << p; /* a new bot in the first free slot */
        g_join_session[p] = 0;
        printf("[bots] player %d is now a computer player\n", p + 1);
        fflush(stdout);
        return;
    }
}

/* Fill a bot port's controller for this frame; returns 0 when there's no match to play. */
int bots_pad(int port, PadState *out, uint32_t now_ms) {
    if (!bots_active(port)) return 0;
    if (g_game_state != 22) {
        /* menus: a connected controller that presses nothing, except on the character-select screen.
         * There, each time the screen opens: A to join, the stick right a random number of times
         * (a random character), then A now and then to pick it and get ready */
        memset(out, 0, sizeof *out);
        out->connected = 1;
        if (!charsel_open()) return 1;
        if (g_join_session[port] != g_charsel_session) {
            g_join_session[port] = g_charsel_session;
            g_join_ms[port] = now_ms + (uint32_t)port * 300; /* not all at once */
            g_steps[port] = (int)(bots_rand() % 6);
        }
        if (now_ms < g_join_ms[port]) return 1;
        uint32_t t = now_ms - g_join_ms[port];
        uint32_t steps_end = 900 + (uint32_t)g_steps[port] * 450;
        if (t < 150) out->buttons |= 0x100;                   /* join */
        else if (t >= 900 && t < steps_end) {                  /* next character */
            if ((t - 900) % 450 < 150) { out->buttons |= 0x2; out->stick_x = 100; }
        } else if (t >= steps_end + 300 && (t - steps_end) % 1500 < 120)
            out->buttons |= 0x100; /* pick, handicap, ready (more A once ready does nothing) */
        return 1;
    }
    if (!(bot_mask() >> port & 1)) return 0;
    Pose me, other;
    if (!pose(port, &me)) { /* no player object: defeated, waiting to come back -> press A */
        BotMind *dm = &g_mind[port];
        if (dm->alive) { /* it just died: if it was falling, remember where it went over the edge */
            dm->alive = 0;
            if (dm->hist[0][1] - dm->hist[3][1] > 8.0f) haz_add(dm->hist[0][0], dm->hist[0][1], dm->hist[0][2]);
        }
        memset(out, 0, sizeof *out);
        out->connected = 1;
        if (now_ms % 600 < 150) out->buttons |= 0x100;
        return 1;
    }
    /* target: the nearest other player */
    int target = -1;
    float best = 1e30f, tx = 0, tz = 0, ty = 0;
    BotMind *m = &g_mind[port];
    for (int pass = 0; pass < 2 && target < 0; pass++) /* second pass: everyone is ignored, take any */
        for (int p = 0; p < NPLAYERS; p++) {
            if (p == port || !pose(p, &other) || (pass == 0 && now_ms < m->ignore_until[p])) continue;
            float dx = other.x - me.x, dy = (other.y - me.y) * 3.0f, dz = other.z - me.z;
            float d = dx * dx + dy * dy + dz * dz; /* height counts extra: another floor is far */
            if (d < best) { best = d; target = p; tx = other.x; ty = other.y; tz = other.z; }
        }
    memset(out, 0, sizeof *out);
    out->connected = 1;
    if (target < 0) return 1;

    float dx = tx - me.x, dz = tz - me.z, dist = sqrtf(dx * dx + dz * dz);
    float flen = sqrtf(me.fx * me.fx + me.fz * me.fz);
    if (flen < 1e-3f || dist < 1e-3f) return 1;
    /* angle from facing to the target: positive = counter-clockwise (x towards z) = stick right */
    float cross = (me.fx * dz - me.fz * dx) / (flen * dist), dot = (me.fx * dx + me.fz * dz) / (flen * dist);
    float ang = atan2f(cross, dot);
    float deg = fabsf(ang) * 57.3f;

    /* progress check every 250 ms: hardly moving while trying to walk means something is in the way */
    if (!m->alive) { /* (back) in the game: start its history afresh */
        m->alive = 1;
        for (int k = 0; k < 4; k++) { m->hist[k][0] = me.x; m->hist[k][1] = me.y; m->hist[k][2] = me.z; }
    }
    if (now_ms - m->last_check >= 250) {
        memmove(m->hist[0], m->hist[1], sizeof m->hist[0] * 3);
        m->hist[3][0] = me.x; m->hist[3][1] = me.y; m->hist[3][2] = me.z;
        if (hypotf(me.x - m->last_x, me.z - m->last_z) > 1.0f) m->still_since = m->idle_since = now_ms;
        m->last_x = me.x;
        m->last_z = me.z;
        m->last_check = now_ms;
    }

    float steer = ang; /* where to turn: the target, a waypoint on the way to it, or the wander heading */
    float dy = ty - me.y;
    int walk = deg < 50 && (dist > 18 || fabsf(dy) > 4);
    if (!m->idle_since) m->idle_since = now_ms;
    if (now_ms - m->idle_since > 3000 && now_ms >= m->wander_until) {
        /* standing still for 3 s (shooting at a wall, below a ledge...): leave that target for a while */
        if (!m->rng) m->rng = 0x9E3779B9u * (uint32_t)(port + 1) ^ now_ms;
        m->ignore_until[target] = now_ms + 6000;
        float h = atan2f(me.fz, me.fx) + 1.57f + (float)(next_rand(m) % 1000) / 1000.0f * 3.14f;
        m->wander_x = cosf(h);
        m->wander_z = sinf(h);
        m->wander_until = now_ms + 2500;
        m->idle_since = now_ms;
    }
    if (dist > 30 || fabsf(dy) > 4) { /* not right there: follow the learned map if it knows a way */
        int wp = nav_next(nav_nearest(me.x, me.y, me.z), nav_nearest(tx, ty, tz));
        if (wp >= 0) {
            float wx = g_nav[wp].x - me.x, wz = g_nav[wp].z - me.z, wd = sqrtf(wx * wx + wz * wz);
            if (wd > 1.0f) {
                steer = atan2f((me.fx * wz - me.fz * wx) / (flen * wd), (me.fx * wx + me.fz * wz) / (flen * wd));
                walk = fabsf(steer) < 0.9f;
            }
        }
    }
    if (now_ms < m->wander_until) {
        float wx = m->wander_x, wz = m->wander_z;
        steer = atan2f((me.fx * wz - me.fz * wx) / flen, (me.fx * wx + me.fz * wz) / flen);
        walk = fabsf(steer) < 0.9f;
    }
    if (walk && haz_near(me.x + me.fx / flen * 8.0f, me.y, me.z + me.fz / flen * 8.0f, 7.0f)) {
        /* a known drop ahead: turn back instead */
        if (!m->rng) m->rng = 0x9E3779B9u * (uint32_t)(port + 1) ^ now_ms;
        float h = atan2f(me.fz, me.fx) + 3.14f + ((float)(next_rand(m) % 1000) / 1000.0f - 0.5f) * 1.5f;
        m->wander_x = cosf(h);
        m->wander_z = sinf(h);
        m->wander_until = now_ms + 1500;
        walk = 0;
    }
    if (walk) {
        out->stick_y = 100;
        if (now_ms - m->still_since > 1000) { /* walked into something: head off somewhere else */
            if (!m->rng) m->rng = 0x9E3779B9u * (uint32_t)(port + 1) ^ now_ms;
            float h = atan2f(me.fz, me.fx) + 1.57f + (float)(next_rand(m) % 1000) / 1000.0f * 3.14f;
            m->wander_x = cosf(h);
            m->wander_z = sinf(h);
            m->wander_until = now_ms + 2000 + next_rand(m) % 1500;
            m->still_since = now_ms;
        }
    } else {
        m->still_since = now_ms;
    }
    int turn = (int)(steer * 120.0f); /* full deflection beyond ~48 degrees */
    out->stick_x = (int8_t)(turn > 100 ? 100 : turn < -100 ? -100 : turn);
    if (deg < 10 && dist < 150 && now_ms % 300 < 100) out->buttons |= 0x100; /* A: fire */
    static int debug = -1; /* SFA_BOTS_DEBUG=1: each bot's view every 5 s */
    if (debug < 0) debug = getenv("SFA_BOTS_DEBUG") != NULL;
    static uint32_t last_dbg[NPLAYERS];
    if (debug && now_ms - last_dbg[port] >= 5000) {
        last_dbg[port] = now_ms;
        printf("[bot %d] at (%.0f, %.0f, %.0f) facing (%.2f, %.2f) -> player %d at %.0f (dy %.0f), angle %.0f, %s%s, "
               "stick %d,%d\n", port + 1, me.x, me.y, me.z, me.fx, me.fz, target + 1, dist, dy, ang * 57.3f,
               now_ms < m->wander_until ? "wandering" : "chasing", walk ? " walking" : "", out->stick_x, out->stick_y);
        fflush(stdout);
    }
    return 1;
}
