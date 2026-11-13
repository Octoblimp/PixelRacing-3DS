/* OpenGL ES 1.1 subset on citro3d -- see gles.h.
 *
 * Design: GL calls update a state block; glDrawArrays snapshots that state into a draw packet
 * (vertex data copied to a per-frame linear ring unless it already lives in linear memory).
 * gfxGL_EndFrame replays the packets onto the top screen -- twice, with a per-eye projection
 * skew, when the 3D slider is up -- and onto the bottom screen. Recording makes stereo and
 * mid-frame clears (drawn as full-screen quads) straightforward.
 *
 * Coordinate conventions: the game works in GL window space for a 400x240 (top) or 320x240
 * (bottom) screen. The 3DS framebuffers are rotated 90 degrees and the PICA clips z to
 * [-w, 0], so every projection is premultiplied by
 *   tilt  : x' = y, y' = -x
 *   zfix  : z' = -0.5 z - 0.5 w     (0 at the near plane, -w at the far plane)
 * and the depth value is 0 at the near plane and 1 at the far plane, as in GL, so depth
 * comparisons are GL's own and the depth buffer is cleared to 1 (see fill_common for why
 * near has to be the 0 end). Texture t is flipped
 * (t' = 1 - t) because the game's textures have t = 0 at the image top (CGImage upload order)
 * while tex3ds stores images with t = 0 at the bottom. */
#include <3ds.h>
#include <citro3d.h>
#include <tex3ds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gles.h"
#include "gl_shbin.h"
#include "sys.h"

#define MAX_TEXTURES 512
#define MAX_PACKETS 2048
#define RING_SIZE (3u << 20) /* per frame; two frames in flight */

#define DISPLAY_TRANSFER_FLAGS                                                                    \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |              \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |  \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct {
    bool used, has_data;
    bool transposed; /* framebuffer copies: rotated relative to GL */
    C3D_Tex tex;
    int w, h;
    GPU_TEXTURE_FILTER_PARAM min, mag;
    GPU_TEXTURE_WRAP_PARAM ws, wt;
} GLTex;

typedef struct {
    bool enabled;
    GLint size;
    GLenum type;
    GLsizei stride;
    const void *ptr;
} GLArray;

enum { PK_DRAW, PK_CLEAR, PK_SCREEN };

typedef struct {
    u8 kind;
    u8 screen;
    /* draw */
    GPU_Primitive_t prim;
    u16 count;
    u8 nattr;
    u8 attr_fmt[3], attr_size[3];
    u32 buf_addr; /* linear address of interleaved or single buffer */
    u16 stride;
    bool interleaved_direct; /* buffer holds all enabled attribs with their own offsets */
    bool game_vb;            /* buf_addr points at the game's own vertex data (not the ring) */
    bool batched;            /* small unlit draw, modelview applied on the CPU: see batch_draw */
    u32 attr_addr[3];        /* per-attribute addresses when not interleaved */
    u16 attr_stride[3];
    float fixed_nrm[3], fixed_tc[2];
    /* matrices: row-major 4x4 for uniforms */
    C3D_Mtx proj, mv, tm;
    bool persp;
    bool wbuf;    /* perspective draw with linear (W-buffer) depth, see fill_common */
    float wscale; /* GPUREG_DEPTHMAP_SCALE for the W-buffer: -1 / far */
    C3D_FVec color, lpos, lamb, ldif, lflag, tcflip;
    /* state */
    GLuint tex;
    bool texturing, blend, depth_test, depth_write, cull, scissor, fog;
    GPU_TESTFUNC depth_func;
    GPU_BLENDFACTOR src, dst;
    GPU_CULLMODE cull_mode;
    s16 vp[4], sc[4];
    float fog_start, fog_end;
    u32 fog_color;
    /* clear */
    u32 clear_color;
    u16 clear_mask;
} Packet;

static C3D_RenderTarget *s_top_l, *s_top_r, *s_bot;
static DVLB_s *s_dvlb;
static shaderProgram_s s_prog;
static int u_proj, u_mv, u_tm, u_color, u_lpos, u_lamb, u_ldif, u_lflag, u_tcflip, u_fog;

static GLTex s_tex[MAX_TEXTURES];
static GLuint s_bound;
static GLArray s_va, s_na, s_ta;
static GLenum s_matmode = GL_MODELVIEW;
static float s_m[3][16]; /* modelview, projection, texture (GL column-major) */
static float s_color[4] = {1, 1, 1, 1};
static float s_clear[4];
static bool s_lighting, s_light0, s_colormat, s_texturing, s_blend, s_depth_test, s_cull,
    s_scissor, s_fog;
static bool s_depth_write = true;
static GLenum s_depth_func = GL_LESS, s_cull_face = GL_BACK;
static GLenum s_bsrc = GL_ONE, s_bdst = GL_ZERO;
static float s_light_amb[4] = {0, 0, 0, 1}, s_light_dif[4] = {1, 1, 1, 1},
             s_light_pos[4] = {0, 0, 1, 0};
static float s_model_amb[4] = {0.2f, 0.2f, 0.2f, 1};
static float s_fog_start = 0, s_fog_end = 1, s_fog_color[4];
static GLint s_vp[4] = {0, 0, 400, 240}, s_sc[4] = {0, 0, 400, 240};
static GfxScreen s_screen = GFX_SCREEN_TOP;
static float s_iod;

static Packet *s_pk, *s_pkbuf[3];
static int s_npk, s_last_draws;
static int s_frame;
static float s_cmd_peak;
static u8 *s_ring[3];
static u32 s_ring_off;
static int s_ring_idx;

/* ------------------------------------------------------------------ helpers */

static inline bool is_linear(const void *p) {
    u32 a = (u32)p;
    return (a >= 0x14000000 && a < 0x1C000000) || (a >= 0x30000000 && a < 0x40000000);
}

void *gfxGL_LinearAlloc(uint32_t size) { return linearMemAlign(size, 0x80); }
void gfxGL_LinearFree(void *p) { if (p) linearFree(p); }
void gfxGL_Flush(const void *p, uint32_t size) { GSPGPU_FlushDataCache(p, size); }
int gfxGL_LastDrawCount(void) { return s_last_draws; }
static float s_wait_ms, s_build_ms, s_sync_ms;
void gfxGL_FrameTimes(float *wait_ms, float *build_ms, float *gpu_ms, float *sync_ms) {
    *wait_ms = s_wait_ms;
    *build_ms = s_build_ms;
    *gpu_ms = C3D_GetDrawingTime();
    *sync_ms = s_sync_ms;
}

/* ------------------------------------------------------------------ textures in VRAM
 *
 * The hardware log (build 10, New 3DS) shows the GPU's time per frame hardly depends on the
 * number of draws (63 draws took as long as 240): it goes on the pixels, and drawing the top
 * screen a second time for 3D doubled it. Every texture was in main memory (FCRAM), which the
 * GPU reads far slower than its own VRAM. So .t3x textures move into VRAM while it has room
 * (about 3.8 MB after the render targets); the rest stay in main memory.
 *
 * The move: the texture is loaded into main memory as before, a VRAM texture of the same shape
 * replaces it straight away, and the render thread copies the data across before it starts
 * the next frame, with citro3d's C3D_SyncTextureCopy (the CPU never writes VRAM, and only the
 * render thread touches citro3d). Not inside the frame: a race loads dozens of textures at
 * once, more than citro3d's GX queue holds (it panics when full). The main-memory copy is
 * freed by the main thread afterwards. */
#define VRAM_RESERVE (64 * 1024)
#define MAX_VJOBS 128
enum { VJ_FREE, VJ_QUEUED, VJ_DONE };
typedef struct {
    void *staging, *dst;
    u32 size;
    volatile int state;
} VramJob;
static VramJob s_vjob[MAX_VJOBS];
static LightLock s_vlock;

static inline bool is_vram(const void *p) {
    u32 a = (u32)p;
    return a >= 0x1F000000 && a < 0x1F600000;
}

/* Main thread: frees the main-memory copies of finished moves. Call with s_vlock held. */
static void vram_reap(void) {
    for (int i = 0; i < MAX_VJOBS; i++)
        if (s_vjob[i].state == VJ_DONE) {
            linearFree(s_vjob[i].staging);
            s_vjob[i].state = VJ_FREE;
        }
}

/* Main thread: moves a freshly loaded main-memory texture to VRAM if it fits. */
static void tex_to_vram(C3D_Tex *tex) {
    u32 size = tex->size;
    if (!tex->data || is_vram(tex->data) || vramSpaceFree() < size + VRAM_RESERVE) return;
    C3D_Tex v;
    C3D_TexInitParams ip = {tex->width, tex->height, tex->maxLevel, tex->fmt, GPU_TEX_2D, true};
    if (!C3D_TexInitWithParams(&v, NULL, ip)) return;
    if (v.size != size) {
        C3D_TexDelete(&v);
        return;
    }
    LightLock_Lock(&s_vlock);
    vram_reap();
    int slot = -1;
    for (int i = 0; i < MAX_VJOBS && slot < 0; i++)
        if (s_vjob[i].state == VJ_FREE) slot = i;
    if (slot < 0) {
        LightLock_Unlock(&s_vlock);
        C3D_TexDelete(&v);
        return;
    }
    GSPGPU_FlushDataCache(tex->data, size);
    s_vjob[slot].staging = tex->data;
    s_vjob[slot].dst = v.data;
    s_vjob[slot].size = size;
    s_vjob[slot].state = VJ_QUEUED;
    LightLock_Unlock(&s_vlock);
    v.param = tex->param;
    v.border = tex->border;
    v.lodParam = tex->lodParam;
    *tex = v; /* the main-memory copy now belongs to the job */
}

/* Main thread: deletes a texture; a move into it that hasn't been issued yet is dropped.
 * First the render thread finishes the frame it is building: that frame may draw this
 * texture, and with the texture gone (or being reloaded, like the help pages each turn) it
 * drew untextured: a white page that stayed up while the next picture loaded. */
static void tex_delete(C3D_Tex *tex) {
    gfxGL_WaitIdle();
    if (is_vram(tex->data)) {
        LightLock_Lock(&s_vlock);
        for (int i = 0; i < MAX_VJOBS; i++)
            if (s_vjob[i].state == VJ_QUEUED && s_vjob[i].dst == tex->data)
                s_vjob[i].state = VJ_DONE;
        LightLock_Unlock(&s_vlock);
    }
    C3D_TexDelete(tex);
}

