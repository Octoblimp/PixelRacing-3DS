/* MTX/VEC shim (wii.m: the RVL SDK matrix library re-implemented for iOS) and the SMD model
 * slots (smd.m): loader, vertex packer, node matrices and model draw. [0x37148, 0x38c24)
 *
 * Model slots: 260 x 0x2d4 bytes at 0x5d3f4 (flag "slots initialised" at 0x5d3f0).
 *   +0x000 u8     loaded
 *   +0x004 s32    vertex count (3 x triangles)
 *   +0x008 ptr    packed vertices, 16 bytes each: s16 pos[3] (x scale), s16 nrm[3] (x 32677),
 *                 s16 u*1024, s16 (1-v)*1024
 *   +0x00c char   name[0x1c]  (node name from the .bin)
 *   +0x068 s32    parent[]    (node parent, -1 = root; the loader only writes [0])
 *   +0x1ec ptr    bones: node count x {f32 pos[3] (x scale), f32 rot[3] (radians)}
 *   +0x1f8 5 x 0x14 bytes (first word of each cleared on free; unused here)
 *   +0x254 s32    -1 after load
 *   +0x25c s32    0 on free
 *   +0x264 ptr    node count x Mtx34: node world matrices (with translation)
 *   +0x268 ptr    node count x Mtx34: node rotation matrices
 *   +0x26c s32    node count
 *   +0x270 f32[3] model position (= bone 0 pos after the Z-up -> Y-up swap)
 *   +0x27c f32[3] model rotation, degrees (0 on load)
 *   +0x288 f32[9] model orientation 3x3, stored column-major (identity on load)
 *   +0x2ac f32    ground offset: -min(pos z) of the packed vertices (bounds pass)
 *   +0x2b0 f32[3] bounding box min (x, z, y), +0x2bc f32[3] max (x, z, y) (packed units)
 */
#include "ios.h"

#define SMD_SLOTS 0x5d3f4u
#define SMD_SLOT_SIZE 0x2d4
#define SMD_INITED 0x5d3f0u

#define P2I(p) ((int)(intptr_t)(p))
#define I2P(i) ((void *)(intptr_t)(i))

/* i-th Mtx34 of an array (fn_00004614: base + i * 0x30). */
static float (*m37148_mtx(void *arr, int i))[4]
{
    return (float (*)[4])I2P(fn_00004614(P2I(arr), i));
}

static uint8_t *m37148_slot(int slot)
{
    return (uint8_t *)G(SMD_SLOTS) + slot * SMD_SLOT_SIZE;
}

/* 0x00037148 -- MTXCopy(src, dst) */
void fn_00037148(void *src, void *dst)
{
    float *s = src, *d = dst;
    if (s == NULL) {
        sys_log("assert: MTXCopy (src != 0)\n");
        return;
    }
    if (d == NULL) {
        sys_log("assert: MTXCopy (dst != 0)\n");
        return;
    }
    if (s == d)
        return;
    for (int i = 0; i < 12; i++)
        d[i] = s[i];
}

/* 0x000371d0 -- MTXTrans(m, x, y, z) */
void fn_000371d0(void *mtx, float x, float y, float z)
{
    float (*m)[4] = mtx;
    if (m == NULL) {
        sys_log("assert: MTXTrans (m != 0)\n");
        return;
    }
    m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = x;
    m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f; m[1][3] = y;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f; m[2][3] = z;
}

/* 0x00037230 -- MTXScale(m, x, y, z) */
void fn_00037230(void *mtx, float x, float y, float z)
{
    float (*m)[4] = mtx;
    if (m == NULL) {
        sys_log("assert: MTXScale (m != 0)\n");
        return;
    }
    m[0][0] = x;    m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = 0.0f;
    m[1][0] = 0.0f; m[1][1] = y;    m[1][2] = 0.0f; m[1][3] = 0.0f;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = z;    m[2][3] = 0.0f;
}

