// Alien Hominid HD Vita: animation viewer.
// Loads the game's .pak movies from the memory card (copied from the user's own
// install) and plays their animations with the cc-vita renderer.
//   Up/Down:        previous/next movie
//   Left/Right:     previous/next animation (frame label), or root frame if none
//   L/R:            zoom          left stick: pan        CROSS: reset view
//   SELECT+Up:      toggle the 60 fps cap                START: quit
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/power.h>
#include <stdio.h>
#include <string.h>
#include <vita2d.h>

#include "ah_view.h"
#include "render.h"

#define DATA_DIR "ux0:data/AlienHominidHD/"
#define MAX_BATCH 65535   // u16 indices; a multiple of 3

// Movies in the order the viewer offers them: characters first, then the rest.
// Missing files are skipped.
static const char *const MOVIES[] = {
    "game/alien", "game/army", "game/fbi", "game/kgb", "game/abullets", "game/agrenades",
    "game/ebullets", "game/egrenades", "game/neutron", "game/pinata", "game/pudding",
    "game/chal12", "game/chal31", "game/chal34", "game/hud", "game/hud_unlock",
    "game/backgrounds", "game/backgrounds2", "game/backgrounds3", "game/backgrounds4",
    "game/attract", "game/menu", "game/pause", "game/scorebrd", "game/ssmm", "game/legal",
    "game/loading", "game/launcher", "game/movie11", "game/movieend", "game/endofdemo",
    "levels/level11", "levels/level12", "levels/level13", "levels/level14", "levels/level15",
    "levels/level21", "levels/level22", "levels/level23", "levels/level24", "levels/level25",
    "levels/level26", "levels/level31", "levels/level32", "levels/level33", "levels/level34",
    "levels/level35",
};
#define NMOVIES ((int)(sizeof MOVIES / sizeof MOVIES[0]))

static void set_stencil(SceGxmStencilFunc func, int ref, SceGxmStencilOp op) {
    // Triangle winding varies with flips, so set both faces.
    SceGxmContext *ctx = vita2d_get_context();
    sceGxmSetFrontStencilRef(ctx, ref);
    sceGxmSetBackStencilRef(ctx, ref);
    sceGxmSetFrontStencilFunc(ctx, func, SCE_GXM_STENCIL_OP_KEEP, op, op, 0xFF, 0xFF);
    sceGxmSetBackStencilFunc(ctx, func, SCE_GXM_STENCIL_OP_KEEP, op, op, 0xFF, 0xFF);
}

// ---- GPU memory (as in viewer.c) ------------------------------------------------

extern SceGxmVertexProgram *_vita2d_colorVertexProgram;
extern SceGxmFragmentProgram *_vita2d_colorFragmentProgram;
extern const SceGxmProgramParameter *_vita2d_colorWvpParam;
extern float _vita2d_ortho_matrix[16];

typedef struct {
    SceUID uid;
    void *addr;
} GpuBlock;

static void *gpu_alloc(size_t bytes, void *ctx) {
    GpuBlock *blk = ctx;
    size_t size = (bytes + 4095) & ~(size_t)4095;
    blk->uid = sceKernelAllocMemBlock("ah_mesh", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, NULL);
    if (blk->uid < 0) return NULL;
    sceKernelGetMemBlockBase(blk->uid, &blk->addr);
    if (sceGxmMapMemory(blk->addr, size, SCE_GXM_MEMORY_ATTRIB_READ) < 0) {
        sceKernelFreeMemBlock(blk->uid);
        blk->uid = -1;
        return NULL;
    }
    return blk->addr;
}

static void gpu_free(GpuBlock *blk) {
    if (blk->uid < 0) return;
    sceGxmUnmapMemory(blk->addr);
    sceKernelFreeMemBlock(blk->uid);
    blk->uid = -1;
}

static uint16_t *g_indices;   // 0, 1, 2, ... for sceGxmDraw
static GpuBlock g_index_block = {-1, NULL};

static void init_indices(void) {
    g_indices = gpu_alloc(sizeof(uint16_t) * MAX_BATCH, &g_index_block);
    for (int i = 0; g_indices && i < MAX_BATCH; i++) g_indices[i] = i;
}

// wvp = projection * item matrix (see viewer.c).
static void item_wvp(const Mat *m, float out[16]) {
    const float *o = _vita2d_ortho_matrix;
    memcpy(out, o, sizeof(float) * 16);
    int t = o[12] != 0 || o[13] != 0;
    float ox = o[0], oy = o[5], otx = t ? o[12] : o[3], oty = t ? o[13] : o[7];
    out[0] = ox * m->a;
    out[5] = oy * m->d;
    out[t ? 4 : 1] = ox * m->c;
    out[t ? 1 : 4] = oy * m->b;
    out[t ? 12 : 3] = ox * m->tx + otx;
    out[t ? 13 : 7] = oy * m->ty + oty;
}

