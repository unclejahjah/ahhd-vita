// The online leaderboard on the Vita: the ah_lb_* hooks of ah_host.c, talking to the score
// server in server/ over plain HTTP (the Vita's own HTTPS doesn't trust the certificate
// Cloudflare serves; scores are signed instead). Everything runs on one network thread, so
// the game never waits on Wi-Fi.
//
// The build needs -DAH_LB_URL=http://... and -DAH_LB_KEY=... (the server's SCORE_KEY);
// without them, or with a file named "offline" in the data folder, nothing goes online.
//
// Files in ux0:data/AlienHominidHD/:
//   player_id.txt    a random id made once, so the board keeps one row per player
//   lb_pending.txt   scores not sent yet (no Wi-Fi), sent next time

#include <psp2/apputil.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/libssl.h>
#include <psp2/net/http.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/rtc.h>
#include <psp2/sysmodule.h>
#include <psp2/system_param.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ah_host.h"
#include "ah_online.h"

#if defined(AH_LB_URL) && defined(AH_LB_KEY)
#define LB_BUILT 1
#else
#define LB_BUILT 0
#define AH_LB_URL ""
#define AH_LB_KEY ""
#endif

#define MAX_ROWS 101
#define MAX_PENDING 32

static char g_dir[128];
static char g_player[33], g_name[200];
static int g_enabled;

static SceUID g_lock = -1, g_wake = -1;
// what the game asked for (main thread) and what came back (network thread), under g_lock
static struct { char board[16]; int difficulty, score; unsigned time; } g_pending[MAX_PENDING];
static int g_npending;
static int g_req_wanted, g_req_difficulty, g_req_timeframe;
static int g_req_state;   // 0 none, 1 asked, 2 done, -1 failed
static AhLbRow g_rows[MAX_ROWS], g_shown[MAX_ROWS];
static int g_nrows, g_me;

static void lock(void) { sceKernelLockMutex(g_lock, 1, NULL); }
static void unlock(void) { sceKernelUnlockMutex(g_lock, 1); }

// ---- SHA-256 and HMAC (the score's signature) ---------------------------------------------

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