/* 0x0003728c -- MTXConcat(a, b, ab): ab = a * b (ab may alias a or b) */
void fn_0003728c(void *ma, void *mb, void *mab)
{
    float (*a)[4] = ma, (*b)[4] = mb, (*ab)[4] = mab;
    Mtx34 tmp;
    float (*m)[4];
    if (a == NULL) {
        sys_log("assert: MTXConcat (a != 0)\n");
        return;
    }
    if (b == NULL) {
        sys_log("assert: MTXConcat (b != 0)\n");
        return;
    }
    if (ab == NULL) {
        sys_log("assert: MTXConcat (ab != 0)\n");
        return;
    }
    m = (ab == a || ab == b) ? tmp : ab;
    for (int r = 0; r < 3; r++) {
        m[r][0] = a[r][0] * b[0][0] + a[r][1] * b[1][0] + a[r][2] * b[2][0];
        m[r][1] = a[r][0] * b[0][1] + a[r][1] * b[1][1] + a[r][2] * b[2][1];
        m[r][2] = a[r][0] * b[0][2] + a[r][1] * b[1][2] + a[r][2] * b[2][2];
        m[r][3] = a[r][0] * b[0][3] + a[r][1] * b[1][3] + a[r][2] * b[2][3] + a[r][3];
    }
    if (m == tmp)
        fn_00037148(tmp, ab);
}

/* 0x0003759c -- MTXInverse(src, inv): returns 0 (inv untouched) if src is singular */
int fn_0003759c(void *msrc, void *minv)
{
    float (*s)[4] = msrc, (*inv)[4] = minv;
    Mtx34 tmp;
    float (*m)[4];
    float det;
    if (s == NULL) {
        sys_log("assert: MTXInverse (src != 0)\n");
        return 0;
    }
    if (inv == NULL) {
        sys_log("assert: MTXInverse (inv != 0)\n");
        return 0;
    }
    m = (s == inv) ? tmp : inv;
    det = s[0][0] * s[1][1] * s[2][2] + s[0][1] * s[1][2] * s[2][0] +
          s[0][2] * s[1][0] * s[2][1] - s[2][0] * s[1][1] * s[0][2] -
          s[1][0] * s[0][1] * s[2][2] - s[0][0] * s[2][1] * s[1][2];
    if (det == 0.0f)
        return 0;
    det = 1.0f / det;
    m[0][0] = (s[1][1] * s[2][2] - s[2][1] * s[1][2]) * det;
    m[0][1] = -(s[0][1] * s[2][2] - s[2][1] * s[0][2]) * det;
    m[0][2] = (s[0][1] * s[1][2] - s[1][1] * s[0][2]) * det;
    m[1][0] = -(s[1][0] * s[2][2] - s[2][0] * s[1][2]) * det;
    m[1][1] = (s[0][0] * s[2][2] - s[2][0] * s[0][2]) * det;
    m[1][2] = -(s[0][0] * s[1][2] - s[1][0] * s[0][2]) * det;
    m[2][0] = (s[1][0] * s[2][1] - s[2][0] * s[1][1]) * det;
    m[2][1] = -(s[0][0] * s[2][1] - s[2][0] * s[0][1]) * det;
    m[2][2] = (s[0][0] * s[1][1] - s[1][0] * s[0][1]) * det;
    m[0][3] = -m[0][0] * s[0][3] - m[0][1] * s[1][3] - m[0][2] * s[2][3];
    m[1][3] = -m[1][0] * s[0][3] - m[1][1] * s[1][3] - m[1][2] * s[2][3];
    m[2][3] = -m[2][0] * s[0][3] - m[2][1] * s[1][3] - m[2][2] * s[2][3];
    if (m == tmp)
        fn_00037148(tmp, inv);
    return 1;
}

/* 0x00037844 -- MTXRotRad(m, axis, rad) */
void fn_00037844(void *m, int axis, float rad)
{
    if (m == NULL) {
        sys_log("assert: MTXRotRad (m != 0)\n");
        return;
    }
    fn_000378a0(m, axis, sinf(rad), cosf(rad));
}

/* 0x000378a0 -- MTXRotTrig(m, axis 'x'/'y'/'z' (either case), sinA, cosA) */
void fn_000378a0(void *mtx, int axis, float sinA, float cosA)
{
    float (*m)[4] = mtx;
    if (m == NULL) {
        sys_log("assert: MTXRotTrig (m != 0)\n");
        return;
    }
    switch (axis) {
    case 'X':
    case 'x':
        m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f;  m[0][3] = 0.0f;
        m[1][0] = 0.0f; m[1][1] = cosA; m[1][2] = -sinA; m[1][3] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = sinA; m[2][2] = cosA;  m[2][3] = 0.0f;
        break;
    case 'Y':
    case 'y':
        m[0][0] = cosA;  m[0][1] = 0.0f; m[0][2] = sinA; m[0][3] = 0.0f;
        m[1][0] = 0.0f;  m[1][1] = 1.0f; m[1][2] = 0.0f; m[1][3] = 0.0f;
        m[2][0] = -sinA; m[2][1] = 0.0f; m[2][2] = cosA; m[2][3] = 0.0f;
        break;
    case 'Z':
    case 'z':
        m[0][0] = cosA; m[0][1] = -sinA; m[0][2] = 0.0f; m[0][3] = 0.0f;
        m[1][0] = sinA; m[1][1] = cosA;  m[1][2] = 0.0f; m[1][3] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = 0.0f;  m[2][2] = 1.0f; m[2][3] = 0.0f;
        break;
    default:
        sys_log("assert: MTXRotTrig bad axis %d\n", axis);
        break;
    }
}

