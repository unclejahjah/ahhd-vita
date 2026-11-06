// Alien Hominid HD host functions. Each is matched to how the level scripts use it
// (LEVEL11 disassembly, 2026-11-03); unknown ones are stubs that return what makes
// the scripts take the plain single-player path.
#include "ah_host.h"
#include "cc_host.h"
#include "cc_buttons.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

void (*ah_log)(const char *line);

// Front-end calls whose meaning is still being worked out: logged with their arguments.
static void log_call(AsVm *vm, const char *name, int argc, AsVal *argv) {
    if (!ah_log) return;
    char line[512];
    int n = snprintf(line, sizeof line, "%s(", name);
    for (int i = 0; i < argc && n < (int)sizeof line - 64; i++)
        n += snprintf(line + n, sizeof line - n, "%s%s", i ? ", " : "", as_tostr(vm, argv[i])->s);
    snprintf(line + n, sizeof line - n, ")");
    ah_log(line);
}
#define LOGGED(fn, ret)                                                          \
    static AsVal fn(AsVm *vm, AsVal this_, int argc, AsVal *argv) {              \
        (void)this_;                                                             \
        log_call(vm, #fn, argc, argv);                                           \
        return ret;                                                              \
    }
LOGGED(MenuRequestIsFullGame, as_bool(1))   // the full game, not the trial
LOGGED(MEMCARD_Start, as_undef())
LOGGED(MoveKeyboardUserToPort, as_undef())
LOGGED(DropMeFromGame, as_undef())
LOGGED(ClearUserToControllerBindings, as_undef())
LOGGED(StartControllerPairing, as_undef())
LOGGED(GetPrimaryUserPort, as_num(0))
LOGGED(CreateLocalGame, as_undef())
LOGGED(CancelGame, as_undef())
LOGGED(StopControllerPairing, as_undef())
LOGGED(ShowSignInScreen, as_undef())
LOGGED(EndingAttract, as_undef())
LOGGED(MenuAchievements, as_undef())
LOGGED(MenuMultiplayer, as_undef())
LOGGED(MenuReturnToArcade, as_undef())
LOGGED(MenuUnlockFullGame, as_undef())
LOGGED(ListenForKeyBinding, as_undef())
LOGGED(ResetKeyBindings, as_undef())
LOGGED(ChangeVideoSettings, as_undef())

#define ARG(i) ((i) < argc ? argv[i] : as_undef())

static AsVal ret_false(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)vm; (void)this_; (void)argc; (void)argv;
    return as_bool(0);
}
static AsVal ret_undef(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)vm; (void)this_; (void)argc; (void)argv;
    return as_undef();
}

// The memory card: 256 bytes per controller port, which the scripts address with
// MEMCARD_Seek(port, offset), then MEMCARD_Read(port) / MEMCARD_Write(port, value), each
// moving on a byte. Offsets: hats 0-31 (f_UnlockHat), Load Level from 32, minigames around
// 49-56 (f_UnlockMiniGame), 58 on Missile Mastar scores, 64-66, 69 vibration, 70 aim
// style, 72 lives, 73 turbo fire; the highest the scripts use is 81. All start at 0.
// Every port reads and writes card 0: the options menu only ever writes port 1's card, so
// player 2 in co-op would otherwise join on difficulty 0 with no hats, and their unlocks
// would land on a card nobody reads. Each port keeps its own read/write position.
#define MEMCARD_PORTS 4
#define MEMCARD_SIZE 256
static unsigned char memcard[MEMCARD_PORTS][MEMCARD_SIZE];
static int memcard_pos[MEMCARD_PORTS];

static int port_arg(AsVm *vm, AsVal v) {
    int p = (int)as_tonum(vm, v);
    return p >= 0 && p < MEMCARD_PORTS ? p : 0;
}

static AsVal memcard_seek(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    int off = (int)as_tonum(vm, ARG(1));
    memcard_pos[port_arg(vm, ARG(0))] = off >= 0 && off < MEMCARD_SIZE ? off : 0;
    return as_undef();
}

static AsVal memcard_read(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    int p = port_arg(vm, ARG(0));
    int v = memcard[0][memcard_pos[p]];
    memcard_pos[p] = (memcard_pos[p] + 1) % MEMCARD_SIZE;
    return as_num(v);
}

static AsVal memcard_write(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    int p = port_arg(vm, ARG(0));
    memcard[0][memcard_pos[p]] = (unsigned char)as_tonum(vm, ARG(1));
    memcard_pos[p] = (memcard_pos[p] + 1) % MEMCARD_SIZE;
    return as_undef();
}

// ---- the game layers ---------------------------------------------------------------
// The exe runs one root movie at a time (legal, attract, menu, a level...) and opens
// pause.pak over it for the main menu (menu.pak's TurnMainMenuOn) and the pause menu
// (START in a level). It tells that layer what to show through the flash global g_nReset
// (1 pause, 2 main menu, 3 profile signed out, 4 controller disconnected), which the
// layer's f_CheckReset reads and clears. UnPause and ChangeMovie close it.

int ah_paused;                      // the root movie is frozen under the pause menu
float ah_background_alpha;          // SetBackgroundAlpha: how much the overlay darkens the game (0..100)
static int playing;                 // SetPlaying
static int overlay_open, overlay_close;

static void open_overlay(AsVm *vm, int reset) {
    cc_set_flash_global(vm, "g_nReset", as_num(reset));
    overlay_open = 1;
}

static AsVal TurnMainMenuOn(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    log_call(vm, "TurnMainMenuOn", argc, argv);
    open_overlay(vm, 2);
    return as_undef();
}

// ---- sound: the front end plays it (audio.c on the Vita) -----------------------------
void (*ah_music_pause)(int paused);
void (*ah_audio_volumes)(float music, float sfx);
void (*ah_sfx_stop_all)(void);
void (*ah_sfx_hold)(int hold);

static void music_pause(int paused) {
    if (ah_music_pause) ah_music_pause(paused);
}

// PauseMusic(paused): the pause menu resumes the music with PauseMusic(false)
static AsVal PauseMusic(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    music_pause(as_tobool(vm, ARG(0)));
    return as_undef();
}

// SetMusicVolume(0..100), SetSfxVolume(0..100) and the Get pair, from the options menu
// (its sliders start from the Get values)
static double music_volume = 100, sfx_volume = 100;

static double volume_arg(AsVm *vm, AsVal v, double old) {
    double n = as_tonum(vm, v);
    return isnan(n) ? old : n < 0 ? 0 : n > 100 ? 100 : n;
}
static AsVal SetMusicVolume(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    music_volume = volume_arg(vm, ARG(0), music_volume);
    if (ah_audio_volumes) ah_audio_volumes((float)music_volume / 100, -1);
    return as_undef();
}
static AsVal SetSfxVolume(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    sfx_volume = volume_arg(vm, ARG(0), sfx_volume);
    if (ah_audio_volumes) ah_audio_volumes(-1, (float)sfx_volume / 100);
    return as_undef();
}
static AsVal GetMusicVolume(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)vm; (void)this_; (void)argc; (void)argv;
    return as_num(music_volume);
}
static AsVal GetSfxVolume(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)vm; (void)this_; (void)argc; (void)argv;
    return as_num(sfx_volume);
}