static void draw_list(const DrawList *dl) {
    static const SceGxmStencilOp ops[] = {SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_INCR, SCE_GXM_STENCIL_OP_DECR};
    if (!g_indices) return;
    SceGxmContext *ctx = vita2d_get_context();
    sceGxmSetVertexProgram(ctx, _vita2d_colorVertexProgram);
    sceGxmSetFragmentProgram(ctx, _vita2d_colorFragmentProgram);
    for (int c = 0; c < dl->ncmd; c++) {
        const DrawCmd *cmd = &dl->cmd[c];
        if (!cmd->count) continue;
        set_stencil(SCE_GXM_STENCIL_FUNC_EQUAL, cmd->ref, ops[cmd->op]);
        for (int k = cmd->start; k < cmd->start + cmd->count; k++) {
            const DrawItem *it = &dl->items[k];
            if (it->bf) continue;   // bitmap fills are not drawn by the viewer
            const Vertex *v = it->vp;
            if (!v || it->col || it->cv >= 0) {   // recoloured: this frame's vertices in mesh space
                Vertex *t = vita2d_pool_memalign(it->mesh->nverts * sizeof(Vertex), sizeof(Vertex));
                if (!t) continue;
                for (int i = 0; i < it->mesh->nverts; i++) {
                    Vertex s = render_item_vertex(dl, it, i);
                    t[i] = (Vertex){it->mesh->xy[i * 2], it->mesh->xy[i * 2 + 1], 0.5f, s.color};
                }
                v = t;
            }
            float wvp[16];
            item_wvp(&it->m, wvp);
            for (int off = 0; off < it->mesh->nverts; off += MAX_BATCH) {
                int count = it->mesh->nverts - off < MAX_BATCH ? it->mesh->nverts - off : MAX_BATCH;
                void *ub;
                sceGxmReserveVertexDefaultUniformBuffer(ctx, &ub);
                sceGxmSetUniformDataF(ub, _vita2d_colorWvpParam, 0, 16, wvp);
                sceGxmSetVertexStream(ctx, 0, v + off);
                sceGxmDraw(ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, g_indices, count);
            }
        }
    }
    set_stencil(SCE_GXM_STENCIL_FUNC_ALWAYS, 1, SCE_GXM_STENCIL_OP_KEEP);
}

