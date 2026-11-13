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

static Packet *s_pk, *s_pkbuf[2];
static int s_npk, s_last_draws;
static int s_prev_npk, s_frame;
static float s_cmd_peak;
static u8 *s_ring[2];
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

static void *ring_alloc(u32 size) {
    size = (size + 15) & ~15u;
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

void gfxGL_Init(void) {
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4);
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

    s_pkbuf[0] = malloc(sizeof(Packet) * MAX_PACKETS);
    s_pkbuf[1] = malloc(sizeof(Packet) * MAX_PACKETS);
    s_pk = s_pkbuf[0];
    s_ring[0] = linearAlloc(RING_SIZE);
    s_ring[1] = linearAlloc(RING_SIZE);
    for (int i = 0; i < 3; i++) mtx_identity(s_m[i]);
}

void gfxGL_Exit(void) {
    for (int i = 0; i < MAX_TEXTURES; i++)
        if (s_tex[i].has_data) C3D_TexDelete(&s_tex[i].tex);
    linearFree(s_ring[0]);
    linearFree(s_ring[1]);
    free(s_pkbuf[0]);
    free(s_pkbuf[1]);
    shaderProgramFree(&s_prog);
    DVLB_Free(s_dvlb);
    C3D_RenderTargetDelete(s_top_l);
    C3D_RenderTargetDelete(s_top_r);
    C3D_RenderTargetDelete(s_bot);
    C3D_Fini();
}

void gfxGL_SetStereo(float iod) { s_iod = iod; }

void gfxGL_BeginFrame(void) {
    s_pk = s_pkbuf[s_frame & 1];
    s_npk = 0;
    s_ring_idx ^= 1;
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

static void set_fog(const Packet *p) {
    C3D_TexEnv *env = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env);
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    if (!p->fog) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, u_fog, 1.0f, 0.0f, 0, 0);
        return;
    }
    /* f = (end - d) / (end - start), d = -eye.z */
    float inv = 1.0f / (p->fog_end - p->fog_start);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_fog, p->fog_end * inv, inv, 0, 0);
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

static void replay(const Packet *p, int scr_w, float eye) {
    u32 x, y, w, h;
    fb_rect(scr_w, p->vp, &x, &y, &w, &h);
    if (!w || !h) return; /* a 0-size viewport is undefined on the PICA: skip the draw */
    C3D_SetViewport(x, y, w, h);
    if (p->scissor) {
        u32 sx, sy, sw, sh;
        fb_rect(scr_w, p->sc, &sx, &sy, &sw, &sh);
        u32 ex = sx + sw, ey = sy + sh;
        if (ex > 240) ex = 240;
        if (ey > (u32)scr_w) ey = scr_w;
        if (sx >= ex || sy >= ey) return; /* scissored away entirely */
        C3D_SetScissor(GPU_SCISSOR_NORMAL, sx, sy, ex, ey);
    } else {
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    }

    C3D_Mtx proj = p->proj;
    if (eye != 0.0f && p->persp) {
        /* stereo skew: x_ndc += eye * (1 - D / w), screen plane D = 2 units in front */
        const float D = 2.0f;
        for (int j = 0; j < 4; j++) proj.r[0].c[j] += eye * proj.r[3].c[j];
        proj.r[0].w -= eye * D;
    }
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_proj, &proj);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_mv, &p->mv);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_tm, &p->tm);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_color, p->color.x, p->color.y, p->color.z, p->color.w);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_lpos, p->lpos.x, p->lpos.y, p->lpos.z, p->lpos.w);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_lamb, p->lamb.x, p->lamb.y, p->lamb.z, p->lamb.w);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_ldif, p->ldif.x, p->ldif.y, p->ldif.z, p->ldif.w);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_lflag, p->lflag.x, 0, 0, 0);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_tcflip, p->tcflip.x, p->tcflip.y, 0, 0);

    C3D_DepthTest(p->depth_test, p->depth_test ? p->depth_func : GPU_ALWAYS,
                  p->kind == PK_CLEAR
                      ? ((p->clear_mask & GL_COLOR_BUFFER_BIT ? GPU_WRITE_COLOR : 0) |
                         (p->clear_mask & GL_DEPTH_BUFFER_BIT ? GPU_WRITE_DEPTH : 0))
                      : (p->depth_write ? GPU_WRITE_ALL : GPU_WRITE_COLOR));
    C3D_DepthMap(!p->wbuf, p->wbuf ? p->wscale : -1.0f, 0.0f);
    C3D_CullFace(p->cull ? p->cull_mode : GPU_CULL_NONE);
    if (p->blend)
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, p->src, p->dst, p->src, p->dst);
    else
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    set_fog(p);

    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    const GLTex *t = p->tex < MAX_TEXTURES ? &s_tex[p->tex] : NULL;
    if (p->texturing && t && t->has_data) {
        C3D_TexBind(0, (C3D_Tex *)&t->tex);
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
    } else {
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    }

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

