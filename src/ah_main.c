// Alien Hominid HD Vita: runs the game's own ActionScript from the user's copy of the
// game in ux0:data/AlienHominidHD/{game,levels,fonts}. The flow is the original's:
// legal -> title (attract) -> main menu (menu.pak with pause.pak over it) -> intro ->
// levels. The game logic runs at its 30 frames a second; drawing is the GPU shape
// renderer from cc-vita.
//
// Controls (player 1): left stick or D-pad move, CROSS jump / accept, SQUARE shoot,
// CIRCLE grenade / back, TRIANGLE interact, L/R roll left/right, START pause.
// SELECT+UP shows the stats overlay, SELECT+START quits.
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/power.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <vita2d.h>

#include "as.h"
#include "as_render.h"
#include "ah_host.h"
#include "audio.h"
#include "cc_host.h"
#include "hostfont.h"
#include "render.h"

// parsed SWFs and the script heap (meshes live in GPU memory blocks outside it)
int _newlib_heap_size_user = 160 * 1024 * 1024;

#define DATA_DIR "ux0:data/AlienHominidHD/"
#define MAX_BATCH 65535   // u16 indices; a multiple of 3
#define SCREEN_W 960.0f
#define SCREEN_H 544.0f

static void set_stencil(SceGxmStencilFunc func, int ref, SceGxmStencilOp op) {
    // Triangle winding varies with flips, so set both faces.
    SceGxmContext *ctx = vita2d_get_context();
    sceGxmSetFrontStencilRef(ctx, ref);
    sceGxmSetBackStencilRef(ctx, ref);
    sceGxmSetFrontStencilFunc(ctx, func, SCE_GXM_STENCIL_OP_KEEP, op, op, 0xFF, 0xFF);
    sceGxmSetBackStencilFunc(ctx, func, SCE_GXM_STENCIL_OP_KEEP, op, op, 0xFF, 0xFF);
}

// ---- GPU memory -------------------------------------------------------------------

extern SceGxmVertexProgram *_vita2d_colorVertexProgram;
extern SceGxmFragmentProgram *_vita2d_colorFragmentProgram;
extern const SceGxmProgramParameter *_vita2d_colorWvpParam;
extern SceGxmVertexProgram *_vita2d_textureVertexProgram;
extern SceGxmFragmentProgram *_vita2d_textureTintFragmentProgram;
extern const SceGxmProgramParameter *_vita2d_textureWvpParam;
extern SceGxmProgramParameter *_vita2d_textureTintColorParam;
extern float _vita2d_ortho_matrix[16];

typedef struct {
    SceUID uid;
    void *addr;
    size_t size;
} GpuBlock;

static size_t g_gpu_bytes;   // for the overlay

// Uncached main memory the GPU can read (for data written every frame or in place).
static void *gpu_alloc(size_t bytes, void *ctx) {
    GpuBlock *blk = ctx;
    size_t size = (bytes + 4095) & ~(size_t)4095;
    blk->uid = sceKernelAllocMemBlock("cc_gpu", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, NULL);
    if (blk->uid < 0) return NULL;
    sceKernelGetMemBlockBase(blk->uid, &blk->addr);
    if (sceGxmMapMemory(blk->addr, size, SCE_GXM_MEMORY_ATTRIB_READ) < 0) {
        sceKernelFreeMemBlock(blk->uid);
        blk->uid = -1;
        return NULL;
    }
    g_gpu_bytes += blk->size = size;
    return blk->addr;
}

// Meshes: video memory when there's room (fast for the GPU, written once), else main memory.
static void *mesh_alloc(size_t bytes, void *ctx) {
    GpuBlock *blk = ctx;
    size_t size = (bytes + 256 * 1024 - 1) & ~(size_t)(256 * 1024 - 1);
    blk->uid = sceKernelAllocMemBlock("cc_mesh", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, NULL);
    if (blk->uid >= 0) {
        sceKernelGetMemBlockBase(blk->uid, &blk->addr);
        if (sceGxmMapMemory(blk->addr, size, SCE_GXM_MEMORY_ATTRIB_READ) >= 0) {
            g_gpu_bytes += blk->size = size;
            return blk->addr;
        }
        sceKernelFreeMemBlock(blk->uid);
    }
    return gpu_alloc(bytes, ctx);
}