// ---- the save file -----------------------------------------------------------------
// The exe wrote the memory-card bytes to disk (hats 0-31, Load Level from 32, minigames
// around 49-56, options 69-73) and kept the volumes. Here they go to one file: "AHS2",
// then each port's 256 bytes, then the music and sound volumes (0-100) as one byte each.
// ah_save_tick writes it when anything changed; it's written to a temporary name first
// so a crash mid-write can't lose the old one.
#define SAVE_MAGIC "AHS2"   // "AHS1" files came from a build that mixed up MEMCARD_Seek's arguments: ignored
static unsigned char saved[MEMCARD_PORTS][MEMCARD_SIZE + 2];
static char save_path[256];
// the Gore option (GLOBAL_gore): the exe kept it in its own settings, not on the memory
// card. Saved in port 2's first spare byte: 0 never set, 1 off, 2 on.
static int gore_setting;

static void save_image(unsigned char out[MEMCARD_PORTS][MEMCARD_SIZE + 2]);

int ah_save_load(const char *path) {
    snprintf(save_path, sizeof save_path, "%s", path);
    save_image(saved);   // nothing on disk yet: the defaults count as saved
    FILE *f = fopen(path, "rb");
    if (!f) {   // stopped between removing the old file and renaming the new one
        char tmp[264];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        f = fopen(tmp, "rb");
    }
    if (!f) return 0;
    unsigned char buf[4 + sizeof saved];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n != sizeof buf || memcmp(buf, SAVE_MAGIC, 4)) return 0;
    memcpy(saved, buf + 4, sizeof saved);
    for (int p = 0; p < MEMCARD_PORTS; p++) memcpy(memcard[p], saved[p], MEMCARD_SIZE);
    if (saved[0][MEMCARD_SIZE] <= 100) music_volume = saved[0][MEMCARD_SIZE];
    if (saved[0][MEMCARD_SIZE + 1] <= 100) sfx_volume = saved[0][MEMCARD_SIZE + 1];
    if (ah_audio_volumes) ah_audio_volumes((float)music_volume / 100, (float)sfx_volume / 100);
    if (saved[1][MEMCARD_SIZE] <= 2) gore_setting = saved[1][MEMCARD_SIZE];
    return 1;
}

static void save_image(unsigned char out[MEMCARD_PORTS][MEMCARD_SIZE + 2]) {
    memset(out, 0, sizeof saved);
    for (int p = 0; p < MEMCARD_PORTS; p++) memcpy(out[p], memcard[p], MEMCARD_SIZE);
    out[0][MEMCARD_SIZE] = (unsigned char)music_volume;
    out[0][MEMCARD_SIZE + 1] = (unsigned char)sfx_volume;
    out[1][MEMCARD_SIZE] = (unsigned char)gore_setting;
}

int ah_save_tick(void) {
    if (!save_path[0]) return 0;
    unsigned char now[MEMCARD_PORTS][MEMCARD_SIZE + 2];
    save_image(now);
    if (!memcmp(now, saved, sizeof now)) return 0;
    char tmp[264];
    snprintf(tmp, sizeof tmp, "%s.tmp", save_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fwrite(SAVE_MAGIC, 1, 4, f) == 4 && fwrite(now, 1, sizeof now, f) == sizeof now;
    ok = fclose(f) == 0 && ok;
    remove(save_path);
    if (!ok || rename(tmp, save_path) != 0) return -1;
    memcpy(saved, now, sizeof now);
    if (ah_log) ah_log("saved");
    return 1;
}


static int pause_opened;   // the overlay is the in-game pause menu (START), not the main menu

static AsVal UnPause(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    log_call(vm, "UnPause", argc, argv);
    ah_paused = 0;
    overlay_close = 1;
    music_pause(0);
    if (ah_sfx_hold) ah_sfx_hold(0);
    // pause.pak's first frame sets g_bNoPause while its menu is up; the exe let the game
    // pause again once it closed. (Quitting from it sets g_bNoPause after this call.)
    if (pause_opened) cc_set_flash_global(vm, "g_bNoPause", as_num(0));
    pause_opened = 0;
    return as_undef();
}

static AsVal SetBackgroundAlpha(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    ah_background_alpha = (float)as_tonum(vm, ARG(0));
    return as_undef();
}

static AsVal SetPlaying(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    playing = as_tobool(vm, ARG(0));
    (void)playing;
    return as_undef();
}

AsMc *ah_loading;   // see "the loading screen" below

int ah_request_pause(AsVm *vm, int port) {
    if (ah_paused || ah_loading || as_tobool(vm, cc_flash_global(vm, "g_bNoPause"))) return 0;
    cc_set_flash_global(vm, "g_nPausePlayer", as_num(port));
    open_overlay(vm, 1);
    ah_paused = 1;
    pause_opened = 1;
    music_pause(1);
    if (ah_sfx_hold) ah_sfx_hold(1);   // the game's sounds wait too (looping ones would drone on)
    return 1;
}

static char root_file[64];   // the root movie's file ("level15.cok6")

// A story level or challenge is the root: there START is the pause menu, and SELECT is the
// "start" key the HUD reads to spend a continue (a coin) after the last life
// g_bPort<n>Active: the scripts set it true while that port's alien plays and false while
// its HUD waits for START to join (or to spend a continue). START on an active port opens
// the pause menu; on any other it goes to the scripts. port is 0-based; the scripts' own
// numbering is 1-based (GLOBAL_alien2_port = 2), but a 0-based g_bPort0Active is honoured
// too. -1: the scripts never set it.
int ah_port_active(AsVm *vm, int port) {
    char name[32];
    snprintf(name, sizeof name, "g_bPort%dActive", 0);
    int base = cc_flash_global(vm, name).t == V_UNDEF ? 1 : 0;
    snprintf(name, sizeof name, "g_bPort%dActive", port + base);
    AsVal v = cc_flash_global(vm, name);
    return v.t == V_UNDEF ? -1 : as_tobool(vm, v);
}

int ah_in_level(void) {
    return !strncasecmp(root_file, "level", 5) || !strncasecmp(root_file, "chal", 4);
}

// The game the pause menu's How to Play describes (its currentgamex): the exe kept one
// pause.pak for the whole run, so the main menu's choice was still there; here the layer
// is new each time, so it comes from the movie being played.
static const char *current_gamex(void) {
    static const char *const own[] = {"pdagames", "chal12", "chal31", "chal34", "neutron", "pinata", "ssmm"};
    for (size_t i = 0; i < sizeof own / sizeof *own; i++)
        if (!strncasecmp(root_file, own[i], strlen(own[i]))) return own[i];
    if (!strncasecmp(root_file, "pudding", 7)) return "ayce";
    return "ah";
}

static void set_root(AsVm *vm, Movie *m) { vm->root = as_mc_new(vm, m, &m->root, NULL, 0); }

int ah_start(AsVm *vm, const char *first) {
    Movie *m = vm->loader(vm, first);
    if (!m) return 0;
    snprintf(root_file, sizeof root_file, "%s", first);
    set_root(vm, m);
    if (gore_setting) cc_set_flash_global(vm, "GLOBAL_gore", as_bool(gore_setting == 2));   // from the save
    return 1;
}

static char ah_next_movie[64];

// ---- PDA Games ------------------------------------------------------------------------
// pdagames.pak plays levels the exe read from data/game/pda1.pak (the 50 built in, pack
// id 0) and pda2-6.pak (the five downloadable packs of 100: ids 1-5). Each is a ZIP
// holding NAME.PDAG.BREC: 32 bytes, "GADP", 12 bytes, u32 level count, then the levels,
// each 11 int16s (?, spawn x, y, ?, enemy spawn x, y, scale, exit x, y, scale, object
// count) and 5 int16s an object (type, x, y, scale, ?). f_PDALoadLevelArray fills the
// script's a_levelData with one level's numbers, which its f_buildLevel reads in order.
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
char ah_data_dir[256];   // where game/ and levels/ are, e.g. "ux0:data/AlienHominidHD/"

#define PDA_PACKS 6
static struct { int tried, n; int16_t *v; uint32_t *at; } pda[PDA_PACKS];

static void pda_open(int k) {
    if (k < 0 || k >= PDA_PACKS || pda[k].tried) return;
    pda[k].tried = 1;
    char path[300];
    snprintf(path, sizeof path, "%sgame/pda%d.pak", ah_data_dir, k + 1);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *z = len > 30 ? malloc(len) : NULL;
    int ok = z && fread(z, 1, len, f) == (size_t)len;
    fclose(f);
    if (!ok || le32(z) != 0x04034b50) { free(z); return; }
    int method = z[8] | z[9] << 8;
    uint32_t csize = le32(z + 18), usize = le32(z + 22);
    long start = 30 + (z[26] | z[27] << 8) + (z[28] | z[29] << 8);
    uint8_t *d = start + (long)csize <= len && usize >= 52 ? malloc(usize) : NULL;
    if (d && method == 0 && csize == usize) memcpy(d, z + start, usize);
    else if (d && method == 8) {
        z_stream zs = {0};
        zs.next_in = z + start; zs.avail_in = csize; zs.next_out = d; zs.avail_out = usize;
        if (!(inflateInit2(&zs, -15) == Z_OK && inflate(&zs, Z_FINISH) == Z_STREAM_END)) { free(d); d = NULL; }
        inflateEnd(&zs);
    } else { free(d); d = NULL; }
    free(z);
    if (!d || memcmp(d + 32, "GADP", 4)) { free(d); return; }
    int n = (int)le32(d + 48);
    uint32_t words = (usize - 52) / 2;
    int16_t *v = malloc(words * sizeof *v + 1);
    uint32_t *at = malloc((n > 0 ? n : 1) * sizeof *at);
    for (uint32_t i = 0; i < words; i++) v[i] = (int16_t)(d[52 + 2 * i] | d[53 + 2 * i] << 8);
    free(d);
    uint32_t w = 0;
    int i = 0;
    for (; i < n && w + 11 <= words; i++) {   // where each level starts
        at[i] = w;
        int objects = v[w + 10];
        if (objects < 0 || w + 11 + 5u * objects > words) break;
        w += 11 + 5 * objects;
    }
    pda[k].n = i;
    pda[k].v = v;
    pda[k].at = at;
}

static int pda_count(int k) { pda_open(k); return k >= 0 && k < PDA_PACKS ? pda[k].n : 0; }

// Fills arr with level `level` (1-based) of pack k.
static void pda_fill(AsVm *vm, AsVal arr, int k, int level) {
    if (getenv("PDADBG")) printf("  [pda] fill pack %d level %d (%d levels) arr %d\n", k, level, pda_count(k), arr.t == V_OBJ);
    if (arr.t != V_OBJ || level < 1 || level > pda_count(k)) return;
    const int16_t *l = pda[k].v + pda[k].at[level - 1];
    int words = 11 + 5 * l[10];
    for (int i = 0; i < words; i++) {
        char key[12];
        snprintf(key, sizeof key, "%d", i);
        as_set(vm, arr, as_atom(vm, key), as_num(l[i]));
    }
}

static int live_packs(void) {
    int n = 0;
    while (n + 1 < PDA_PACKS && pda_count(n + 1) > 0) n++;
    return n;
}

// f_PDALoadLevelArray(a_levelData, Level): the built-in levels
static AsVal f_PDALoadLevelArray(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    pda_fill(vm, ARG(0), 0, (int)as_tonum(vm, ARG(1)));
    return as_undef();
}

// f_PDALoadLiveLevelArray(controller, pack id, a_levelData, Level)
static AsVal f_PDALoadLiveLevelArray(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    pda_fill(vm, ARG(2), (int)as_tonum(vm, ARG(1)), (int)as_tonum(vm, ARG(3)));
    return as_undef();
}

// f_PDAGetNumLiveLevelsInPack(controller, pack id)
static AsVal f_PDAGetNumLiveLevelsInPack(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    return as_num(pda_count((int)as_tonum(vm, ARG(1))));
}

// f_PDAGetNumLivePacks(controller): the downloadable packs (all of them come with the PC copy)
static AsVal f_PDAGetNumLivePacks(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)vm; (void)this_; (void)argc; (void)argv;
    return as_num(live_packs());
}