typedef struct { uint32_t h[8]; uint8_t buf[64]; uint64_t len; size_t n; } Sha;

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(Sha *s, const uint8_t *p) {
    uint32_t w[64], a[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(a, s->h, sizeof a);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = a[7] + (ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + K[i] + w[i];
        uint32_t t2 = (ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
        memmove(a + 1, a, 7 * sizeof *a);
        a[4] += t1;
        a[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) s->h[i] += a[i];
}
static void sha_init(Sha *s) {
    static const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, h0, sizeof h0);
    s->len = 0, s->n = 0;
}
static void sha_add(Sha *s, const void *data, size_t len) {
    const uint8_t *p = data;
    s->len += len;
    while (len--) {
        s->buf[s->n++] = *p++;
        if (s->n == 64) sha_block(s, s->buf), s->n = 0;
    }
}
static void sha_end(Sha *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, zero = 0;
    sha_add(s, &pad, 1);
    while (s->n != 56) sha_add(s, &zero, 1);
    for (int i = 7; i >= 0; i--) { uint8_t b = (uint8_t)(bits >> (i * 8)); sha_add(s, &b, 1); }
    for (int i = 0; i < 8; i++) out[i * 4] = s->h[i] >> 24, out[i * 4 + 1] = s->h[i] >> 16, out[i * 4 + 2] = s->h[i] >> 8, out[i * 4 + 3] = s->h[i];
}
static void hmac_hex(const char *key, const char *msg, char out[65]) {
    uint8_t k[64] = {0}, ipad[64], opad[64], inner[32], mac[32];
    size_t kl = strlen(key);
    Sha s;
    if (kl > 64) { sha_init(&s); sha_add(&s, key, kl); sha_end(&s, k); }
    else memcpy(k, key, kl);
    for (int i = 0; i < 64; i++) ipad[i] = k[i] ^ 0x36, opad[i] = k[i] ^ 0x5c;
    sha_init(&s); sha_add(&s, ipad, 64); sha_add(&s, msg, strlen(msg)); sha_end(&s, inner);
    sha_init(&s); sha_add(&s, opad, 64); sha_add(&s, inner, 32); sha_end(&s, mac);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", mac[i]);
}

// ---- files ---------------------------------------------------------------------------------

static int read_file(const char *name, char *buf, int size) {
    char path[192];
    snprintf(path, sizeof path, "%s%s", g_dir, name);
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return -1;
    int n = sceIoRead(fd, buf, size - 1);
    sceIoClose(fd);
    buf[n > 0 ? n : 0] = 0;
    return n;
}

static void write_file(const char *name, const char *buf) {
    char path[192];
    snprintf(path, sizeof path, "%s%s", g_dir, name);
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, buf, strlen(buf));
    sceIoClose(fd);
}

static void load_player_id(void) {
    char buf[64];
    if (read_file("player_id.txt", buf, sizeof buf) >= 32) {
        memcpy(g_player, buf, 32);
        g_player[32] = 0;
        int ok = 1;
        for (int i = 0; i < 32; i++) ok &= (g_player[i] >= '0' && g_player[i] <= '9') || (g_player[i] >= 'a' && g_player[i] <= 'f');
        if (ok) return;
    }
    // a new random id: the system clock's ticks and the process time, hashed
    SceRtcTick t;
    sceRtcGetCurrentTick(&t);
    char seed[64];
    snprintf(seed, sizeof seed, "%llu-%llu", (unsigned long long)t.tick, (unsigned long long)sceKernelGetProcessTimeWide());
    char hex[65];
    hmac_hex(seed, g_name, hex);
    memcpy(g_player, hex, 32);
    g_player[32] = 0;
    write_file("player_id.txt", g_player);
}

static void load_pending(void) {
    static char buf[4096];
    if (read_file("lb_pending.txt", buf, sizeof buf) <= 0) return;
    for (char *line = strtok(buf, "\n"); line && g_npending < MAX_PENDING; line = strtok(NULL, "\n")) {
        int d, sc;
        unsigned t;
        char board[16];
        if (sscanf(line, "%15s %d %d %u", board, &d, &sc, &t) != 4) continue;
        snprintf(g_pending[g_npending].board, sizeof g_pending[0].board, "%s", board);
        g_pending[g_npending].difficulty = d, g_pending[g_npending].score = sc, g_pending[g_npending].time = t;
        g_npending++;
    }
}

static void save_pending(void) {   // with g_lock held
    static char buf[4096];
    int n = 0;
    buf[0] = 0;
    for (int i = 0; i < g_npending; i++)
        n += snprintf(buf + n, sizeof buf - n, "%s %d %d %u\n", g_pending[i].board, g_pending[i].difficulty, g_pending[i].score, g_pending[i].time);
    write_file("lb_pending.txt", buf);
}

static unsigned unix_now(void) {
    SceRtcTick t;
    sceRtcGetCurrentTickUtc(&t);
    return (unsigned)(t.tick / 1000000ULL - 62135596800ULL);   // ticks are microseconds since year 1
}

// ---- HTTP ------------------------------------------------------------------------------------

static int g_tmpl = -1;

// GET (body NULL) or POST a form; the reply in out. The status code, or <0.
static int http(const char *path, const char *body, char *out, int outsize) {
    char url[256];
    snprintf(url, sizeof url, "%s%s", AH_LB_URL, path);
    int conn = sceHttpCreateConnectionWithURL(g_tmpl, url, 0);
    if (conn < 0) return -1;
    int req = sceHttpCreateRequestWithURL(conn, body ? SCE_HTTP_METHOD_POST : SCE_HTTP_METHOD_GET, url, body ? strlen(body) : 0);
    if (req < 0) { sceHttpDeleteConnection(conn); return -1; }
    if (body) sceHttpAddRequestHeader(req, "Content-Type", "application/x-www-form-urlencoded", SCE_HTTP_HEADER_OVERWRITE);
    int status = -1, n = 0;
    if (sceHttpSendRequest(req, body, body ? strlen(body) : 0) >= 0 && sceHttpGetStatusCode(req, &status) >= 0) {
        int r;
        while (n < outsize - 1 && (r = sceHttpReadData(req, out + n, outsize - 1 - n)) > 0) n += r;
    } else {
        status = -1;
    }
    out[n] = 0;
    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    return status;
}

static void url_encode(const char *s, char *out, size_t size) {
    size_t n = 0;
    for (; *s && n + 4 < size; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') out[n++] = c;
        else n += snprintf(out + n, size - n, "%%%02X", c);
    }
    out[n] = 0;
}

// 1 sent (or refused for good), 0 try again later
static int send_score(const char *board, int difficulty, int score, unsigned time) {
    (void)board;   // only "ahhd" is queued
    char msg[320], sig[65], name[640], body[1024], reply[64];
    snprintf(msg, sizeof msg, "%s|%s|%d|%d|%u", g_player, g_name, difficulty, score, time);
    hmac_hex(AH_LB_KEY, msg, sig);
    url_encode(g_name, name, sizeof name);
    snprintf(body, sizeof body, "player=%s&name=%s&difficulty=%d&score=%d&time=%u&sig=%s", g_player, name, difficulty, score, time, sig);
    int st = http("/score", body, reply, sizeof reply);
    if (st == 429) sceKernelDelayThread(11 * 1000000);   // one score every 10 s per player
    return st == 200 || (st >= 400 && st < 500 && st != 429);
}

static int fetch_board(int difficulty, int timeframe) {
    static char buf[MAX_ROWS * 48 + 64];
    char path[128];
    snprintf(path, sizeof path, "/top?difficulty=%d&timeframe=%d&player=%s", difficulty, timeframe, g_player);
    if (http(path, NULL, buf, sizeof buf) != 200) return 0;
    int n = 0, me = -1;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strncmp(line, "me ", 3)) { me = atoi(line + 3); continue; }
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2 || n == MAX_ROWS) continue;
        *t1 = *t2 = 0;
        g_rows[n].rank = atoi(line);
        snprintf(g_rows[n].name, sizeof g_rows[n].name, "%s", t1 + 1);
        g_rows[n].score = atof(t2 + 1);
        n++;
    }
    lock();
    g_nrows = n, g_me = me < n ? me : -1;
    unlock();
    return 1;
}

