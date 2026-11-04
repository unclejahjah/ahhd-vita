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
LOGGED(LoginToOnlineAccount, as_bool(0))
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

// Options are stored per controller port in a small byte array the scripts
// address with MEMCARD_Seek(offset, port) then MEMCARD_Read(port) / MEMCARD_Write(value, port).
// Offsets seen: 69 vibration, 70 aim style, 73 turbo fire. All default to 0.
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
    int off = (int)as_tonum(vm, ARG(0));
    memcard_pos[port_arg(vm, ARG(1))] = off >= 0 && off < MEMCARD_SIZE ? off : 0;
    return as_undef();
}

static AsVal memcard_read(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    int p = port_arg(vm, ARG(0));
    int v = memcard[p][memcard_pos[p]];
    memcard_pos[p] = (memcard_pos[p] + 1) % MEMCARD_SIZE;
    return as_num(v);
}

static AsVal memcard_write(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    int p = port_arg(vm, ARG(1));
    memcard[p][memcard_pos[p]] = (unsigned char)as_tonum(vm, ARG(0));
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

static int pause_opened;   // the overlay is the in-game pause menu (START), not the main menu

static AsVal UnPause(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    log_call(vm, "UnPause", argc, argv);
    ah_paused = 0;
    overlay_close = 1;
    music_pause(0);
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
    return 1;
}

static void set_root(AsVm *vm, Movie *m) { vm->root = as_mc_new(vm, m, &m->root, NULL, 0); }

int ah_start(AsVm *vm, const char *first) {
    Movie *m = vm->loader(vm, first);
    if (!m) return 0;
    set_root(vm, m);
    return 1;
}

static char ah_next_movie[64];

// ---- the loading screen ----------------------------------------------------------------
// The exe put loading.pak over the screen while it loaded a level. Its script and the exe
// talk through flash globals: g_bLoaded (the exe: the level is in), g_bLevelLoaded
// (loading.pak: done, after its 4-second minimum with a tip, or at once after the
// scoreboard). Here: show it, load the level once it has been drawn, then hand over.

static int loading_frames;       // ticks it has run
static char loading_target[64];  // file still to load, "" once loaded

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
    ah_loading = NULL;
    as_set(vm, as_objv(vm->global), as_atom(vm, "__ah_loading"), as_undef());
}

const char *ah_tick(AsVm *vm) {
    static char changed[64];
    if (ah_loading) {   // only the loading screen runs
        run_as_root(vm, &ah_loading, 0);
        loading_frames++;
        if (loading_target[0] && loading_frames >= 2) {   // it is on screen: load now
            snprintf(changed, sizeof changed, "%s", loading_target);
            loading_target[0] = 0;
            int ok = change_root(vm, changed);
            cc_set_flash_global(vm, "g_bLoaded", as_bool(1));
            if (!ok) end_loading(vm);
            return ok ? changed : NULL;
        }
        if (!loading_target[0] && (as_tobool(vm, cc_flash_global(vm, "g_bLevelLoaded")) || loading_frames > 30 * 30))
            end_loading(vm);
        return NULL;
    }
    if (!ah_paused) as_tick(vm);
    if (overlay_open) {
        overlay_open = overlay_close = 0;
        Movie *o = vm->loader(vm, "pause.pak");
        vm->overlay = o ? as_mc_new(vm, o, &o->root, NULL, 0) : NULL;
        if (!o) ah_paused = 0;
    }
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
        loading_frames = 0;
        snprintf(loading_target, sizeof loading_target, "%s", changed);
        vm->overlay = NULL;
        ah_paused = 0;
        if (ah_sfx_stop_all) ah_sfx_stop_all();
        return NULL;
    }
    return change_root(vm, changed) ? changed : NULL;
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
    uint8_t expect[8];         // what must be there
    uint8_t put[5];            // what goes there
    const char *why;
} Patch;

static const Patch patches[] = {
    // f_BuildPlayerOptionsMenu: jump over the VIBRATION item (text 31) to the next one; the
    // Vita has no rumble
    {"pause.pak", 0x45e78, {0x96, 0x15, 0x00, 0x04, 0x05, 0x04, 0x02, 0x03}, {0x99, 0x02, 0x00, 0xcb, 0x00},
     "no vibration option"},
};

void ah_patch_movie(const char *file, Movie *m) {
    cc_buttons_ah_glyphs(m);   // PlayStation buttons like the Castle Crashers build
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    for (size_t i = 0; i < sizeof patches / sizeof *patches; i++) {
        const Patch *p = &patches[i];
        if (strcasecmp(base, p->file) || p->at + 8 > m->len || memcmp(m->data + p->at, p->expect, 8)) continue;
        memcpy(m->data + p->at, p->put, 5);
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
// (ah_load_strings), string N is its Nth record and f_GetText is only the fallback (the
// SWF's copy is older and lacks some, e.g. 347 "Exit Game").

#define MAX_TEXT 1024
static char *texts[MAX_TEXT];
static char *dct[MAX_TEXT];   // 1-based
static int ndct;
static AsMc *text_clip;
static int text_tried;

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

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

static const char *text_for_number(AsVm *vm, int n) {
    if (n > 0 && n <= ndct && n < MAX_TEXT) return dct[n];
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
    {"menu_lt", "l1"},    {"menu_rt", "r1"},       {"start", "start"}, {"jump", "x"},
    {"shoot", "square"},  {"grenade", "circle"},   {"interact", "triangle"},
    {"roll_left", "l1"},  {"roll_right", "r1"},
};

static AsVal GetInputBinding(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    AsVal b = ARG(0);
    if (b.t != V_OBJ) return as_bool(0);
    const char *action = as_tostr(vm, as_get(vm, b, as_atom(vm, "s_action")))->s;
    const char *button = NULL;
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
static AsVal SetTextToGamerTag(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    for (int i = 1; i < argc && i < 3; i++)
        if (argv[i].t == V_OBJ) as_set(vm, argv[i], as_atom(vm, "text"), as_strv(vm->empty));
    return as_undef();
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
    as_def_fn(vm, g, "IsPlayerOnline", ret_false);      // no online play on the Vita
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
    DEF(MenuReturnToArcade); DEF(MenuUnlockFullGame); DEF(LoginToOnlineAccount); DEF(ListenForKeyBinding);
    DEF(ResetKeyBindings); DEF(ChangeVideoSettings); DEF(SetMusicVolume); DEF(SetSfxVolume); DEF(GetMusicVolume); DEF(GetSfxVolume); DEF(PauseMusic); DEF(SetTextToGamerTag);
#undef DEF
}