static void show_message(vita2d_pgf *font, const char *line1, const char *line2) {
    vita2d_set_clear_color(RGBA8(237, 237, 224, 255));
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_pgf_draw_text(font, 40, 240, RGBA8(40, 40, 40, 255), 1.0f, line1);
    if (line2) vita2d_pgf_draw_text(font, 40, 280, RGBA8(90, 90, 90, 255), 0.8f, line2);
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

// Loads the first movie that exists from index *idx, stepping by dir, and uploads
// its meshes into blk. Returns NULL (with err set) if none of them load.
static Movie *load_movie(vita2d_pgf *font, int *idx, int dir, GpuBlock *blk, char *err, int errlen) {
    for (int tries = 0; tries < NMOVIES; tries++) {
        char path[160], msg[96];
        snprintf(path, sizeof path, DATA_DIR "%s.pak", MOVIES[*idx]);
        snprintf(msg, sizeof msg, "Loading %s.pak...", MOVIES[*idx]);
        show_message(font, msg, NULL);
        Movie *m = swf_load(path, err, errlen);
        if (m) {
            render_tessellate_all(m);
            render_upload(m, gpu_alloc, blk);   // keeps the CPU copy: recoloured draws read mesh->xy
            return m;
        }
        *idx = (*idx + dir + NMOVIES) % NMOVIES;
    }
    return NULL;
}

int main(void) {
    scePowerSetArmClockFrequency(444);
    scePowerSetGpuClockFrequency(222);
    vita2d_init_advanced_with_msaa(16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_4X);
    vita2d_set_vblank_wait(1);
    vita2d_pgf *font = vita2d_load_default_pgf();
    init_indices();

    char err[256] = "";
    int idx = 0;
    GpuBlock movie_gpu = {-1, NULL};
    Movie *m = load_movie(font, &idx, 1, &movie_gpu, err, sizeof err);

    DrawList vb = {0};
    GpuBlock cache_gpu = {-1, NULL};
    const size_t cache_bytes = 8 * 1024 * 1024;
    RecolourCache cache;
    recolour_cache_init(&cache, gpu_alloc(cache_bytes, &cache_gpu), cache_bytes);
    vb.cache = &cache;

    int label = 0, frame = 0, tick = 0, vsync = 0, vblank = 1;
    int stand = m ? sprite_find_label(&m->root, "stand") : -1;
    for (int i = 0; m && i < m->root.nlabels; i++)
        if (m->root.labels[i].frame == stand) label = i;
    float zoom = 0, pan_x = 0, pan_y = 0;   // zoom 0: the movie's default
    SceUInt64 last = sceKernelGetProcessTimeWide();
    float fps = 0;
    int frames_counted = 0, tris_max = 0, shown_tris = 0;

    SceCtrlData pad, old = {0};
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    for (;;) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~old.buttons;
        old = pad;
        if (pressed & SCE_CTRL_START) break;
        int debug = pad.buttons & SCE_CTRL_SELECT;
        if (debug && (pressed & SCE_CTRL_UP)) { vblank = !vblank; vita2d_set_vblank_wait(vblank); }

        if (!debug && (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN))) {
            int dir = pressed & SCE_CTRL_DOWN ? 1 : -1;
            vita2d_wait_rendering_done();   // the GPU may still be reading the old meshes
            swf_free(m);
            gpu_free(&movie_gpu);
            void *cache_mem = cache.mem;    // entries point at the old movie's meshes
            recolour_cache_free(&cache);
            recolour_cache_init(&cache, cache_mem, cache_bytes);
            idx = (idx + dir + NMOVIES) % NMOVIES;
            m = load_movie(font, &idx, dir, &movie_gpu, err, sizeof err);
            label = frame = tick = 0;
            zoom = pan_x = pan_y = 0;
            last = sceKernelGetProcessTimeWide();
            frames_counted = 0;
        }
        if (m) {
            int n = m->root.nlabels ? m->root.nlabels : m->root.nframes;
            int *sel = m->root.nlabels ? &label : &frame;
            if (!debug && (pressed & SCE_CTRL_RIGHT) && n) { *sel = (*sel + 1) % n; tick = 0; }
            if (!debug && (pressed & SCE_CTRL_LEFT) && n) { *sel = (*sel + n - 1) % n; tick = 0; }
            if (zoom <= 0) zoom = ah_default_zoom(m);
            if (pad.buttons & SCE_CTRL_RTRIGGER) zoom *= 1.02f;
            if (pad.buttons & SCE_CTRL_LTRIGGER) zoom /= 1.02f;
            float sx = (pad.lx - 128) / 128.0f, sy = (pad.ly - 128) / 128.0f;
            if (sx > 0.2f || sx < -0.2f) pan_x -= sx * 8;
            if (sy > 0.2f || sy < -0.2f) pan_y -= sy * 8;
            if (pressed & SCE_CTRL_CROSS) { zoom = ah_default_zoom(m); pan_x = pan_y = 0; }
        }

        render_begin(&vb);
        if (m && m->root.nframes) {
            vb.view[2] = 0;   // no culling: zoom and pan move parts on and off screen
            int fi = m->root.nlabels ? m->root.labels[label].frame : frame;
            render_display_list(m, &m->root.frames[fi], ah_view_matrix(m, zoom, pan_x, pan_y), CX_IDENTITY, tick, &vb);
            if (render_stats.triangles > tris_max) tris_max = render_stats.triangles;
        }

        if (m) vita2d_set_clear_color(RGBA8(m->bg[0], m->bg[1], m->bg[2], 255));
        else vita2d_set_clear_color(RGBA8(237, 237, 224, 255));
        vita2d_start_drawing();
        vita2d_clear_screen();
        if (m) {
            draw_list(&vb);
            int dark = m->bg[0] + m->bg[1] + m->bg[2] < 300;
            unsigned int ink = dark ? RGBA8(255, 255, 255, 255) : RGBA8(40, 40, 40, 255);
            if (m->root.nlabels)
                vita2d_pgf_draw_textf(font, 16, 28, ink, 1.0f, "%s.pak   %s  (%d/%d)", MOVIES[idx],
                                      m->root.labels[label].name, label + 1, m->root.nlabels);
            else
                vita2d_pgf_draw_textf(font, 16, 28, ink, 1.0f, "%s.pak   frame %d/%d", MOVIES[idx], frame + 1,
                                      m->root.nframes);
            vita2d_pgf_draw_textf(font, 16, 52, ink, 0.8f, "%.0f fps%s   peak %d tris   draws %d", fps,
                                  vblank ? "" : " (uncapped)", shown_tris, render_stats.draws);
            vita2d_pgf_draw_text(font, 16, 528, ink, 0.8f,
                                 "Up/Down: movie   Left/Right: animation   L/R: zoom   stick: pan   CROSS: reset   START: quit");
        } else {
            vita2d_pgf_draw_text(font, 40, 240, RGBA8(160, 30, 30, 255), 1.0f, err);
            vita2d_pgf_draw_text(font, 40, 280, RGBA8(40, 40, 40, 255), 1.0f,
                                 "Copy the game and levels folders from your own install's data folder");
            vita2d_pgf_draw_text(font, 40, 310, RGBA8(40, 40, 40, 255), 1.0f, "to " DATA_DIR ", then restart.");
            vita2d_pgf_draw_text(font, 40, 350, RGBA8(40, 40, 40, 255), 1.0f, "Press START to quit.");
        }
        vita2d_end_drawing();
        vita2d_swap_buffers();

        if (++vsync % 2 == 0) tick++;   // the game animates at 30 fps
        frames_counted++;
        SceUInt64 now = sceKernelGetProcessTimeWide();
        if (now - last >= 1000000) {
            fps = frames_counted * 1000000.0f / (now - last);
            shown_tris = tris_max;
            tris_max = 0;
            frames_counted = 0;
            last = now;
        }
    }

    vita2d_wait_rendering_done();
    swf_free(m);
    gpu_free(&movie_gpu);
    gpu_free(&g_index_block);
    recolour_cache_free(&cache);
    gpu_free(&cache_gpu);
    vita2d_free_pgf(font);
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
