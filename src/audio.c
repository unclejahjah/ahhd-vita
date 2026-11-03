#include "audio.h"

#include "msadpcm.h"
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define OUT_RATE 48000             // the music's rate
#define OUT_FRAMES 1024            // per output block (~21 ms)
#define MAX_SFX 512
#define MAX_VOICES 32
#define CACHE_BYTES (24 * 1024 * 1024)   // decoded sound effects kept in memory (of the 160 MB heap)
#define MUSIC_VOLUME 0.7f

enum { SFX_NONE, SFX_QUEUED, SFX_READY, SFX_FAILED };

typedef struct {
    char name[64];                 // linkage without extension
    volatile int state;
    int16_t *pcm;                  // stereo
    int frames, rate;
    uint32_t last_use;
} Sfx;

typedef struct {
    int active;
    int sfx;
    double pos;                    // in source frames
    float vl, vr;
    int loops;                     // further plays after this one
    const void *owner;
} Voice;

static char g_dir[128];
static Sfx g_sfx[MAX_SFX];
static int g_nsfx;
static size_t g_cache_bytes;
static uint32_t g_clock;
static Voice g_voices[MAX_VOICES];
static SceKernelLwMutexWork g_lock;
static int g_port = -1;
static volatile int g_quit;
static SceUID g_mix_thread = -1, g_load_thread = -1, g_load_sema = -1;

// music, owned by the mixer thread (requests come through these)
static char g_music_req[64];
static volatile int g_music_req_loop, g_music_req_seq;
static Adpcm g_music;              // g_music.f: playing
static int g_music_loop, g_music_rate;
static volatile int g_music_paused;
static volatile float g_music_vol = 1, g_sfx_vol = 1;
static char g_music_path[192];

static void lock(void) { sceKernelLockLwMutex(&g_lock, 1, NULL); }
static void unlock(void) { sceKernelUnlockLwMutex(&g_lock, 1); }

// ---- sound effects: decoded whole on the loader thread ------------------------------

static void decode_sfx(Sfx *s) {
    char path[256];
    snprintf(path, sizeof path, "%ssounds/%s.adpcm", g_dir, s->name);
    Adpcm a;
    if (!adpcm_open(&a, path)) { s->state = SFX_FAILED; return; }
    long total = adpcm_total_frames(&a);
    int16_t *pcm = malloc((size_t)(total + a.spb) * 4);
    size_t n = 0;
    int got;
    while (pcm && (got = adpcm_read_block(&a, pcm + n * 2)) > 0) n += got;
    long rate = a.rate;
    adpcm_close(&a);
    if (!n) { free(pcm); s->state = SFX_FAILED; return; }
    lock();
    s->pcm = pcm;
    s->frames = (int)n;
    s->rate = (int)rate;
    g_cache_bytes += n * 4;
    s->state = SFX_READY;
    // over budget: drop the least recently used effects nobody is playing
    while (g_cache_bytes > CACHE_BYTES) {
        int victim = -1;
        for (int i = 0; i < g_nsfx; i++) {
            if (g_sfx[i].state != SFX_READY || &g_sfx[i] == s) continue;
            int playing = 0;
            for (int v = 0; v < MAX_VOICES; v++) playing |= g_voices[v].active && g_voices[v].sfx == i;
            if (!playing && (victim < 0 || g_sfx[i].last_use < g_sfx[victim].last_use)) victim = i;
        }
        if (victim < 0) break;
        g_cache_bytes -= (size_t)g_sfx[victim].frames * 4;
        free(g_sfx[victim].pcm);
        g_sfx[victim].pcm = NULL;
        g_sfx[victim].state = SFX_NONE;
    }
    unlock();
}

static int load_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    while (!g_quit) {
        sceKernelWaitSema(g_load_sema, 1, NULL);
        for (int i = 0; i < g_nsfx && !g_quit; i++)
            if (g_sfx[i].state == SFX_QUEUED) decode_sfx(&g_sfx[i]);
    }
    return 0;
}

// ---- mixing ------------------------------------------------------------------------

