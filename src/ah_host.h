// Host functions Alien Hominid HD's scripts expect from the game exe, on top of
// cc_host.c's shared ones (Key, flash globals, music, storage...).
#ifndef AH_HOST_H
#define AH_HOST_H

#include "as.h"

void ah_host_init(AsVm *vm);   // call after cc_host_init
// Loads `first` (e.g. "legal.pak") as the root movie and pause.pak over it.
int ah_start(AsVm *vm, const char *first);
// One game frame: the root (unless paused), then the overlay. Returns the file name when the
// scripts changed the root movie (ChangeMovie) this frame, else NULL.
const char *ah_tick(AsVm *vm);
// START in game: opens the pause menu for that port (0-3); 0 when the game allows no pause now.
int ah_request_pause(AsVm *vm, int port);
// The game's text table, data/loc/en.dct (or another language); the number of strings, 0 if
// unreadable. Without it the SWF's own older copy answers.
int ah_load_strings(const char *path);

// The save file (memory-card bytes and volumes): read at startup, then written whenever
// it changes (call ah_save_tick now and then; 1 written, 0 unchanged, -1 failed).
int ah_save_load(const char *path);
int ah_save_tick(void);
// Vita changes to the game's own scripts (e.g. no vibration option); call on each loaded file.
void ah_patch_movie(const char *file, Movie *m);
// The game's loading screen while a level loads, or NULL. While it is up, draw only it.
extern char ah_data_dir[256];   // set before the game starts: where game/ and levels/ are
extern AsMc *ah_loading;
extern AsMc *ah_unlock_layer;   // the unlock banners, drawn over the game
extern int ah_paused;
int ah_in_level(void);   // a story level or challenge is running (START pauses, SELECT is its start key)
extern float ah_background_alpha;
// sound, set by the front end: pause/resume music, volumes 0..1 (negative: unchanged),
// stop every sound effect
extern void (*ah_music_pause)(int paused);
extern void (*ah_audio_volumes)(float music, float sfx);
extern void (*ah_sfx_stop_all)(void);
extern void (*ah_sfx_hold)(int hold);   // freeze (1) or resume (0) the sounds playing now
// online leaderboards (the main game's board), all optional:
typedef struct { int rank; char name[32]; double score; } AhLbRow;
extern const char *(*ah_lb_player_name)(void);                         // shown on the boards
extern int (*ah_lb_online)(void);                                      // option on, network up
extern void (*ah_lb_submit)(const char *board, int difficulty, double score);
extern void (*ah_lb_request)(const char *board, int difficulty, int timeframe, int around_me);   // timeframe 0 week, 1 month, 2 all
extern int (*ah_lb_poll)(const AhLbRow **rows, int *nrows, int *me);   // 0 busy, 1 done, -1 failed
extern void (*ah_log)(const char *line);   // optional: where logged calls go

#endif
