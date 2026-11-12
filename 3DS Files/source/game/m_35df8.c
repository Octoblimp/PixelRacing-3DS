/* 0x35df8..0x37148: language tables (language.m), save files (sg.m) and the sound engine.
 *
 * The iOS sound engine (0x36330..0x37120) was a small C++ object over OpenAL (one source per
 * loaded effect, gain scaled by an effects volume of 0.25) plus an AudioQueue music player the
 * game never uses: the music tracks are looping effects too. The game only talks to it through
 * the id-based wrapper at 0x36014..0x36190, which is what this file implements on ndsp. The
 * wrapper's tables in the image are kept up to date (0x5d0e0 buffer, 0x5d1e0 source,
 * 0x5d2e0 loop flag per id) in case game code reads them. */
#include <3ds.h>
#include "ios.h"

/* ================================================================ language (language.m) */

/* 0x00035df8 -- loads sys_msg_<lang>.txt and lang_<lang>.txt (falls back to ENG) */
int fn_00035df8(void) {
    char name[64], lang[32];
    fn_00004414(lang);
    snprintf(name, sizeof name, "sys_msg_%s.txt", lang);
    const char *p = fn_000058c8(0, name) ? name : "sys_msg_ENG.txt";
    int n = fn_00035eb4(p, (char ***)G_PTR(0x3f388));
    if (n < 0) return n;
    G_S32(0x3fef0) = n;
    snprintf(name, sizeof name, "lang_%s.txt", lang);
    p = fn_000058c8(0, name) ? name : "lang_ENG.txt";
    int m = fn_00035eb4(p, (char ***)G_PTR(0x3f384));
    if (m < 0) return m;
    G_S32(0x3feec) = m;
    return 1;
}

/* 0x00035eb4 -- splits a CRLF text resource into a table of 32-byte-rounded strings */
int fn_00035eb4(const char *name, char ***lines) {
    char *buf = NULL;
    if (fn_000058b8(&buf, 0, name) < 0) return -1;
    /* one entry per CR (every line, including the last, ends in CR LF) */
    int n = 0;
    for (char *p = buf; (p = memchr(p + 1, '\r', strlen(p) - 1)) != NULL;) n++;
    *lines = fn_000055f8(n * 4);
    char *p = buf;
    for (int i = 0; i < n; i++) {
        char *cr = memchr(p, '\r', strlen(p));
        int len = (int)(cr - p);
        char *s = fn_000055f8(((len + 1) / 0x20) * 0x20 + 0x20);
        (*lines)[i] = s;
        memcpy(s, p, len);
        s[len] = 0;
        p = cr + 2;
    }
    if (buf) fn_00005654(buf);
    return n;
}

/* 0x00035f88 -- LanguageItem(idx): string idx of the game text table */
const char *fn_00035f88(int idx) {
    if (idx < 0 && idx >= G_S32(0x3feec)) sys_log("assert: LanguageItem idx %d", idx);
    return (*(char ***)G_PTR(0x3f384))[idx];
}

/* ================================================================ save files (sg.m) */

/* 0x000361a4 -- documents directory into the path buffer 0x3fef4 */
void fn_000361a4(void) {
    strcpy((char *)G(0x3fef4), ios_doc_dir());
}

/* 0x000361d4 -- SgWrite(name, data, size) */
int fn_000361d4(const char *name, const void *data, int size) {
    char path[256];
    if (!G_U8(0x3fef4)) fn_000361a4();
    snprintf(path, sizeof path, "%s/%s", (char *)G(0x3fef4), name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        sys_log("SgWrite: cannot open %s", path);
        return 0;
    }
    const uint8_t *p = data;
    while (size > 0) {
        size_t w = fwrite(p, 1, size, f);
        if (!w) break;
        p += w;
        size -= (int)w;
    }
    fclose(f);
    return 1;
}