// f_PDALiveLevelIndexToPublisherId(controller, index): pack ids are 1-5
static AsVal f_PDALiveLevelIndexToPublisherId(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    return as_num((int)as_tonum(vm, ARG(1)) + 1);
}

// f_PDAGetLiveLevelName(controller, index, field): the pack's name in the menu
static AsVal f_PDAGetLiveLevelName(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    char t[32];
    snprintf(t, sizeof t, "LEVEL PACK %d", (int)as_tonum(vm, ARG(1)) + 1);
    if (ARG(2).t == V_OBJ) as_set(vm, ARG(2), as_atom(vm, "text"), as_strv(as_str(vm, t)));
    return as_undef();
}

// f_PDASetLevelBoxText(field, Level): the level number on the PDA screen
static AsVal f_PDASetLevelBoxText(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    char t[32];
    snprintf(t, sizeof t, "LEVEL %d", (int)as_tonum(vm, ARG(1)));
    if (ARG(0).t == V_OBJ) as_set(vm, ARG(0), as_atom(vm, "text"), as_strv(as_str(vm, t)));
    return as_undef();
}

// f_SetCreditText(clip, text, shadow, n): the exe filled in credit n. Its credits aren't in
// the game's files, so the ending goes back to the menu (movieend.pak's f_exit) when the
// roll would start, instead of scrolling empty lines.
static AsVal f_SetCreditText(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_; (void)argc; (void)argv;
    static uint32_t done_tick = (uint32_t)-1;
    if (done_tick == vm->tick || ah_next_movie[0]) return as_undef();
    done_tick = vm->tick;
    AsVal fn = as_get(vm, as_objv(&vm->root->obj), as_atom(vm, "f_exit"));
    if (fn.t == V_OBJ) as_call(vm, fn, as_objv(&vm->root->obj), 0, NULL);
    return as_undef();
}

// ---- the unlock pop-up ---------------------------------------------------------------
// DisplayUnlock(type) (0 hat, 1 minigame, 2 achievement): the exe kept hud_unlock.pak over
// the game and passed each unlock to its f_DisplayUnlock(type, data), which queues the
// "HAT UNLOCKED" / "MINIGAME UNLOCKED" banner and its sound. Scripts call it mid-tick, so
// unlocks wait here until the game's tick is done.
AsMc *ah_unlock_layer;
static void run_as_root(AsVm *vm, AsMc **clip, int keep_tick);
static int unlock_queue[16], unlock_n, unlock_count;

static AsVal DisplayUnlock(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    log_call(vm, "DisplayUnlock", argc, argv);
    if (unlock_n < 16) unlock_queue[unlock_n++] = (int)as_tonum(vm, ARG(0));
    return as_undef();
}

static void run_unlocks(AsVm *vm) {
    if (!ah_unlock_layer && !unlock_n) return;
    if (!ah_unlock_layer) {
        Movie *m = vm->loader(vm, "hud_unlock.pak");
        if (!m) { unlock_n = 0; return; }
        ah_unlock_layer = as_mc_new(vm, m, &m->root, NULL, 0);
        as_set(vm, as_objv(vm->global), as_atom(vm, "__ah_unlock"), as_objv(&ah_unlock_layer->obj));   // kept alive
    }
    run_as_root(vm, &ah_unlock_layer, 1);
    if (!unlock_n) return;
    AsVal fn = as_get(vm, as_objv(&ah_unlock_layer->obj), as_atom(vm, "f_DisplayUnlock"));
    if (fn.t == V_OBJ) {
        AsMc *root = vm->root;
        vm->root = ah_unlock_layer;   // its _root while it runs
        for (int i = 0; i < unlock_n; i++) {
            // the data only tells repeats apart: the exe passed something per unlock
            AsVal args[2] = {as_num(unlock_queue[i]), as_num(++unlock_count)};
            as_call(vm, fn, as_objv(&ah_unlock_layer->obj), 2, args);
        }
        vm->root = root;
    }
    unlock_n = 0;
}

