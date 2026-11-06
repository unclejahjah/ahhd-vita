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
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
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
#include "ah_online.h"
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

// "../alien/alien.swf", "HUD" -> "alien.pak", "hud.pak"
static void pak_name(const char *url, char *name, size_t size) {
    const char *base = strrchr(url, '/');
    base = base ? base + 1 : url;
    snprintf(name, size, "%s", base);
    char *dot = strrchr(name, '.');
    if (dot) *dot = 0;
    for (char *p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
    strncat(name, ".pak", size - strlen(name) - 1);
}

static int pak_path(const char *name, char *path, size_t size) {
    static const char *dirs[] = {"game", "levels"};
    for (int i = 0; i < 2; i++) {
        snprintf(path, size, DATA_DIR "%s/%s", dirs[i], name);
        SceIoStat st;
        if (sceIoGetstat(path, &st) >= 0) return 1;
    }
    return 0;
}

static Movie *cached_movie(const char *name) {
    for (int i = 0; i < g_nmovies; i++)
        if (!strcmp(g_movies[i].name, name)) return g_movies[i].m;
    return NULL;
}

// The part of loading a file that needs no GPU: read, parse, patch, tessellate. The
// background readers run it too, several files at once.
static SceUID g_patch_lock = -1;   // ah_patch_movie isn't safe to run twice at once
static Movie *read_movie(const char *path, char *err, size_t errlen) {
    Movie *m = swf_load(path, err, (int)errlen);
    if (!m) return NULL;
    if (g_patch_lock >= 0) sceKernelLockMutex(g_patch_lock, 1, NULL);
    ah_patch_movie(path, m);
    if (g_patch_lock >= 0) sceKernelUnlockMutex(g_patch_lock, 1);
    render_tessellate_all(m);
    return m;
}

// The part that does: bitmaps become textures, the meshes go to GPU memory.
static Movie *install_movie(const char *name, Movie *m) {
    if (g_nmovies == MAX_MOVIES) {
        swf_free(m);
        return NULL;
    }
    for (int c = 0; c < 65536; c++) {
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
    if (render_upload(m, mesh_alloc, blk)) render_release_cpu(m);
    snprintf(g_movies[g_nmovies].name, sizeof g_movies[0].name, "%s", name);
    g_movies[g_nmovies++].m = m;
    return m;
}

// ---- background reads, while the loading screen plays -----------------------------------
// Parsing and tessellating a level's files took 10 to 15 seconds on one core with the
// screen frozen. Readers on all three cores do it now while the loading screen animates;
// the main thread only installs what they finish.

#define MAX_JOBS 16
#define NREADERS 3
enum { JOB_FREE, JOB_QUEUED, JOB_READING, JOB_READ };
static struct { char name[64], path[256]; Movie *m; volatile int state; } g_jobs[MAX_JOBS];
static SceUID g_jobs_lock = -1, g_jobs_sema = -1;

static int reader_thread(SceSize args, void *argp) {
    (void)args, (void)argp;
    for (;;) {
        sceKernelWaitSema(g_jobs_sema, 1, NULL);
        sceKernelLockMutex(g_jobs_lock, 1, NULL);
        int j = -1;
        for (int i = 0; i < MAX_JOBS && j < 0; i++)
            if (g_jobs[i].state == JOB_QUEUED) j = i;
        if (j >= 0) g_jobs[j].state = JOB_READING;
        sceKernelUnlockMutex(g_jobs_lock, 1);
        if (j < 0) continue;
        char err[256];
        g_jobs[j].m = read_movie(g_jobs[j].path, err, sizeof err);   // NULL: the loader reports it later
        __atomic_store_n(&g_jobs[j].state, JOB_READ, __ATOMIC_RELEASE);
    }
    return 0;
}

static void readers_start(void) {
    g_patch_lock = sceKernelCreateMutex("ah_patch", 0, 0, NULL);
    g_jobs_lock = sceKernelCreateMutex("ah_jobs", 0, 0, NULL);
    g_jobs_sema = sceKernelCreateSema("ah_jobs", 0, 0, MAX_JOBS, NULL);
    static const int cores[NREADERS] = {SCE_KERNEL_CPU_MASK_USER_1, SCE_KERNEL_CPU_MASK_USER_2, SCE_KERNEL_CPU_MASK_USER_0};
    for (int i = 0; i < NREADERS; i++) {
        // below the main thread, so the loading screen keeps its frame rate
        SceUID t = sceKernelCreateThread("ah_reader", reader_thread, 0x10000100 + 20, 0x40000, 0, cores[i], NULL);
        if (t >= 0) sceKernelStartThread(t, 0, NULL);
    }
}

// the main thread takes job j's movie into the cache
static Movie *finish_job(int j) {
    Movie *m = g_jobs[j].m ? install_movie(g_jobs[j].name, g_jobs[j].m) : NULL;
    g_jobs[j].m = NULL;
    g_jobs[j].state = JOB_FREE;
    return m;
}

static void vita_prefetch(const char *const *files, int n) {
    if (g_jobs_sema < 0) return;
    for (int f = 0; f < n; f++) {
        char name[64], path[256];
        pak_name(files[f], name, sizeof name);
        if (cached_movie(name)) continue;
        int j = -1, dup = 0;
        for (int i = 0; i < MAX_JOBS; i++) {
            if (g_jobs[i].state != JOB_FREE && !strcmp(g_jobs[i].name, name)) dup = 1;
            if (g_jobs[i].state == JOB_FREE && j < 0) j = i;
        }
        if (dup || j < 0 || !pak_path(name, path, sizeof path)) continue;
        snprintf(g_jobs[j].name, sizeof g_jobs[j].name, "%s", name);
        snprintf(g_jobs[j].path, sizeof g_jobs[j].path, "%s", path);
        g_jobs[j].m = NULL;
        sceKernelLockMutex(g_jobs_lock, 1, NULL);
        g_jobs[j].state = JOB_QUEUED;
        sceKernelUnlockMutex(g_jobs_lock, 1);
        sceKernelSignalSema(g_jobs_sema, 1);
    }
}

// installs the reads that are done (one per call, to spread the uploads over frames) and
// says how many are still going
static int vita_prefetch_pending(void) {
    int pending = 0, installed = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        int st = __atomic_load_n(&g_jobs[i].state, __ATOMIC_ACQUIRE);
        if (st == JOB_READ && !installed) { finish_job(i); installed = 1; continue; }
        if (st != JOB_FREE) pending++;
    }
    return pending;
}

static Movie *vita_loader(AsVm *vm, const char *url) {
    (void)vm;
    char name[64];
    pak_name(url, name, sizeof name);
    Movie *m = cached_movie(name);
    if (m) return m;
    for (int i = 0; i < MAX_JOBS; i++) {   // being read in the background: wait for it
        if (g_jobs[i].state == JOB_FREE || strcmp(g_jobs[i].name, name)) continue;
        while (__atomic_load_n(&g_jobs[i].state, __ATOMIC_ACQUIRE) != JOB_READ) sceKernelDelayThread(1000);
        if ((m = finish_job(i))) return m;
        break;   // it failed there: load it here, to report why
    }
    if (g_nmovies == MAX_MOVIES) return NULL;
    char path[256], err[256];
    if (!pak_path(name, path, sizeof path)) {
        snprintf(g_last_error, sizeof g_last_error, "missing %s", name);
        return NULL;
    }
    if (g_show_loads) {
        char msg[128];
        snprintf(msg, sizeof msg, "Loading %s...", name);
        show_message(msg, NULL);
    }
    m = read_movie(path, err, sizeof err);
    if (!m) {
        snprintf(g_last_error, sizeof g_last_error, "%s: %s", name, err);
        return NULL;
    }
    return install_movie(name, m);
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

// ---- controls -> the key codes the scripts read ------------------------------------
// The scripts read every port through Key.isDown, each port with its own codes.
// Port 1, in game (f_GetAlienButtons): arrows 37-40, shoot 65, jump 83, grenade 68,
// interact 87, roll left 81, roll right 69, start 49. Menus (pause.pak's pk1 clip): arrows,
// accept 97, back 106, start 49, select 53.
// Port 2, in game: left 74, up 73, right 76, down 75, shoot 70, jump 71, grenade 72,
// interact 84, roll left 82, roll right 89, start 50. Menus (pk2): accept 99, back 108,
// select 54. 54 is also one of f_ScreenSize's debug keys (it changes player 1's hat), so it
// is only sent while a menu layer is open.
// On a PS TV the second controller (sceCtrl port 2, or the next paired one) is player 2.

enum {
    K_LEFT = 37, K_UP = 38, K_RIGHT = 39, K_DOWN = 40, K_START = 49, K_SELECT = 53,
    K_SHOOT = 65, K_GRENADE = 68, K_ROLL_RIGHT = 69, K_ROLL_LEFT = 81, K_JUMP = 83, K_INTERACT = 87,
    K_ACCEPT = 97, K_BACK = 106,
};

typedef struct {
    uint8_t left, up, right, down, shoot, jump, grenade, interact, roll_left, roll_right, start;
    uint8_t accept, back, select;
} PortKeys;

static const PortKeys PORT_KEYS[2] = {
    {K_LEFT, K_UP, K_RIGHT, K_DOWN, K_SHOOT, K_JUMP, K_GRENADE, K_INTERACT, K_ROLL_LEFT, K_ROLL_RIGHT, K_START,
     K_ACCEPT, K_BACK, K_SELECT},
    {74, 73, 76, 75, 70, 71, 72, 84, 82, 89, 50, 99, 108, 54},
};

#define STICK_DEAD 50   // of 128

// menu: a menu layer is open or no level is running (player 2's d-pad then also sends the
// arrow keys, which the menus share). select_ok: a menu layer is open, the only time
// player 2's Select (54) is safe (neutron and pinata aren't levels but run f_ScreenSize)
static void map_pad(const SceCtrlData *pad, int port, int menu, int select_ok) {
    const PortKeys *k = &PORT_KEYS[port];
    unsigned b = pad->buttons;
    int lx = pad->lx - 128, ly = pad->ly - 128;
    int left = (b & SCE_CTRL_LEFT) || lx <= -STICK_DEAD, right = (b & SCE_CTRL_RIGHT) || lx >= STICK_DEAD;
    int up = (b & SCE_CTRL_UP) || ly <= -STICK_DEAD, down = (b & SCE_CTRL_DOWN) || ly >= STICK_DEAD;
    if (left) cc_keys[k->left] = 1;
    if (right) cc_keys[k->right] = 1;
    if (up) cc_keys[k->up] = 1;
    if (down) cc_keys[k->down] = 1;
    if (port && menu) {
        if (left) cc_keys[K_LEFT] = 1;
        if (right) cc_keys[K_RIGHT] = 1;
        if (up) cc_keys[K_UP] = 1;
        if (down) cc_keys[K_DOWN] = 1;
    }
    if (b & SCE_CTRL_CROSS) cc_keys[k->jump] = cc_keys[k->accept] = 1;
    if (b & SCE_CTRL_SQUARE) cc_keys[k->shoot] = 1;
    if (b & SCE_CTRL_CIRCLE) cc_keys[k->grenade] = cc_keys[k->back] = 1;
    if (b & SCE_CTRL_TRIANGLE) cc_keys[k->interact] = 1;
    if (b & SCE_CTRL_LTRIGGER) cc_keys[k->roll_left] = 1;
    if (b & SCE_CTRL_RTRIGGER) cc_keys[k->roll_right] = 1;
    if (b & SCE_CTRL_START) cc_keys[k->start] = 1;
    if (port && select_ok && (b & SCE_CTRL_SELECT)) cc_keys[k->select] = 1;
}

// The controllers: on a PS TV they are sceCtrl ports 1-4, player 1 on port 1 and player 2
// on the next paired port. A handheld Vita (or a PS TV with one controller) has only
// player 1, on port 0 as before; player 2 then reads all zeros.
static void pad_ports(int *p1, int *p2) {
    static SceUInt64 checked;
    static int port1, port2;
    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (!checked || now - checked >= 1000000) {   // once a second: controllers come and go
        checked = now;
        SceCtrlPortInfo info;
        port1 = port2 = 0;
        if (sceCtrlGetControllerPortInfo(&info) >= 0) {
            if (info.port[1] != SCE_CTRL_TYPE_UNPAIRED) port1 = 1;
            for (int p = 2; p <= 4 && !port2; p++)
                if (info.port[p] != SCE_CTRL_TYPE_UNPAIRED) port2 = p;
        }
    }
    *p1 = port1;
    *p2 = port2;
}

static void read_pad(int port, SceCtrlData *pad) {
    int r = port ? sceCtrlPeekBufferPositiveExt2(port, pad, 1) : sceCtrlPeekBufferPositive(0, pad, 1);
    if (r < 0) {
        memset(pad, 0, sizeof *pad);
        pad->lx = pad->ly = pad->rx = pad->ry = 128;
    } else if (port) {
        // Ext2 reports L/R as L1/R1, not the LTRIGGER/RTRIGGER bits map_pad tests
        if (pad->buttons & SCE_CTRL_L1) pad->buttons |= SCE_CTRL_LTRIGGER;
        if (pad->buttons & SCE_CTRL_R1) pad->buttons |= SCE_CTRL_RTRIGGER;
    }
}

static void read_pads(SceCtrlData *pad1, SceCtrlData *pad2) {
    int p1, p2;
    pad_ports(&p1, &p2);
    read_pad(p1, pad1);
    if (p2) read_pad(p2, pad2);
    else {
        memset(pad2, 0, sizeof *pad2);
        pad2->lx = pad2->ly = pad2->rx = pad2->ry = 128;
    }
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
    snprintf(ah_data_dir, sizeof ah_data_dir, "%s", DATA_DIR);
    ah_load_strings(DATA_DIR "loc/en.dct");   // the menus' words (the SWF's copy is the fallback)
    // the PC copy's data/sounds and data/music; the game runs silent without them
    audio_init(DATA_DIR);
    cc_music_play = audio_music_play;
    cc_music_stop = audio_music_stop;
    ah_music_pause = audio_music_pause;
    ah_audio_volumes = audio_set_volumes;
    ah_sfx_stop_all = stop_all_sfx;
    ah_sfx_hold = audio_sfx_hold;
    ah_online_init(DATA_DIR);   // the online leaderboard, when this build has a server
    readers_start();
    ah_prefetch = vita_prefetch;
    ah_prefetch_pending = vita_prefetch_pending;
    ah_save_load(DATA_DIR "save.dat");   // unlocks, Load Level, options and volumes
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

    SceCtrlData pad, old = {0}, pad2, old2 = {0};
    int overlay = 0;
    SceUInt64 last = sceKernelGetProcessTimeWide(), acc = 0, prev = last;
    float fps = 0, tick_ms = 0, build_ms = 0, shown_tick = 0, shown_build = 0;
    int frames = 0, ticks = 0;
    unsigned heat_clock = 0;
    uint32_t last_free = 0;
    const SceUInt64 tick_us = 1000000 / 30;   // the game's frame rate

    for (;;) {
        read_pads(&pad, &pad2);
        unsigned pressed = pad.buttons & ~old.buttons, pressed2 = pad2.buttons & ~old2.buttons;
        old = pad;
        old2 = pad2;
        int debug = pad.buttons & SCE_CTRL_SELECT;
        if (debug && (pressed & SCE_CTRL_START)) break;
        if (debug && (pressed & SCE_CTRL_UP)) g_show_loads = overlay = !overlay;
        int level = ah_in_level() && !vm->overlay;
        memset(cc_keys, 0, sizeof cc_keys);
        if (!debug) map_pad(&pad, 0, !level, 0);
        // player 2's d-pad works the shared menu arrows, except in player 1's pause menu
        int p1_paused = ah_paused && as_tonum(vm, cc_flash_global(vm, "g_nPausePlayer")) == 0;
        map_pad(&pad2, 1, !level && !p1_paused, vm->overlay != NULL);   // 54 only in a menu layer
        // In a level START is the exe's: on a port whose alien is playing it opens the pause
        // menu for that port; on a port whose HUD is waiting (player 2's "press start" to
        // join, or a continue after the last life) it goes to the scripts. SELECT alone is also
        // the start key there, as the HUD's prompt shows it (for player 1, Select with Start or
        // Up stays the debug combos). Before the scripts say which ports play, player 1's
        // START pauses and player 2's goes to the scripts.
        if (level) {
            int p1 = ah_port_active(vm, 0), p2 = ah_port_active(vm, 1);
            cc_keys[K_START] = 0;
            if (debug && !(pad.buttons & (SCE_CTRL_START | SCE_CTRL_UP))) cc_keys[K_START] = 1;
            if (p1 == 0 && !debug && (pad.buttons & SCE_CTRL_START)) cc_keys[K_START] = 1;
            if (p2 == 1) cc_keys[PORT_KEYS[1].start] = 0;
            // the HUD shows the start key as SELECT in a level (GetInputBinding), so player 2's
            // SELECT joins (or continues) as well while their alien waits
            else if (pad2.buttons & SCE_CTRL_SELECT) cc_keys[PORT_KEYS[1].start] = 1;
            if (p1 != 0 && !debug && (pressed & SCE_CTRL_START)) ah_request_pause(vm, 0);
            else if (p2 == 1 && (pressed2 & SCE_CTRL_START)) ah_request_pause(vm, 1);
        } else if (!debug && (pressed & SCE_CTRL_START) && !vm->overlay) {
            ah_request_pause(vm, 0);   // the menus read START themselves; this is for the rest
        }

        SceUInt64 now = sceKernelGetProcessTimeWide();
        if (now - prev > 1000000) audio_resumed();   // a second without a frame: the app was suspended
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
            if (ah_loading) break;   // each loading-screen tick is shown before the next (one may load the level)
        }
        if (ticked && vm->tick % 60 == 0) ah_save_tick();   // written within two seconds of a change

        const Movie *root = vm->root->movie;
        if (ticked) {
            // not under the loading screen: the files read for the level aren't in use yet
            if (!ah_loading && vm->tick - last_free >= 90 && mallinfo().uordblks > (size_t)_newlib_heap_size_user / 2) {
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
            if (ah_loading) {   // the loading screen covers everything
                float kl = SCREEN_W / ah_loading->movie->stage_w;
                Mat sl = {kl / 20, 0, 0, kl / 20, 0, 0};
                as_render_mc(ah_loading, sl, CX_IDENTITY, &vb);
            } else {
                as_render_mc(vm->root, screen, CX_IDENTITY, &vb);
                if (ah_unlock_layer) {   // unlock banners
                    float ku = SCREEN_W / ah_unlock_layer->movie->stage_w;
                    Mat su = {ku / 20, 0, 0, ku / 20, 0, 0};
                    as_render_mc(ah_unlock_layer, su, CX_IDENTITY, &vb);
                }
            }
            if (vm->overlay && !ah_loading) {   // the menu/pause layer
                float ko = SCREEN_W / vm->overlay->movie->stage_w;
                Mat so = {ko / 20, 0, 0, ko / 20, 0, 0};
                as_render_mc(vm->overlay, so, CX_IDENTITY, &vb);
            }
            build_ms += (sceKernelGetProcessTimeWide() - t0) / 1000.0f;
        }

        // a frame with nothing on it between two movies (the title clears itself the tick
        // before it loads the menu) would flash the stage colour: keep the last picture up
        // instead, for a few frames (about 100 ms) at most
        static int presented, held;
        if (presented && vb.n == 0 && !overlay && held < 6) {
            held++;
            sceKernelDelayThread(16667);   // what waiting for the swap would have taken
            continue;
        }
        held = 0;
        presented = 1;
        const Movie *shown = ah_loading ? ah_loading->movie : root;
        vita2d_set_clear_color(RGBA8(shown->bg[0], shown->bg[1], shown->bg[2], 255));
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

    ah_save_tick();
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