/* Render thread, before C3D_FrameBegin: C3D_SyncTextureCopy waits for the previous frame's
 * GPU work, then copies and waits for the copy, so the data is there before any draw of the
 * frame about to be built. */
static void vram_jobs_run(void) {
    LightLock_Lock(&s_vlock);
    for (int i = 0; i < MAX_VJOBS; i++)
        if (s_vjob[i].state == VJ_QUEUED) {
            C3D_SyncTextureCopy((u32 *)s_vjob[i].staging, 0, (u32 *)s_vjob[i].dst, 0,
                                s_vjob[i].size, 8);
            s_vjob[i].state = VJ_DONE;
        }
    LightLock_Unlock(&s_vlock);
}

void gfxGL_TexStats(uint32_t *vram_kb, uint32_t *main_kb, uint32_t *vram_free_kb) {
    u32 v = 0, m = 0;
    for (int i = 0; i < MAX_TEXTURES; i++)
        if (s_tex[i].used && s_tex[i].has_data) {
            if (is_vram(s_tex[i].tex.data)) v += s_tex[i].tex.size;
            else m += s_tex[i].tex.size;
        }
    *vram_kb = v / 1024;
    *main_kb = m / 1024;
    *vram_free_kb = vramSpaceFree() / 1024;
}

/* The start is 16-byte aligned and the size is not rounded, so batch_draw can append to the
 * last allocation with ring_append. */
static void *ring_alloc(u32 size) {
    u32 off = (s_ring_off + 15) & ~15u;
    if (off + size > RING_SIZE) return NULL;
    s_ring_off = off + size;
    return s_ring[s_ring_idx] + off;
}

static void *ring_append(u32 size) {
    if (s_ring_off + size > RING_SIZE) return NULL;
    void *p = s_ring[s_ring_idx] + s_ring_off;
    s_ring_off += size;
    return p;
}

static void mtx_identity(float *m) {
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1;
}

/* r = a * b, GL column-major */
static void mtx_mul(float *r, const float *a, const float *b) {
    float t[16];
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++)
            t[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1] +
                            a[2 * 4 + rr] * b[c * 4 + 2] + a[3 * 4 + rr] * b[c * 4 + 3];
    memcpy(r, t, 64);
}

static void to_c3d(C3D_Mtx *o, const float *m) {
    for (int r = 0; r < 4; r++) {
        o->r[r].x = m[0 * 4 + r];
        o->r[r].y = m[1 * 4 + r];
        o->r[r].z = m[2 * 4 + r];
        o->r[r].w = m[3 * 4 + r];
    }
}

static float *cur_mtx(void) {
    switch (s_matmode) {
    case GL_PROJECTION: return s_m[1];
    case GL_TEXTURE: return s_m[2];
    default: return s_m[0];
    }
}

static GPU_TESTFUNC depth_func(GLenum f) {
    switch (f) { /* depth is 0 near, 1 far, as in GL */
    case GL_NEVER: return GPU_NEVER;
    case GL_LESS: return GPU_LESS;
    case GL_EQUAL: return GPU_EQUAL;
    case GL_LEQUAL: return GPU_LEQUAL;
    case GL_GREATER: return GPU_GREATER;
    case GL_NOTEQUAL: return GPU_NOTEQUAL;
    case GL_GEQUAL: return GPU_GEQUAL;
    default: return GPU_ALWAYS;
    }
}

static GPU_BLENDFACTOR blend_factor(GLenum f) {
    switch (f) {
    case GL_ZERO: return GPU_ZERO;
    case GL_ONE: return GPU_ONE;
    case GL_SRC_COLOR: return GPU_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR: return GPU_ONE_MINUS_SRC_COLOR;
    case GL_SRC_ALPHA: return GPU_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return GPU_ONE_MINUS_SRC_ALPHA;
    case GL_DST_ALPHA: return GPU_DST_ALPHA;
    case GL_ONE_MINUS_DST_ALPHA: return GPU_ONE_MINUS_DST_ALPHA;
    case GL_DST_COLOR: return GPU_DST_COLOR;
    case GL_ONE_MINUS_DST_COLOR: return GPU_ONE_MINUS_DST_COLOR;
    case GL_SRC_ALPHA_SATURATE: return GPU_SRC_ALPHA_SATURATE;
    default: return GPU_ONE;
    }
}

static int type_size(GLenum t) {
    switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    default: return 4;
    }
}

static GPU_FORMATS type_fmt(GLenum t) {
    switch (t) {
    case GL_BYTE: return GPU_BYTE;
    case GL_UNSIGNED_BYTE: return GPU_UNSIGNED_BYTE;
    case GL_SHORT: return GPU_SHORT;
    default: return GPU_FLOAT;
    }
}

static float read_comp(const u8 *p, GLenum t) {
    switch (t) {
    case GL_BYTE: return *(const s8 *)p;
    case GL_UNSIGNED_BYTE: return *p;
    case GL_SHORT: return *(const s16 *)p;
    case GL_UNSIGNED_SHORT: return *(const u16 *)p;
    case GL_FIXED: return *(const s32 *)p / 65536.0f;
    default: return *(const float *)p;
    }
}

static inline int clamp255(float v) {
    int i = (int)(v * 255.0f + 0.5f);
    return i < 0 ? 0 : i > 255 ? 255 : i;
}

static u32 rgba8(const float *c) {
    return (u32)clamp255(c[0]) | (u32)clamp255(c[1]) << 8 | (u32)clamp255(c[2]) << 16 |
           (u32)clamp255(c[3]) << 24;
}

/* ------------------------------------------------------------------ init / frame */

static void fog_init(void);
static void render_thread_start(void);
static void render_thread_stop(void);

void gfxGL_Init(void) {
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4);
    LightLock_Init(&s_vlock);
    s_top_l = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    s_top_r = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    s_bot = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(s_top_l, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    C3D_RenderTargetSetOutput(s_top_r, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);
    C3D_RenderTargetSetOutput(s_bot, GFX_BOTTOM, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

    s_dvlb = DVLB_ParseFile((u32 *)gl_shbin, gl_shbin_size);
    shaderProgramInit(&s_prog);
    shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
    u_proj = shaderInstanceGetUniformLocation(s_prog.vertexShader, "projection");
    u_mv = shaderInstanceGetUniformLocation(s_prog.vertexShader, "modelView");
    u_tm = shaderInstanceGetUniformLocation(s_prog.vertexShader, "texMtx");
    u_color = shaderInstanceGetUniformLocation(s_prog.vertexShader, "curColor");
    u_lpos = shaderInstanceGetUniformLocation(s_prog.vertexShader, "lightPos");
    u_lamb = shaderInstanceGetUniformLocation(s_prog.vertexShader, "lightAmb");
    u_ldif = shaderInstanceGetUniformLocation(s_prog.vertexShader, "lightDif");
    u_lflag = shaderInstanceGetUniformLocation(s_prog.vertexShader, "lightFlag");
    u_tcflip = shaderInstanceGetUniformLocation(s_prog.vertexShader, "tcFlip");
    u_fog = shaderInstanceGetUniformLocation(s_prog.vertexShader, "fogParam");
    C3D_BindProgram(&s_prog);
    fog_init();

    for (int i = 0; i < 3; i++) {
        s_pkbuf[i] = malloc(sizeof(Packet) * MAX_PACKETS);
        s_ring[i] = linearAlloc(RING_SIZE);
    }
    s_pk = s_pkbuf[0];
    for (int i = 0; i < 3; i++) mtx_identity(s_m[i]);
    render_thread_start();
}

void gfxGL_Exit(void) {
    render_thread_stop();
    for (int i = 0; i < MAX_TEXTURES; i++)
        if (s_tex[i].has_data) tex_delete(&s_tex[i].tex);
    for (int i = 0; i < MAX_VJOBS; i++)
        if (s_vjob[i].state != VJ_FREE) linearFree(s_vjob[i].staging);
    for (int i = 0; i < 3; i++) {
        linearFree(s_ring[i]);
        free(s_pkbuf[i]);
    }
    shaderProgramFree(&s_prog);
    DVLB_Free(s_dvlb);
    C3D_RenderTargetDelete(s_top_l);
    C3D_RenderTargetDelete(s_top_r);
    C3D_RenderTargetDelete(s_bot);
    C3D_Fini();
}

void gfxGL_SetStereo(float iod) { s_iod = iod; }
static float s_plane = 120.0f, s_hud;
void gfxGL_SetStereoDepth(float plane, float hud) {
    s_plane = plane;
    s_hud = hud;
}

void gfxGL_BeginFrame(void) {
    s_ring_idx = (s_ring_idx + 1) % 3;
    s_pk = s_pkbuf[s_ring_idx];
    s_npk = 0;
    s_ring_off = 0;
    s_screen = GFX_SCREEN_TOP;
}

/* Forget the draws recorded so far this frame (sys.c: ticks that are not shown). The ring
 * side stays the same: the other side may still be in use by the GPU. */
void gfxGL_DiscardDraws(void) {
    s_npk = 0;
    s_ring_off = 0;
    s_screen = GFX_SCREEN_TOP;
}

void gfxGL_SetScreen(GfxScreen screen) {
    s_screen = screen;
    s_vp[0] = s_vp[1] = 0;
    s_vp[2] = screen == GFX_SCREEN_TOP ? 400 : 320;
    s_vp[3] = 240;
}

/* ------------------------------------------------------------------ packet replay */

/* Fog without the PICA's fog unit. On real hardware, enabling the fog unit (GPU_FOG) made
 * depth writes unreliable: a triangle drawn later covered a nearer one drawn earlier in the same
 * draw call, on most frames (Jul's hair, Max's ears, the podium cars). Azahar does not show it.
 * Seen on a New 3DS through the debugger: fog mode 0 -> right on every frame; fog mode 5 ->
 * wrong, with the Z-buffer or the W-buffer, with or without the flip bit.
 * So the vertex shader computes GL's linear fog factor from the eye-space depth and passes it as
 * texcoord1; texture unit 1 holds an 8-texel luminance ramp, and TexEnv stage 1 blends:
 * colour = stage0 * f + fog colour * (1 - f). */
static C3D_Tex s_fogtex;

static void fog_init(void) {
    C3D_TexInit(&s_fogtex, 8, 8, GPU_L8);
    u8 *d = s_fogtex.data;
    for (u32 y = 0; y < 8; y++)
        for (u32 x = 0; x < 8; x++) {
            u32 m = (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
            d[m] = (u8)(x * 255 / 7);
        }
    C3D_TexFlush(&s_fogtex);
    C3D_TexSetFilter(&s_fogtex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&s_fogtex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
}

static struct RcFog { int fog; u32 fog_color; } *rc_fog(void);
static void unif_set(int reg, float x, float y, float z, float w);

static void set_fog(const Packet *p) {
    struct RcFog *rc = rc_fog();
    if (!p->fog) {
        unif_set(u_fog, 1.0f, 0.0f, 0, 0);
        if (rc->fog != 0) {
            C3D_TexEnvInit(C3D_GetTexEnv(1));
            rc->fog = 0;
        }
        return;
    }
    /* f = (end - d) / (end - start), d = -eye.z */
    float inv = 1.0f / (p->fog_end - p->fog_start);
    unif_set(u_fog, p->fog_end * inv, inv, 0, 0);
    if (rc->fog == 1 && rc->fog_color == p->fog_color) return;
    rc->fog = 1, rc->fog_color = p->fog_color;
    C3D_TexEnv *env = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env);
    C3D_TexBind(1, &s_fogtex);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_TEXTURE1);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, 0, 0);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, p->fog_color);
}