// ---- the loading screen ----------------------------------------------------------------
// The exe put loading.pak over the screen while it loaded a level. Its script and the exe
// talk through flash globals: g_bLoaded (the exe: the level is in), g_bLevelLoaded
// (loading.pak: done, after its 4-second minimum with a tip, or at once after the
// scoreboard). Here: show it, load the level once it has been drawn, then hand over.

static int loading_frames;       // ticks it has run
#define LOAD_AT 4                // the tick the level loads on: its art has been shown
#define PRIME_TICKS 3            // level ticks run behind it
static char loading_target[64];  // file still to load, "" once loaded

void (*ah_prefetch)(const char *const *files, int n);
int (*ah_prefetch_pending)(void);

// The files each level's first frames load (found by running each one in the desktop
// harness). The front end reads them in the background while the loading screen plays,
// so the level loads from memory when the screen hands over.
static const char *const level_files[][9] = {
    {"chal12", "hud", "alien", "fbi", "abullets", "agrenades", "ebullets", "egrenades"},
    {"chal31", "hud", "alien", "abullets", "agrenades", "backgrounds3"},
    {"chal34", "hud", "alien", "army", "abullets", "agrenades", "ebullets", "egrenades", "backgrounds4"},
    {"level11", "hud", "alien", "fbi", "abullets", "agrenades", "ebullets", "egrenades", "backgrounds"},
    {"level12", "hud", "alien", "fbi", "abullets", "agrenades", "ebullets", "egrenades"},
    {"level13", "hud", "alien", "fbi", "abullets", "agrenades", "ebullets", "egrenades", "backgrounds"},
    {"level14", "hud", "alien", "fbi", "abullets", "agrenades", "ebullets", "egrenades", "backgrounds"},
    {"level15"},
    {"level21", "hud", "alien", "kgb", "abullets", "agrenades", "ebullets", "egrenades", "backgrounds2"},
    {"level22", "hud", "alien", "kgb", "abullets", "agrenades", "ebullets", "egrenades", "backgrounds2"},
    {"level23", "hud", "alien", "abullets", "agrenades", "ebullets", "egrenades", "backgrounds2"},
    {"level24", "hud", "alien", "kgb", "abullets", "agrenades", "ebullets", "egrenades", "backgrounds2"},
    {"level25", "hud", "alien", "abullets", "agrenades", "ebullets", "egrenades", "backgrounds2"},
    {"level26"},
    {"level31", "hud", "alien", "abullets", "agrenades", "ebullets", "egrenades", "backgrounds3"},
    {"level32", "hud", "alien", "army", "abullets", "agrenades", "ebullets", "egrenades", "backgrounds3"},
    {"level33", "hud", "alien", "army", "abullets", "agrenades", "ebullets", "egrenades", "backgrounds4"},
    {"level34", "hud", "alien", "army", "abullets", "agrenades", "ebullets", "egrenades", "backgrounds4"},
    {"level35", "hud", "alien", "abullets", "agrenades", "ebullets", "egrenades", "backgrounds4"},
};

static void prefetch_level(const char *file) {
    if (!ah_prefetch) return;
    char base[64];
    snprintf(base, sizeof base, "%s", file);
    char *dot = strchr(base, '.');
    if (dot) *dot = 0;
    for (size_t i = 0; i < sizeof level_files / sizeof *level_files; i++) {
        if (strcasecmp(base, level_files[i][0])) continue;
        int n = 0;
        while (n < 9 && level_files[i][n]) n++;
        ah_prefetch(level_files[i], n);
        return;
    }
    const char *one[] = {base};   // a level not in the table: at least its own file
    ah_prefetch(one, 1);
}

static int wants_loading_screen(const char *file) {
    return !strncasecmp(file, "level", 5) || !strncasecmp(file, "chal", 4);
}

static void run_as_root(AsVm *vm, AsMc **clip, int keep_tick) {
    AsMc *root = vm->root;
    uint32_t tick = vm->tick;
    vm->root = *clip;
    as_tick(vm);
    *clip = vm->root;
    vm->root = root;
    if (keep_tick) vm->tick = tick;
}

// Swaps the root for `file`; 0 if it can't be loaded.
static int change_root(AsVm *vm, const char *file) {
    Movie *m = vm->loader(vm, file);
    if (!m) return 0;
    snprintf(root_file, sizeof root_file, "%s", file);
    // as in Flash, a new root movie silences the old one's sounds (car engines loop)
    if (ah_sfx_stop_all) ah_sfx_stop_all();
    music_pause(0);
    set_root(vm, m);
    vm->overlay = NULL;
    ah_paused = 0;
    pause_opened = 0;
    // the menus and game-over screens set g_bNoPause; a new movie starts pausable (the
    // levels never clear it themselves)
    cc_set_flash_global(vm, "g_bNoPause", as_num(0));
    return 1;
}

static void end_loading(AsVm *vm) {
    if (ah_log) ah_log("loading screen done");
    ah_loading = NULL;
    as_set(vm, as_objv(vm->global), as_atom(vm, "__ah_loading"), as_undef());
}

// The legal screens at startup (legal.pak): the exe made you sit through them. Accept or
// Start jumps the clip that runs them (the one long timeline, 390 frames) to its last
// frame, whose script loads the title as usual.
static AsMc *longest_clip(AsMc *mc, int depth) {
    AsMc *best = mc->def && mc->def->nframes >= 300 ? mc : NULL;
    for (int i = 0; i < mc->ndisp && !best && depth < 4; i++)
        if (mc->disp[i].kind == DISP_MC && mc->disp[i].mc) best = longest_clip(mc->disp[i].mc, depth + 1);
    return best;
}

static void skip_legal(AsVm *vm) {
    static int was_down = 1;   // a button held at boot doesn't count
    int down = cc_keys[97] || cc_keys[49];
    if (down && !was_down && strncasecmp(root_file, "legal", 5) == 0) {
        AsMc *c = longest_clip(vm->root, 0);
        if (c && c->frame < c->def->nframes - 1) {
            if (ah_log) ah_log("legal screens skipped");
            as_mc_goto(vm, c, c->def->nframes - 1, 1);
        }
    }
    was_down = down;
}

// the menu layer (pause.pak) a script asked for with TurnMainMenuOn or the pause button
static void open_requested_overlay(AsVm *vm) {
    if (!overlay_open) return;
    overlay_open = overlay_close = 0;
    Movie *o = vm->loader(vm, "pause.pak");
    vm->overlay = o ? as_mc_new(vm, o, &o->root, NULL, 0) : NULL;
    if (!o) ah_paused = 0;
    if (vm->overlay && ah_paused)
        as_set(vm, as_objv(&vm->overlay->obj), as_atom(vm, "currentgamex"), as_strv(as_str(vm, current_gamex())));
}