/* 0x000379a8 -- VECNormalize(src, dst) (no zero-length check) */
void fn_000379a8(float *src, float *dst)
{
    float inv = 1.0f / sqrtf(src[0] * src[0] + src[1] * src[1] + src[2] * src[2]);
    dst[0] = src[0] * inv;
    dst[1] = src[1] * inv;
    dst[2] = src[2] * inv;
}

/* 0x000379fc -- MTXLookAt(m, camPos, camUp, target) */
void fn_000379fc(void *mtx, float *camPos, float *camUp, float *target)
{
    float (*m)[4] = mtx;
    float lx, ly, lz, rx, ry, rz, ux, uy, uz, inv;
    if (m == NULL) {
        sys_log("assert: MTXLookAt (m != 0)\n");
        return;
    }
    if (camPos == NULL) {
        sys_log("assert: MTXLookAt (camPos != 0)\n");
        return;
    }
    if (camUp == NULL) {
        sys_log("assert: MTXLookAt (camUp != 0)\n");
        return;
    }
    if (target == NULL) {
        sys_log("assert: MTXLookAt (target != 0)\n");
        return;
    }
    /* look = normalize(camPos - target) */
    lx = camPos[0] - target[0];
    ly = camPos[1] - target[1];
    lz = camPos[2] - target[2];
    inv = 1.0f / sqrtf(lx * lx + ly * ly + lz * lz);
    lx *= inv;
    ly *= inv;
    lz *= inv;
    /* right = normalize(up x look) */
    rx = camUp[1] * lz - camUp[2] * ly;
    ry = camUp[2] * lx - camUp[0] * lz;
    rz = camUp[0] * ly - camUp[1] * lx;
    inv = 1.0f / sqrtf(rx * rx + ry * ry + rz * rz);
    rx *= inv;
    ry *= inv;
    rz *= inv;
    m[0][0] = rx;
    m[0][1] = ry;
    m[0][2] = rz;
    m[0][3] = -(camPos[0] * rx + camPos[1] * ry + camPos[2] * rz);
    /* up = look x right */
    ux = ly * rz - lz * ry;
    uy = lz * rx - lx * rz;
    uz = lx * ry - ly * rx;
    m[1][0] = ux;
    m[1][1] = uy;
    m[1][2] = uz;
    m[1][3] = -(camPos[0] * ux + camPos[1] * uy + camPos[2] * uz);
    m[2][0] = lx;
    m[2][1] = ly;
    m[2][2] = lz;
    m[2][3] = -(camPos[0] * lx + camPos[1] * ly + camPos[2] * lz);
}

/* 0x00037c28 -- VECCrossProduct(a, b, axb). No Ghidra function and no symbols.csv row (it sits
 * after MTXLookAt's assert tail); called twice from the game tick. */
void fn_00037c28(float *a, float *b, float *axb)
{
    float x = a[1] * b[2] - a[2] * b[1];
    float y = a[2] * b[0] - a[0] * b[2];
    float z = a[0] * b[1] - a[1] * b[0];
    axb[0] = x;
    axb[1] = y;
    axb[2] = z;
}

/* 0x00037c78 -- MTXPerspective through GL: glFrustumf from fovY (degrees), aspect, n, f into
 * the projection matrix, read back into out[16]. Matrix mode is left GL_PROJECTION. */
void fn_00037c78(float *out, float fovY, float aspect, float n, float f)
{
    float t, r;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    t = tanf(fovY * 3.14f / 360.0f) * n;
    r = t * aspect;
    glFrustumf(-r, r, -t, t, n, f);
    glGetFloatv(GL_PROJECTION_MATRIX, out);
}

/* 0x00037d0c -- MTXOrtho through GL: glOrthof(l, r, b, t, n, f) into the projection matrix,
 * read back into out[16]. Matrix mode is left GL_PROJECTION. */
