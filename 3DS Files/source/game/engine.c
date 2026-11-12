/* Engine core of the iOS game: GL state helpers, timer/pause, touch buttons, matrices,
 * texture table (LoadTPL), sprite and text drawing, resource file I/O, gluProject.
 * Functional translation of [0x27c0, 0x5ac4) (game functions only). */
#include "ios.h"

/* Texture table (slot 0x3e960): 260 entries of 0x20 bytes at 0x4060c.
 *   +0x00 GLuint name   +0x04 float potW  +0x08 float potH  +0x0c float w  +0x10 float h
 *   +0x14 float u (=w/potW)  +0x18 float v (=h/potH)  +0x1c u8 loaded */
#define ENG_TEX(id) ((uint8_t *)G(0x4060c) + (id) * 0x20)
/* Currently bound texture name cache (slot 0x3e95c), -1 = unknown. */
#define ENG_BOUND G_S32(0x40608)

/* 0x000027c0 -- main(): NSAutoreleasePool + UIApplicationMain. The 3DS main loop lives in
 * the platform layer (game_main.c / sys.c); this only remains for completeness. */
int fn_000027c0(int p1, int p2) {
    (void)p1;
    (void)p2;
    return 0;
}

/* 0x00003ccc -- sets flag 0x3f814 */
void fn_00003ccc(void) {
    G_U8(0x3f814) = 1;
}

/* 0x00003cdc -- GL_COLOR_MATERIAL on/off */
void fn_00003cdc(int p1) {
    if (p1 == 1) {
        glEnable(GL_COLOR_MATERIAL);
        return;
    }
    glDisable(GL_COLOR_MATERIAL);
}

/* 0x00003cf8 -- cull face mode (1 front, 2 back, 3 both) */
void fn_00003cf8(int p1) {
    GLenum mode;
    if (p1 == 3)
        mode = GL_FRONT_AND_BACK;
    else if (p1 == 2)
        mode = GL_BACK;
    else if (p1 == 1)
        mode = GL_FRONT;
    else
        return;
    glCullFace(mode);
}

/* 0x00003d20 -- SetFog(type, start, end, near, far, color 0x00BBGGRR): linear GL fog */
void fn_00003d20(int type, float start, float end, float nearz, float farz, unsigned color) {
    GLfloat c[4];
    (void)type;
    (void)nearz;
    (void)farz;
    c[0] = (float)(int)(color & 0xff) / 255.0f;
    c[1] = (float)(int)((color >> 8) & 0xff) / 255.0f;
    c[2] = (float)(int)((color >> 16) & 0xff) / 255.0f;
    c[3] = 0.0f; /* the original leaves the 4th component as stack garbage */
    glFogfv(GL_FOG_COLOR, c);
    glFogf(GL_FOG_MODE, 9729.0f); /* GL_LINEAR as float */
    glFogf(GL_FOG_START, start);
    glFogf(GL_FOG_END, end);
    glEnable(GL_FOG);
}