/* GL window rect (origin bottom-left, landscape) -> rotated framebuffer rect. */
static void fb_rect(int scr_w, const s16 *r, u32 *x, u32 *y, u32 *w, u32 *h) {
    int gx = r[0], gy = r[1], gw = r[2], gh = r[3];
    int fx = gy, fy = scr_w - gx - gw;
    if (fx < 0) { gh += fx; fx = 0; }
    if (fy < 0) { gw += fy; fy = 0; }
    *x = fx;
    *y = fy;
    *w = gh > 0 ? gh : 0;
    *h = gw > 0 ? gw : 0;
}

/* replay() state cache. Every citro3d setter marks its state dirty and C3D_DrawArrays then
 * re-emits all of it, so setting the whole state on each of ~300-500 draws made building the
 * GPU commands the largest part of a frame. Only what differs from the previous draw is set.
 * Reset at the start of each render target (C3D_FrameDrawOn sets the viewport). */
static struct {
    bool valid;
    C3D_FVec unif[96]; /* last value written per vertex-shader float uniform register */
    u8 unif_ok[96];
    u32 vp[4], sc[5];
    int depth_test, depth_func, depth_wr, wbuf, cull, blend, src, dst;
    float wscale;
    const void *tex0;
    int env0; /* 1 textured, 2 colour only */
} s_rc;

static struct RcFog s_rc_fog;
static struct RcFog *rc_fog(void) { return &s_rc_fog; }

static void unif_set(int reg, float x, float y, float z, float w) {
    if (reg < 0 || reg >= 96) return;
    C3D_FVec *c = &s_rc.unif[reg];
    if (s_rc.unif_ok[reg] && c->x == x && c->y == y && c->z == z && c->w == w) return;
    c->x = x, c->y = y, c->z = z, c->w = w;
    s_rc.unif_ok[reg] = 1;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, reg, x, y, z, w);
}

static void rc_reset(void) {
    memset(&s_rc, 0, sizeof s_rc);
    s_rc.depth_test = s_rc.cull = s_rc.blend = -1;
    s_rc_fog.fog = -1;
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
}

static void unif_mtx(int reg, const C3D_Mtx *m) {
    if (reg < 0 || reg + 4 > 96) return;
    bool same = true;
    for (int i = 0; i < 4 && same; i++)
        same = s_rc.unif_ok[reg + i] && !memcmp(&s_rc.unif[reg + i], &m->r[i], sizeof(C3D_FVec));
    if (same) return;
    for (int i = 0; i < 4; i++) {
        s_rc.unif[reg + i] = m->r[i];
        s_rc.unif_ok[reg + i] = 1;
    }
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, reg, m);
}

static float s_rplane = 120.0f, s_rhud; /* the frame being built: screen plane, HUD depth */

static void replay(const Packet *p, int scr_w, float eye) {
    u32 x, y, w, h;
    fb_rect(scr_w, p->vp, &x, &y, &w, &h);
    if (!w || !h) return; /* a 0-size viewport is undefined on the PICA: skip the draw */
    u32 sc[5] = {GPU_SCISSOR_DISABLE, 0, 0, 0, 0};
    if (p->scissor) {
        u32 sx, sy, sw, sh;
        fb_rect(scr_w, p->sc, &sx, &sy, &sw, &sh);
        u32 ex = sx + sw, ey = sy + sh;
        if (ex > 240) ex = 240;
        if (ey > (u32)scr_w) ey = scr_w;
        if (sx >= ex || sy >= ey) return; /* scissored away entirely */
        sc[0] = GPU_SCISSOR_NORMAL, sc[1] = sx, sc[2] = sy, sc[3] = ex, sc[4] = ey;
    }
    u32 vp[4] = {x, y, w, h};
    if (!s_rc.valid || memcmp(vp, s_rc.vp, sizeof vp)) {
        C3D_SetViewport(x, y, w, h);
        memcpy(s_rc.vp, vp, sizeof vp);
    }
    if (!s_rc.valid || memcmp(sc, s_rc.sc, sizeof sc)) {
        C3D_SetScissor((GPU_SCISSORMODE)sc[0], sc[1], sc[2], sc[3], sc[4]);
        memcpy(s_rc.sc, sc, sizeof sc);
    }

    C3D_Mtx proj = p->proj;
    if (eye != 0.0f && p->persp) {
        /* Stereo: each eye's picture is shifted sideways by x_ndc += eye * (1 - D / w), so
         * things at distance D (the screen plane) don't move, farther ones go behind the screen
         * and nearer ones come out of it. D = 120 (Pop-out: Normal; the 3D page sets it):
         * the race camera has the player's car about 186 units away (logged in a race), so
         * the car sits just behind the screen and the track beside the camera (down to ~30
         * units) doesn't jump out too far. The top framebuffer is rotated (final_projection:
         * y' = -x), so the screen's horizontal is the framebuffer's -y: in clip space
         * y' -= eye * (w - D). */
        const float D = s_rplane;
        for (int j = 0; j < 4; j++) proj.r[1].c[j] -= eye * proj.r[3].c[j];
        proj.r[1].w += eye * D;
    } else if (eye != 0.0f && s_rhud != 0.0f) {
        /* 2D (w = 1): the whole draw moves the way a near thing does, out of the screen */
        proj.r[1].w += eye * s_rhud;
    }
    unif_mtx(u_proj, &proj);
    unif_mtx(u_mv, &p->mv);
    unif_mtx(u_tm, &p->tm);
    unif_set(u_color, p->color.x, p->color.y, p->color.z, p->color.w);
    unif_set(u_lpos, p->lpos.x, p->lpos.y, p->lpos.z, p->lpos.w);
    unif_set(u_lamb, p->lamb.x, p->lamb.y, p->lamb.z, p->lamb.w);
    unif_set(u_ldif, p->ldif.x, p->ldif.y, p->ldif.z, p->ldif.w);
    unif_set(u_lflag, p->lflag.x, 0, 0, 0);
    unif_set(u_tcflip, p->tcflip.x, p->tcflip.y, 0, 0);

    int dfunc = p->depth_test ? p->depth_func : GPU_ALWAYS;
    int dwr = p->kind == PK_CLEAR
                  ? ((p->clear_mask & GL_COLOR_BUFFER_BIT ? GPU_WRITE_COLOR : 0) |
                     (p->clear_mask & GL_DEPTH_BUFFER_BIT ? GPU_WRITE_DEPTH : 0))
                  : (p->depth_write ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
    if (s_rc.depth_test != p->depth_test || s_rc.depth_func != dfunc || s_rc.depth_wr != dwr) {
        C3D_DepthTest(p->depth_test, (GPU_TESTFUNC)dfunc, (GPU_WRITEMASK)dwr);
        s_rc.depth_test = p->depth_test, s_rc.depth_func = dfunc, s_rc.depth_wr = dwr;
    }
    float ws = p->wbuf ? p->wscale : -1.0f;
    if (!s_rc.valid || s_rc.wbuf != p->wbuf || s_rc.wscale != ws) {
        C3D_DepthMap(!p->wbuf, ws, 0.0f);
        s_rc.wbuf = p->wbuf, s_rc.wscale = ws;
    }
    int cull = p->cull ? p->cull_mode : GPU_CULL_NONE;
    if (s_rc.cull != cull) {
        C3D_CullFace((GPU_CULLMODE)cull);
        s_rc.cull = cull;
    }
    int bsrc = p->blend ? p->src : GPU_ONE, bdst = p->blend ? p->dst : GPU_ZERO;
    if (s_rc.blend != 1 || s_rc.src != bsrc || s_rc.dst != bdst) {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, (GPU_BLENDFACTOR)bsrc, (GPU_BLENDFACTOR)bdst,
                       (GPU_BLENDFACTOR)bsrc, (GPU_BLENDFACTOR)bdst);
        s_rc.blend = 1, s_rc.src = bsrc, s_rc.dst = bdst;
    }
    if (!s_rc.valid) C3D_AlphaTest(false, GPU_ALWAYS, 0);
    set_fog(p);

    const GLTex *t = p->tex < MAX_TEXTURES ? &s_tex[p->tex] : NULL;
    bool textured = p->texturing && t && t->has_data;
    if (textured && s_rc.tex0 != &t->tex) {
        C3D_TexBind(0, (C3D_Tex *)&t->tex);
        s_rc.tex0 = &t->tex;
    }
    int env0 = textured ? 1 : 2;
    if (s_rc.env0 != env0) {
        C3D_TexEnv *env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        if (textured) {
            C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
            C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
        } else {
            C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        }
        s_rc.env0 = env0;
    }
    s_rc.valid = true;

    C3D_AttrInfo *ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    for (int i = 0; i < 3; i++) {
        if (p->attr_size[i])
            AttrInfo_AddLoader(ai, i, p->attr_fmt[i], p->attr_size[i]);
        else
            AttrInfo_AddFixed(ai, i);
    }
    if (!p->attr_size[1]) {
        C3D_FVec *f = C3D_FixedAttribGetWritePtr(1);
        f->x = p->fixed_nrm[0]; f->y = p->fixed_nrm[1]; f->z = p->fixed_nrm[2]; f->w = 0;
    }
    if (!p->attr_size[2]) {
        C3D_FVec *f = C3D_FixedAttribGetWritePtr(2);
        f->x = p->fixed_tc[0]; f->y = p->fixed_tc[1]; f->z = 0; f->w = 1;
    }
    if (p->interleaved_direct) {
        /* one buffer; loaders 0..n in order with their natural sizes filling the stride */
        u64 perm = 0;
        int n = 0;
        int used = 0;
        for (int i = 0; i < 3; i++)
            if (p->attr_size[i]) {
                perm |= (u64)i << (4 * n++);
                used += p->attr_size[i] * (p->attr_fmt[i] == GPU_FLOAT ? 4 :
                                           p->attr_fmt[i] == GPU_SHORT ? 2 : 1);
            }
        int pad = p->stride - used; /* 0xC..0xF = 4..16 bytes of padding */
        if (pad >= 4 && pad <= 16) perm |= (u64)(0xB + pad / 4) << (4 * n++);
        BufInfo_Add(bi, (void *)p->buf_addr, p->stride, n, perm);
    } else {
        for (int i = 0; i < 3; i++)
            if (p->attr_size[i]) BufInfo_Add(bi, (void *)p->attr_addr[i], p->attr_stride[i], 1, i);
    }
    C3D_DrawArrays(p->prim, 0, p->count);
}