const char *ah_tick(AsVm *vm) {
    static char changed[64];
    skip_legal(vm);
    if (ah_loading) {   // only the loading screen runs
        run_as_root(vm, &ah_loading, 0);
        loading_frames++;
        // load once its art is on screen and the background reads are in (or after 60 s)
        if (loading_target[0] && loading_frames >= LOAD_AT &&
            (!ah_prefetch_pending || !ah_prefetch_pending() || loading_frames > 60 * 60)) {
            snprintf(changed, sizeof changed, "%s", loading_target);
            loading_target[0] = 0;
            extern double as_time_ms(void);
            double t0 = as_time_ms();
            int ok = change_root(vm, changed);
            double t1 = as_time_ms();
            cc_set_flash_global(vm, "g_bLoaded", as_bool(1));
            if (!ok) { end_loading(vm); return NULL; }
            // the level's first frames load its parts (hud, players, enemies, backgrounds):
            // run them now, behind the loading screen, so all the loading is one pause
            // before its animation starts (and no black screen after it)
            for (int i = 0; i < PRIME_TICKS; i++) as_tick(vm);
            if (ah_log) { char b[128]; snprintf(b, sizeof b, "loaded %s: file %.0f ms, first ticks %.0f ms", changed, t1 - t0, as_time_ms() - t1); ah_log(b); }
            return changed;
        }
        if (!loading_target[0] && (as_tobool(vm, cc_flash_global(vm, "g_bLevelLoaded")) || loading_frames > 30 * 30))
            end_loading(vm);
        return NULL;
    }
    if (!ah_paused) {
        as_tick(vm);
        run_unlocks(vm);
    }
    open_requested_overlay(vm);
    if (vm->overlay) run_as_root(vm, &vm->overlay, !ah_paused);   // the overlay runs as its own _root
    if (overlay_close) {
        overlay_close = 0;
        vm->overlay = NULL;
    }
    if (!ah_next_movie[0]) return NULL;
    snprintf(changed, sizeof changed, "%s", ah_next_movie);
    ah_next_movie[0] = 0;
    Movie *l = wants_loading_screen(changed) ? vm->loader(vm, "loading.pak") : NULL;
    if (l) {
        cc_set_flash_global(vm, "g_bLoaded", as_bool(0));
        cc_set_flash_global(vm, "g_bLevelLoaded", as_bool(0));
        ah_loading = as_mc_new(vm, l, &l->root, NULL, 0);
        as_set(vm, as_objv(vm->global), as_atom(vm, "__ah_loading"), as_objv(&ah_loading->obj));   // kept alive
        // its first two frames draw nothing but the green stage: run them now, so the
        // first one shown has its art
        for (loading_frames = 0; loading_frames < 2; loading_frames++) run_as_root(vm, &ah_loading, 0);
        snprintf(loading_target, sizeof loading_target, "%s", changed);
        prefetch_level(changed);
        vm->overlay = NULL;
        ah_paused = 0;
        if (ah_sfx_stop_all) ah_sfx_stop_all();
        return NULL;
    }
    if (!change_root(vm, changed)) return NULL;
    // a new root has drawn nothing until its first frame runs, and the stage colour (the
    // menu's green) would show for a frame: run that frame now, before it's presented
    as_tick(vm);
    open_requested_overlay(vm);   // the main menu's PDA, which that frame turns on
    if (vm->overlay) run_as_root(vm, &vm->overlay, 1);
    return changed;
}

// ChangeMovie("attract.cok6"): the exe swaps the root movie for another file. The front end
// does it between ticks (ah_take_movie_change), so the running script finishes first.
static AsVal ChangeMovie(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    log_call(vm, "ChangeMovie", argc, argv);
    const char *name = as_tostr(vm, ARG(0))->s;
    const char *dot = strchr(name, '.');
    int n = dot ? (int)(dot - name) : (int)strlen(name);
    if (n > 0 && n < (int)sizeof ah_next_movie - 5) snprintf(ah_next_movie, sizeof ah_next_movie, "%.*s.pak", n, name);
    return as_undef();
}

// SetFlashGlobal, as cc_host's, but a player's score is kept whole: the end-of-level
// screen adds an accuracy bonus of 10 * hits / shots without rounding it, and the next
// level's HUD then showed the score with a string of decimals.
static AsVal SetFlashGlobal(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    const char *name = as_tostr(vm, ARG(0))->s;
    AsVal v = ARG(1);
    int n = 0;
    if (v.t == V_NUM && sscanf(name, "GLOBAL_alien%*d_score%n", &n) >= 0 && n && !name[n] && isfinite(v.n))
        v = as_num(trunc(v.n));
    if (!strcmp(name, "GLOBAL_gore")) gore_setting = as_tobool(vm, v) ? 2 : 1;   // saved (see gore_setting)
    cc_set_flash_global(vm, name, v);
    return as_undef();
}

// QuitTo("menu.cok6"): leave the game for a front-end movie ("goto_menu": the main menu)
static AsVal QuitTo(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    log_call(vm, "QuitTo", argc, argv);
    const char *name = as_tostr(vm, ARG(0))->s;
    if (!strcmp(name, "goto_menu")) return ChangeMovie(vm, this_, 1, (AsVal[]){as_strv(as_str(vm, "menu.cok6"))});
    return ChangeMovie(vm, this_, argc, argv);
}

// ---- changes to the game's own scripts, for the Vita ------------------------------------
// Each one is checked byte for byte before it is applied, so another version of a file is
// left alone.

typedef struct {
    const char *file;          // e.g. "pause.pak"
    uint32_t at;               // offset in the SWF
    uint8_t n;                 // bytes checked and written (0: 8 checked, 5 written)
    uint8_t expect[20];        // what must be there
    uint8_t put[20];           // what goes there
    const char *why;
} Patch;

// "Not; If" -> "Pop; Jump": the If's branch is always taken
#define ALWAYS_JUMP(lo, hi) {0x12, 0x9d, 0x02, 0x00, lo, hi}, {0x17, 0x99, 0x02, 0x00, lo, hi}

static const Patch patches[] = {
    // f_BuildPlayerOptionsMenu: jump over the VIBRATION item (text 31) to the next one; the
    // Vita has no rumble
    {"pause.pak", 0x45e78, 0, {0x96, 0x15, 0x00, 0x04, 0x05, 0x04, 0x02, 0x03}, {0x99, 0x02, 0x00, 0xcb, 0x00},
     "no vibration option"},
    // f_BuildMainOptionsMenu: jump over the CREDITS item (text 25) to the end of the menu; the
    // credits text lived in the exe, so the roll would be empty
    {"pause.pak", 0x44b38, 0, {0x96, 0x14, 0x00, 0x04, 0x04, 0x04, 0x03, 0x03}, {0x99, 0x02, 0x00, 0x3a, 0x00},
     "no credits option"},

    // ---- Leaderboards: one global board (no friends filter, no gamer card), and a
    // LIFETIME timeframe next to MONTHLY and WEEKLY
    // f_SetIconsLeaderboard: jump over the Triangle "SHOW GAMER CARD" button
    {"pause.pak", 0x3debf, 0, {0x96, 0x04, 0x00, 0x04, 0x02, 0x08, 0x46, 0x4e}, {0x99, 0x02, 0x00, 0x7a, 0x00},
     "no gamer card button"},
    // the board's input: Triangle (gamer card) and Square (filter) do nothing
    {"pause.pak", 0x4d631, 6, ALWAYS_JUMP(0x32, 0x00), "no gamer card"},
    {"pause.pak", 0x4d678, 6, ALWAYS_JUMP(0x56, 0x01), "no friends filter"},
    // the board's top line: skip the Square "FILTER: ..." label
    {"pause.pak", 0x4b89c, 0, {0x96, 0x15, 0x00, 0x07, 0xeb, 0x03, 0x00, 0x00}, {0x99, 0x02, 0x00, 0x2a, 0x01},
     "no filter label"},
    // f_LEADERBOARDSELECT_*: friendsfilter starts at 0 (everyone) instead of 1 (friends)
#define ALL_SCORES(at) {"pause.pak", at, 11, {0xa3, 0x08, 0x00, 0x09, 0x86, 0x01, 0x07, 0x01, 0x00, 0x00, 0x00}, \
                        {0xa3, 0x08, 0x00, 0x09, 0x86, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00}, "global board"}
    ALL_SCORES(0x3fa51), ALL_SCORES(0x3fc79), ALL_SCORES(0x3fecb), ALL_SCORES(0x40103), ALL_SCORES(0x403a3),
    // f_BuildAHLeaderboardSelectMenu, the TIMEFRAME item: "index = 0" becomes ahTF(item), which
    // gives it LIFETIME, MONTHLY and WEEKLY (texts 151-153)
    {"pause.pak", 0x46e15, 18,
     {0x96, 0x0e, 0x00, 0x04, 0x02, 0x09, 0x8f, 0x01, 0x06, 0, 0, 0, 0, 0, 0, 0, 0, 0x4f},
     {0x96, 0x0d, 0x00, 0x04, 0x02, 0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 'a', 'h', 'T', 'F', 0x00, 0x3d, 0x17},
     "lifetime timeframe"},