/* 0x00003da4 -- clear color (0xAABBGGRR) + clear color and depth */
void fn_00003da4(int p1) {
    uint32_t c = (uint32_t)p1;
    glClearColor((float)(c & 0xff) / 255.0f, (float)((c >> 8) & 0xff) / 255.0f,
                 (float)((c >> 16) & 0xff) / 255.0f, (float)(c >> 24) / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/* 0x00003e0c -- light model ambient from 0x00BBGGRR */
void fn_00003e0c(int p1) {
    uint32_t c = (uint32_t)p1;
    GLfloat a[4];
    a[0] = (float)((double)(int)(c & 0xff) / 255.0);
    a[1] = (float)((double)(int)((c >> 8) & 0xff) / 255.0);
    a[2] = (float)((double)(int)((c >> 16) & 0xff) / 255.0);
    a[3] = 1.0f;
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, a);
}

/* 0x00003e78 -- light 0 position (w = 0.01) */
void fn_00003e78(float x, float y, float z) {
    GLfloat p[4];
    p[0] = x;
    p[1] = y;
    p[2] = z;
    p[3] = u2f(0x3c23d70a); /* 0.01f */
    glLightfv(GL_LIGHT0, GL_POSITION, p);
}

/* 0x00003ea0 -- empty */
void fn_00003ea0(void) {
}

/* 0x00003ea4 -- light 0 diffuse + specular from 0x00BBGGRR */
void fn_00003ea4(int p1) {
    uint32_t c = (uint32_t)p1;
    GLfloat d[4];
    d[0] = (float)((double)(int)(c & 0xff) / 255.0);
    d[1] = (float)((double)(int)((c >> 8) & 0xff) / 255.0);
    d[2] = (float)((double)(int)((c >> 16) & 0xff) / 255.0);
    d[3] = 1.0f;
    glLightfv(GL_LIGHT0, GL_DIFFUSE, d);
    glLightfv(GL_LIGHT0, GL_SPECULAR, d);
}

/* 0x00003f24 -- read one RGBA pixel at (x, y) (y from the top) into out[4] */
void fn_00003f24(int p1, int p2, int p3) {
    uint8_t px[4];
    uint8_t *out = (uint8_t *)p3;
    int y = (int)((~(uint32_t)p2 | 0xffff0000u) + G_U16(0x42938));
    glFlush();
    glReadPixels(p1, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    out[0] = px[0];
    out[1] = px[1];
    out[2] = px[2];
    out[3] = px[3];
}

/* 0x00003f7c -- viewport + scissor from fractions of the framebuffer size 0x42938
 * (u16 lo = *0x427fc, u16 hi = *0x42800): x/w scale with lo but go to GL's y/height,
 * y/h scale with hi and go to GL's x/width (rotated landscape-on-portrait framebuffer). */
void fn_00003f7c(float x, float y, float w, float h) {
    uint32_t wh = G_U32(0x42938);
    float fw = (float)(int)(wh & 0xffff); /* lo */
    float fh = (float)(int)(wh >> 16);    /* hi */
    float a = fh * y, b = fw * x, c = fh * h, d = fw * w;
    glViewport((GLint)a, (GLint)b, (GLsizei)c, (GLsizei)d);
    glDepthRangef(0.0f, 1.0f);
    glScissor((GLint)(uint32_t)a, (GLint)(uint32_t)b, (GLsizei)(uint32_t)c,
              (GLsizei)(uint32_t)d);
    glEnable(GL_SCISSOR_TEST);
}

/* 0x00004028 -- engine init: framebuffer size, timer, texture table, virtual screen
 * (640x480 if 0x3f83c bit 0 else 720x480), 2D ortho and 3D perspective matrices */
void fn_00004028(void) {
    int w;
    uint32_t wh;
    G_U16(0x42938) = (uint16_t)G_U32(0x427fc);
    G_U8(0x482c0) = 0;
    G_U16(0x4293a) = (uint16_t)G_U32(0x42800);
    G_U8(0x42804) = 0;
    G_S32(0x482c4) = 0;
    G_S32(0x482c4) = fn_00004208();
    fn_000055b8();
    for (int i = 0; i <= 0x2060; i += 0x20)
        ENG_TEX(0)[i + 0x1c] = 0; /* 260 entries: not loaded */
    w = (G_U8(0x3f83c) & 1) ? 640 : 720;
    G_S32(0x40368) = w;    /* virtual screen width */
    G_S32(0x40374) = 480;  /* virtual screen height */
    G_S32(0x40380) = 0;
    G_S32(0x40384) = 0;
    G_S32(0x4037c) = 480;
    G_S32(0x40370) = w;
    /* ortho(top 0, bottom 480, left 0, right w, near 0, far 1) */
    fn_00037d0c(G(0x42838), 0.0f, 480.0f, 0.0f, (float)w, 0.0f, 1.0f);
    wh = G_U32(0x42938);
    fn_00037c78(G(0x42878), 90.0f, (float)(wh & 0xffff) / (float)(wh >> 16), 10.0f,
                10000.0f);
    fn_00003cf8(1);
    G_U8(0x42760) = 'Z'; /* Euler rotation order used by fn_00004620 */
    G_U8(0x42764) = 'Y';
    G_U8(0x42768) = 'X';
    G_S32(0x4036c) = G_S32(0x40368) / 2;
    G_S32(0x40378) = G_S32(0x40374) / 2;
}

/* 0x00004208 -- game clock in ms (mach_absolute_time); frozen while paused */
int fn_00004208(void) {
    if (G_U8(0x482c0) == 1)
        return G_S32(0x482c8);
    return (int)(ios_ms() - G_U32(0x482c4));
}

/* 0x00004254 -- begin 3D frame: clear (color only if p1), reset bind cache, arrays, depth */
void fn_00004254(int p1) {
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(p1 != 0 ? (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT) : GL_DEPTH_BUFFER_BIT);
    ENG_BOUND = -1;
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    fn_000042b0(1, GL_LESS, 1);
}

/* 0x000042b0 -- depth test (enable, func, write mask) */
void fn_000042b0(int p1, int p2, int p3) {
    glDepthMask(p3 == 1);
    if (p1 == 1) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc((GLenum)p2);
        return;
    }
    glDisable(GL_DEPTH_TEST);
}

/* 0x000042e0 -- reset frame/timing counters */
void fn_000042e0(void) {
    G_S32(0x3f830) = fn_00004208();
    G_S32(0x3f82c) = 0;
    G_U8(0x3e914) = 1;
    G_S32(0x3f834) = 0;
    G_S32(0x3e924) = 0;
    G_S32(0x3e920) = 0;
    G_U8(0x3e918) = 1;
    G_S32(0x3f838) = 0;
    G_U8(0x3e91c) = 1;
    G_S32(0x3f818) = 0;
}

/* 0x0000436c -- debug FPS text (always prints 0.000) */
void fn_0000436c(int p1) {
    char buf[64];
    if ((p1 & 0xff) == 0)
        return;
    snprintf(buf, sizeof(buf), "%.3f", 0.0);
    glColor4f(u2f(0x3f804674), u2f(0x3f804674), u2f(0x3f804674), u2f(0x3f804674));
    fn_00005394((float)G_S32(0x4036c) + -80.0f, (float)G_S32(0x40374) + -30.0f, 0.0f,
                (int)buf, 0.5f, 0.5f, 0);
}

/* 0x00004400 -- rand() % n */
int fn_00004400(int n) {
    return rand() % n;
}

/* 0x00004414 -- language code "ENG" */
void fn_00004414(char *out) {
    memcpy(out, "ENG", 4);
}

/* 0x0000442c -- touch buttons: hit-test the 20 buttons at 0x40388 against the 10 touches.
 * Button (0x20): float x, y, r; float relx @0xc, rely @0x10; u8 held @0x14,
 * u8 pressed @0x15 (edge), u8 active @0x16, u8 @0x17, u8 @0x18, int @0x1c */
void fn_0000442c(void) {
    uint8_t *btns = G(0x40388);
    uint8_t *touch = G(0x4268c);
    for (int off = 0;; off += 0x20) {
        uint8_t *b = btns + off;
        if (AT_U8(b, 0x16) != 0) {
            float r = AT_F32(b, 8);
            int i;
            AT_U8(b, 0x15) = 0;
            for (i = 0; i < 10; i++) {
                uint8_t *t = touch + i * 0x14;
                float x, y, tx, ty;
                if (AT_U8(t, 0) == 0)
                    continue;
                x = AT_F32(b, 0);
                tx = AT_F32(t, 4);
                if (!(tx > x - r) || !(tx < x + r))
                    continue;
                y = AT_F32(b, 4);
                ty = AT_F32(t, 8);
                if (!(ty > y - r) || !(ty < y + r))
                    continue;
                if (AT_U8(b, 0x14) == 0)
                    AT_U8(b, 0x15) = 1;
                AT_U8(b, 0x14) = 1;
                AT_F32(b, 0xc) = (tx - x) / r;
                AT_F32(b, 0x10) = (ty - y) / r;
                break;
            }
            if (i == 10) {
                AT_U8(b, 0x14) = 0;
                AT_U8(b, 0x15) = 0;
            }
        }
        if (off == 0x260)
            return;
    }
}

/* 0x00004500 -- define touch button idx */
void fn_00004500(float x, float y, float r, int idx, int p5, int p6, int p7) {
    uint8_t *b = (uint8_t *)G(0x40388) + idx * 0x20;
    AT_F32(b, 0) = x;
    AT_F32(b, 4) = y;
    AT_F32(b, 8) = r;
    AT_U8(b, 0x14) = 0;
    AT_U8(b, 0x15) = 0;
    AT_U8(b, 0x16) = 1;
    AT_U8(b, 0x17) = (uint8_t)p5;
    AT_S32(b, 0x1c) = p6;
    /* +0x18 is the "draw me" flag (only fn_00023bd4 reads it). The original stores p7; the 3DS
     * port draws no on-screen buttons -- the physical buttons press them (game_main.c
     * feed_buttons) -- so it stays 0. */
    (void)p7;
    AT_U8(b, 0x18) = 0;
}

/* 0x00004540 -- is paused */
int fn_00004540(void) {
    return G_U8(0x482c0);
}

/* 0x00004550 -- request pause */
void fn_00004550(void) {
    G_U8(0x482c1) = 1;
}

/* 0x00004560 -- clear pause request */
void fn_00004560(void) {
    G_U8(0x482c1) = 0;
}

/* 0x00004570 -- enter pause if requested: freezes the clock, calls fn_000360fc; 1 if paused now */
int fn_00004570(void) {
    int t = fn_00004208();
    if (G_U8(0x482c0) != 0)
        return 0;
    if (G_U8(0x482c1) != 1)
        return 0;
    G_S32(0x482c8) = t;
    G_U8(0x482c0) = 1;
    fn_000360fc();
    return 1;
}

/* 0x000045a0 -- empty */
void fn_000045a0(void) {
}

/* 0x000045a4 -- leave pause: shifts the clock base by the paused time; 1 if it was paused.
 * (Unnamed function after 0x45a0, called from the tick at 0x9da8; not in symbols.csv.) */
int fn_000045a4(void) {
    int t;
    if (G_U8(0x482c0) != 1)
        return 0;
    G_U8(0x482c0) = 0;
    t = fn_00004208();
    G_S32(0x482c4) = t - G_S32(0x482c8) + G_S32(0x482c4);
    return 1;
}

/* 0x000045d0 -- Mtx34 (row-major) -> GL 4x4 (column-major) */
void *fn_000045d0(int p1, int p2) {
    const float *m = (const float *)p1;
    float *o = (float *)p2;
    o[0] = m[0];
    o[1] = m[4];
    o[2] = m[8];
    o[3] = 0.0f;
    o[4] = m[1];
    o[5] = m[5];
    o[6] = m[9];
    o[7] = 0.0f;
    o[8] = m[2];
    o[9] = m[6];
    o[10] = m[10];
    o[11] = 0.0f;
    o[12] = m[3];
    o[13] = m[7];
    o[14] = m[11];
    o[15] = 1.0f;
    return o;
}

/* 0x00004614 -- base + i * 0x30 (Mtx34 array element) */
int fn_00004614(int p1, int p2) {
    return p1 + p2 * 0x30;
}

/* 0x00004620 -- Euler rotation matrix from degrees v[3]; axis order from 0x42760/64/68:
 * m = R(c0, a0) * R(c1, a1) * R(c2, a2) */
int fn_00004620(int p1, int p2) {
    const float *v = (const float *)p2;
    float tmp[12];
    char c0 = (char)G_U8(0x42760), c1 = (char)G_U8(0x42764), c2 = (char)G_U8(0x42768);
    float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f; /* uninitialised in the original */
    const float d2r = u2f(0x3c8efa39);    /* 0.0174533f */
    if (c0 == 'X')
        a0 = v[0];
    else if (c1 == 'X')
        a1 = v[0];
    else if (c2 == 'X')
        a2 = v[0];
    if (c0 == 'Y')
        a0 = v[1];
    else if (c1 == 'Y')
        a1 = v[1];
    else if (c2 == 'Y')
        a2 = v[1];
    if (c0 == 'Z')
        a0 = v[2];
    else if (c1 == 'Z')
        a1 = v[2];
    else if (c2 == 'Z')
        a2 = v[2];
    fn_00037844((void *)p1, (int)(int8_t)c2, a2 * d2r);
    fn_00037844(tmp, (int)(int8_t)c1, a1 * d2r);
    fn_0003728c(tmp, (void *)p1, (void *)p1);
    fn_00037844(tmp, (int)(int8_t)c0, a0 * d2r);
    fn_0003728c(tmp, (void *)p1, (void *)p1);
    return p1;
}

/* 0x00004754 -- m = T(pos) * Euler(rot) */
int fn_00004754(int p1, int p2, int p3) {
    const float *t = (const float *)p2;
    float tmp[12];
    fn_00004620(p1, p3);
    fn_000371d0(tmp, t[0], t[1], t[2]);
    fn_0003728c(tmp, (void *)p1, (void *)p1);
    return p1;
}

/* 0x00004784 -- m = T(p) * Rz(deg) * T(-p): rotation about a pivot */
int fn_00004784(int p1, int p2, float p3) {
    const float *p = (const float *)p2;
    float r[12], t[12], rt[12];
    fn_00037844(r, 'Z', p3 * u2f(0x3c8efa39));
    fn_000371d0(t, -p[0], -p[1], -p[2]);
    fn_0003728c(r, t, rt);
    fn_000371d0(t, p[0], p[1], p[2]);
    fn_0003728c(t, rt, (void *)p1);
    return p1;
}

/* 0x000047f8 -- VECSet */
void fn_000047f8(int v, float x, float y, float z) {
    float *o = (float *)v;
    o[0] = x;
    o[1] = y;
    o[2] = z;
}

/* 0x00004800 -- VECCopy(dst, src) */
void fn_00004800(int p1, int p2) {
    float *d = (float *)p1;
    const float *s = (const float *)p2;
    d[0] = s[0];
    d[1] = s[1];
    d[2] = s[2];
}

/* 0x00004810 -- smallest power of two >= x (at least 2) */
float fn_00004810(float x) {
    float p = 2.0f;
    if (x > 2.0f) {
        do
            p = p + p;
        while (p < x);
    }
    return p;
}

/* 0x00004838 -- create an empty RGBA render texture of w x h (padded to POT).
 * tex: {GLuint name, float potW, potH, w, h, u, v}; flags pick the wrap modes. */
void fn_00004838(int tex, int w, int h, int flags) {
    uint8_t *t = (uint8_t *)tex;
    float fw = (float)w, fh = (float)h;
    float pw = 2.0f, ph = 2.0f;
    GLint wrap;
    AT_F32(t, 0xc) = fw;
    AT_F32(t, 0x10) = fh;
    if (fw > 2.0f) {
        do
            pw = pw + pw;
        while (pw < fw);
    }
    AT_F32(t, 4) = pw;
    if (fh > 2.0f) {
        do
            ph = ph + ph;
        while (ph < fh);
    }
    AT_F32(t, 8) = ph;
    AT_F32(t, 0x14) = fw / pw;
    AT_F32(t, 0x18) = fh / ph;
    glGenTextures(1, (GLuint *)t);
    glBindTexture(GL_TEXTURE_2D, AT_U32(t, 0));
    ENG_BOUND = -1;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    wrap = (flags & 0x40) ? GL_MIRRORED_REPEAT : (flags & 0x10) ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); /* sic: overrides */
    wrap = (flags & 0x80) ? GL_MIRRORED_REPEAT : (flags & 0x20) ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT); /* sic: overrides */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)AT_F32(t, 4), (GLsizei)AT_F32(t, 8), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

