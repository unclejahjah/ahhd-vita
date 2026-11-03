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
extern int ah_paused;
extern float ah_background_alpha;
extern void (*ah_log)(const char *line);   // optional: where logged calls go

#endif