#define TIMEFRAMES(at, reg) {"pause.pak", at, 18, \
     {0x96, 0x0e, 0x00, 0x04, reg, 0x09, 0x8f, 0x01, 0x06, 0, 0, 0, 0, 0, 0, 0, 0, 0x4f}, \
     {0x96, 0x0d, 0x00, 0x04, reg, 0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 'a', 'h', 'T', 'F', 0x00, 0x3d, 0x17}, \
     "lifetime timeframe"}
    TIMEFRAMES(0x4719a, 0x02),   // f_BuildPDALeaderboardSelectMenu
    TIMEFRAMES(0x47386, 0x03),   // f_BuildSSMMLeaderboardSelectMenu
    // f_UPDATELBTIMEFRAME_LM (all three menus): three choices (index up to 2), not two
    {"pause.pak", 0x52ec0, 0, {0x96, 0x05, 0x00, 0x07, 0x01, 0x00, 0x00, 0x00}, {0x96, 0x05, 0x00, 0x07, 0x02},
     "three timeframes"},
    // the end-of-level scoreboard asks IsPlayerOnline before it sends the score (and shows the
    // sign-in notice when the answer is no); its frames ask ahScoresOnline instead, which is
    // yes while the leaderboard is online (IsPlayerOnline stays no for the match menus)
#define SCORES_ONLINE(at) {"scorebrd.pak", at, 15, "IsPlayerOnline", "ahScoresOnline", "scores sent online"}
    SCORES_ONLINE(0x21a6b), SCORES_ONLINE(0x225db), SCORES_ONLINE(0x22a48),
    // f_LEADERBOARDSELECT_AH/_PDA/_SSMM: expiretime = the choice (0 lifetime, 1 monthly, 2 weekly, as the
    // board's own label reads it: text 151 + expiretime), not the choice + 1
#define AS_CHOSEN(at) {"pause.pak", at, 0, {0x96, 0x05, 0x00, 0x07, 0x01, 0x00, 0x00, 0x00}, {0x96, 0x05, 0x00, 0x07, 0x00}, \
                       "timeframe as chosen"}
    AS_CHOSEN(0x3fba1), AS_CHOSEN(0x40019), AS_CHOSEN(0x4023f),   // _AH, _PDA, _SSMM
};

// ahTF(item): the leaderboard TIMEFRAME choices (see the patch above)
static AsVal ahTF(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    AsVal item = ARG(0);
    if (item.t != V_OBJ) return as_undef();
    as_set(vm, item, as_atom(vm, "s0"), as_num(151));   // LIFETIME
    as_set(vm, item, as_atom(vm, "s1"), as_num(152));   // MONTHLY
    as_set(vm, item, as_atom(vm, "s2"), as_num(153));   // WEEKLY
    as_set(vm, item, as_atom(vm, "index"), as_num(0));
    return as_undef();
}

void ah_patch_movie(const char *file, Movie *m) {
    cc_buttons_ah_glyphs(m);   // PlayStation buttons like the Castle Crashers build
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    for (size_t i = 0; i < sizeof patches / sizeof *patches; i++) {
        const Patch *p = &patches[i];
        int n = p->n ? p->n : 8;
        if (strcasecmp(base, p->file) || p->at + n > m->len || memcmp(m->data + p->at, p->expect, n)) continue;
        memcpy(m->data + p->at, p->put, p->n ? p->n : 5);
        if (ah_log) {
            char line[96];
            snprintf(line, sizeof line, "patched %s: %s", p->file, p->why);
            ah_log(line);
        }
    }
}

// ---- the game's words -------------------------------------------------------------
// Text fields ask for string N ("ntext = N"). The exe had the table; pause.pak carries the
// same table as f_GetText(N) in its second frame, which only runs when the movie is played
// outside the game (console_version false). That frame's script is run once on a clip of
// its own, nowhere in the display, so its test stubs stay out of the game's way; then
// f_GetText answers for the exe. Answers are kept.
// The exe's own table is data/loc/<language>.dct; when the front end loads it
// (ah_load_strings), string N is its Nth record. It fills in what f_GetText lacks (e.g.
// 347 "Exit Game"); f_GetText comes first, as the PC table numbers some strings differently
// (the loading screen's tips, 280 on, came out blank from it).

#define MAX_TEXT 1024
static char *texts[MAX_TEXT];
static char *dct[MAX_TEXT];   // 1-based
static int ndct;
static AsMc *text_clip;
static int text_tried;


// A .dct: u32 count (top bit set), u32 1, then records of u32 key bytes, u32 value bytes,
// u32 character count, the key and the value (UTF-8, both NUL terminated).
int ah_load_strings(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = len > 8 ? malloc(len) : NULL;
    int ok = d && fread(d, 1, len, f) == (size_t)len;
    fclose(f);
    if (!ok) { free(d); return 0; }
    int count = (int)(le32(d) & 0xffff);
    long p = 8;
    int n = 0;
    while (n < count && n + 1 < MAX_TEXT && p + 12 <= len) {
        uint32_t kl = le32(d + p), vl = le32(d + p + 4);
        if (kl > (uint32_t)len || vl > (uint32_t)len || p + 12 + kl + vl > (uint64_t)len) break;
        const char *v = (const char *)d + p + 12 + kl;
        free(dct[n + 1]);
        dct[n + 1] = vl ? strndup(v, vl) : strdup("");
        n++;
        p += 12 + kl + vl;
    }
    free(d);
    ndct = n;
    for (int i = 0; i < MAX_TEXT; i++) { free(texts[i]); texts[i] = NULL; }
    return n;
}

static const char *swf_text(AsVm *vm, int n);

static const char *text_for_number(AsVm *vm, int n) {
    const char *s = swf_text(vm, n);
    if ((!s || !strcmp(s, "NO TEXT FOUND")) && n > 0 && n <= ndct && n < MAX_TEXT) return dct[n];
    return s;
}