/* romfs:/textures.txt: "name origW origH potW potH fmt" per texture (platform cache). */
typedef struct {
    char name[48];
    int w, h;
} EngTexInfo;

static EngTexInfo *eng_texinfo;
static int eng_texinfo_n = -1;

static void eng_texinfo_load(void) {
    char line[160];
    int cap = 0;
    FILE *f;
    eng_texinfo_n = 0;
    f = fopen("romfs:/textures.txt", "r");
    if (!f) {
        sys_log("engine: romfs:/textures.txt missing\n");
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        EngTexInfo e;
        int pw, ph;
        if (sscanf(line, "%47s %d %d %d %d", e.name, &e.w, &e.h, &pw, &ph) < 3)
            continue;
        if (eng_texinfo_n == cap) {
            EngTexInfo *n;
            cap = cap ? cap * 2 : 256;
            n = realloc(eng_texinfo, cap * sizeof(*n));
            if (!n)
                break;
            eng_texinfo = n;
        }
        eng_texinfo[eng_texinfo_n++] = e;
    }
    fclose(f);
}

static const EngTexInfo *eng_texinfo_find(const char *name) {
    if (eng_texinfo_n < 0)
        eng_texinfo_load();
    for (int i = 0; i < eng_texinfo_n; i++)
        if (strcmp(eng_texinfo[i].name, name) == 0)
            return &eng_texinfo[i];
    return NULL;
}