/* ------------------------------------------------------------------ idle bottom screen */

/* The bottom screen shows the 3D Pixel Racing logo (romfs bottom_logo.t3x, 320x240 in the
 * top-left of a POT texture) on frames where the game drew nothing there, so it is never a
 * black void. It is drawn through the GL layer itself with the caller's GL state saved around
 * it, so the game's own state (bound texture, matrices, enables) is untouched. */
static GLuint s_idle_tex;
static float s_idle_u, s_idle_v;

void gfxGL_SetBottomIdle(const char *t3x_path, int w, int h) {
    GLuint keep = s_bound;
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    int tw = 0, th = 0;
    if (glTexImageT3X(t3x_path, &tw, &th) && tw > 0 && th > 0) {
        s_idle_tex = t;
        s_idle_u = (float)w / (float)tw;
        s_idle_v = (float)h / (float)th;
    } else {
        glDeleteTextures(1, &t);
    }
    s_bound = keep;
}

/* Overlays: drawing the port's own screens (the bottom-screen logo, the controls menu) through
 * the GL layer in the middle of the game's frame. Everything the game can set is saved on
 * Begin and put back on End, so the game's next draw sees exactly the state it left. */
typedef struct {
    GLuint bound;
    GLArray va, na, ta;
    GLenum matmode, depth_func, cull_face, bsrc, bdst;
    float m[3][16], color[4];
    bool lighting, light0, colormat, texturing, blend, depth_test, depth_write, cull, scissor, fog;
    GLint vp[4], sc[4];
    GfxScreen screen;
} GLSnap;
static GLSnap s_snap;
static int s_overlay_depth;

void gfxGL_OverlayBegin(GfxScreen screen) {
    if (s_overlay_depth++) return; /* nested: the outer Begin already saved */
    GLSnap *s = &s_snap;
    s->bound = s_bound;
    s->va = s_va, s->na = s_na, s->ta = s_ta;
    s->matmode = s_matmode, s->depth_func = s_depth_func, s->cull_face = s_cull_face;
    s->bsrc = s_bsrc, s->bdst = s_bdst;
    memcpy(s->m, s_m, sizeof s->m);
    memcpy(s->color, s_color, sizeof s->color);
    s->lighting = s_lighting, s->light0 = s_light0, s->colormat = s_colormat;
    s->texturing = s_texturing, s->blend = s_blend, s->depth_test = s_depth_test;
    s->depth_write = s_depth_write, s->cull = s_cull, s->scissor = s_scissor, s->fog = s_fog;
    memcpy(s->vp, s_vp, sizeof s->vp);
    memcpy(s->sc, s_sc, sizeof s->sc);
    s->screen = s_screen;

    /* a plain 2D setup: pixels, origin top-left, alpha blending, white, nothing else */
    int w = screen == GFX_SCREEN_TOP ? 400 : 320;
    gfxGL_SetScreen(screen);
    s_lighting = s_depth_test = s_cull = s_scissor = s_fog = s_colormat = false;
    s_depth_write = false;
    s_texturing = true;
    s_blend = true;
    s_bsrc = GL_SRC_ALPHA, s_bdst = GL_ONE_MINUS_SRC_ALPHA;
    glViewport(0, 0, w, 240);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0, (float)w, 240, 0, -1, 1);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    s_color[0] = s_color[1] = s_color[2] = s_color[3] = 1.0f;
    s_va.enabled = true;
    s_na.enabled = false;
    s_ta.enabled = false;
}

static struct {
    GfxScreen screen;
    GLint vp[4], sc[4];
    bool scissor;
    GLenum matmode;
    float proj[16];
} s_push;

void gfxGL_ScreenPush(GfxScreen screen) {
    s_push.screen = s_screen;
    memcpy(s_push.vp, s_vp, sizeof s_vp);
    memcpy(s_push.sc, s_sc, sizeof s_sc);
    s_push.scissor = s_scissor;
    s_push.matmode = s_matmode;
    memcpy(s_push.proj, s_m[1], sizeof s_push.proj);
    gfxGL_SetScreen(screen);
    s_scissor = false;
}

void gfxGL_ScreenPop(void) {
    s_screen = s_push.screen;
    memcpy(s_vp, s_push.vp, sizeof s_vp);
    memcpy(s_sc, s_push.sc, sizeof s_sc);
    s_scissor = s_push.scissor;
    s_matmode = s_push.matmode;
    memcpy(s_m[1], s_push.proj, sizeof s_push.proj);
}

void gfxGL_OverlayEnd(void) {
    if (s_overlay_depth <= 0 || --s_overlay_depth) return;
    const GLSnap *s = &s_snap;
    s_bound = s->bound;
    s_va = s->va, s_na = s->na, s_ta = s->ta;
    s_matmode = s->matmode, s_depth_func = s->depth_func, s_cull_face = s->cull_face;
    s_bsrc = s->bsrc, s_bdst = s->bdst;
    memcpy(s_m, s->m, sizeof s_m);
    memcpy(s_color, s->color, sizeof s_color);
    s_lighting = s->lighting, s_light0 = s->light0, s_colormat = s->colormat;
    s_texturing = s->texturing, s_blend = s->blend, s_depth_test = s->depth_test;
    s_depth_write = s->depth_write, s_cull = s->cull, s_scissor = s->scissor, s_fog = s->fog;
    memcpy(s_vp, s->vp, sizeof s_vp);
    memcpy(s_sc, s->sc, sizeof s_sc);
    s_screen = s->screen;
}

void gfxGL_OverlayRect(float x, float y, float w, float h, float r, float g, float b, float a) {
    float v[8] = {x, y, x + w, y, x, y + h, x + w, y + h};
    bool tex = s_texturing, ta = s_ta.enabled;
    s_texturing = false;
    s_ta.enabled = false;
    glColor4f(r, g, b, a);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, v);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    s_texturing = tex;
    s_ta.enabled = ta;
}

static void draw_idle(GfxScreen screen) {
    float tc[8] = {0, 0, s_idle_u, 0, 0, s_idle_v, s_idle_u, s_idle_v}; /* y down: t = 0 on top */
    static const float pos_bottom[8] = {0, 0, 320, 0, 0, 240, 320, 240};
    static const float pos_top[8] = {40, 0, 360, 0, 40, 240, 360, 240}; /* centred on 400 */
    const float *pos = screen == GFX_SCREEN_TOP ? pos_top : pos_bottom;
    gfxGL_OverlayBegin(screen);
    s_blend = false;
    glBindTexture(GL_TEXTURE_2D, s_idle_tex);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, pos);
    glTexCoordPointer(2, GL_FLOAT, 0, tc);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gfxGL_OverlayEnd();
}

void gfxGL_DrawBottomIdle(void) {
    if (s_idle_tex) draw_idle(GFX_SCREEN_BOTTOM);
}

void gfxGL_DrawTopIdle(void) {
    if (s_idle_tex) draw_idle(GFX_SCREEN_TOP);
}

/* Debug log: one line per packet, so a GPU hang on hardware (which Azahar never shows) can be
 * traced to the draw that caused it. */
static void dump_frame(const char *what, int frame, const Packet *pk, int n) {
    sys_log("gpu: %s %d, %d packets", what, frame, n);
    for (int i = 0; i < n && i < 400; i++) {
        const Packet *p = &pk[i];
        const GLTex *t = p->tex < MAX_TEXTURES ? &s_tex[p->tex] : NULL;
        sys_log("  #%d %s %s prim %x n %u vp %d,%d,%d,%d%s buf %08lx stride %u attr %u/%u %u/%u %u/%u"
                " tex %u%s %dx%d blend %d depth %d/%d fog %d cull %d",
                i, p->screen == GFX_SCREEN_TOP ? "top" : "bot", p->kind == PK_CLEAR ? "clear" : "draw",
                (unsigned)p->prim, (unsigned)p->count, p->vp[0], p->vp[1], p->vp[2], p->vp[3],
                p->scissor ? " sc" : "", (unsigned long)p->buf_addr, (unsigned)p->stride,
                p->attr_fmt[0], p->attr_size[0], p->attr_fmt[1], p->attr_size[1], p->attr_fmt[2],
                p->attr_size[2], (unsigned)p->tex, p->texturing ? "" : "(off)",
                t && t->has_data ? t->tex.width : 0, t && t->has_data ? t->tex.height : 0,
                p->blend, p->depth_test, p->depth_write, p->fog, p->cull);
    }
}

/* Vertex data the GPU reads straight from the game's memory may still sit in the CPU data
 * cache (the game may have just written it). Clean each buffer once per frame: draws mostly
 * reuse a few buffers, so ranges are merged by start address before the cache is cleaned. */