void fn_00037d0c(float *out, float t, float b, float l, float r, float n, float f)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(l, r, b, t, n, f);
    glGetFloatv(GL_PROJECTION_MATRIX, out);
}

/* 0x00037d5c -- MTXLightOrtho(m, t, b, l, r, scaleS, scaleT, transS, transT) */
void fn_00037d5c(void *mtx, float t, float b, float l, float r, float scaleS, float scaleT,
                 float transS, float transT)
{
    float (*m)[4] = mtx;
    float tb = 1.0f / (t - b);
    float rl = 1.0f / (r - l);
    m[0][0] = (rl + rl) * scaleS;
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = transS - (r + l) * rl * scaleS;
    m[1][0] = 0.0f;
    m[1][1] = (tb + tb) * scaleT;
    m[1][2] = 0.0f;
    m[1][3] = transT - (t + b) * tb * scaleT;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = 0.0f;
    m[2][3] = 1.0f;
}

/* 0x00037dec -- VECAdd(a, b, ab) */
void fn_00037dec(float *a, float *b, float *ab)
{
    ab[0] = a[0] + b[0];
    ab[1] = a[1] + b[1];
    ab[2] = a[2] + b[2];
}

/* 0x00037e24 -- VECScale(src, dst, scale) */
void fn_00037e24(float *src, float *dst, float scale)
{
    dst[0] = src[0] * scale;
    dst[1] = src[1] * scale;
    dst[2] = src[2] * scale;
}