/* 0x000049a0 -- LoadTPL body: load texture `name` (bundle "<name>.png") into table slot id.
 * iOS decoded the PNG with CoreGraphics into a POT RGBA bitmap (image in the top-left corner)
 * and uploaded RGBA8 (flags & 1) or RGBA4444. Here the romfs .t3x is already POT-padded the
 * same way; w/h (+0xc/+0x10) are the ORIGINAL image size from textures.txt, potW/potH
 * (+4/+8) the real texture size, so u/v = w/potW, h/potH address the image exactly.
 * *pixels receives the decoded bitmap on iOS (the only caller frees it): NULL here. */
void fn_000049a0(int p1, int p2, int p3, int p4) {
    int id = p1;
    const char *name = (const char *)p2;
    unsigned flags = (unsigned)p4;
    uint8_t *t = ENG_TEX(id);
    const EngTexInfo *info;
    char file[300], path[320];
    int tw = 0, th = 0;
    bool ok;
    float fw, fh, pw = 2.0f, ph = 2.0f;
    GLint wrap;

    fn_00004dac(id);
    glGenTextures(1, (GLuint *)t);
    glBindTexture(GL_TEXTURE_2D, AT_U32(ENG_TEX(id), 0));
    ENG_BOUND = -1;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    wrap = (flags & 0x40) ? GL_MIRRORED_REPEAT : (flags & 0x10) ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    wrap = (flags & 0x80) ? GL_MIRRORED_REPEAT : (flags & 0x20) ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glTexParameterf(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, 1.0f);

    snprintf(file, sizeof(file), "%s.png", name);
    ok = glTexImageT3X(ios_res_path(file, path, sizeof(path)), &tw, &th);
    if (!ok)
        sys_log("engine: texture %s failed to load\n", name);

    info = eng_texinfo_find(name);
    if (info) {
        fw = (float)info->w;
        fh = (float)info->h;
    } else {
        fw = (float)tw;
        fh = (float)th;
    }
    AT_F32(t, 0xc) = fw;
    AT_F32(t, 0x10) = fh;
    if (ok && tw > 0 && th > 0) {
        pw = (float)tw;
        ph = (float)th;
    } else {
        /* the original's own POT rounding (minimum 2) */
        if (fw > 2.0f) {
            do
                pw = pw + pw;
            while (pw < fw);
        }
        if (fh > 2.0f) {
            do
                ph = ph + ph;
            while (ph < fh);
        }
    }
    AT_F32(t, 4) = pw;
    AT_F32(t, 8) = ph;
    AT_F32(t, 0x14) = fw / pw;
    AT_F32(t, 0x18) = fh / ph;
    if (p3)
        *(void **)p3 = NULL;
    AT_U8(t, 0x1c) = 1;
}

