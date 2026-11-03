// Alien Hominid HD host functions. Each is matched to how the level scripts use it
// (LEVEL11 disassembly, 2026-11-03); unknown ones are stubs that return what makes
// the scripts take the plain single-player path.
#include "ah_host.h"
#include "cc_host.h"

#include <stdio.h>
#include <string.h>

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
LOGGED(MenuRequestIsFullGame, as_undef())
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
LOGGED(QuitTo, as_undef())
LOGGED(EndingAttract, as_undef())
LOGGED(MenuAchievements, as_undef())
LOGGED(MenuMultiplayer, as_undef())
LOGGED(MenuReturnToArcade, as_undef())
LOGGED(MenuUnlockFullGame, as_undef())
LOGGED(LoginToOnlineAccount, as_bool(0))
LOGGED(ListenForKeyBinding, as_undef())
LOGGED(ResetKeyBindings, as_undef())
LOGGED(ChangeVideoSettings, as_undef())
LOGGED(SetMusicVolume, as_undef())
LOGGED(SetSfxVolume, as_undef())
LOGGED(PauseMusic, as_undef())

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

static AsVal UnPause(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)this_;
    log_call(vm, "UnPause", argc, argv);
    ah_paused = 0;
    overlay_close = 1;
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

int ah_request_pause(AsVm *vm, int port) {
    if (ah_paused || as_tobool(vm, cc_flash_global(vm, "g_bNoPause"))) return 0;
    cc_set_flash_global(vm, "g_nPausePlayer", as_num(port));
    open_overlay(vm, 1);
    ah_paused = 1;
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

const char *ah_tick(AsVm *vm) {
    static char changed[64];
    if (!ah_paused) as_tick(vm);
    if (overlay_open) {
        overlay_open = overlay_close = 0;
        Movie *o = vm->loader(vm, "pause.pak");
        vm->overlay = o ? as_mc_new(vm, o, &o->root, NULL, 0) : NULL;
        if (!o) ah_paused = 0;
    }
    if (vm->overlay) {   // the overlay runs as its own _root
        AsMc *root = vm->root;
        uint32_t tick = vm->tick;
        vm->root = vm->overlay;
        as_tick(vm);
        vm->overlay = vm->root;
        vm->root = root;
        vm->tick = ah_paused ? vm->tick : tick;
    }
    if (overlay_close) {
        overlay_close = 0;
        vm->overlay = NULL;
    }
    if (!ah_next_movie[0]) return NULL;
    snprintf(changed, sizeof changed, "%s", ah_next_movie);
    ah_next_movie[0] = 0;
    Movie *m = vm->loader(vm, changed);
    if (!m) return NULL;
    set_root(vm, m);
    vm->overlay = NULL;
    ah_paused = 0;
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


// GetInputBinding(b): which button an action is on, for the button glyphs. b.s_action is the
// action ("menu_select", "jump"...); the answer goes in b.s_device and b.s_button, which
// pick a frame of the game's own glyph clip ("ps4_x"). The PS4 glyphs match the Vita's face
// buttons; the shoulder ones show as L1/R1.
static const char *const VITA_BUTTONS[][2] = {
    {"menu_select", "x"}, {"menu_back", "circle"}, {"menu_x", "square"}, {"menu_y", "triangle"},
    {"menu_lt", "l1"},    {"menu_rt", "r1"},       {"start", "options"}, {"jump", "x"},
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
    as_set(vm, b, as_atom(vm, "s_device"), as_strv(as_atom(vm, "ps4")));
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

static AsVal ret_one(AsVm *vm, AsVal this_, int argc, AsVal *argv) {
    (void)vm; (void)this_; (void)argc; (void)argv;
    return as_num(1);
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
    as_pin(vm, "console_version", as_bool(1), NULL);
    as_def_fn(vm, g, "Get480Mode", ret_false);
    as_def_fn(vm, g, "GetInputBinding", GetInputBinding);
    as_def_fn(vm, g, "GetTextLocalizationScale", ret_one);          // 16:9 output
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
    DEF(ChangeMovie); DEF(UnPause); DEF(MenuRequestIsFullGame); DEF(MEMCARD_Start); DEF(SetPlaying);
    DEF(TurnMainMenuOn); DEF(MoveKeyboardUserToPort); DEF(DropMeFromGame);
    DEF(ClearUserToControllerBindings); DEF(StartControllerPairing); DEF(SetBackgroundAlpha);
    DEF(GetPrimaryUserPort); DEF(CreateLocalGame); DEF(CancelGame); DEF(StopControllerPairing);
    DEF(ShowSignInScreen); DEF(QuitTo); DEF(EndingAttract); DEF(MenuAchievements); DEF(MenuMultiplayer);
    DEF(MenuReturnToArcade); DEF(MenuUnlockFullGame); DEF(LoginToOnlineAccount); DEF(ListenForKeyBinding);
    DEF(ResetKeyBindings); DEF(ChangeVideoSettings); DEF(SetMusicVolume); DEF(SetSfxVolume); DEF(PauseMusic); DEF(SetTextToGamerTag);
#undef DEF
}