static void flush_game_vbs(void) {
    enum { NR = 32 };
    u32 lo[NR], hi[NR];
    int n = 0;
    for (int i = 0; i < s_npk; i++) {
        const Packet *p = &s_pk[i];
        if (!p->game_vb) continue;
        u32 a = p->buf_addr, b = a + (u32)p->count * p->stride;
        int j = 0;
        while (j < n && (a > hi[j] + 4096 || b + 4096 < lo[j])) j++;
        if (j < n) {
            if (a < lo[j]) lo[j] = a;
            if (b > hi[j]) hi[j] = b;
        } else if (n < NR) {
            lo[n] = a, hi[n] = b, n++;
        } else {
            svcFlushProcessDataCache(CUR_PROCESS_HANDLE, a, b - a);
        }
    }
    for (int j = 0; j < n; j++) svcFlushProcessDataCache(CUR_PROCESS_HANDLE, lo[j], hi[j] - lo[j]);
}

/* C3D_FrameBegin(SYNCDRAW) waits for the previous frame's GPU work with no time limit, so a
 * GPU fault freezes the console with nothing on screen and no log. Wait in steps instead and,
 * after 2 s, log the frame that never finished. */
/* One frame's recorded draws, handed to the render thread. */
typedef struct {
    const Packet *pk;
    int npk;
    float iod, plane, hud;
    int frame;
    u32 ring_kb;
    int copy_tex; /* glCopyTexImage2D of the frame before: texture to fill, 0 = none */
} RenderJob;

static RenderJob s_job, s_prev_job;

/* glCopyTexImage2D (the pause screen's backdrop: the game grabs the race picture once when
 * it pauses). The request made while recording frame N is carried out when frame N + 1 is
 * built, first thing after the GPU has finished frame N and before the screens are cleared:
 * the top screen's (left eye's) colour buffer then still holds frame N's finished picture.
 * The copy is a GX TextureCopy straight from the colour buffer, which is stored in the same
 * 8x8-tile layout as a texture: each 240-pixel tile row goes into a 256-texel texture row,
 * 400 rows into 512. The picture stays rotated as the framebuffer is (fill_common maps the
 * game's texture coordinates for such "transposed" textures). */
static int s_copy_req, s_copy_prev;

static void copy_top_to_texture(int id) {
    GLTex *t = &s_tex[id];
    if (!t->used || !t->has_data || !t->transposed) return;
    const u32 fb_w = 240, fb_h = 400, line = fb_w * 8 * 4;
    GX_TextureCopy((u32 *)s_top_l->frameBuf.colorBuf, GX_BUFFER_DIM(line >> 4, 0),
                   (u32 *)t->tex.data, GX_BUFFER_DIM(line >> 4, (t->tex.width * 8 * 4 - line) >> 4),
                   fb_w * fb_h * 4, 8);
}

static void frame_begin_watchdog(void) {
    C3D_FrameSync();
    u32 t0 = sys_ms();
    bool reported = false;
    while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
        if (!reported && sys_ms() - t0 > 2000) {
            reported = true;
            sys_log("gpu: HANG -- frame %d has not finished after 2 s", s_prev_job.frame);
            dump_frame("hung frame", s_prev_job.frame, s_prev_job.pk, s_prev_job.npk);
        }
        svcSleepThread(1000000);
    }
    if (reported) sys_log("gpu: frame %d finished after %lu ms", s_prev_job.frame, (unsigned long)(sys_ms() - t0));
}

/* Turns one frame's packets into GPU commands and submits them. Runs on the render thread
 * (or inline when there is none); the only code that touches citro3d's context after init. */
static void render_frame(const RenderJob *j) {
    float iod = j->iod;
    s_rplane = j->plane;
    s_rhud = j->hud;
    u64 t0 = svcGetSystemTick();
    vram_jobs_run();
    frame_begin_watchdog();
    u64 t1 = svcGetSystemTick();
    s_wait_ms = (float)(t1 - t0) / CPU_TICKS_PER_MSEC;
    if (j->copy_tex) copy_top_to_texture(j->copy_tex);
    gfxSet3D(iod > 0.0f);
    /* depth 1 = far (24 bits; the top byte of a D24S8 word is the stencil) */
    C3D_RenderTargetClear(s_top_l, C3D_CLEAR_ALL, 0x000000FF, 0x00FFFFFF);
    C3D_RenderTargetClear(s_bot, C3D_CLEAR_ALL, 0x000000FF, 0x00FFFFFF);
    if (iod > 0.0f) C3D_RenderTargetClear(s_top_r, C3D_CLEAR_ALL, 0x000000FF, 0x00FFFFFF);
    int draws = 0;
    for (int pass = 0; pass < (iod > 0.0f ? 3 : 2); pass++) {
        GfxScreen want = pass == 1 ? GFX_SCREEN_BOTTOM : GFX_SCREEN_TOP;
        C3D_RenderTarget *rt = pass == 0 ? s_top_l : pass == 1 ? s_bot : s_top_r;
        float eye = pass == 0 ? -iod : pass == 2 ? iod : 0.0f;
        int scr_w = want == GFX_SCREEN_TOP ? 400 : 320;
        C3D_FrameDrawOn(rt);
        rc_reset();
        for (int i = 0; i < j->npk; i++) {
            const Packet *p = &j->pk[i];
            if (p->screen != want) continue;
            replay(p, scr_w, eye);
            draws++;
        }
    }
    float use = C3D_GetCmdBufUsage();
    if (use > s_cmd_peak) {
        s_cmd_peak = use;
        if (use > 0.5f)
            sys_log("gpu: command buffer %d%% full (frame %d, %d draws)", (int)(use * 100), j->frame, draws);
    }
    C3D_FrameEnd(0);
    s_build_ms = (float)(svcGetSystemTick() - t1) / CPU_TICKS_PER_MSEC;
    if (j->frame <= 3) sys_log("gpu: frame %d submitted, %d draws, ring %lu KB, cmdbuf %d%%", j->frame,
                               draws, (unsigned long)j->ring_kb, (int)(use * 100));
    s_prev_job = *j;
    s_last_draws = draws;
}

/* The render thread. On the 3DS the game's ticks and turning the recorded draws into GPU
 * commands each take a large share of a frame, and the Old 3DS runs out of time with many cars
 * on screen. The commands for frame N are built on another core while the main core already
 * runs the game ticks of frame N + 1: three packet lists and vertex rings rotate (recording,
 * being built, on the GPU). New 3DS: core 2, which is the game's own. Old 3DS (or if core 2 is
 * refused): core 1, the system core, of which an application may use up to 80%. If neither
 * works the frame is built inline, as before. */
static Thread s_rthread;
static LightEvent s_ev_go, s_ev_done;
static volatile bool s_rquit, s_rbusy;
static int s_rcore = -1;

static void render_main(void *arg) {
    (void)arg;
    for (;;) {
        LightEvent_Wait(&s_ev_go);
        if (s_rquit) break;
        render_frame(&s_job);
        LightEvent_Signal(&s_ev_done);
    }
}

static void render_thread_start(void) {
    LightEvent_Init(&s_ev_go, RESET_ONESHOT);
    LightEvent_Init(&s_ev_done, RESET_ONESHOT);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    static const int k_cores[2] = {2, 1};
    for (int i = n3ds ? 0 : 1; i < 2 && !s_rthread; i++) {
        int core = k_cores[i];
        if (core == 1) {
            Result r = APT_SetAppCpuTimeLimit(80);
            if (R_FAILED(r)) {
                sys_log("gfx: system core time limit refused (%08lx)", (unsigned long)r);
                continue;
            }
        }
        s_rthread = threadCreate(render_main, NULL, 32 * 1024, prio > 0x18 ? prio - 1 : 0x18, core, false);
        if (s_rthread) s_rcore = core;
    }
    if (s_rthread)
        sys_log("gfx: GPU commands built on core %d", s_rcore);
    else
        sys_log("gfx: no second core; GPU commands built inline");
}

/* Wait until the render thread has submitted the frame it was given. Before the main thread
 * reuses that frame's buffers, and before the HOME Menu / sleep / exit (sys.c apt_hook), when
 * nothing may be talking to the GPU. */
void gfxGL_WaitIdle(void) {
    if (!s_rbusy) return;
    u64 t = svcGetSystemTick();
    LightEvent_Wait(&s_ev_done);
    s_rbusy = false;
    s_sync_ms = (float)(svcGetSystemTick() - t) / CPU_TICKS_PER_MSEC;
}

static void render_thread_stop(void) {
    if (!s_rthread) return;
    gfxGL_WaitIdle();
    s_rquit = true;
    LightEvent_Signal(&s_ev_go);
    threadJoin(s_rthread, U64_MAX);
    threadFree(s_rthread);
    s_rthread = NULL;
}

void gfxGL_EndFrame(void) {
    if (s_idle_tex) {
        bool bottom = false;
        for (int i = 0; i < s_npk && !bottom; i++) bottom = s_pk[i].screen == GFX_SCREEN_BOTTOM;
        if (!bottom) draw_idle(GFX_SCREEN_BOTTOM);
    }
    s_frame++;
    if (s_frame <= 3) dump_frame("frame", s_frame, s_pk, s_npk);
    /* written by this core: clean it out of the CPU cache here */
    GSPGPU_FlushDataCache(s_ring[s_ring_idx], s_ring_off);
    flush_game_vbs();
    s_sync_ms = 0;
    gfxGL_WaitIdle();
    s_job = (RenderJob){s_pk, s_npk, s_iod, s_plane, s_hud, s_frame, s_ring_off >> 10, s_copy_prev};
    s_copy_prev = s_copy_req;
    s_copy_req = 0;
    if (s_rthread) {
        s_rbusy = true;
        LightEvent_Signal(&s_ev_go);
    } else {
        render_frame(&s_job);
    }
}

/* ------------------------------------------------------------------ packet capture */

static Packet *new_packet(int kind) {
    if (s_npk >= MAX_PACKETS) return NULL;
    Packet *p = &s_pk[s_npk++];
    memset(p, 0, sizeof *p);
    p->kind = kind;
    p->screen = s_screen;
    for (int i = 0; i < 4; i++) {
        p->vp[i] = s_vp[i];
        p->sc[i] = s_sc[i];
    }
    return p;
}