/* 0x00004dac -- delete texture slot id */
int fn_00004dac(int p1) {
    uint8_t *t = ENG_TEX(p1);
    glDeleteTextures(1, (const GLuint *)t);
    AT_U8(t, 0x1c) = 0;
    if (ENG_BOUND == AT_S32(t, 0))
        ENG_BOUND = -1;
    return 1;
}

/* 0x00004df0 -- load texture and free the CPU copy */
void fn_00004df0(int p1, int p2, int p3) {
    void *pixels = NULL;
    fn_000049a0(p1, p2, (int)&pixels, p3);
    free(pixels);
}

/* 0x00004e08 -- bind texture slot id (cached) */
void fn_00004e08(int p1) {
    uint8_t *t = ENG_TEX(p1);
    if (AT_S32(t, 0) == ENG_BOUND)
        return;
    glBindTexture(GL_TEXTURE_2D, AT_U32(t, 0));
    ENG_BOUND = AT_S32(t, 0);
}

/* 0x00004e44 -- load Mtx34 as the GL modelview */
void fn_00004e44(int p1) {
    float m[16];
    glMatrixMode(GL_MODELVIEW);
    fn_000045d0(p1, (int)m);
    glLoadMatrixf(m);
}

/* 0x00004e68 -- 2D modelview: scale about a point, m (0x4276c) = T(p) * S(s) * T(-p) */
void fn_00004e68(float px, float py, float pz, float sx, float sy, float sz) {
    float s[12], t[12], st[12], gm[16];
    float *m = G(0x4276c);
    fn_00037230(s, sx, sy, sz);
    fn_000371d0(t, -px, -py, -pz);
    fn_0003728c(s, t, st);
    fn_000371d0(t, px, py, pz);
    fn_0003728c(t, st, m);
    glMatrixMode(GL_MODELVIEW);
    fn_000045d0((int)m, (int)gm);
    glLoadMatrixf(gm);
}