static void draw_bottom_idle(void) {
    float tc[8] = {0, 0, s_idle_u, 0, 0, s_idle_v, s_idle_u, s_idle_v}; /* y down: t = 0 on top */
    static const float pos[8] = {0, 0, 320, 0, 0, 240, 320, 240};
    gfxGL_OverlayBegin(GFX_SCREEN_BOTTOM);
    s_blend = false;
    glBindTexture(GL_TEXTURE_2D, s_idle_tex);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, pos);
    glTexCoordPointer(2, GL_FLOAT, 0, tc);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gfxGL_OverlayEnd();
}

void gfxGL_DrawBottomIdle(void) {
    if (s_idle_tex) draw_bottom_idle();
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
static void frame_begin_watchdog(void) {
    C3D_FrameSync();
    u32 t0 = sys_ms();
    bool reported = false;
    sys_stage("render (waiting for the GPU)");
    while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
        if (!reported && sys_ms() - t0 > 2000) {
            reported = true;
            sys_log("gpu: HANG -- frame %d has not finished after 2 s", s_frame - 1);
            dump_frame("hung frame", s_frame - 1, s_pk == s_pkbuf[0] ? s_pkbuf[1] : s_pkbuf[0], s_prev_npk);
        }
        svcSleepThread(1000000);
    }
    if (reported) sys_log("gpu: frame %d finished after %lu ms", s_frame - 1, (unsigned long)(sys_ms() - t0));
    sys_stage("render (building GPU commands)");
}

void gfxGL_EndFrame(void) {
    if (s_idle_tex) {
        bool bottom = false;
        for (int i = 0; i < s_npk && !bottom; i++) bottom = s_pk[i].screen == GFX_SCREEN_BOTTOM;
        if (!bottom) draw_bottom_idle();
    }
    float iod = s_iod;
    gfxSet3D(iod > 0.0f);
    s_frame++;
    if (s_frame <= 3) dump_frame("frame", s_frame, s_pk, s_npk);
    frame_begin_watchdog();
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
        for (int i = 0; i < s_npk; i++) {
            const Packet *p = &s_pk[i];
            if (p->screen != want) continue;
            replay(p, scr_w, eye);
            draws++;
        }
    }
    GSPGPU_FlushDataCache(s_ring[s_ring_idx], s_ring_off);
    flush_game_vbs();
    float use = C3D_GetCmdBufUsage();
    if (use > s_cmd_peak) {
        s_cmd_peak = use;
        if (use > 0.5f)
            sys_log("gpu: command buffer %d%% full (frame %d, %d draws)", (int)(use * 100), s_frame, draws);
    }
    C3D_FrameEnd(0);
    if (s_frame <= 3) sys_log("gpu: frame %d submitted, %d draws, ring %lu KB, cmdbuf %d%%", s_frame,
                              draws, (unsigned long)(s_ring_off >> 10), (int)(use * 100));
    s_prev_npk = s_npk;
    s_last_draws = draws;
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

static void final_projection(C3D_Mtx *out, const float *proj) {
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
    to_c3d(&p->tm, s_m[2]);
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

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
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
        if (s_tex[i].has_data) C3D_TexDelete(&s_tex[i].tex);
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
    if (t->has_data) C3D_TexDelete(&t->tex);
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
    if (t->has_data) C3D_TexDelete(&t->tex);
    t->has_data = false;
    Tex3DS_Texture t3x = Tex3DS_TextureImportStdio(f, &t->tex, NULL, false);
    fclose(f);
    if (!t3x) return false;
    Tex3DS_TextureFree(t3x);
    t->has_data = true;
    t->w = t->tex.width;
    t->h = t->tex.height;
    t->transposed = false;
    if (w) *w = t->w;
    if (h) *h = t->h;
    apply_params(t);
    return true;
}

void glCopyTexImage2D(GLenum target, GLint level, GLenum ifmt, GLint x, GLint y, GLsizei w,
                      GLsizei h, GLint border) {
    /* TODO(#12): framebuffer -> texture copy for the pause backdrop. For now the texture
     * gets a dark fill so the paused screen still reads. */
    (void)target; (void)level; (void)ifmt; (void)x; (void)y; (void)border;
    GLTex *t = &s_tex[s_bound];
    if (!s_bound || !t->used) return;
    if (!t->has_data) {
        if (!C3D_TexInit(&t->tex, w, h, GPU_RGBA8)) return;
        t->has_data = true;
        t->w = w;
        t->h = h;
    }
    u32 *d = t->tex.data;
    for (u32 i = 0; i < t->tex.size / 4; i++) d[i] = 0x202020FF;
    GSPGPU_FlushDataCache(t->tex.data, t->tex.size);
    apply_params(t);
}

void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, GLvoid *px) {
    /* TODO(#12): only used for 1-pixel colour picks; returns black. */
    (void)x; (void)y; (void)fmt; (void)type;
    memset(px, 0, (size_t)w * h * 4);
}
