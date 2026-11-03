// How the Alien Hominid viewer frames a movie's root timeline on the 960x544 screen.
// Shared by the Vita app (src/ah_viewer.c) and the desktop harness (--root).
#ifndef AH_VIEW_H
#define AH_VIEW_H

#include "swf.h"

#define AH_SCREEN_W 960.0f
#define AH_SCREEN_H 544.0f

// The default zoom for a movie: characters (root timelines with labels) at 2.5x,
// anything else scaled so its stage fills the screen.
float ah_default_zoom(const Movie *m);

// Root matrix (twips -> screen pixels). zoom <= 0 means ah_default_zoom; pan is in
// screen pixels. Characters stand at (480, 420); stages are centred.
Mat ah_view_matrix(const Movie *m, float zoom, float pan_x, float pan_y);

#endif