/* 0x00004f24 -- load the 2D ortho projection (0x42838) */
void fn_00004f24(void) {
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(G(0x42838));
}

/* 0x00004f44 -- 3D camera: perspective projection (0x42878), optional screen roll
 * (0x3f81c bit 0, angle 0x3f820), look-at into the view matrix 0x4279c */
void fn_00004f44(int pos, int target, float upx, float upy, float upz) {
    float up[3];
    up[0] = upx;
    up[1] = upy;
    up[2] = upz;
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(G(0x42878));
    if (G_U8(0x3f81c) & 1)
        glRotatef(G_F32(0x3f820), 0.0f, 0.0f, 1.0f);
    fn_000379fc(G(0x4279c), (float *)pos, up, (float *)target);
}

/* 0x00004fb4 -- glColor4f from 0..255 components */
void fn_00004fb4(int r, int g, int b, int a) {
    const float k = u2f(0x3b80c73b); /* 1/255 */
    glColor4f((float)(unsigned)r * k, (float)(unsigned)g * k, (float)(unsigned)b * k,
              (float)(unsigned)a * k);
}

/* 0x00005008 -- glColor4f from packed 0xRRGGBBAA */
void fn_00005008(unsigned rgba) {
    glColor4f((float)(rgba >> 24) / 255.0f, (float)((rgba >> 16) & 0xff) / 255.0f,
              (float)((rgba >> 8) & 0xff) / 255.0f, (float)(rgba & 0xff) / 255.0f);
}

/* 0x00005068 -- begin 2D: texturing + blending, identity texture matrix, no cull,
 * optional depth clear, depth off, no normals, no lighting */
void fn_00005068(int p1) {
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glDisable(GL_CULL_FACE);
    if (p1 == 1)
        glClear(GL_DEPTH_BUFFER_BIT);
    fn_000042b0(0, GL_ALWAYS, 0);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_LIGHTING);
}

/* 0x000050cc -- draw a textured quad (triangle strip) at z = -z with texture slot tex.
 * UVs u0,v0 + du,dv are in image units and get scaled by the slot's u/v; flags: 1 flip U,
 * 2 flip V, 8 nearest filtering. mtx (Mtx34 or 0) transforms the vertices. */