static const char *swf_text(AsVm *vm, int n) {
    if (n <= 0 || n >= MAX_TEXT) return NULL;
    if (texts[n]) return texts[n];
    if (!text_tried) {
        text_tried = 1;
        Movie *m = vm->loader(vm, "pause.pak");
        for (int i = 0; m && i < m->root.nactions; i++)
            if (m->root.actions[i].frame == 1) {
                text_clip = as_mc_new(vm, m, &m->root, NULL, 0);
                // kept alive through _global
                as_set(vm, as_objv(vm->global), as_atom(vm, "__ah_text_clip"), as_objv(&text_clip->obj));
                as_run_block(vm, m, m->root.actions[i].off, m->root.actions[i].len, text_clip);
                break;
            }
    }
    if (!text_clip) return NULL;
    AsVal fn = as_get(vm, as_objv(&text_clip->obj), as_atom(vm, "f_GetText"));
    if (fn.t != V_OBJ) return NULL;
    AsVal arg = as_num(n);
    AsVal r = as_call(vm, fn, as_objv(&text_clip->obj), 1, &arg);
    if (r.t != V_STR) return NULL;
    texts[n] = strdup(as_tostr(vm, r)->s);
    if (ah_log && !strcmp(texts[n], "NO TEXT FOUND")) {
        char line[64];
        snprintf(line, sizeof line, "no text for ntext %d", n);
        ah_log(line);
    }
    return texts[n];
}

// GetInputBinding(b): which button an action is on, for the button glyphs. b.s_action is the
// action ("menu_select", "jump"...); the answer goes in b.s_device and b.s_button, which
// pick a frame of the game's own glyph clip ("ps4_x"). The PS4 glyphs match the Vita's face
// buttons; the shoulder ones show as L1/R1.
static const char *const VITA_BUTTONS[][2] = {
    {"menu_select", "x"}, {"menu_back", "circle"}, {"menu_x", "square"}, {"menu_y", "triangle"},
    {"menu_lt", "l1"},    {"menu_rt", "r1"},       {"start", "start"}, {"select", "select"}, {"jump", "x"},
    {"shoot", "square"},  {"grenade", "circle"},   {"interact", "triangle"},
    {"roll_left", "l1"},  {"roll_right", "r1"},
};

static AsVal GetInputBinding(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    AsVal b = ARG(0);
    if (b.t != V_OBJ) return as_bool(0);
    const char *action = as_tostr(vm, as_get(vm, b, as_atom(vm, "s_action")))->s;
    const char *button = NULL;
    if (!strcmp(action, "start") && ah_in_level()) action = "select";   // see ah_in_level
    for (size_t i = 0; i < sizeof VITA_BUTTONS / sizeof *VITA_BUTTONS; i++)
        if (!strcmp(action, VITA_BUTTONS[i][0])) button = VITA_BUTTONS[i][1];
    if (!button) {
        log_call(vm, "GetInputBinding (unknown action)", 1, &argv[0]);
        if (ah_log) ah_log(action);
        button = "x";
    }
    as_set(vm, b, as_atom(vm, "s_device"), as_strv(as_atom(vm, "ps3")));
    as_set(vm, b, as_atom(vm, "s_button"), as_strv(as_atom(vm, button)));
    return as_bool(1);
}

// SetTextToGamerTag(port, field, field2): the signed-in profile's name. The Vita has none to show.
// f_SetLoadingLevelText(daText, daBgText, level, area): the loading screen's title, e.g.
// "LEVEL 1-1" (the executable fills both fields, the text and its shadow)
static AsVal f_SetLoadingLevelText(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    log_call(vm, "f_SetLoadingLevelText", argc, argv);
    int level = (int)as_tonum(vm, ARG(2)), area = (int)as_tonum(vm, ARG(3));
    char t[32] = "";
    if (area > 0 && level > 0) snprintf(t, sizeof t, "LEVEL %d-%d", area, level);
    for (int i = 0; i < argc && i < 2; i++)
        if (argv[i].t == V_OBJ) as_set(vm, argv[i], as_atom(vm, "text"), as_strv(as_str(vm, t)));
    return as_undef();
}

static const char *lb_name(void);
static AsVal SetTextToGamerTag(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    for (int i = 1; i < argc && i < 3; i++)
        if (argv[i].t == V_OBJ) as_set(vm, argv[i], as_atom(vm, "text"), as_strv(ah_lb_player_name ? as_str(vm, lb_name()) : vm->empty));
    return as_undef();
}

// ---- online leaderboards ----------------------------------------------------------------
// The menus' Leaderboards screen and the scoreboard talk to the exe through these calls; the
// front end supplies the backend (ah_lb_*: a score server over Wi-Fi on the Vita, a fake
// board in the desktop harness). Only the main game's board ("ahhd", by difficulty) is
// online; with no backend, or the option off, they answer "not online" as before.
//   UpdateLeaderboard(port, "ahhd", difficulty, score)   after a level, from the scoreboard
//   LoginToOnlineAccount(port)                           opening Leaderboards
//   MenuLeaderboards(port, filter, firstrank, command, ...), filter 0 all / 1 friends / 2 me:
//     0 (board, ranked, difficulty, timeframe 0 lifetime/1 month/2 week) start   100 status: 0 busy, >0 ready, <0 error
//     200 rows   201 the player's row (-1: none)   300 (row, rank, name, score, score2 fields)
//     310 (count, offset) scroll   500 (row) select   999 release

const char *(*ah_lb_player_name)(void);
int (*ah_lb_online)(void);
void (*ah_lb_submit)(const char *board, int difficulty, double score);
void (*ah_lb_request)(const char *board, int difficulty, int timeframe, int around_me);
int (*ah_lb_poll)(const AhLbRow **rows, int *nrows, int *me);

static int lb_on(void) { return ah_lb_online && ah_lb_poll && ah_lb_online(); }

static const char *lb_name(void) {
    const char *n = ah_lb_player_name ? ah_lb_player_name() : NULL;
    return n && *n ? n : "Player";
}

// Co-op games send no scores (Jah's call: both players share the Vita's one name). Player 2
// has joined once the scripts give GLOBAL_alien2_port a port (a new game sets it to -1; it
// stays set for the rest of the run).
static int coop_game(AsVm *vm) {
    AsVal where[3] = {cc_flash_global(vm, "GLOBAL_alien2_port"),
                      as_get(vm, as_objv(vm->global), as_atom(vm, "GLOBAL_alien2_port")),
                      vm->root ? as_get(vm, as_objv(&vm->root->obj), as_atom(vm, "GLOBAL_alien2_port")) : as_undef()};
    for (int i = 0; i < 3; i++)
        if (where[i].t == V_NUM && where[i].n >= 0) return 1;
    return 0;
}

static AsVal UpdateLeaderboard(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    log_call(vm, "UpdateLeaderboard", argc, argv);
    if (argc < 4 || !ah_lb_submit || !ah_lb_online || !ah_lb_online()) return as_undef();
    if (coop_game(vm)) return as_undef();
    const char *board = as_tostr(vm, argv[1])->s;
    double score = as_tonum(vm, argv[3]);
    if (strcmp(board, "ahhd") || !isfinite(score) || score <= 0) return as_undef();   // the main game only
    ah_lb_submit(board, (int)as_tonum(vm, argv[2]), score);
    return as_undef();
}

static AsVal LoginToOnlineAccount(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    log_call(vm, "LoginToOnlineAccount", argc, argv);
    return as_bool(lb_on());
}

// Online play stays off: only the Leaderboards screen (f_UPDATELEADERBOARD) is told the
// player is online; the match menus still see "offline" and send the player to their notice.
static AsVal IsPlayerOnline(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_; (void)argc; (void)argv;
    int lb = vm->cur_fn && vm->cur_fn->fname && !strcmp(vm->cur_fn->fname->s, "f_UPDATELEADERBOARD");
    return as_bool(lb && lb_on());
}

static AsVal ahScoresOnline(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)vm; (void)this_; (void)argc; (void)argv;
    return as_bool(lb_on());
}