/* The game sets the projection and texture matrices rarely but draws hundreds of times a
 * frame: their converted forms are kept until the GL matrix changes. */
static void final_projection_(C3D_Mtx *out, const float *proj);
static void final_projection(C3D_Mtx *out, const float *proj) {
    static float s_in[16];
    static C3D_Mtx s_out;
    static bool s_ok;
    if (!s_ok || memcmp(s_in, proj, 64)) {
        memcpy(s_in, proj, 64);
        final_projection_(&s_out, proj);
        s_ok = true;
    }
    *out = s_out;
}

static void to_c3d_tm(C3D_Mtx *out, const float *m) {
    static float s_in[16];
    static C3D_Mtx s_out;
    static bool s_ok;
    if (!s_ok || memcmp(s_in, m, 64)) {
        memcpy(s_in, m, 64);
        to_c3d(&s_out, m);
        s_ok = true;
    }
    *out = s_out;
}

static void final_projection_(C3D_Mtx *out, const float *proj) {
    static const float tilt_zfix[16] = {
        /* column-major: x' = y, y' = -x, z' = -0.5 z - 0.5 w, w' = w */
        0, -1, 0, 0,
        1, 0, 0, 0,
        0, 0, -0.5f, 0,
        0, 0, -0.5f, 1,
    };
    float m[16];
    mtx_mul(m, tilt_zfix, proj);
    to_c3d(out, m);
}

static void fill_common(Packet *p) {
    final_projection(&p->proj, s_m[1]);
    p->persp = s_m[1][11] != 0.0f;
    /* Depth for perspective draws uses the W-buffer. With the Z-buffer (z/w) the game's near
     * plane of 10 puts everything past ~250 units into the bottom 4% of the depth range, and
     * the 3DS GPU's depth interpolation is too coarse there: surfaces a few units apart (a
     * face and the ears behind it, a car body and the driver inside) showed through each other
     * and flickered on hardware, though not in emulators. The W-buffer stores
     *   depth = z' * scale  (z' = clip z after zfix = -f (d - n) / (f - n), d = eye distance)
     * with scale = -1/f, i.e. (d - n) / (f - n): linear in distance, near 0, far 1.
     *
     * Near must be the 0 end. The GPU carries z'/w in 24-bit floats (16-bit mantissa) and the
     * W-buffer multiplies it back by w * scale. Build 5 had it the other way round (near 1,
     * scale -1/n): close surfaces then had depths just under 1, where those floats are
     * coarsest, and the error was scaled up by d/n on top. Jul's hair, a shell 1% larger than
     * the head (0.2 units in front of the face at d = 60..100), lost the depth test in patches
     * that changed every frame (seen on hardware through the debugger, at a steady 60 fps).
     * With near at 0 the same surfaces sit at depths around 0.05..0.1 and the factor is d/f:
     * at least ten times finer close up, and no coarser anywhere. */
    p->wbuf = false;
    if (p->persp && s_m[1][11] == -1.0f) {
        const float *m = s_m[1];
        float n = m[14] / (m[10] - 1.0f), f = m[14] / (m[10] + 1.0f);
        if (n > 0.0f && f > n) {
            p->wbuf = true;
            p->wscale = -1.0f / f;
        }
    }
    to_c3d(&p->mv, s_m[0]);
    if (s_bound < MAX_TEXTURES && s_tex[s_bound].transposed) {
        /* A framebuffer copy: s' = GL's t (texel s runs up the screen) and t' = 1 - GL's s
         * (colour buffer row n is screen column n, and the texture's t reads row
         * (1 - t') * height). Checked in Azahar: the pause backdrop lines up with the race. */
        float swap[16] = {0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1};
        float m[16];
        mtx_mul(m, swap, s_m[2]);
        to_c3d_tm(&p->tm, m);
    } else {
        to_c3d_tm(&p->tm, s_m[2]);
    }
    p->color = FVec4_New(s_color[0], s_color[1], s_color[2], s_color[3]);
    bool lit = s_lighting && s_light0;
    p->lflag = FVec4_New(lit ? 1.0f : 0.0f, 0, 0, 0);
    if (lit) {
        const float *ma = s_colormat ? s_color : (const float[4]){0.2f, 0.2f, 0.2f, 1.0f};
        const float *md = s_colormat ? s_color : (const float[4]){0.8f, 0.8f, 0.8f, 1.0f};
        p->lamb = FVec4_New(s_model_amb[0] * ma[0] + s_light_amb[0] * ma[0],
                            s_model_amb[1] * ma[1] + s_light_amb[1] * ma[1],
                            s_model_amb[2] * ma[2] + s_light_amb[2] * ma[2], md[3]);
        p->ldif = FVec4_New(s_light_dif[0] * md[0], s_light_dif[1] * md[1],
                            s_light_dif[2] * md[2], 0);
        p->lpos = FVec4_New(s_light_pos[0], s_light_pos[1], s_light_pos[2], s_light_pos[3]);
    }
    p->tcflip = FVec4_New(1.0f, -1.0f, 0, 0);
    if (s_bound < MAX_TEXTURES && s_tex[s_bound].transposed) p->tcflip = FVec4_New(0, 1.0f, 0, 0);
    p->tex = s_bound;
    p->texturing = s_texturing;
    p->blend = s_blend;
    p->src = blend_factor(s_bsrc);
    p->dst = blend_factor(s_bdst);
    p->depth_test = s_depth_test;
    p->depth_write = s_depth_write;
    p->depth_func = depth_func(s_depth_func);
    p->cull = s_cull && s_cull_face != GL_FRONT_AND_BACK;
    /* GL front = CCW; tilt keeps winding, so cull the CW (back) or CCW (front) side */
    p->cull_mode = s_cull_face == GL_FRONT ? GPU_CULL_FRONT_CCW : GPU_CULL_BACK_CCW;
    p->scissor = s_scissor;
    p->fog = s_fog && p->persp;
    if (p->fog) {
        p->fog_start = s_fog_start;
        p->fog_end = s_fog_end > s_fog_start ? s_fog_end : s_fog_start + 1.0f;
        p->fog_color = rgba8(s_fog_color) & 0xFFFFFF;
    }
}

/* Small unlit draws (HUD digits and letters, sprites, particles: ~200 of the ~340 draws in a
 * race, ~5 vertices each) cost citro3d far more in per-draw state than in vertices. They are
 * stored as triangle lists with the modelview already applied, so a run of them with the same
 * state becomes one draw. Lit draws are left alone: the shader needs the modelview for the
 * normals and the light. */
#define BATCH_MAX_VERTS 96

static bool batch_same(const Packet *a, const Packet *b) {
#define EQ(f) (!memcmp(&a->f, &b->f, sizeof a->f))
    return EQ(screen) && EQ(persp) && EQ(wbuf) && EQ(wscale) && EQ(proj) && EQ(tm) && EQ(color) &&
           EQ(tcflip) && EQ(tex) && EQ(texturing) && EQ(blend) && EQ(src) && EQ(dst) &&
           EQ(depth_test) && EQ(depth_write) && EQ(depth_func) && EQ(cull) && EQ(cull_mode) &&
           EQ(scissor) && EQ(sc) && EQ(vp) && EQ(fog) && EQ(fog_start) && EQ(fog_end) &&
           EQ(fog_color) && EQ(fixed_nrm) && EQ(fixed_tc) && EQ(attr_size) && EQ(stride);
#undef EQ
}

/* p = the new packet (last in s_pk, fill_common done). Returns false if the draw doesn't
 * qualify; then the caller stores it the usual way. */
static bool batch_draw(Packet *p, GPU_Primitive_t prim, GLint first, int count,
                       const GLArray *const arr[3], const bool en[3], const int nsize[3]) {
    const float *m = s_m[0];
    if (p->lflag.x != 0.0f || count > BATCH_MAX_VERTS) return false;
    if (s_va.size < 2 || s_va.size > 3) return false;
    if (m[3] != 0.0f || m[7] != 0.0f || m[11] != 0.0f || m[15] != 1.0f) return false;
    int out = prim == GPU_TRIANGLES ? count : (count - 2) * 3;
    int comps = 3 + (en[1] ? 3 : 0) + (en[2] ? nsize[2] : 0);

    /* the packet as it will be stored */
    p->prim = GPU_TRIANGLES;
    p->batched = true;
    p->interleaved_direct = true;
    p->stride = comps * 4;
    memset(p->attr_size, 0, sizeof p->attr_size);
    for (int i = 0; i < 3; i++) {
        if (!en[i]) continue;
        p->attr_fmt[i] = GPU_FLOAT;
        p->attr_size[i] = i == 0 ? 3 : nsize[i];
    }
    if (!en[1]) p->fixed_nrm[2] = 1.0f;
    Mtx_Identity(&p->mv);

    Packet *q = s_npk >= 2 ? &s_pk[s_npk - 2] : NULL;
    bool merge = q && q->kind == PK_DRAW && q->batched && q->count + out <= 0xFFFF &&
                 q->buf_addr + (u32)q->count * q->stride == (u32)(s_ring[s_ring_idx] + s_ring_off) &&
                 batch_same(q, p);
    float *dst = merge ? ring_append(out * comps * 4) : ring_alloc(out * comps * 4);
    if (!dst) {
        s_npk--;
        return true; /* out of ring: drop the draw, as the repack path does */
    }
    for (int k = 0; k < out; k++) {
        int v;
        if (prim == GPU_TRIANGLES) {
            v = k;
        } else {
            int t = k / 3, c = k % 3;
            if (prim == GPU_TRIANGLE_FAN)
                v = c == 0 ? 0 : t + c;
            else /* strip: odd triangles swap their first two vertices to keep the winding */
                v = (t & 1) && c < 2 ? t + 1 - c : t + c;
        }
        float *o = dst + k * comps;
        const GLArray *a = arr[0];
        int ts = type_size(a->type);
        const u8 *src = (const u8 *)a->ptr + (first + v) * (a->stride ? a->stride : a->size * ts);
        float x = read_comp(src, a->type), y = read_comp(src + ts, a->type);
        float z = a->size > 2 ? read_comp(src + 2 * ts, a->type) : 0.0f;
        *o++ = m[0] * x + m[4] * y + m[8] * z + m[12];
        *o++ = m[1] * x + m[5] * y + m[9] * z + m[13];
        *o++ = m[2] * x + m[6] * y + m[10] * z + m[14];
        for (int i = 1; i < 3; i++) {
            if (!en[i]) continue;
            a = arr[i];
            int n = nsize[i];
            ts = type_size(a->type);
            src = (const u8 *)a->ptr + (first + v) * (a->stride ? a->stride : n * ts);
            for (int c = 0; c < n; c++) *o++ = read_comp(src + c * ts, a->type);
        }
    }
    if (merge) {
        q->count += out;
        s_npk--;
    } else {
        p->buf_addr = (u32)dst;
        p->count = out;
    }
    return true;
}