static int online_now(void) {
    int state = 0;
    return sceNetCtlInetGetState(&state) >= 0 && state == SCE_NETCTL_STATE_CONNECTED;
}

static int net_thread(SceSize args, void *argp) {
    (void)args, (void)argp;
    for (;;) {
        sceKernelWaitSema(g_wake, 1, NULL);
        if (!online_now()) {   // asked for a board: say so at once; queued scores wait
            lock();
            if (g_req_wanted) g_req_wanted = 0, g_req_state = -1;
            unlock();
            continue;
        }
        lock();
        int want = g_req_wanted, d = g_req_difficulty, tf = g_req_timeframe;
        g_req_wanted = 0;
        unlock();
        if (want) {
            int ok = fetch_board(d, tf);
            lock();
            g_req_state = ok ? 2 : -1;
            unlock();
        }
        for (;;) {   // then the queued scores, oldest first
            lock();
            if (!g_npending) { unlock(); break; }
            char board[16];
            snprintf(board, sizeof board, "%s", g_pending[0].board);
            int dd = g_pending[0].difficulty, sc = g_pending[0].score;
            unsigned t = g_pending[0].time;
            unlock();
            if (!send_score(board, dd, sc, t)) break;
            lock();
            memmove(g_pending, g_pending + 1, (size_t)(--g_npending) * sizeof *g_pending);
            save_pending();
            unlock();
            lock();
            int asked = g_req_wanted;
            unlock();
            if (asked) break;   // a board asked for meanwhile goes first
        }
    }
    return 0;
}

// ---- the hooks -------------------------------------------------------------------------------

