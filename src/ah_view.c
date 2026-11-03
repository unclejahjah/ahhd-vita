#include "ah_view.h"

float ah_default_zoom(const Movie *m) {
    if (m->root.nlabels || m->stage_w <= 0 || m->stage_h <= 0) return 2.5f;
    float zx = AH_SCREEN_W / m->stage_w, zy = AH_SCREEN_H / m->stage_h;
    return zx < zy ? zx : zy;
}

Mat ah_view_matrix(const Movie *m, float zoom, float pan_x, float pan_y) {
    if (zoom <= 0) zoom = ah_default_zoom(m);
    float k = zoom / 20.0f;
    if (m->root.nlabels)
        return (Mat){k, 0, 0, k, 480.0f + pan_x, 420.0f + pan_y};
    return (Mat){k, 0, 0, k, (AH_SCREEN_W - m->stage_w * zoom) / 2 + pan_x, (AH_SCREEN_H - m->stage_h * zoom) / 2 + pan_y};
}