static bool s_skip_draws;
void gfxGL_SkipDraws(bool skip) { s_skip_draws = skip; }

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (s_skip_draws) return;
    if (count <= 0 || !s_va.enabled || !s_va.ptr) return;
    if (s_cull && s_cull_face == GL_FRONT_AND_BACK) return;
    GPU_Primitive_t prim;
    switch (mode) {
    case GL_TRIANGLES: prim = GPU_TRIANGLES; break;
    case GL_TRIANGLE_STRIP: prim = GPU_TRIANGLE_STRIP; break;
    case GL_TRIANGLE_FAN: prim = GPU_TRIANGLE_FAN; break;
    default: return; /* points/lines are not used by the game */
    }
    /* The PICA stalls on draws it can't finish (an incomplete triangle, a count that wraps to
     * 0 in the 16-bit field), and a stalled GPU freezes the whole console. */
    if (prim == GPU_TRIANGLES) count -= count % 3;
    if (count < 3) return;
    if (count > 0xFFFF) {
        static bool s_warned;
        if (!s_warned) { s_warned = true; sys_log("gl: draw of %ld vertices skipped", (long)count); }
        return;
    }
    Packet *p = new_packet(PK_DRAW);
    if (!p) return;
    p->prim = prim;
    p->count = count;
    fill_common(p);

    const GLArray *arr[3] = {&s_va, &s_na, &s_ta};
    bool en[3] = {true, s_na.enabled && s_na.ptr, s_ta.enabled && s_ta.ptr};
    int nsize[3] = {s_va.size, 3, s_ta.size};

    /* Direct path: arrays already in linear memory, interleaved in attribute order with
     * natural packing (pos, [nrm], [tc]) and one shared stride. */
    bool direct = is_linear(s_va.ptr);
    u32 base = (u32)s_va.ptr + first * (s_va.stride ? s_va.stride : s_va.size * type_size(s_va.type));
    int stride = s_va.stride ? s_va.stride : s_va.size * type_size(s_va.type);
    int expect = s_va.size * type_size(s_va.type);
    for (int i = 1; i < 3 && direct; i++) {
        if (!en[i]) continue;
        const GLArray *a = arr[i];
        int asz = nsize[i] * type_size(a->type);
        u32 addr = (u32)a->ptr + first * (a->stride ? a->stride : asz);
        if ((a->stride ? a->stride : asz) != stride || addr != base + expect) direct = false;
        expect += asz;
    }
    /* trailing padding (e.g. the 36-byte .bin vertex) is expressed as PICA padding attribs */
    if (direct && (expect > stride || ((stride - expect) & 3) || stride - expect > 16)) direct = false;
    if (direct && (base & 3) && s_va.type == GL_FLOAT) direct = false;

    if (direct) {
        p->interleaved_direct = true;
        p->buf_addr = base;
        p->stride = stride;
        p->game_vb = true; /* flushed from the CPU cache in gfxGL_EndFrame */
        for (int i = 0; i < 3; i++) {
            if (!en[i]) continue;
            p->attr_fmt[i] = type_fmt(arr[i]->type);
            p->attr_size[i] = nsize[i];
        }
    } else {
        /* Repack into floats: [pos][nrm][tc] */
        if (batch_draw(p, prim, first, count, arr, en, nsize)) return;
        int comps = s_va.size + (en[1] ? 3 : 0) + (en[2] ? s_ta.size : 0);
        float *dst = ring_alloc(count * comps * 4);
        if (!dst) { s_npk--; return; }
        for (int v = 0; v < count; v++) {
            float *o = dst + v * comps;
            for (int i = 0; i < 3; i++) {
                if (!en[i]) continue;
                const GLArray *a = arr[i];
                int n = nsize[i], ts = type_size(a->type);
                const u8 *src = (const u8 *)a->ptr + (first + v) * (a->stride ? a->stride : n * ts);
                for (int c = 0; c < n; c++) *o++ = read_comp(src + c * ts, a->type);
            }
        }
        p->interleaved_direct = true;
        p->buf_addr = (u32)dst;
        p->stride = comps * 4;
        for (int i = 0; i < 3; i++) {
            if (!en[i]) continue;
            p->attr_fmt[i] = GPU_FLOAT;
            p->attr_size[i] = nsize[i];
        }
    }
    if (!en[1]) { p->fixed_nrm[2] = 1.0f; }
}

void glClear(GLbitfield mask) {
    if (s_skip_draws) return;
    mask &= GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT;
    if (!mask) return;
    /* Full-screen quad at the far plane (depth 1) through the normal pipeline. The clip
     * range is z in [-w, 0] and the quad's w is 1: z sits a hair inside -1 so it can't be
     * clipped away, and is written as depth 0.999999. */
    float *v = ring_alloc(4 * 3 * 4);
    if (!v) return;
    const float zf = -0.999999f;
    const float q[12] = {-1, -1, zf, 1, -1, zf, -1, 1, zf, 1, 1, zf};
    memcpy(v, q, sizeof q);
    Packet *p = new_packet(PK_CLEAR);
    if (!p) return;
    p->kind = PK_CLEAR;
    p->clear_mask = mask;
    p->prim = GPU_TRIANGLE_STRIP;
    p->count = 4;
    static const float tilt[16] = {0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    to_c3d(&p->proj, tilt);
    Mtx_Identity(&p->mv);
    Mtx_Identity(&p->tm);
    p->color = FVec4_New(s_clear[0], s_clear[1], s_clear[2], s_clear[3]);
    p->tcflip = FVec4_New(0, 1, 0, 0);
    p->depth_test = true;
    p->depth_func = GPU_ALWAYS;
    p->vp[0] = p->vp[1] = 0;
    p->vp[2] = s_screen == GFX_SCREEN_TOP ? 400 : 320;
    p->vp[3] = 240;
    p->scissor = s_scissor;
    p->interleaved_direct = true;
    p->buf_addr = (u32)v;
    p->stride = 12;
    p->attr_fmt[0] = GPU_FLOAT;
    p->attr_size[0] = 3;
    p->fixed_nrm[2] = 1.0f;
}

/* ------------------------------------------------------------------ state */

static bool *cap_ptr(GLenum cap) {
    switch (cap) {
    case GL_LIGHTING: return &s_lighting;
    case GL_LIGHT0: return &s_light0;
    case GL_COLOR_MATERIAL: return &s_colormat;
    case GL_TEXTURE_2D: return &s_texturing;
    case GL_BLEND: return &s_blend;
    case GL_DEPTH_TEST: return &s_depth_test;
    case GL_CULL_FACE: return &s_cull;
    case GL_SCISSOR_TEST: return &s_scissor;
    case GL_FOG: return &s_fog;
    default: return NULL;
    }
}

void glEnable(GLenum cap) { bool *b = cap_ptr(cap); if (b) *b = true; }
void glDisable(GLenum cap) { bool *b = cap_ptr(cap); if (b) *b = false; }

static GLArray *array_ptr(GLenum a) {
    switch (a) {
    case GL_VERTEX_ARRAY: return &s_va;
    case GL_NORMAL_ARRAY: return &s_na;
    case GL_TEXTURE_COORD_ARRAY: return &s_ta;
    default: return NULL;
    }
}
void glEnableClientState(GLenum a) { GLArray *p = array_ptr(a); if (p) p->enabled = true; }
void glDisableClientState(GLenum a) { GLArray *p = array_ptr(a); if (p) p->enabled = false; }

void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
    s_va.size = size; s_va.type = type; s_va.stride = stride; s_va.ptr = ptr;
}
void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *ptr) {
    s_na.size = 3; s_na.type = type; s_na.stride = stride; s_na.ptr = ptr;
}
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
    s_ta.size = size; s_ta.type = type; s_ta.stride = stride; s_ta.ptr = ptr;
}

void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    s_color[0] = r; s_color[1] = g; s_color[2] = b; s_color[3] = a;
}
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) {
    s_clear[0] = r; s_clear[1] = g; s_clear[2] = b; s_clear[3] = a;
}
void glBlendFunc(GLenum s, GLenum d) { s_bsrc = s; s_bdst = d; }
void glDepthFunc(GLenum f) { s_depth_func = f; }
void glDepthMask(GLboolean f) { s_depth_write = f; }
void glDepthRangef(GLclampf n, GLclampf f) { (void)n; (void)f; }
void glCullFace(GLenum m) { s_cull_face = m; }
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    s_vp[0] = x; s_vp[1] = y; s_vp[2] = w; s_vp[3] = h;
}
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    s_sc[0] = x; s_sc[1] = y; s_sc[2] = w; s_sc[3] = h;
}
void glFlush(void) {}
void glActiveTexture(GLenum t) { (void)t; }

void glMatrixMode(GLenum m) { s_matmode = m; }
void glLoadIdentity(void) { mtx_identity(cur_mtx()); }
void glLoadMatrixf(const GLfloat *m) { memcpy(cur_mtx(), m, 64); }

static void mult_cur(const float *m) {
    float *c = cur_mtx();
    mtx_mul(c, c, m);
}

void glScalef(GLfloat x, GLfloat y, GLfloat z) {
    float m[16];
    mtx_identity(m);
    m[0] = x; m[5] = y; m[10] = z;
    mult_cur(m);
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
    float len = sqrtf(x * x + y * y + z * z);
    if (len == 0.0f) return;
    x /= len; y /= len; z /= len;
    float a = angle * (float)M_PI / 180.0f, c = cosf(a), s = sinf(a), t = 1 - c;
    float m[16] = {
        x * x * t + c,     y * x * t + z * s, x * z * t - y * s, 0,
        x * y * t - z * s, y * y * t + c,     y * z * t + x * s, 0,
        x * z * t + y * s, y * z * t - x * s, z * z * t + c,     0,
        0, 0, 0, 1,
    };
    mult_cur(m);
}