void fn_000050cc(int tex, float x, float y, float w, float h, float z, float u0, float v0,
                 float du, float dv, int mtx, int flags) {
    float *vtx = G(0x42808); /* 4 x vec3 */
    float *tc = G(0x402f8);  /* 4 x vec2 */
    uint8_t *t = ENG_TEX(tex);
    float nz = -z, x1 = x + w, y1 = y + h;
    float u1 = u0 + du, v1 = v0 + dv;
    float ua, va, ub, vb, su, sv;
    GLfloat filt;
    vtx[0] = x;
    vtx[1] = y;
    vtx[2] = nz;
    vtx[3] = x1;
    vtx[4] = y;
    vtx[5] = nz;
    vtx[6] = x;
    vtx[7] = y1;
    vtx[8] = nz;
    vtx[9] = x1;
    vtx[10] = y1;
    vtx[11] = nz;
    if (mtx != 0)
        fn_00038090((void *)mtx, vtx, vtx, 4);
    ua = (flags & 1) ? u1 : u0;
    va = (flags & 2) ? v1 : v0;
    ub = (flags & 1) ? u0 : u1;
    vb = (flags & 2) ? v0 : v1;
    tc[0] = ua;
    tc[1] = va;
    tc[2] = ub;
    tc[3] = va;
    tc[4] = ua;
    tc[5] = vb;
    tc[6] = ub;
    tc[7] = vb;
    su = AT_F32(t, 0x14);
    sv = AT_F32(t, 0x18);
    for (int i = 0; i < 4; i++) {
        tc[i * 2] *= su;
        tc[i * 2 + 1] *= sv;
    }
    if (AT_S32(t, 0) != ENG_BOUND) {
        glBindTexture(GL_TEXTURE_2D, AT_U32(t, 0));
        ENG_BOUND = AT_S32(t, 0);
    }
    filt = (flags & 8) ? 9728.0f : 9729.0f; /* GL_NEAREST : GL_LINEAR, as floats */
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
    glVertexPointer(3, GL_FLOAT, 0, vtx);
    glTexCoordPointer(2, GL_FLOAT, 0, tc);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* 0x0000526c -- draw sprite: flags & 4 scales x/w/px by screenW/640 and y/h/py by
 * screenH/480; a non-zero angle (degrees) rotates about (px, py) */
void fn_0000526c(int tex, float x, float y, float w, float h, float z, float u0, float v0,
                 float du, float dv, float angle, float px, float py, int flags) {
    float m[12], piv[3];
    int mtx = 0;
    if (flags & 4) {
        float sy = (float)G_S32(0x40374) / 480.0f;
        float sx = (float)G_S32(0x40368) / 640.0f;
        py = sy * py;
        px = sx * px;
        y = sy * y;
        x = sx * x;
        h = sy * h;
        w = sx * w;
    }
    if (angle != 0.0f) {
        piv[0] = px;
        piv[1] = py;
        piv[2] = 0.0f;
        fn_00004784((int)m, (int)piv, angle);
        mtx = (int)m;
    }
    fn_000050cc(tex, x, y, w, h, z, u0, v0, du, dv, mtx, flags);
}

/* 0x00005354 -- draw whole texture as a sprite */
void fn_00005354(int tex, float x, float y, float w, float h, float z, int flags) {
    fn_0000526c(tex, x, y, w, h, z, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, flags);
}

/* 0x00005394 -- draw text with the font in texture slot 0 (16 x 8 cells of ASCII 32..127,
 * per-char u start at 0x48460 and width at 0x482e0); returns the width. nodraw: measure only
 * (and leaves GL state alone). */
float fn_00005394(float x, float y, float z, int str, float sx, float sy, int nodraw) {
    const char *s = (const char *)str;
    float *vtx = G(0x40318);
    float *tc = G(0x40348);
    float cx = x;
    if ((nodraw & 0xff) == 0) {
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        if (AT_S32(ENG_TEX(0), 0) != ENG_BOUND) {
            glBindTexture(GL_TEXTURE_2D, AT_U32(ENG_TEX(0), 0));
            ENG_BOUND = AT_S32(ENG_TEX(0), 0);
        }
    }
    if (*s != 0) {
        float hgt = sy * 40.0f;
        float wsc = sx * 40.0f;
        float y1 = hgt + y;
        float nz = -z;
        char c = *s++;
        do {
            int i = (int)(int8_t)c - 0x20;
            int row = i / 16, col = i % 16;
            float cw = G_F32(0x482e0 + i * 4);
            float cu = G_F32(0x48460 + i * 4);
            float adv = cw * 16.0f * wsc;
            float ue = cu + cw;
            float vt = (float)row * 0.125f + u2f(0x3a800000); /* + 1/1024 */
            float vbm = vt + u2f(0x3df80000);                /* + 0.121094 */
            float x0 = cx + (cu + (float)col * -0.0625f) * 16.0f * wsc;
            tc[0] = cu;
            tc[1] = vt;
            tc[2] = ue;
            tc[3] = vt;
            tc[4] = cu;
            tc[5] = vbm;
            tc[6] = ue;
            tc[7] = vbm;
            cx = x0 + adv;
            vtx[0] = x0;
            vtx[1] = y;
            vtx[2] = nz;
            vtx[3] = cx;
            vtx[4] = y;
            vtx[5] = nz;
            vtx[6] = x0;
            vtx[7] = y1;
            vtx[8] = nz;
            vtx[9] = cx;
            vtx[10] = y1;
            vtx[11] = nz;
            if ((nodraw & 0xff) == 0) {
                glVertexPointer(3, GL_FLOAT, 0, vtx);
                glTexCoordPointer(2, GL_FLOAT, 0, tc);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
            c = *s++;
        } while (c != 0);
    }
    return cx - x;
}

/* 0x00005594 -- copy an NSString's UTF-8 text into dst. On the port the "NSString" is
 * already a C string (e.g. ios_doc_dir()). */
void *fn_00005594(int p1, int p2) {
    strcpy((char *)p2, (const char *)p1);
    return (void *)p2;
}

/* 0x000055b8 -- empty (memory report hook) */
void fn_000055b8(void) {
}

/* 0x000055bc -- memory report (stub values on iOS too) */
void fn_000055bc(void) {
    puts("************* MEM FREE *************");
    printf("%.2f MB \n%.0f bytes \n", 0.0, 0.0);
}

/* 0x000055f8 -- CxAlloc32Mem1: allocation (no zero fill), asserts on failure */
void *fn_000055f8(int size) {
    void *p = ios_alloc((uint32_t)size);
    if (p == NULL)
        sys_log("assert: CxAlloc32Mem failed, requested %d KB\n", (int)((uint32_t)size >> 10));
    return p;
}

/* 0x00005654 -- CxFree (returned 1 on iOS) */
void fn_00005654(void *p) {
    ios_free(p);
}

/* 0x00005660 -- basename without directory and extension */
void fn_00005660(const char *path, char *out) {
    const char *s = strrchr(path, '/');
    char *d;
    strcpy(out, s ? s + 1 : path);
    d = strrchr(out, '.');
    if (d)
        *d = 0;
}

/* 0x00005694 -- fopenRes(name, mode): open a bundle resource (directories dropped);
 * mode must start with 'r'. Returns the FILE *. */
void *fn_00005694(const char *name, const char *mode) {
    char base[256], ext[256], file[520], path[600];
    const char *dot;
    if (mode[0] != 'r') {
        sys_log("assert: fopenRes mode[0]=='r'\n");
        return NULL;
    }
    fn_00005660(name, base);
    dot = strrchr(name, '.');
    if (dot) {
        strncpy(ext, dot + 1, sizeof(ext) - 1);
        ext[sizeof(ext) - 1] = 0;
    } else {
        ext[0] = 0;
    }
    if (ext[0])
        snprintf(file, sizeof(file), "%s.%s", base, ext);
    else
        snprintf(file, sizeof(file), "%s", base);
    ios_res_path(file, path, sizeof(path));
    return fopen(path, mode[1] == 't' ? "r" : mode);
}

/* 0x00005784 -- read a resource file in binary mode into *out (CxAlloc32Mem1); returns size */
int fn_00005784(void *out, int p2, const char *name) {
    (void)p2;
    return fn_00005794(out, name, 0);
}

/* 0x00005794 -- CxReadFile ("CxGetFileSize"): size the file, allocate (+1 and NUL-terminate
 * in text mode), read it; returns the size */
int fn_00005794(void *out, const char *name, int text) {
    char **pout = (char **)out;
    FILE *f = fn_00005694(name, "r");
    int size, left;
    uint8_t *dst;
    bool t = (text & 0xff) != 0;
    if (f == NULL) {
        sys_log("assert: CxGetFileSize fp (%s)\n", name);
        *pout = NULL;
        return 0;
    }
    fseek(f, 0, SEEK_END);
    size = (int)ftell(f);
    fclose(f);
    if (size == 0) {
        sys_log("assert: Cannot do CxGetFileSize on file %s\n", name);
        *pout = NULL;
        return 0;
    }
    *pout = fn_000055f8(size + (t ? 1 : 0));
    f = fn_00005694(name, t ? "rt" : "rb");
    if (f == NULL || *pout == NULL) {
        if (f)
            fclose(f);
        return size;
    }
    dst = (uint8_t *)*pout;
    left = size;
    while (left > 0) {
        size_t n = fread(dst, 1, (size_t)left, f);
        if (n == 0)
            break; /* the original loops forever on a short read */
        left -= (int)n;
        dst += n;
    }
    fclose(f);
    if (t)
        (*pout)[size] = 0;
    return size;
}

/* 0x000058b8 -- read a resource file as NUL-terminated text; returns the size */
int fn_000058b8(void *out, int p2, const char *name) {
    (void)p2;
    return fn_00005794(out, name, 1);
}

/* 0x000058c8 -- does the resource exist */
int fn_000058c8(int p1, const char *name) {
    FILE *f;
    (void)p1;
    f = fn_00005694(name, "r");
    if (f == NULL)
        return 0;
    fclose(f);
    return 1;
}

/* 0x000058f0 -- gluProject-like: obj (vec3) through mv and proj (GL column-major 4x4),
 * viewport vp (int[4], only w/h used) -> win (x, y from the top, depth 0..1).
 * Divides by eye-space -z; returns 0 if that is 0. */
int fn_000058f0(int obj, int mv, int proj, int vp, void *win) {
    const float *o = (const float *)obj;
    const float *M = (const float *)mv;
    const float *P = (const float *)proj;
    const int *v = (const int *)vp;
    float *out = (float *)win;
    float ex, ey, ez, ew, nz, inv, nx, ny, nd;
    ex = M[0] * o[0] + M[4] * o[1] + M[8] * o[2] + M[12];
    ey = M[1] * o[0] + M[5] * o[1] + M[9] * o[2] + M[13];
    ez = M[2] * o[0] + M[6] * o[1] + M[10] * o[2] + M[14];
    ew = M[3] * o[0] + M[7] * o[1] + M[11] * o[2] + M[15];
    nz = -ez;
    if ((double)nz == 0.0)
        return 0;
    inv = 1.0f / nz;
    nx = (P[0] * ex + P[4] * ey + P[8] * ez + P[12] * ew) * inv;
    ny = (P[1] * ex + P[5] * ey + P[9] * ez + P[13] * ew) * inv;
    nd = (P[2] * ex + P[6] * ey + P[10] * ez + P[14] * ew) * inv;
    out[0] = (float)(((double)nx * 0.5 + 0.5) * (double)v[2]);
    out[1] = (float)((0.5 - (double)ny * 0.5) * (double)v[3]);
    out[2] = (float)(((double)nd + 1.0) * 0.5);
    return 1;
}