static void music_close(void) {
    if (g_music.f) adpcm_close(&g_music);
}

static void music_open(void) {
    music_close();
    if (!adpcm_open(&g_music, g_music_path)) return;
    g_music_rate = g_music.rate;
}

// Adds up to n frames of music at the output rate into mix.
static void mix_music(int32_t *mix, int n) {
    static int16_t buf[(512 + 4) * 2];   // decoded frames: one block (and two kept)
    static int have, used;               // frames in buf, frames consumed
    static double frac;
    if (!g_music.f) { have = used = 0; frac = 0; return; }
    if (g_music_paused) return;
    double step = (double)g_music_rate / OUT_RATE;
    float vol = MUSIC_VOLUME * g_music_vol;
    for (int i = 0; i < n; i++) {
        while (used + 1 >= have) {   // keep two frames to interpolate between
            int keep = have - used;
            if (keep > 0) memmove(buf, buf + used * 2, keep * 4);
            have = keep > 0 ? keep : 0;
            used = 0;
            int got = g_music.spb <= 512 ? adpcm_read_block(&g_music, buf + have * 2) : 0;
            if (got > 0) { have += got; continue; }
            if (g_music_loop && g_music.data_size) { adpcm_rewind(&g_music); continue; }
            music_close();
            have = used = 0;
            return;
        }
        float t = (float)frac;
        for (int c = 0; c < 2; c++) {
            float a = buf[used * 2 + c], b = buf[(used + 1) * 2 + c];
            mix[i * 2 + c] += (int32_t)((a + (b - a) * t) * vol);
        }
        frac += step;
        while (frac >= 1) { frac -= 1; used++; }
    }
}

static int mix_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    static int16_t out[2][OUT_FRAMES * 2];
    static int32_t mix[OUT_FRAMES * 2];
    int which = 0, music_seq = 0;
    while (!g_quit) {
        // the latest music request: a track, or "" to stop
        if (g_music_req_seq != music_seq) {
            lock();
            music_seq = g_music_req_seq;
            int stop = !g_music_req[0];
            snprintf(g_music_path, sizeof g_music_path, "%smusic/%s.adpcm", g_dir, g_music_req);
            g_music_loop = g_music_req_loop;
            unlock();
            if (stop) {
                music_close();
            } else {
                music_open();
            }
        }
        memset(mix, 0, sizeof mix);
        mix_music(mix, OUT_FRAMES);
        lock();
        for (int v = 0; v < MAX_VOICES; v++) {
            Voice *vo = &g_voices[v];
            if (!vo->active) continue;
            Sfx *s = &g_sfx[vo->sfx];
            if (s->state != SFX_READY) {
                if (s->state == SFX_FAILED || s->state == SFX_NONE) vo->active = 0;
                continue;   // still decoding: starts when it's ready
            }
            double step = (double)s->rate / OUT_RATE;
            float sv = g_sfx_vol;
            for (int i = 0; i < OUT_FRAMES; i++) {
                int p = (int)vo->pos;
                if (p >= s->frames) {
                    if (vo->loops > 0) { vo->loops--; vo->pos -= s->frames; p = (int)vo->pos; }
                    else { vo->active = 0; break; }
                }
                mix[i * 2] += (int32_t)(s->pcm[p * 2] * vo->vl * sv);
                mix[i * 2 + 1] += (int32_t)(s->pcm[p * 2 + 1] * vo->vr * sv);
                vo->pos += step;
            }
        }
        unlock();
        for (int i = 0; i < OUT_FRAMES * 2; i++) {
            int32_t x = mix[i];
            out[which][i] = x > 32767 ? 32767 : x < -32768 ? -32768 : x;
        }
        sceAudioOutOutput(g_port, out[which]);   // blocks until the port takes it
        which ^= 1;
    }
    return 0;
}

// ---- API ---------------------------------------------------------------------------

