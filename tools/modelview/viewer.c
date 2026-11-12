/* Model viewer: GL-layer bring-up test. Build with `make MODELVIEW=1`.
 * L/R pick model, circle pad rotates, up/down zoom, START quits. Bottom screen shows a 2D
 * textured quad (font atlas) to exercise ortho + sprites. */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gles.h"
#include "sys.h"

static const char *const k_models[] = {
    "car_01_body", "car_02_body", "car_05_body", "car_11_body", "face_01", "helmet",
    "cup", "obj_00", "obj_10", "flag", "3d_logo", "clock",
};
#define NMODELS (int)(sizeof k_models / sizeof k_models[0])

static int s_cur = -1, s_count;
static void *s_verts;
static GLuint s_tex, s_font;
static float s_yaw, s_pitch = 20, s_dist = 60;

static void load(int idx) {
    char path[128];
    if (s_verts) gfxGL_LinearFree(s_verts);
    s_verts = NULL;
    snprintf(path, sizeof path, "romfs:/%s.bin", k_models[idx]);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    s_verts = gfxGL_LinearAlloc(sz);
    fread(s_verts, 1, sz, f);
    fclose(f);
    s_count = *(u32 *)((u8 *)s_verts + 0x1C);
    gfxGL_Flush(s_verts, sz);
    snprintf(path, sizeof path, "romfs:/%s.t3x", k_models[idx]);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexImageT3X(path, NULL, NULL);
    s_cur = idx;
    sys_log("model %s: %d verts", k_models[idx], s_count);
}

void app_init(void) {
    glGenTextures(1, &s_tex);
    glGenTextures(1, &s_font);
    glBindTexture(GL_TEXTURE_2D, s_font);
    glTexImageT3X("romfs:/font.t3x", NULL, NULL);
    load(0);
}

void app_exit(void) { gfxGL_LinearFree(s_verts); }

void app_tick(bool render) {
    hidScanInput();
    u32 down = hidKeysDown(), held = hidKeysHeld();
    if (down & KEY_START) sys_quit();
    if (down & KEY_R) load((s_cur + 1) % NMODELS);
    if (down & KEY_L) load((s_cur + NMODELS - 1) % NMODELS);
    circlePosition cp;
    hidCircleRead(&cp);
    s_yaw += cp.dx / 50.0f;
    s_pitch += cp.dy / 80.0f;
    if (held & KEY_UP) s_dist *= 0.98f;
    if (held & KEY_DOWN) s_dist *= 1.02f;
    gfxGL_SetStereo(osGet3DSliderState() * 0.1f);
    if (!render) return;

    /* top: lit, fogged, textured model */
    gfxGL_SetScreen(GFX_SCREEN_TOP);
    glViewport(0, 0, 400, 240);
    glClearColor(0.3f, 0.5f, 0.8f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    float n = 1, a = 400.0f / 240.0f, t = n * 0.57735f;
    glFrustumf(-t * a, t * a, -t, t, n, 1000);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    float lp[4] = {0.3f, 1, 0.5f, 0};
    glLightfv(GL_LIGHT0, GL_POSITION, lp);
    float la[4] = {0.3f, 0.3f, 0.3f, 1}, ld[4] = {0.9f, 0.9f, 0.9f, 1};
    glLightfv(GL_LIGHT0, GL_AMBIENT, la);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, ld);
    glScalef(1, 1, 1);
    /* translate via a matrix load: the game has no glTranslatef either */
    float m[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    m[14] = -s_dist;
    glLoadMatrixf(m);
    glRotatef(s_pitch, 1, 0, 0);
    glRotatef(s_yaw, 0, 1, 0);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glEnable(GL_FOG);
    float fc[4] = {0.3f, 0.5f, 0.8f, 1};
    glFogfv(GL_FOG_COLOR, fc);
    glFogf(GL_FOG_START, 40);
    glFogf(GL_FOG_END, 200);
    glColor4f(1, 1, 1, 1);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    if (s_verts) {
        u8 *v = (u8 *)s_verts + 0x64;
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_NORMAL_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glVertexPointer(3, GL_FLOAT, 36, v);
        glNormalPointer(GL_FLOAT, 36, v + 12);
        glTexCoordPointer(2, GL_FLOAT, 36, v + 24);
        glDrawArrays(GL_TRIANGLES, 0, s_count);
    }
    glDisable(GL_FOG);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);

    /* bottom: 2D quad with the font atlas */
    gfxGL_SetScreen(GFX_SCREEN_BOTTOM);
    glViewport(0, 0, 320, 240);
    glClearColor(0.1f, 0.1f, 0.1f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0, 320, 240, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, s_font);
    static const float quad[] = {32, 8, 0, 288, 8, 0, 32, 232, 0, 288, 232, 0};
    static const float uv[] = {0, 0, 1, 0, 0, 1, 1, 1};
    glDisableClientState(GL_NORMAL_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, quad);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
}