static void gpu_free(GpuBlock *blk) {
    if (blk->uid < 0) return;
    sceGxmUnmapMemory(blk->addr);
    sceKernelFreeMemBlock(blk->uid);
    g_gpu_bytes -= blk->size;
    blk->uid = -1;
}

static uint16_t *g_indices;   // 0, 1, 2, ... for sceGxmDraw
static GpuBlock g_index_block;

// ---- drawing ----------------------------------------------------------------------

// wvp = projection * item matrix (vita2d's projection keeps its translation at [12],[13]
// or, transposed, at [3],[7]).
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

// vita2d's colour shader again, but reading colours from a second stream: positions come
// from the uploaded mesh (stream 0) and colours from a recoloured array (stream 1), so a
// colour transform only rewrites 4 bytes a vertex and never reads video memory back.
extern const SceGxmProgram color_v_gxp_start;
static SceGxmVertexProgram *g_split_vp;

static void make_split_program(void) {
    SceGxmShaderPatcher *sp = vita2d_get_shader_patcher();
    const SceGxmProgram *prog = &color_v_gxp_start;
    SceGxmShaderPatcherId id;
    if (sceGxmShaderPatcherRegisterProgram(sp, prog, &id) < 0) return;
    const SceGxmProgramParameter *pos = sceGxmProgramFindParameterByName(prog, "aPosition");
    const SceGxmProgramParameter *col = sceGxmProgramFindParameterByName(prog, "aColor");
    if (!pos || !col) return;
    SceGxmVertexAttribute attr[2];
    attr[0].streamIndex = 0;
    attr[0].offset = 0;
    attr[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attr[0].componentCount = 3;
    attr[0].regIndex = sceGxmProgramParameterGetResourceIndex(pos);
    attr[1].streamIndex = 1;
    attr[1].offset = 0;
    attr[1].format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;
    attr[1].componentCount = 4;
    attr[1].regIndex = sceGxmProgramParameterGetResourceIndex(col);
    SceGxmVertexStream st[2] = {{sizeof(Vertex), SCE_GXM_INDEX_SOURCE_INDEX_16BIT},
                                {sizeof(uint32_t), SCE_GXM_INDEX_SOURCE_INDEX_16BIT}};
    if (sceGxmShaderPatcherCreateVertexProgram(sp, id, attr, 2, st, 2, &g_split_vp) < 0) g_split_vp = NULL;
}

// Draws the items from..to-1 of a frame's list (all of it: draw_list).
static void draw_list_range(const DrawList *dl, int from, int to) {
    static const SceGxmStencilOp ops[] = {SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_INCR, SCE_GXM_STENCIL_OP_DECR};
    if (!g_indices) return;
    SceGxmContext *ctx = vita2d_get_context();
    uint32_t *cv = NULL;   // this frame's recoloured colours, where the GPU can read them
    if (dl->ncv) {
        cv = vita2d_pool_memalign(dl->ncv * sizeof(uint32_t), sizeof(uint32_t));
        if (!cv) return;   // temp pool exhausted; skip rather than crash
        memcpy(cv, dl->cv, dl->ncv * sizeof(uint32_t));
    }
    int textured = -1;   // which program pair is set: 0 colour, 1 texture, 2 colour with its own colour stream
    for (int c = 0; c < dl->ncmd; c++) {
        const DrawCmd *cmd = &dl->cmd[c];
        if (!cmd->count) continue;
        int k0 = cmd->start < from ? from : cmd->start, k1 = cmd->start + cmd->count > to ? to : cmd->start + cmd->count;
        if (k0 >= k1) continue;
        set_stencil(SCE_GXM_STENCIL_FUNC_EQUAL, cmd->ref, ops[cmd->op]);
        for (int k = k0; k < k1; k++) {
            const DrawItem *it = &dl->items[k];
            float wvp[16];
            item_wvp(&it->m, wvp);
            if (it->bf) {   // bitmap fill: textured, tinted by the colour transform
                vita2d_texture *tex = it->bf->bitmap ? it->bf->bitmap->tex : NULL;
                if (!tex) continue;
                const TexVertex *tv = it->mesh->gpu;
                if (!tv) {   // not uploaded: this frame's copy
                    TexVertex *t = vita2d_pool_memalign(it->mesh->nverts * sizeof(TexVertex), sizeof(TexVertex));
                    if (!t || !it->mesh->xy) continue;
                    for (int i = 0; i < it->mesh->nverts; i++)
                        t[i] = (TexVertex){it->mesh->xy[i * 2], it->mesh->xy[i * 2 + 1], 0.5f, it->mesh->uv[i * 2],
                                           it->mesh->uv[i * 2 + 1]};
                    tv = t;
                }
                if (textured != 1) {
                    sceGxmSetVertexProgram(ctx, _vita2d_textureVertexProgram);
                    sceGxmSetFragmentProgram(ctx, _vita2d_textureTintFragmentProgram);
                    textured = 1;
                }
                SceGxmTextureFilter filt = it->bf->smooth ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT;
                vita2d_texture_set_filters(tex, filt, filt);
                SceGxmTextureAddrMode am = it->bf->clip ? SCE_GXM_TEXTURE_ADDR_CLAMP : SCE_GXM_TEXTURE_ADDR_REPEAT;
                sceGxmTextureSetUAddrMode(&tex->gxm_tex, am);
                sceGxmTextureSetVAddrMode(&tex->gxm_tex, am);
                sceGxmSetFragmentTexture(ctx, 0, &tex->gxm_tex);
                void *fub;
                sceGxmReserveFragmentDefaultUniformBuffer(ctx, &fub);
                sceGxmSetUniformDataF(fub, _vita2d_textureTintColorParam, 0, 4, it->tint);
                for (int off = 0; off < it->mesh->nverts; off += MAX_BATCH) {
                    int count = it->mesh->nverts - off < MAX_BATCH ? it->mesh->nverts - off : MAX_BATCH;
                    void *ub;
                    sceGxmReserveVertexDefaultUniformBuffer(ctx, &ub);
                    sceGxmSetUniformDataF(ub, _vita2d_textureWvpParam, 0, 16, wvp);
                    sceGxmSetVertexStream(ctx, 0, tv + off);
                    sceGxmDraw(ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, g_indices, count);
                }
                continue;
            }
            const Mesh *mesh = it->mesh;
            const uint32_t *colours = it->col ? it->col : it->cv >= 0 && cv ? cv + it->cv : NULL;
            const Vertex *v = it->vp;
            if (!v || (colours && !g_split_vp)) {
                // not uploaded (or no split program): whole vertices for this frame
                Vertex *t = vita2d_pool_memalign(mesh->nverts * sizeof(Vertex), sizeof(Vertex));
                if (!t) continue;
                for (int i = 0; i < mesh->nverts; i++) {
                    if (mesh->xy) { t[i].x = mesh->xy[i * 2]; t[i].y = mesh->xy[i * 2 + 1]; }
                    else { t[i].x = it->vp[i].x; t[i].y = it->vp[i].y; }
                    t[i].z = 0.5f;
                    if (colours) t[i].color = colours[i];
                    else memcpy(&t[i].color, mesh->rgba + i * 4, 4);
                }
                v = t;
                colours = NULL;
            }
            int want = colours ? 2 : 0;
            if (textured != want) {
                sceGxmSetVertexProgram(ctx, want == 2 ? g_split_vp : _vita2d_colorVertexProgram);
                sceGxmSetFragmentProgram(ctx, _vita2d_colorFragmentProgram);
                textured = want;
            }
            for (int off = 0; off < mesh->nverts; off += MAX_BATCH) {
                int count = mesh->nverts - off < MAX_BATCH ? mesh->nverts - off : MAX_BATCH;
                void *ub;
                sceGxmReserveVertexDefaultUniformBuffer(ctx, &ub);
                sceGxmSetUniformDataF(ub, _vita2d_colorWvpParam, 0, 16, wvp);
                sceGxmSetVertexStream(ctx, 0, v + off);
                if (colours) sceGxmSetVertexStream(ctx, 1, colours + off);
                sceGxmDraw(ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, g_indices, count);
            }
        }
    }
    set_stencil(SCE_GXM_STENCIL_FUNC_ALWAYS, 1, SCE_GXM_STENCIL_OP_KEEP);
}

static void draw_list(const DrawList *dl) { draw_list_range(dl, 0, dl->n); }

static vita2d_pgf *g_font;

// ---- heat haze --------------------------------------------------------------------------
// A clip the game gave AddShader("Heat") (lava world's backdrop) shimmers. Its own items go
// into a screen-sized texture first; then the screen gets what's before it, that texture in
// thin rows each shifted sideways by a moving wave, and the rest of the frame on top.
// Starts the screen's drawing either way.

static vita2d_texture *g_heat_rt;

static void draw_frame(const DrawList *vb, unsigned clock) {
    if (vb->heat_at < 0 || vb->heat_end <= vb->heat_at ||
        (!g_heat_rt && !(g_heat_rt = vita2d_create_empty_texture_rendertarget(SCREEN_W, SCREEN_H,
                                                                                SCE_GXM_TEXTURE_FORMAT_A8B8G8R8)))) {
        vita2d_start_drawing();
        vita2d_clear_screen();
        draw_list(vb);
        return;
    }
    unsigned clear = 0;
    vita2d_start_drawing_advanced(g_heat_rt, 0);
    vita2d_set_clear_color(RGBA8(0, 0, 0, 0));
    vita2d_clear_screen();
    draw_list_range(vb, vb->heat_at, vb->heat_end);
    vita2d_end_drawing();
    vita2d_wait_rendering_done();   // the texture is read by the next scene
    (void)clear;
    vita2d_start_drawing();
    vita2d_set_clear_color(RGBA8(0, 0, 0, 255));
    vita2d_clear_screen();
    draw_list_range(vb, 0, vb->heat_at);
    int x0 = vb->heat[0] < 0 ? 0 : (int)vb->heat[0], x1 = vb->heat[2] > SCREEN_W ? SCREEN_W : (int)vb->heat[2];
    int y0 = vb->heat[1] < 0 ? 0 : (int)vb->heat[1], y1 = vb->heat[3] > SCREEN_H ? SCREEN_H : (int)vb->heat[3];
    for (int y = y0; y < y1; y += 4) {
        float dx = 2.5f * sinf(clock * 0.21f + y * 0.09f) + 1.5f * sinf(clock * 0.13f + y * 0.023f);
        int h = y + 4 > y1 ? y1 - y : 4;
        vita2d_draw_texture_part(g_heat_rt, x0 + dx, y, x0, y, x1 - x0, h);
    }
    draw_list_range(vb, vb->heat_end, vb->n);
}

static int g_show_loads;   // the debug overlay (SELECT+UP) is on

static void show_message(const char *line1, const char *line2) {
    vita2d_set_clear_color(RGBA8(20, 20, 24, 255));
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_pgf_draw_text(g_font, 40, 240, RGBA8(240, 240, 240, 255), 1.0f, line1);
    if (line2) vita2d_pgf_draw_text(g_font, 40, 280, RGBA8(170, 170, 170, 255), 0.8f, line2);
    vita2d_end_drawing();
    vita2d_swap_buffers();
}


// ---- loading the game's files -------------------------------------------------------

// "../alien/alien.swf", "attract.pak" -> ux0:data/AlienHominidHD/game/alien.pak (or levels/).
// Each file is parsed and uploaded once and kept while it's in use.
#define MAX_MOVIES 96
static struct { char name[64]; Movie *m; GpuBlock gpu; } g_movies[MAX_MOVIES];
static int g_nmovies;
static char g_last_error[256];

static Movie *vita_loader(AsVm *vm, const char *url) {
    (void)vm;
    const char *base = strrchr(url, '/');
    base = base ? base + 1 : url;
    char name[64];
    snprintf(name, sizeof name, "%s", base);
    char *dot = strrchr(name, '.');
    if (dot) *dot = 0;
    for (char *p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
    strncat(name, ".pak", sizeof name - strlen(name) - 1);
    for (int i = 0; i < g_nmovies; i++)
        if (!strcmp(g_movies[i].name, name)) return g_movies[i].m;
    if (g_nmovies == MAX_MOVIES) return NULL;
    static const char *dirs[] = {"game", "levels"};
    for (int i = 0; i < 2; i++) {
        char path[256], err[256];
        snprintf(path, sizeof path, DATA_DIR "%s/%s", dirs[i], name);
        SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (fd < 0) continue;
        sceIoClose(fd);
        if (g_show_loads) {
            char msg[128];
            snprintf(msg, sizeof msg, "Loading %s...", name);
            show_message(msg, NULL);
        }
        Movie *m = swf_load(path, err, sizeof err);
        if (!m) {
            snprintf(g_last_error, sizeof g_last_error, "%s: %s", name, err);
            return NULL;
        }
        ah_patch_movie(path, m);
        for (int c = 0; c < 65536; c++) {   // bitmaps become textures
            if (m->chars[c].type != CHAR_BITMAP) continue;
            Bitmap *bm = m->chars[c].ptr;
            vita2d_texture *tex = vita2d_create_empty_texture_format(bm->w, bm->h, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
            if (!tex) continue;
            uint8_t *dst = vita2d_texture_get_datap(tex);
            unsigned stride = vita2d_texture_get_stride(tex);
            for (int y = 0; y < bm->h; y++) memcpy(dst + y * stride, bm->rgba + (size_t)y * bm->w * 4, (size_t)bm->w * 4);
            bm->tex = tex;
            free(bm->rgba);
            bm->rgba = NULL;
        }
        GpuBlock *blk = &g_movies[g_nmovies].gpu;
        blk->uid = -1;
        render_tessellate_all(m);
        if (render_upload(m, mesh_alloc, blk)) render_release_cpu(m);
        snprintf(g_movies[g_nmovies].name, sizeof g_movies[0].name, "%s", name);
        g_movies[g_nmovies++].m = m;
        return m;
    }
    snprintf(g_last_error, sizeof g_last_error, "missing %s", name);
    return NULL;
}

// Frees the files no clip or script function belongs to any more (levels left behind).
static int free_unused_movies(AsVm *vm, RecolourCache *cache, void *cache_mem, size_t cache_bytes) {
    Movie *ms[MAX_MOVIES];
    uint8_t used[MAX_MOVIES];
    for (int i = 0; i < g_nmovies; i++) ms[i] = g_movies[i].m;
    if (!as_movies_used(vm, ms, g_nmovies, used)) return 0;
    int unused = 0;
    for (int i = 0; i < g_nmovies; i++) unused += !used[i];
    if (!unused) return 0;
    vita2d_wait_rendering_done();
    recolour_cache_free(cache);
    recolour_cache_init(cache, cache_mem, cache_bytes);
    int n = 0;
    for (int i = 0; i < g_nmovies; i++) {
        if (used[i]) { g_movies[n++] = g_movies[i]; continue; }
        Movie *m = g_movies[i].m;
        for (int c = 0; c < 65536; c++)
            if (m->chars[c].type == CHAR_BITMAP && ((Bitmap *)m->chars[c].ptr)->tex)
                vita2d_free_texture(((Bitmap *)m->chars[c].ptr)->tex);
        as_forget_movie(m);
        swf_free(m);
        gpu_free(&g_movies[i].gpu);
    }
    g_nmovies = n;
    return unused;
}

// ---- controls -> the key codes the scripts read for port 1 ---------------------------
// In game (f_GetAlienButtons): arrows 37-40, shoot 65, jump 83, grenade 68, interact 87,
// roll left 81, roll right 69, start 49. Menus (pause.pak's pk1 clip): arrows, accept 97,
// back 106, start 49, select 53.

enum {
    K_LEFT = 37, K_UP = 38, K_RIGHT = 39, K_DOWN = 40, K_START = 49, K_SELECT = 53,
    K_SHOOT = 65, K_GRENADE = 68, K_ROLL_RIGHT = 69, K_ROLL_LEFT = 81, K_JUMP = 83, K_INTERACT = 87,
    K_ACCEPT = 97, K_BACK = 106,
};

#define STICK_DEAD 50   // of 128

static void map_pad(const SceCtrlData *pad) {
    memset(cc_keys, 0, sizeof cc_keys);
    unsigned b = pad->buttons;
    int lx = pad->lx - 128, ly = pad->ly - 128;
    if ((b & SCE_CTRL_LEFT) || lx <= -STICK_DEAD) cc_keys[K_LEFT] = 1;
    if ((b & SCE_CTRL_RIGHT) || lx >= STICK_DEAD) cc_keys[K_RIGHT] = 1;
    if ((b & SCE_CTRL_UP) || ly <= -STICK_DEAD) cc_keys[K_UP] = 1;
    if ((b & SCE_CTRL_DOWN) || ly >= STICK_DEAD) cc_keys[K_DOWN] = 1;
    if (b & SCE_CTRL_CROSS) cc_keys[K_JUMP] = cc_keys[K_ACCEPT] = 1;
    if (b & SCE_CTRL_SQUARE) cc_keys[K_SHOOT] = 1;
    if (b & SCE_CTRL_CIRCLE) cc_keys[K_GRENADE] = cc_keys[K_BACK] = 1;
    if (b & SCE_CTRL_TRIANGLE) cc_keys[K_INTERACT] = 1;
    if (b & SCE_CTRL_LTRIGGER) cc_keys[K_ROLL_LEFT] = 1;
    if (b & SCE_CTRL_RTRIGGER) cc_keys[K_ROLL_RIGHT] = 1;
    if (b & SCE_CTRL_START) cc_keys[K_START] = 1;
}

static void vita_warn(AsVm *vm, const char *msg) {
    (void)vm;
    snprintf(g_last_error, sizeof g_last_error, "%s", msg);
}

static void load_font(void) {
    char err[256];
    HostFont *font = hostfont_load(DATA_DIR "fonts/synjunicode_20.fnt", DATA_DIR "fonts/synjunicode_20_0.png", err,
                                   sizeof err);
    if (!font) {
        snprintf(g_last_error, sizeof g_last_error, "%s", err);
        return;
    }
    Bitmap *a = &font->atlas;
    vita2d_texture *tex = vita2d_create_empty_texture_format(a->w, a->h, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    if (!tex) return;
    uint8_t *dst = vita2d_texture_get_datap(tex);
    unsigned stride = vita2d_texture_get_stride(tex);
    for (int y = 0; y < a->h; y++) memcpy(dst + y * stride, a->rgba + (size_t)y * a->w * 4, (size_t)a->w * 4);
    a->tex = tex;
    as_render_set_font(font);
}

static void vita_sound_start(AsVm *vm, const char *linkage, float volume, float pan, int loops, const void *owner) {
    (void)vm;
    audio_sfx_play(linkage, volume, pan, loops, owner);
}

static void vita_sound_stop(AsVm *vm, const void *owner) {
    (void)vm;
    audio_sfx_stop(owner);
}

static void vita_sound_set(AsVm *vm, const void *owner, float volume, float pan) {
    (void)vm;
    audio_sfx_set(owner, volume, pan);
}

static void stop_all_sfx(void) { audio_sfx_stop(NULL); }

int main(void) {
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    vita2d_init_advanced_with_msaa(16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_4X);
    vita2d_set_vblank_wait(1);
    g_font = vita2d_load_default_pgf();
    make_split_program();
    g_indices = gpu_alloc(sizeof(uint16_t) * MAX_BATCH, &g_index_block);
    for (int i = 0; g_indices && i < MAX_BATCH; i++) g_indices[i] = i;
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    load_font();

    DrawList vb = {0};
    vb.view[2] = SCREEN_W;
    vb.view[3] = SCREEN_H;
    GpuBlock cache_gpu = {-1, NULL};
    const size_t cache_bytes = 16 * 1024 * 1024;
    RecolourCache cache;
    void *cache_mem = gpu_alloc(cache_bytes, &cache_gpu);
    recolour_cache_init(&cache, cache_mem, cache_bytes);
    vb.cache = &cache;

    AsVm *vm = as_new(vita_loader);
    vm->rand_state = (uint32_t)sceKernelGetProcessTimeWide() ^ 0x9E3779B9u;
    vm->warn = vita_warn;
    vm->sound_start = vita_sound_start;
    vm->sound_stop = vita_sound_stop;
    vm->sound_set = vita_sound_set;
    cc_host_init(vm);
    ah_host_init(vm);
    ah_load_strings(DATA_DIR "loc/en.dct");   // the menus' words (the SWF's copy is the fallback)
    // the PC copy's data/sounds and data/music; the game runs silent without them
    audio_init(DATA_DIR);
    cc_music_play = audio_music_play;
    cc_music_stop = audio_music_stop;
    ah_music_pause = audio_music_pause;
    ah_audio_volumes = audio_set_volumes;
    ah_sfx_stop_all = stop_all_sfx;
    as_render_init(vm);
    if (!ah_start(vm, "legal.pak")) {
        SceCtrlData pad;
        for (;;) {
            show_message(g_last_error, "Copy the game, levels and fonts folders from your own game's data folder to "
                                       DATA_DIR "   (START: quit)");
            sceCtrlPeekBufferPositive(0, &pad, 1);
            if (pad.buttons & SCE_CTRL_START) break;
        }
        vita2d_fini();
        sceKernelExitProcess(0);
        return 0;
    }

    SceCtrlData pad, old = {0};
    int overlay = 0;
    SceUInt64 last = sceKernelGetProcessTimeWide(), acc = 0, prev = last;
    float fps = 0, tick_ms = 0, build_ms = 0, shown_tick = 0, shown_build = 0;
    int frames = 0, ticks = 0;
    unsigned heat_clock = 0;
    uint32_t last_free = 0;
    const SceUInt64 tick_us = 1000000 / 30;   // the game's frame rate

    for (;;) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned pressed = pad.buttons & ~old.buttons;
        old = pad;
        int debug = pad.buttons & SCE_CTRL_SELECT;
        if (debug && (pressed & SCE_CTRL_START)) break;
        if (debug && (pressed & SCE_CTRL_UP)) g_show_loads = overlay = !overlay;
        if (debug) memset(cc_keys, 0, sizeof cc_keys);
        else map_pad(&pad);
        // START in a level opens the pause menu (the exe's job in the original); the menus
        // read START themselves
        if (!debug && (pressed & SCE_CTRL_START) && !vm->overlay) ah_request_pause(vm, 0);

        SceUInt64 now = sceKernelGetProcessTimeWide();
        acc += now - prev;
        prev = now;
        if (acc > 2 * tick_us) acc = 2 * tick_us;
        int ticked = 0;
        while (acc >= tick_us) {
            SceUInt64 t0 = sceKernelGetProcessTimeWide();
            ah_tick(vm);
            as_gc(vm);
            tick_ms += (sceKernelGetProcessTimeWide() - t0) / 1000.0f;
            ticks++;
            acc -= tick_us;
            ticked = 1;
        }

        const Movie *root = vm->root->movie;
        if (ticked) {
            if (vm->tick - last_free >= 90 && mallinfo().uordblks > (size_t)_newlib_heap_size_user / 2) {
                last_free = vm->tick;
                free_unused_movies(vm, &cache, cache_mem, cache_bytes);
            }
            if (cache.used > cache.cap * 9 / 10 || cache.entries * 4 >= cache.table_size * 3) {
                vita2d_wait_rendering_done();
                recolour_cache_free(&cache);
                recolour_cache_init(&cache, cache_mem, cache_bytes);
            }
            SceUInt64 t0 = sceKernelGetProcessTimeWide();
            float k = SCREEN_W / root->stage_w;
            Mat screen = {k / 20, 0, 0, k / 20, 0, 0};
            render_begin(&vb);
            as_render_mc(vm->root, screen, CX_IDENTITY, &vb);
            if (vm->overlay) {   // the menu/pause layer
                float ko = SCREEN_W / vm->overlay->movie->stage_w;
                Mat so = {ko / 20, 0, 0, ko / 20, 0, 0};
                as_render_mc(vm->overlay, so, CX_IDENTITY, &vb);
            }
            build_ms += (sceKernelGetProcessTimeWide() - t0) / 1000.0f;
        }

        vita2d_set_clear_color(RGBA8(root->bg[0], root->bg[1], root->bg[2], 255));
        draw_frame(&vb, heat_clock++);
        if (overlay) {
            unsigned ink = RGBA8(255, 255, 255, 255);
            vita2d_draw_rectangle(0, 0, SCREEN_W, 64, RGBA8(0, 0, 0, 150));
            vita2d_pgf_draw_textf(g_font, 12, 24, ink, 0.8f,
                                  "%.0f fps   script %.1f ms   build %.1f ms   %d tris  %d draws   heap %u objs",
                                  fps, shown_tick, shown_build, render_stats.triangles, render_stats.draws,
                                  (unsigned)vm->heap_count);
            vita2d_pgf_draw_textf(g_font, 12, 50, ink, 0.8f, "%d files, %.1f MB GPU, heap %.1f of %d MB   %s",
                                  g_nmovies, g_gpu_bytes / 1048576.0, mallinfo().uordblks / 1048576.0,
                                  _newlib_heap_size_user >> 20, g_last_error);
        }
        vita2d_end_drawing();
        vita2d_swap_buffers();

        frames++;
        now = sceKernelGetProcessTimeWide();
        if (now - last >= 1000000) {
            fps = frames * 1000000.0f / (now - last);
            shown_tick = ticks ? tick_ms / ticks : 0;
            shown_build = ticks ? build_ms / ticks : 0;
            tick_ms = build_ms = 0;
            frames = ticks = 0;
            last = now;
        }
    }

    audio_shutdown();
    vita2d_wait_rendering_done();
    as_free(vm);
    for (int i = 0; i < g_nmovies; i++) {
        for (int c = 0; c < 65536; c++)
            if (g_movies[i].m->chars[c].type == CHAR_BITMAP && ((Bitmap *)g_movies[i].m->chars[c].ptr)->tex)
                vita2d_free_texture(((Bitmap *)g_movies[i].m->chars[c].ptr)->tex);
        swf_free(g_movies[i].m);
        gpu_free(&g_movies[i].gpu);
    }
    recolour_cache_free(&cache);
    gpu_free(&cache_gpu);
    gpu_free(&g_index_block);
    vita2d_free_pgf(g_font);
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