void glFrustumf(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) {
    float m[16] = {0};
    m[0] = 2 * n / (r - l);
    m[5] = 2 * n / (t - b);
    m[8] = (r + l) / (r - l);
    m[9] = (t + b) / (t - b);
    m[10] = -(f + n) / (f - n);
    m[11] = -1;
    m[14] = -2 * f * n / (f - n);
    mult_cur(m);
}

void glOrthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) {
    float m[16] = {0};
    m[0] = 2 / (r - l);
    m[5] = 2 / (t - b);
    m[10] = -2 / (f - n);
    m[12] = -(r + l) / (r - l);
    m[13] = -(t + b) / (t - b);
    m[14] = -(f + n) / (f - n);
    m[15] = 1;
    mult_cur(m);
}

void glGetFloatv(GLenum pname, GLfloat *params) {
    switch (pname) {
    case GL_MODELVIEW_MATRIX: memcpy(params, s_m[0], 64); break;
    case GL_PROJECTION_MATRIX: memcpy(params, s_m[1], 64); break;
    case GL_TEXTURE_MATRIX: memcpy(params, s_m[2], 64); break;
    case GL_VIEWPORT:
        for (int i = 0; i < 4; i++) params[i] = (float)s_vp[i];
        break;
    default: break;
    }
}

void glLightfv(GLenum light, GLenum pname, const GLfloat *v) {
    if (light != GL_LIGHT0) return;
    switch (pname) {
    case GL_AMBIENT: memcpy(s_light_amb, v, 16); break;
    case GL_DIFFUSE: memcpy(s_light_dif, v, 16); break;
    case GL_POSITION: {
        /* stored in eye space, transformed by the modelview current at call time */
        const float *m = s_m[0];
        for (int r = 0; r < 4; r++)
            s_light_pos[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3];
        if (s_light_pos[3] == 0.0f) { /* directional: normalise */
            float l = sqrtf(s_light_pos[0] * s_light_pos[0] + s_light_pos[1] * s_light_pos[1] +
                            s_light_pos[2] * s_light_pos[2]);
            if (l > 0) for (int i = 0; i < 3; i++) s_light_pos[i] /= l;
        }
        break;
    }
    default: break;
    }
}

void glLightModelfv(GLenum pname, const GLfloat *v) {
    if (pname == GL_LIGHT_MODEL_AMBIENT) memcpy(s_model_amb, v, 16);
}

void glFogf(GLenum pname, GLfloat param) {
    switch (pname) {
    case GL_FOG_START: s_fog_start = param; break;
    case GL_FOG_END: s_fog_end = param; break;
    default: break; /* the game only uses GL_LINEAR */
    }
}
void glFogfv(GLenum pname, const GLfloat *v) {
    if (pname == GL_FOG_COLOR) memcpy(s_fog_color, v, 16);
    else glFogf(pname, v[0]);
}

/* ------------------------------------------------------------------ textures */

void glGenTextures(GLsizei n, GLuint *out) {
    for (int k = 0; k < n; k++) {
        out[k] = 0;
        for (int i = 1; i < MAX_TEXTURES; i++) {
            if (!s_tex[i].used) {
                memset(&s_tex[i], 0, sizeof s_tex[i]);
                s_tex[i].used = true;
                s_tex[i].min = s_tex[i].mag = GPU_LINEAR;
                s_tex[i].ws = s_tex[i].wt = GPU_REPEAT;
                out[k] = i;
                break;
            }
        }
    }
}

void glDeleteTextures(GLsizei n, const GLuint *ids) {
    for (int k = 0; k < n; k++) {
        GLuint i = ids[k];
        if (i == 0 || i >= MAX_TEXTURES || !s_tex[i].used) continue;
        if (s_tex[i].has_data) tex_delete(&s_tex[i].tex);
        memset(&s_tex[i], 0, sizeof s_tex[i]);
        if (s_bound == i) s_bound = 0;
    }
}

void glBindTexture(GLenum target, GLuint id) {
    (void)target;
    s_bound = id < MAX_TEXTURES ? id : 0;
}

static void apply_params(GLTex *t) {
    if (!t->has_data) return;
    C3D_TexSetFilter(&t->tex, t->mag, t->min);
    C3D_TexSetWrap(&t->tex, t->ws, t->wt);
}

static GPU_TEXTURE_WRAP_PARAM wrap_mode(GLint v) {
    switch (v) {
    case GL_CLAMP_TO_EDGE: return GPU_CLAMP_TO_EDGE;
    case GL_MIRRORED_REPEAT: return GPU_MIRRORED_REPEAT;
    default: return GPU_REPEAT;
    }
}

void glTexParameteri(GLenum target, GLenum pname, GLint v) {
    (void)target;
    GLTex *t = &s_tex[s_bound];
    if (!s_bound || !t->used) return;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: t->min = (v == GL_NEAREST || v == GL_NEAREST_MIPMAP_NEAREST || v == GL_NEAREST_MIPMAP_LINEAR) ? GPU_NEAREST : GPU_LINEAR; break;
    case GL_TEXTURE_MAG_FILTER: t->mag = v == GL_NEAREST ? GPU_NEAREST : GPU_LINEAR; break;
    case GL_TEXTURE_WRAP_S: t->ws = wrap_mode(v); break;
    case GL_TEXTURE_WRAP_T: t->wt = wrap_mode(v); break;
    default: return;
    }
    apply_params(t);
}

void glTexParameterf(GLenum target, GLenum pname, GLfloat v) {
    glTexParameteri(target, pname, (GLint)v);
}

/* Morton index of (x, y) inside an 8x8 tile. */
static inline u32 morton8(u32 x, u32 y) {
    return (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
}

void glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                  GLenum fmt, GLenum type, const GLvoid *pixels) {
    (void)target; (void)ifmt; (void)border; (void)fmt;
    GLTex *t = &s_tex[s_bound];
    if (!s_bound || !t->used || level != 0) return;
    if (t->has_data) tex_delete(&t->tex);
    t->has_data = false;
    bool rgba4 = type == GL_UNSIGNED_SHORT_4_4_4_4;
    if (!C3D_TexInit(&t->tex, w, h, rgba4 ? GPU_RGBA4 : GPU_RGBA8)) return;
    t->has_data = true;
    t->w = w;
    t->h = h;
    t->transposed = false;
    u8 *dst = t->tex.data;
    memset(dst, 0, t->tex.size);
    if (pixels) {
        /* GL rows go up from t = 0; the shader flips t, so row 0 is the top row of the
         * tex3ds-style image (stored last in PICA tile order). */
        for (int y = 0; y < h; y++) {
            u32 fy = (u32)(h - 1 - y);
            for (int x = 0; x < w; x++) {
                u32 tile = (fy / 8) * (w / 8) + (x / 8);
                u32 idx = tile * 64 + morton8(x & 7, fy & 7);
                if (rgba4) {
                    u16 c = ((const u16 *)pixels)[y * w + x];
                    ((u16 *)dst)[idx] = c;
                } else {
                    const u8 *c = (const u8 *)pixels + (y * w + x) * 4;
                    u8 *o = dst + idx * 4;
                    o[0] = c[3]; o[1] = c[2]; o[2] = c[1]; o[3] = c[0];
                }
            }
        }
    }
    GSPGPU_FlushDataCache(dst, t->tex.size);
    apply_params(t);
}

bool glTexImageT3X(const char *path, int *w, int *h) {
    GLTex *t = &s_tex[s_bound];
    if (!s_bound || !t->used) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (t->has_data) tex_delete(&t->tex);
    t->has_data = false;
    Tex3DS_Texture t3x = Tex3DS_TextureImportStdio(f, &t->tex, NULL, false);
    fclose(f);
    if (!t3x) return false;
    Tex3DS_TextureFree(t3x);
    tex_to_vram(&t->tex);
    {
        const char *base = strrchr(path, '/');
        sys_log("tex: %s %lu KB %s", base ? base + 1 : path, (unsigned long)(t->tex.size / 1024),
                is_vram(t->tex.data) ? "vram" : "main");
    }
    t->has_data = true;
    t->w = t->tex.width;
    t->h = t->tex.height;
    t->transposed = false;
    if (w) *w = t->w;
    if (h) *h = t->h;
    apply_params(t);
    return true;
}

/* The game only copies the whole top screen (the pause backdrop, m_20f70.c), so x, y, w and h
 * are taken as that: the bound texture becomes a 256 x 512 framebuffer copy, filled when the
 * next frame is built (copy_top_to_texture). */
void glCopyTexImage2D(GLenum target, GLint level, GLenum ifmt, GLint x, GLint y, GLsizei w,
                      GLsizei h, GLint border) {
    (void)target; (void)level; (void)ifmt; (void)x; (void)y; (void)w; (void)h; (void)border;
    GLTex *t = &s_tex[s_bound];
    if (!s_bound || !t->used) return;
    if (!t->has_data || !t->transposed || t->tex.width != 256 || t->tex.height != 512) {
        gfxGL_WaitIdle(); /* the render thread may still draw with the old texture */
        if (t->has_data) tex_delete(&t->tex);
        t->has_data = false;
        if (!C3D_TexInit(&t->tex, 256, 512, GPU_RGBA8)) return;
        t->has_data = true;
        t->w = 256;
        t->h = 512;
        t->transposed = true;
        memset(t->tex.data, 0, t->tex.size); /* black until the copy lands */
        GSPGPU_FlushDataCache(t->tex.data, t->tex.size);
    }
    apply_params(t);
    s_copy_req = s_bound;
}

/* Returns black. The game reads single pixels twice, and nothing reads the results: the
 * pixel under the last touch every 180 race frames (tick.c, into 0x5c57c) and one texel of
 * texture 0xa4 at race setup (m_0d800.c fn_0000fc28, into 0x3eb8c). Reading the GPU's
 * picture back would mean waiting for it mid-frame, so it isn't done. */
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, GLvoid *px) {
    (void)x; (void)y; (void)fmt; (void)type;
    memset(px, 0, (size_t)w * h * 4);
}