static const char *lb_name(void) { return g_name; }
static int lb_online(void) { return g_enabled; }

static void lb_submit(const char *board, int difficulty, double score) {
    lock();
    if (g_npending == MAX_PENDING) memmove(g_pending, g_pending + 1, (size_t)(--g_npending) * sizeof *g_pending);
    snprintf(g_pending[g_npending].board, sizeof g_pending[0].board, "%s", board);
    g_pending[g_npending].difficulty = difficulty;
    g_pending[g_npending].score = (int)score;
    g_pending[g_npending].time = unix_now();
    g_npending++;
    save_pending();
    unlock();
    sceKernelSignalSema(g_wake, 1);
}

static void lb_request(const char *board, int difficulty, int timeframe, int around_me) {
    (void)board, (void)around_me;   // the reply always has the top rows and the player's own
    lock();
    g_req_wanted = 1, g_req_difficulty = difficulty, g_req_timeframe = timeframe, g_req_state = 1;
    unlock();
    sceKernelSignalSema(g_wake, 1);
}

static int lb_poll(const AhLbRow **rows, int *nrows, int *me) {
    lock();
    int st = g_req_state;
    if (st == 2) {   // a copy the menu keeps reading while the next request may refill g_rows
        memcpy(g_shown, g_rows, sizeof(AhLbRow) * (size_t)g_nrows);
        *rows = g_shown, *nrows = g_nrows, *me = g_me;
        g_req_state = 0;
    }
    unlock();
    return st == 2 ? 1 : st == -1 ? -1 : 0;
}

void ah_online_init(const char *data_dir) {
    snprintf(g_dir, sizeof g_dir, "%s", data_dir);
    SceIoStat st;
    char off[192];
    snprintf(off, sizeof off, "%soffline", g_dir);
    if (!LB_BUILT || sceIoGetstat(off, &st) >= 0) return;

    // the player's name: the Vita account's
    SceAppUtilInitParam init = {0};
    SceAppUtilBootParam boot = {0};
    sceAppUtilInit(&init, &boot);
    if (sceAppUtilSystemParamGetString(SCE_SYSTEM_PARAM_ID_USERNAME, (SceChar8 *)g_name, sizeof g_name) < 0 || !g_name[0])
        snprintf(g_name, sizeof g_name, "Player");
    // the server keeps 16 bytes: cut there, on a whole UTF-8 character
    int cut = 16;
    if ((int)strlen(g_name) > cut) {
        while (cut > 0 && ((unsigned char)g_name[cut] & 0xC0) == 0x80) cut--;
        g_name[cut] = 0;
    }
    load_player_id();
    load_pending();

    static char net_mem[1024 * 1024] __attribute__((aligned(64)));
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    SceNetInitParam np = {net_mem, sizeof net_mem, 0};
    sceNetInit(&np);   // already done is fine
    sceNetCtlInit();
    sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS);
    sceSslInit(300 * 1024);
    sceHttpInit(80 * 1024);
    g_tmpl = sceHttpCreateTemplate("AlienHominidVita/1.0", SCE_HTTP_VERSION_1_1, 1);
    if (g_tmpl < 0) return;
    sceHttpSetResolveTimeOut(g_tmpl, 5 * 1000000);
    sceHttpSetConnectTimeOut(g_tmpl, 8 * 1000000);
    sceHttpSetRecvTimeOut(g_tmpl, 8 * 1000000);

    g_lock = sceKernelCreateMutex("ah_lb", 0, 0, NULL);
    g_wake = sceKernelCreateSema("ah_lb", 0, g_npending ? 1 : 0, 64, NULL);
    SceUID t = sceKernelCreateThread("ah_lb", net_thread, 0x10000100 + 30, 0x10000, 0, 0, NULL);
    if (t < 0) return;
    sceKernelStartThread(t, 0, NULL);
    g_enabled = 1;
    ah_lb_player_name = lb_name;
    ah_lb_online = lb_online;
    ah_lb_submit = lb_submit;
    ah_lb_request = lb_request;
    ah_lb_poll = lb_poll;
}