int audio_init(const char *data_dir) {
    snprintf(g_dir, sizeof g_dir, "%s", data_dir);
    g_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, OUT_FRAMES, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (g_port < 0) return 0;
    sceKernelCreateLwMutex(&g_lock, "cc_audio", 0, 0, NULL);
    g_load_sema = sceKernelCreateSema("cc_sfx_load", 0, 0, 1 << 20, NULL);
    g_mix_thread = sceKernelCreateThread("cc_mix", mix_thread, 0x10000100 - 10, 0x10000, 0, 0, NULL);
    g_load_thread = sceKernelCreateThread("cc_sfx", load_thread, 0x10000100 + 10, 0x10000, 0, 0, NULL);
    if (g_mix_thread < 0 || g_load_thread < 0) return 0;
    sceKernelStartThread(g_mix_thread, 0, NULL);
    sceKernelStartThread(g_load_thread, 0, NULL);
    return 1;
}

void audio_shutdown(void) {
    if (g_port < 0) return;
    g_quit = 1;
    sceKernelSignalSema(g_load_sema, 1);
    sceKernelWaitThreadEnd(g_mix_thread, NULL, NULL);
    sceKernelWaitThreadEnd(g_load_thread, NULL, NULL);
    sceAudioOutReleasePort(g_port);
    g_port = -1;
}

static void set_gain(Voice *v, float volume, float pan) {
    if (volume < 0) volume = 0;
    if (pan < -1) pan = -1;
    if (pan > 1) pan = 1;
    v->vl = volume * (pan > 0 ? 1 - pan : 1);
    v->vr = volume * (pan < 0 ? 1 + pan : 1);
}

void audio_sfx_play(const char *linkage, float volume, float pan, int loops, const void *owner) {
    if (g_port < 0) return;
    char name[64];
    snprintf(name, sizeof name, "%s", linkage);
    char *dot = strrchr(name, '.');
    if (dot) *dot = 0;
    lock();
    int i;
    for (i = 0; i < g_nsfx && strcasecmp(g_sfx[i].name, name); i++) {}
    if (i == g_nsfx) {
        if (g_nsfx == MAX_SFX) { unlock(); return; }
        memset(&g_sfx[i], 0, sizeof g_sfx[i]);
        snprintf(g_sfx[i].name, sizeof g_sfx[i].name, "%s", name);
        g_nsfx++;
    }
    Sfx *s = &g_sfx[i];
    s->last_use = ++g_clock;
    if (s->state == SFX_FAILED) { unlock(); return; }
    if (s->state == SFX_NONE) {
        s->state = SFX_QUEUED;
        sceKernelSignalSema(g_load_sema, 1);
    }
    // a free voice, or the oldest
    int v, best = 0;
    for (v = 0; v < MAX_VOICES && g_voices[v].active; v++) {}
    if (v == MAX_VOICES) v = best;
    g_voices[v] = (Voice){1, i, 0, 0, 0, loops > 1 ? loops - 1 : 0, owner};
    set_gain(&g_voices[v], volume, pan);
    unlock();
}

void audio_sfx_stop(const void *owner) {
    if (g_port < 0) return;
    lock();
    for (int v = 0; v < MAX_VOICES; v++)
        if (!owner || g_voices[v].owner == owner) g_voices[v].active = 0;
    unlock();
}

void audio_music_play(const char *name, int loop) {
    if (g_port < 0) return;
    lock();
    snprintf(g_music_req, sizeof g_music_req, "%s", name);
    g_music_req_loop = loop;
    g_music_req_seq++;
    unlock();
}

void audio_music_stop(void) {
    if (g_port < 0) return;
    lock();
    g_music_req[0] = 0;
    g_music_req_seq++;
    unlock();
}

void audio_music_pause(int paused) { g_music_paused = paused; }
void audio_set_volumes(float music, float sfx) {
    if (music >= 0) g_music_vol = music;
    if (sfx >= 0) g_sfx_vol = sfx;
}

void audio_sfx_set(const void *owner, float volume, float pan) {
    if (g_port < 0 || !owner) return;
    lock();
    for (int v = 0; v < MAX_VOICES; v++)
        if (g_voices[v].active && g_voices[v].owner == owner) set_gain(&g_voices[v], volume, pan);
    unlock();
}