/* 0x00037e54 -- VECMag(v) */
float fn_00037e54(float *v)
{
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

/* 0x00037e80 -- VECDotProduct(a, b) */
float fn_00037e80(float *a, float *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* 0x00037eb4 -- VECReflect(src, normal, dst): i = -unit(src), n = unit(normal),
 * dst = unit(2 (i.n) n - i) */
void fn_00037eb4(float *src, float *normal, float *dst)
{
    float ni = 1.0f / sqrtf(normal[0] * normal[0] + normal[1] * normal[1] +
                            normal[2] * normal[2]);
    float si = 1.0f / sqrtf(src[0] * src[0] + src[1] * src[1] + src[2] * src[2]);
    float nx = normal[0] * ni, ny = normal[1] * ni, nz = normal[2] * ni;
    float ix = -(src[0] * si), iy = -(src[1] * si), iz = -(src[2] * si);
    float cosA = ix * nx + iy * ny + iz * nz;
    float x = (nx + nx) * cosA - ix;
    float y = (ny + ny) * cosA - iy;
    float z = (nz + nz) * cosA - iz;
    float inv = 1.0f / sqrtf(x * x + y * y + z * z);
    dst[0] = x * inv;
    dst[1] = y * inv;
    dst[2] = z * inv;
}

/* 0x00037fa4 -- VECDistance(a, b) */
float fn_00037fa4(float *a, float *b)
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/* 0x00037fe8 -- MTXMultVec(m, src, dst): dst = m * (src, 1); dst may alias src */
void fn_00037fe8(void *mtx, float *src, float *dst)
{
    float (*m)[4] = mtx;
    float x = m[0][0] * src[0] + m[0][1] * src[1] + m[0][2] * src[2] + m[0][3];
    float y = m[1][0] * src[0] + m[1][1] * src[1] + m[1][2] * src[2] + m[1][3];
    float z = m[2][0] * src[0] + m[2][1] * src[1] + m[2][2] * src[2] + m[2][3];
    dst[0] = x;
    dst[1] = y;
    dst[2] = z;
}

/* 0x00038090 -- MTXMultVecArray(m, src, dst, count) */
void fn_00038090(void *mtx, float *src, float *dst, int count)
{
    float (*m)[4] = mtx;
    for (; count != 0; count--, src += 3, dst += 3) {
        float x = m[0][0] * src[0] + m[0][1] * src[1] + m[0][2] * src[2] + m[0][3];
        float y = m[1][0] * src[0] + m[1][1] * src[1] + m[1][2] * src[2] + m[1][3];
        float z = m[2][0] * src[0] + m[2][1] * src[1] + m[2][2] * src[2] + m[2][3];
        dst[0] = x;
        dst[1] = y;
        dst[2] = z;
    }
}

/* 0x00038158 -- SmdInit: frees model slots 2..259 once (slots 0 and 1 are left alone) */
void fn_00038158(void)
{
    if (G_U8(SMD_INITED) != 0)
        return;
    for (int i = 2; i < 260; i++)
        fn_00038184(i);
    G_U8(SMD_INITED) = 1;
}

/* 0x00038184 -- SmdFreeSlot: frees the slot's texture (same id) and buffers, clears it */
int fn_00038184(int slot)
{
    uint8_t *s = m37148_slot(slot);
    fn_00004dac(slot);
    if (AT_PTR(s, 0x1ec) != NULL)
        fn_00005654(AT_PTR(s, 0x1ec));
    AT_PTR(s, 0x1ec) = NULL;
    if (AT_PTR(s, 0x264) != NULL)
        fn_00005654(AT_PTR(s, 0x264));
    AT_PTR(s, 0x264) = NULL;
    if (AT_PTR(s, 0x268) != NULL)
        fn_00005654(AT_PTR(s, 0x268));
    AT_PTR(s, 0x268) = NULL;
    if (AT_PTR(s, 0x8) != NULL)
        fn_00005654(AT_PTR(s, 0x8));
    AT_PTR(s, 0x8) = NULL;
    AT_S32(s, 0x25c) = 0;
    for (int k = 0; k < 5; k++)
        AT_S32(s, 0x1f8 + k * 0x14) = 0;
    AT_U8(s, 0) = 0;
    return slot;
}

/* 0x00038228 -- SmdPackVertices(slot, file, scale): repacks the .bin's 36-byte vertices
 * {s32 bone; f32 pos[3], nrm[3], uv[2]} into 16-byte s16 records at slot+8 */
void fn_00038228(void *model, void *file, float scale)
{
    uint8_t *s = model;
    int count = AT_S32(s, 4);
    int16_t *out = AT_PTR(s, 8);
    /* vertices follow the 0x20 header and node_count x (0x20 node + 0x20 bone) records;
     * +4 skips the bone index */
    const uint8_t *in = (const uint8_t *)file + AT_S32(s, 0x26c) * 0x40 + 0x24;
    if (count < 1)
        return;
    for (int i = 0; i < count; i += 3) {
        for (int k = 0; k < 3; k++) {
            float pos[3], nrm[3], u, v;
            memcpy(pos, in, 12);
            memcpy(nrm, in + 12, 12);
            memcpy(&u, in + 0x18, 4);
            memcpy(&v, in + 0x1c, 4);
            fn_00037e24(pos, pos, scale);
            fn_00037e24(nrm, nrm, 32677.0f);
            out[0] = (int16_t)(int32_t)pos[0];
            out[1] = (int16_t)(int32_t)pos[1];
            out[2] = (int16_t)(int32_t)pos[2];
            out[3] = (int16_t)(int32_t)nrm[0];
            out[4] = (int16_t)(int32_t)nrm[1];
            out[5] = (int16_t)(int32_t)nrm[2];
            out[6] = (int16_t)(int32_t)(u * 1024.0f);
            out[7] = (int16_t)(int32_t)((1.0f - v) * 1024.0f);
            out += 8;
            in += 0x24;
        }
    }
    /* port: the GL layer reads linear-memory arrays in place */
    gfxGL_Flush(AT_PTR(s, 8), (uint32_t)count * 16);
}

/* 0x0003836c -- SmdLoadModel(slot, path, scale, calcBounds, calcBox, unused): loads
 * "<basename of path without extension>.bin" into a model slot. Returns 1, -1 (slots not
 * initialised) or -2 (read error). calcBounds == 1 computes the ground offset (+0x2ac) and,
 * with calcBox == 1, the box (+0x2b0..+0x2c4) from the packed vertices (also after an error,
 * as on iOS). The 6th argument is passed by every caller but never read. */
int fn_0003836c(int slot, const char *path, float scale, int calcBounds, int calcBox,
                int unused)
{
    char name[0x400];
    uint8_t *file = NULL;
    uint8_t *s;
    void *inv0 = NULL, *inv1 = NULL;
    int ret;

    fn_00005660(path, name);
    strcat(name, ".bin");
    if (!(slot < 260)) {
        sys_log("assert: SmdLoadModel smd_slot_id<260 (%d)\n", slot);
        return -1;
    }
    if (!(slot >= 0)) {
        sys_log("assert: SmdLoadModel smd_slot_id>=0 (%d)\n", slot);
        return -1;
    }
    s = m37148_slot(slot);

    if (G_U8(SMD_INITED) == 0) {
        /* iOS also frees an uninitialised register and the name buffer here; dropped */
        ret = -1;
    } else {
        fn_00038184(slot);
        sys_log("SmdLoadModel: loading %s, on slot %d\n", name, slot);
        if (fn_00005784(&file, 0, name) < 0) { /* CxReadFile */
            ret = -2;
        } else {
            int nodes = AT_S32(file, 0x18);
            float *bones;
            /* node records {char name[0x1c]; s32 parent} at 0x20; every node writes the same
             * slot fields (only one node in every shipped model) */
            for (int k = 0; k < nodes; k++) {
                strncpy((char *)s + 0xc, (const char *)file + 0x20 + k * 0x20, 0x1c);
                AT_S32(s, 0x68) = AT_S32(file, 0x3c + k * 0x20);
            }
            AT_S32(s, 0x26c) = nodes;
            AT_PTR(s, 0x264) = fn_000055f8(nodes * 0x30);
            AT_PTR(s, 0x268) = fn_000055f8(AT_S32(s, 0x26c) * 0x30);
            inv0 = fn_000055f8(AT_S32(s, 0x26c) * 0x30);
            inv1 = fn_000055f8(AT_S32(s, 0x26c) * 0x30);
            bones = fn_000055f8(AT_S32(s, 0x26c) * 0x18);
            AT_PTR(s, 0x1ec) = bones;
            /* bone records {f32 pos[3], rot[3]; 8 bytes} follow the node records */
            for (int k = 0; k < AT_S32(s, 0x26c); k++) {
                memcpy(bones + k * 6, file + 0x20 + AT_S32(s, 0x26c) * 0x20 + k * 0x20, 0x18);
                fn_00037e24(bones + k * 6, bones + k * 6, scale);
            }
            /* bone 0: Z-up -> Y-up: pos (x, z, y), rot (rx - pi/2, rz, -ry) */
            {
                float t;
                bones[3] = bones[3] + -1.57079506f; /* 0xbfc90fd0 */
                t = bones[4];
                bones[4] = bones[5];
                bones[5] = -t;
                t = bones[1];
                bones[1] = bones[2];
                bones[2] = t;
            }
            /* orientation = identity */
            AT_F32(s, 0x288) = 1.0f;
            AT_F32(s, 0x28c) = 0.0f;
            AT_F32(s, 0x290) = 0.0f;
            AT_F32(s, 0x294) = 0.0f;
            AT_F32(s, 0x298) = 1.0f;
            AT_F32(s, 0x29c) = 0.0f;
            AT_F32(s, 0x2a0) = 0.0f;
            AT_F32(s, 0x2a4) = 0.0f;
            AT_F32(s, 0x2a8) = 1.0f;
            AT_F32(s, 0x2ac) = ((float *)AT_PTR(s, 0x1ec))[1];
            fn_00004800(P2I(s + 0x270), P2I(AT_PTR(s, 0x1ec))); /* VECCopy(bone 0 pos) */
            fn_000047f8(P2I(s + 0x27c), 0.0f, 0.0f, 0.0f);   /* rotation = 0 */
            fn_0003882c(s, 0, NULL, NULL, NULL);
            fn_000389d0(inv0, AT_PTR(s, 0x264), AT_S32(s, 0x26c));
            fn_000389d0(inv1, AT_PTR(s, 0x268), AT_S32(s, 0x26c));
            AT_S32(s, 4) = AT_S32(file, 0x1c);
            AT_PTR(s, 8) = fn_000055f8(AT_S32(s, 4) << 4);
            fn_00038228(s, file, scale);
            AT_S32(s, 0x254) = -1;
            AT_U8(s, 0) = 1;
            ret = 1;
        }
        if (file != NULL)
            fn_00005654(file);
        file = NULL;
        if (inv0 != NULL)
            fn_00005654(inv0);
        if (inv1 != NULL)
            fn_00005654(inv1);
    }
    if (ret < 0)
        sys_log("SmdLoadModel Error: cannot load %s, errcode %d\n", name, ret);

    if (calcBounds == 1) {
        int16_t minZ = (int16_t)1000000; /* 16960: the iOS code truncates 1e6 to a short */
        float mnx = 1e6f, mnz = 1e6f, mny = 1e6f;
        float mxx = -1e6f, mxz = -1e6f, mxy = -1e6f;
        int count = AT_S32(s, 4);
        AT_F32(s, 0x2b0) = 1e6f;
        AT_F32(s, 0x2b4) = 1e6f;
        AT_F32(s, 0x2b8) = 1e6f;
        AT_F32(s, 0x2bc) = -1e6f;
        AT_F32(s, 0x2c0) = -1e6f;
        AT_F32(s, 0x2c4) = -1e6f;
        if (count >= 1) {
            const int16_t *v = AT_PTR(s, 8);
            for (; count != 0; count--, v += 8) {
                float f;
                if (v[2] < minZ)
                    minZ = v[2];
                if (calcBox != 1)
                    continue;
                f = (float)v[0];
                if (f < mnx) {
                    AT_F32(s, 0x2b0) = f;
                    mnx = f;
                }
                f = (float)v[2];
                if (f < mnz) {
                    AT_F32(s, 0x2b4) = f;
                    mnz = f;
                }
                f = (float)v[1];
                if (f < mny) {
                    AT_F32(s, 0x2b8) = f;
                    mny = f;
                }
                f = (float)v[0];
                if (f > mxx) {
                    AT_F32(s, 0x2bc) = f;
                    mxx = f;
                }
                f = (float)v[2];
                if (f > mxz) {
                    AT_F32(s, 0x2c0) = f;
                    mxz = f;
                }
                f = (float)v[1];
                if (f > mxy) {
                    AT_F32(s, 0x2c4) = f;
                    mxy = f;
                }
            }
        }
        AT_F32(s, 0x2ac) = (float)(-(int)minZ);
    }
    return ret;
}

/* 0x00038818 -- SmdGetSlot(slot): address of the model slot */
void *fn_00038818(int slot)
{
    return m37148_slot(slot);
}

/* 0x0003882c -- SmdUpdateMatrices(model, applyCamera, parentWorld, parentRot, rootOut):
 * builds every node's world matrix (+0x264) and rotation matrix (+0x268). A node with a
 * parent node chains to it; a root node chains to parentWorld/parentRot when given, else it
 * gets the model transform trans(+0x270) * rot(+0x27c) * orient(+0x288) * bone rotation,
 * optionally premultiplied by the camera matrix at 0x4279c. rootOut receives node 0's model
 * transform (before the camera). */
void fn_0003882c(void *model, int applyCamera, void *parentWorld, void *parentRot,
                 void *rootOut)
{
    uint8_t *s = model;
    Mtx34 tmp, orient, trans;
    float rotDeg[3];
    for (int i = 0; i < AT_S32(s, 0x26c); i++) {
        float *bone = (float *)AT_PTR(s, 0x1ec) + i * 6;
        int parent = AT_S32(s, 0x68 + i * 4);
        float (*world)[4], (*rot)[4];
        fn_00037e24(bone + 3, rotDeg, 57.29578f); /* rad -> deg, 0x42652ee1 */
        world = m37148_mtx(AT_PTR(s, 0x264), i);
        rot = m37148_mtx(AT_PTR(s, 0x268), i);
        if (parent != -1) {
            fn_00004754(P2I(tmp), P2I(bone), P2I(rotDeg)); /* trans(pos) * rot(deg) */
            fn_0003728c(m37148_mtx(AT_PTR(s, 0x264), parent), tmp, world);
            fn_00004620(P2I(tmp), P2I(rotDeg));            /* rot(deg) */
            fn_0003728c(m37148_mtx(AT_PTR(s, 0x268), parent), tmp, rot);
        } else if (parentWorld != NULL) {
            fn_00004754(P2I(tmp), P2I(bone), P2I(rotDeg));
            fn_0003728c(parentWorld, tmp, world);
            fn_00004620(P2I(tmp), P2I(rotDeg));
            fn_0003728c(parentRot, tmp, rot);
        } else {
            fn_00004620(P2I(world), P2I(rotDeg));
            /* +0x288 holds the orientation column-major */
            orient[0][0] = AT_F32(s, 0x288);
            orient[1][0] = AT_F32(s, 0x28c);
            orient[2][0] = AT_F32(s, 0x290);
            orient[0][1] = AT_F32(s, 0x294);
            orient[1][1] = AT_F32(s, 0x298);
            orient[2][1] = AT_F32(s, 0x29c);
            orient[0][2] = AT_F32(s, 0x2a0);
            orient[1][2] = AT_F32(s, 0x2a4);
            orient[2][2] = AT_F32(s, 0x2a8);
            orient[0][3] = 0.0f;
            orient[1][3] = 0.0f;
            orient[2][3] = 0.0f;
            fn_0003728c(orient, world, world);
            fn_00004620(P2I(tmp), P2I(s + 0x27c));
            fn_0003728c(tmp, world, world);
            fn_000371d0(trans, AT_F32(s, 0x270), AT_F32(s, 0x274), AT_F32(s, 0x278));
            fn_0003728c(trans, world, world);
            if (i == 0 && rootOut != NULL)
                fn_00037148(world, rootOut);
            if ((applyCamera & 0xff) != 0)
                fn_0003728c(G(0x4279c), world, world);
            /* as on iOS: rot = rot(+0x27c) * world */
            fn_0003728c(tmp, world, rot);
        }
    }
}

/* 0x000389d0 -- SmdInvertMatrixes(dst, src, n): dst[i] = inverse(src[i]) */
void fn_000389d0(void *dst, void *src, int n)
{
    for (int i = 0; i < n; i++) {
        if (!fn_0003759c(m37148_mtx(src, i), m37148_mtx(dst, i)))
            sys_log("SmdInvertMatrixes Error: matrix is not invertible\n");
    }
}

/* 0x00038a20 -- wraps an angle in degrees into [0, 360] */
float fn_00038a20(float deg)
{
    for (;;) {
        if (deg < 0.0f)
            deg += 360.0f;
        else if (deg > 360.0f)
            deg -= 360.0f;
        else
            return deg;
    }
}

/* 0x00038a78 -- SmdSetTextured(on): model draws bind the slot texture (0x48294) */
void fn_00038a78(int on)
{
    G_U8(0x48294) = (uint8_t)on;
}

/* 0x00038a88 -- sets the byte at 0x48298 (not read in this module) */
void fn_00038a88(int on)
{
    G_U8(0x48298) = (uint8_t)on;
}

/* 0x00038a98 -- SmdDrawModel(slot, tex (<0: same as slot), parentSlot (-1: none), parentNode,
 * unused, filter): updates the node matrices (with the camera), binds the texture with the
 * min filter selected by `filter` when texturing is on, loads node 0's matrix and draws the
 * packed triangles. Leaves the matrix mode at GL_TEXTURE. Returns 1. */
int fn_00038a98(int slot, int tex, int parentSlot, int parentNode, int unused, int filter)
{
    uint8_t *s = m37148_slot(slot);
    Mtx34 root;
    if (parentSlot == -1) {
        fn_0003882c(s, 1, NULL, NULL, root);
    } else {
        uint8_t *p = m37148_slot(parentSlot);
        fn_0003882c(s, 1, m37148_mtx(AT_PTR(p, 0x264), parentNode),
                    m37148_mtx(AT_PTR(p, 0x268), parentNode), root);
    }
    if (G_U8(0x48294) != 0) {
        float minFilter;
        if (tex < 0)
            tex = slot;
        switch (filter) {
        case 1: minFilter = 9729.0f; break; /* GL_LINEAR */
        case 2: minFilter = 9984.0f; break; /* GL_NEAREST_MIPMAP_NEAREST */
        case 3: minFilter = 9985.0f; break; /* GL_LINEAR_MIPMAP_NEAREST */
        case 4: minFilter = 9986.0f; break; /* GL_NEAREST_MIPMAP_LINEAR */
        case 5: minFilter = 9987.0f; break; /* GL_LINEAR_MIPMAP_LINEAR */
        default: minFilter = 9728.0f; break; /* GL_NEAREST */
        }
        fn_00004e08(tex); /* bind texture (iOS also leaves 0x84c0 in r1) */
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, 9728.0f);
    }
    fn_00004e44(P2I(m37148_mtx(AT_PTR(s, 0x264), 0))); /* modelview = node 0 */
    glEnableClientState(GL_NORMAL_ARRAY);
    glVertexPointer(3, GL_SHORT, 16, AT_PTR(s, 8));
    glNormalPointer(GL_SHORT, 16, (uint8_t *)AT_PTR(s, 8) + 6);
    glTexCoordPointer(2, GL_SHORT, 16, (uint8_t *)AT_PTR(s, 8) + 12);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glScalef(1.0f / 1024.0f, 1.0f / 1024.0f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, AT_S32(s, 4));
    return 1;
}

/* 0x38c04: 1 when model slot n has its vertices (+0x008 non-null), i.e. it is loaded. The
 * race loader checks slot 0x73 (wheel_01) so the menu's copy is not loaded twice. */
int fn_00038c04(int slot)
{
    return AT_U32((uint8_t *)G(0x5d3f4) + slot * 0x2d4, 8) != 0;
}