static const AhLbRow *lb_rows;
static int lb_nrows, lb_me = -1, lb_state;   // lb_state: 0 none, 1 asked, 2 ready, -1 failed

static void set_text(AsVm *vm, AsVal field, const char *t) {
    if (field.t == V_OBJ) as_set(vm, field, as_atom(vm, "text"), as_strv(as_str(vm, t)));
}

// The board's rows ask for Arial at 14 px; drawn in the game's own font that reads far too
// small on the Vita, so the row fields (one shared definition) get 23 px, the row spacing.
#define LB_ROW_TEXT 460   // twips
static void row_field(AsVm *vm, AsVal field, const char *t) {
    AsMc *mc = field.t == V_OBJ && field.o->type == OBJ_MC ? (AsMc *)field.o : NULL;
    if (mc && mc->text && mc->text->height < LB_ROW_TEXT) ((EditText *)mc->text)->height = LB_ROW_TEXT;
    set_text(vm, field, t);
}

static AsVal MenuLeaderboards(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    int filter = (int)as_tonum(vm, ARG(1)), cmd = (int)as_tonum(vm, ARG(3));
    if (cmd != 100 && cmd != 300) log_call(vm, "MenuLeaderboards", argc, argv);
    if (!lb_on()) return as_num(-1);
    switch (cmd) {
    case 0: {   // board, ranked, difficulty, timeframe
        const char *board = as_tostr(vm, ARG(4))->s;
        lb_rows = NULL, lb_nrows = 0, lb_me = -1;
        if (strcmp(board, "ahhd")) { lb_state = 2; return as_num(1); }   // other boards: empty
        int tf = (int)as_tonum(vm, ARG(7));   // the game's: 0 lifetime, 1 monthly, 2 weekly
        ah_lb_request(board, (int)as_tonum(vm, ARG(6)), tf == 1 ? 1 : tf == 2 ? 0 : 2, filter == 2);
        lb_state = 1;
        return as_num(1);
    }
    case 100:
        if (lb_state == 1) {
            int r = ah_lb_poll(&lb_rows, &lb_nrows, &lb_me);
            if (r > 0) lb_state = 2;
            else if (r < 0) lb_state = -1;
        }
        return as_num(lb_state == 2 ? 1 : lb_state == 1 ? 0 : -3);
    case 200: return as_num(lb_state == 2 ? lb_nrows : 0);
    case 201: return as_num(lb_state == 2 ? lb_me : -1);
    case 300: {   // row (0-based), then its rank, name, score and second score fields
        int i = (int)as_tonum(vm, ARG(4));
        char num[16] = "", score[24] = "";
        const char *name = "";
        if (lb_state == 2 && i >= 0 && i < lb_nrows) {
            snprintf(num, sizeof num, "%d", lb_rows[i].rank);
            snprintf(score, sizeof score, "%.0f", lb_rows[i].score);
            name = lb_rows[i].name;
        }
        row_field(vm, ARG(5), num);
        row_field(vm, ARG(6), name);
        row_field(vm, ARG(7), score);
        row_field(vm, ARG(8), "");
        return as_undef();
    }
    case 999: lb_state = 0; lb_rows = NULL, lb_nrows = 0, lb_me = -1; return as_undef();
    default: return as_num(1);   // 310 scroll (every row is already here), 500 select
    }
}

// GetTextLocalizationScale(N): the _xscale (percent) string N needs to fit; English fits.
static AsVal ret_hundred(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)vm; (void)this_; (void)argc; (void)argv;
    return as_num(100);
}

// The scripts compare this with "xbox", "xboxone", "switch" and "steam" to pick
// button prompts and menus. "steam" is the build the data came from.
static AsVal get_platform_type(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_; (void)argc; (void)argv;
    return as_strv(as_atom(vm, "steam"));
}

void ah_host_init(AsVm *vm) {
    AsObj *g = vm->global;
    // The exe runs every movie as the console build: console_version is true everywhere (the
    // levels then take the game's real paths, pause.pak skips its Flash-test stubs). The
    // pins cc_host made for Castle Crashers' own functions don't apply here.
    vm->npinned = 0;
    vm->text_for_number = text_for_number;
    as_pin(vm, "console_version", as_bool(1), NULL);
    as_def_fn(vm, g, "Get480Mode", ret_false);
    as_def_fn(vm, g, "GetInputBinding", GetInputBinding);
    as_def_fn(vm, g, "GetTextLocalizationScale", ret_hundred);          // 16:9 output
    as_def_fn(vm, g, "GetPlatformType", get_platform_type);
    as_def_fn(vm, g, "MEMCARD_Seek", memcard_seek);
    as_def_fn(vm, g, "MEMCARD_Read", memcard_read);
    as_def_fn(vm, g, "MEMCARD_Write", memcard_write);
    as_def_fn(vm, g, "SetKeyMapping", ret_undef);       // keyboard rebinding: the Vita maps buttons itself
    as_def_fn(vm, g, "EnableMouse", ret_undef);
    as_def_fn(vm, g, "VibrateEnable", ret_undef);       // no rumble
    as_def_fn(vm, g, "SetRichPresenceItem", ret_undef);
    as_def_fn(vm, g, "IsPlayerAllowedOnlinePlay", ret_false);
    as_def_fn(vm, g, "EnteredGameViaInvite", ret_false);
    as_def_fn(vm, g, "PremiumContent", ret_false);
    as_def_fn(vm, g, "IsGameActive", ret_false);        // online sessions
    as_def_fn(vm, g, "IsGameLobbyActive", ret_false);
    as_def_fn(vm, g, "SeedRand", ret_undef);
    as_def_fn(vm, g, "IsSigningIn", ret_false);         // Xbox profile sign-in screen
    as_def_fn(vm, g, "IsKeyboardJoinButtonDown", ret_false);
#define DEF(fn) as_def_fn(vm, g, #fn, fn)
    DEF(ChangeMovie); DEF(SetFlashGlobal); DEF(UnPause); DEF(MenuRequestIsFullGame); DEF(MEMCARD_Start); DEF(SetPlaying);
    DEF(TurnMainMenuOn); DEF(MoveKeyboardUserToPort); DEF(DropMeFromGame);
    DEF(ClearUserToControllerBindings); DEF(StartControllerPairing); DEF(SetBackgroundAlpha);
    DEF(GetPrimaryUserPort); DEF(CreateLocalGame); DEF(CancelGame); DEF(StopControllerPairing);
    DEF(ShowSignInScreen); DEF(QuitTo); DEF(EndingAttract); DEF(MenuAchievements); DEF(MenuMultiplayer);
    DEF(MenuReturnToArcade); DEF(MenuUnlockFullGame); DEF(LoginToOnlineAccount); DEF(IsPlayerOnline); DEF(ahScoresOnline); DEF(UpdateLeaderboard); DEF(MenuLeaderboards); DEF(ahTF); DEF(ListenForKeyBinding);
    DEF(ResetKeyBindings); DEF(ChangeVideoSettings); DEF(SetMusicVolume); DEF(SetSfxVolume); DEF(GetMusicVolume); DEF(GetSfxVolume); DEF(PauseMusic); DEF(SetTextToGamerTag); DEF(f_SetLoadingLevelText); DEF(DisplayUnlock);
    DEF(f_PDALoadLevelArray); DEF(f_PDALoadLiveLevelArray); DEF(f_PDAGetNumLiveLevelsInPack); DEF(f_PDAGetNumLivePacks);
    DEF(f_PDALiveLevelIndexToPublisherId); DEF(f_PDAGetLiveLevelName); DEF(f_PDASetLevelBoxText); DEF(f_SetCreditText);
#undef DEF
}