/* 0x00036274 -- SgRead(name, buf, bufsize): file size, or -1 when there is no file */
int fn_00036274(const char *name, void *buf, int bufsize) {
    char path[256];
    if (!G_U8(0x3fef4)) fn_000361a4();
    int size = fn_00036330(name);
    if (size < 0) return -1;
    if (bufsize < size) {
        sys_log("assert: SgRead %s %d > %d", name, size, bufsize);
        return -1;
    }
    snprintf(path, sizeof path, "%s/%s", (char *)G(0x3fef4), name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t *p = buf;
    for (int left = size; left > 0;) {
        size_t r = fread(p, 1, left, f);
        if (!r) break;
        p += r;
        left -= (int)r;
    }
    fclose(f);
    return size;
}

/* 0x00036330 -- size of a save file, -1 when missing */
int fn_00036330(const char *name) {
    char path[256];
    if (!G_U8(0x3fef4)) fn_000361a4();
    snprintf(path, sizeof path, "%s/%s", (char *)G(0x3fef4), name);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return (int)n;
}

/* ================================================================ sound (ndsp) */

#define SND_IDS 64
#define SND_CHANNELS 24
#define SND_RATE_IOS 11025.0f   /* the OpenAL mixer rate the game asked for */

typedef struct {
    int16_t *pcm;               /* linear memory, mono PCM16 */
    uint32_t nsamples;
    float rate;
    float gain, pitch;
} Snd;

typedef struct {
    int id;                     /* sound id playing on it, -1 = free */
    bool loop;
    ndspWaveBuf wb;
} Chan;

static bool s_snd_ok;
static Snd s_snd[SND_IDS];
static Chan s_chan[SND_CHANNELS];

static float snd_effects_volume(void) { return G_F32(0x3f38c); }

static void snd_apply_mix(int ch, const Snd *s) {
    float v = s->gain * snd_effects_volume() * 2.0f; /* 2x: OpenAL output was louder than ndsp */
    if (v > 1.0f) v = 1.0f;
    float mix[12] = {v, v};
    ndspChnSetMix(ch, mix);
    /* AL_PITCH went to Apple's 3D mixer as k3DMixerParam_PlaybackRate, whose range is
     * 0.5..2.0. The game asks for more (the engine loop idles at about 3.0); unclamped, the
     * loop played at 3x its rate and squealed. */
    float p = s->pitch;
    if (p < 0.5f) p = 0.5f;
    if (p > 2.0f) p = 2.0f;
    ndspChnSetRate(ch, s->rate * p);
}

/* 0x00035fdc -- SoundInit (not found by Ghidra; called once from the tick): engine at
 * 11025 Hz, listener at (0,0,1), effects volume 0.25, music volume 1.0. */
int fn_00035fdc(void) {
    G_F32(0x3f38c) = 0.25f;
    if (s_snd_ok) return 1;
    s_snd_ok = R_SUCCEEDED(ndspInit());
    if (!s_snd_ok) {
        sys_log("ndspInit failed (no dspfirm.cdc?): sound off");
        return 1;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    for (int i = 0; i < SND_CHANNELS; i++) {
        s_chan[i].id = -1;
        ndspChnReset(i);
        ndspChnSetInterp(i, NDSP_INTERP_LINEAR);
        ndspChnSetFormat(i, NDSP_FORMAT_MONO_PCM16);
    }
    return 1;
}

static bool snd_load_wav(const char *path, Snd *s) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[12];
    bool ok = false;
    int bits = 16, chans = 1;
    uint32_t rate = 11025;
    if (fread(hdr, 1, 12, f) == 12 && !memcmp(hdr, "RIFF", 4) && !memcmp(hdr + 8, "WAVE", 4)) {
        uint8_t ck[8];
        while (fread(ck, 1, 8, f) == 8) {
            uint32_t len = ck[4] | ck[5] << 8 | ck[6] << 16 | (uint32_t)ck[7] << 24;
            if (!memcmp(ck, "fmt ", 4)) {
                uint8_t fmt[16];
                if (len < 16 || fread(fmt, 1, 16, f) != 16) break;
                chans = fmt[2] | fmt[3] << 8;
                rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16 | (uint32_t)fmt[7] << 24;
                bits = fmt[14] | fmt[15] << 8;
                fseek(f, len - 16 + (len & 1), SEEK_CUR);
            } else if (!memcmp(ck, "data", 4)) {
                uint8_t *raw = malloc(len);
                if (!raw || fread(raw, 1, len, f) != len) { free(raw); break; }
                uint32_t frames = len / (chans * bits / 8);
                s->pcm = linearAlloc(frames * 2 + 2);
                if (!s->pcm) { free(raw); break; }
                for (uint32_t i = 0; i < frames; i++) {
                    int v;
                    if (bits == 8) v = (raw[i * chans] - 128) << 8;
                    else v = (int16_t)(raw[i * chans * 2] | raw[i * chans * 2 + 1] << 8);
                    s->pcm[i] = (int16_t)v;
                }
                free(raw);
                DSP_FlushDataCache(s->pcm, frames * 2);
                s->nsamples = frames;
                s->rate = (float)rate;
                ok = true;
                break;
            } else {
                fseek(f, len + (len & 1), SEEK_CUR);
            }
        }
    }
    fclose(f);
    return ok;
}

/* 0x00036014 -- loads "<name>.wav" from the bundle as sound `id`; returns id */
int fn_00036014(int id, const char *name) {
    char base[1024], file[1100], path[1200];
    fn_00005660(name, base);
    snprintf(file, sizeof file, "%s.wav", base);
    ios_res_path(file, path, sizeof path);
    if (id < 0 || id >= SND_IDS) return id;
    Snd *s = &s_snd[id];
    if (s->pcm) {
        fn_0003611c(id);
        linearFree(s->pcm);
        s->pcm = NULL;
    }
    s->gain = 1.0f;
    s->pitch = 1.0f;
    if (!snd_load_wav(path, s)) sys_log("sound %d: cannot load %s", id, path);
    G_U32(0x5d0e0 + id * 4) = id + 1;  /* buffer handle */
    G_U8(0x5d1e0 + id) = (uint8_t)id;  /* source */
    return id;
}

/* A channel is busy from ndspChnWaveBufAdd until its buffer is done. ndspChnIsPlaying only
 * turns true on the DSP's next frame, so it cannot tell a channel started this frame from a
 * free one (two sounds started together used to land on the same channel). */
static bool snd_channel_busy(int i) {
    const Chan *c = &s_chan[i];
    if (c->id < 0) return false;
    return c->wb.status == NDSP_WBUF_QUEUED || c->wb.status == NDSP_WBUF_PLAYING;
}

static int snd_channel_for(int id, bool steal) {
    for (int i = 0; i < SND_CHANNELS; i++)
        if (s_chan[i].id == id) return i;
    if (!steal) return -1;
    for (int i = 0; i < SND_CHANNELS; i++)
        if (!snd_channel_busy(i)) return i;
    for (int i = 0; i < SND_CHANNELS; i++)
        if (!s_chan[i].loop) return i;
    return 0;
}

/* 0x000360bc -- play sound `id` from the start (looping when its loop flag is set) */
int fn_000360bc(int id) {
    if (!s_snd_ok || id < 0 || id >= SND_IDS || !s_snd[id].pcm) return 1;
    Snd *s = &s_snd[id];
    int ch = snd_channel_for(id, true);
    Chan *c = &s_chan[ch];
    ndspChnWaveBufClear(ch);
    c->id = id;
    c->loop = G_U32(0x5d2e0 + id * 4) != 0;
    memset(&c->wb, 0, sizeof c->wb);
    c->wb.data_vaddr = s->pcm;
    c->wb.nsamples = s->nsamples;
    c->wb.looping = c->loop;
    snd_apply_mix(ch, s);
    ndspChnWaveBufAdd(ch, &c->wb);
    return 1;
}

/* 0x000360e0 -- stop sound `id` */
void fn_000360e0(int id) {
    if (!s_snd_ok) return;
    int ch = snd_channel_for(id, false);
    if (ch < 0) return;
    ndspChnWaveBufClear(ch);
    s_chan[ch].id = -1;
}

/* 0x000360fc -- stop every sound */
void fn_000360fc(void) {
    if (!s_snd_ok) return;
    for (int i = 0; i < SND_CHANNELS; i++) {
        ndspChnWaveBufClear(i);
        s_chan[i].id = -1;
    }
}

/* 0x0003611c -- stop sound `id` (second entry point, same as 0x360e0) */
void fn_0003611c(int id) { fn_000360e0(id); }

/* 0x00036138 -- sound available */
int fn_00036138(void) { return 1; }

/* 0x0003613c -- gain of sound `id` = (a + b) / 16000 (times the effects volume) */
void fn_0003613c(int id, int a, int b) {
    if (id < 0 || id >= SND_IDS) return;
    s_snd[id].gain = (float)(a + b) / 16000.0f;
    int ch = s_snd_ok ? snd_channel_for(id, false) : -1;
    if (ch >= 0) snd_apply_mix(ch, &s_snd[id]);
}

/* 0x00036174 -- pitch of sound `id` (1.0 = original rate) */
void fn_00036174(int id, float pitch) {
    if (id < 0 || id >= SND_IDS) return;
    s_snd[id].pitch = pitch;
    int ch = s_snd_ok ? snd_channel_for(id, false) : -1;
    if (ch >= 0) snd_apply_mix(ch, &s_snd[id]);
}

/* 0x00036190 -- loop flag of sound `id` (used by the next play) */
void fn_00036190(int id, int loop) {
    if (id < 0 || id >= SND_IDS) return;
    G_U32(0x5d2e0 + id * 4) = loop;
}

/* Shutdown (iOS: engine destructor 0x3706c, never reached in practice). */
void snd_shutdown(void) {
    if (!s_snd_ok) return;
    fn_000360fc();
    ndspExit();
    s_snd_ok = false;
}
