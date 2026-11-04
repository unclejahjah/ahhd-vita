#include "msadpcm.h"

#include <stdlib.h>
#include <string.h>

static const int adapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int16_t rd16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

int adpcm_open(Adpcm *a, const char *path) {
    memset(a, 0, sizeof *a);
    a->f = fopen(path, "rb");
    if (!a->f) return 0;
    uint8_t h[12];
    if (fread(h, 1, 12, a->f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) goto fail;
    int have_fmt = 0;
    for (;;) {
        uint8_t c[8];
        if (fread(c, 1, 8, a->f) != 8) goto fail;
        uint32_t size = rd32(c + 4);
        long body = ftell(a->f);
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t fmt[128];
            if (size < 20 || size > sizeof fmt || fread(fmt, 1, size, a->f) != size) goto fail;
            if (rd16(fmt) != 2) goto fail;   // MS ADPCM only
            a->channels = rd16(fmt + 2);
            a->rate = (int)rd32(fmt + 4);
            a->block_align = (uint16_t)rd16(fmt + 12);
            a->spb = (uint16_t)rd16(fmt + 18);
            a->ncoef = size >= 22 ? (uint16_t)rd16(fmt + 20) : 0;
            if (a->ncoef > 32 || size < 22u + a->ncoef * 4u) goto fail;
            for (int i = 0; i < a->ncoef; i++) {
                a->coef[i][0] = rd16(fmt + 22 + i * 4);
                a->coef[i][1] = rd16(fmt + 24 + i * 4);
            }
            if (a->channels < 1 || a->channels > 2 || a->rate <= 0 || a->ncoef < 1) goto fail;
            if (a->block_align < 7 * a->channels || a->spb < 2) goto fail;
            if (a->spb != 2 + (a->block_align - 7 * a->channels) * 2 / a->channels) goto fail;
            have_fmt = 1;
        } else if (!memcmp(c, "data", 4)) {
            if (!have_fmt) goto fail;
            a->data_start = body;
            a->data_size = size;
            break;
        }
        if (fseek(a->f, body + size + (size & 1), SEEK_SET)) goto fail;
    }
    a->block = malloc(a->block_align);
    if (!a->block) goto fail;
    return 1;
fail:
    fclose(a->f);
    a->f = NULL;
    return 0;
}

void adpcm_close(Adpcm *a) {
    if (a->f) fclose(a->f);
    free(a->block);
    memset(a, 0, sizeof *a);
}

void adpcm_rewind(Adpcm *a) {
    fseek(a->f, a->data_start, SEEK_SET);
    a->data_pos = 0;
}

long adpcm_total_frames(const Adpcm *a) {
    long full = a->data_size / a->block_align, rest = a->data_size % a->block_align;
    long n = full * a->spb;
    if (rest >= 7 * a->channels) n += 2 + (rest - 7 * a->channels) * 2 / a->channels;
    return n;
}

static inline int16_t clamp16(int x) { return x > 32767 ? 32767 : x < -32768 ? -32768 : (int16_t)x; }

int adpcm_decode_block(const Adpcm *a, const uint8_t *in, int bytes, int16_t *out) {
    int ch = a->channels;
    if (bytes < 7 * ch) return 0;
    int pred[2], delta[2], s1[2], s2[2];
    const uint8_t *p = in;
    for (int c = 0; c < ch; c++) { pred[c] = *p < a->ncoef ? *p : 0; p++; }
    for (int c = 0; c < ch; c++, p += 2) delta[c] = rd16(p);
    for (int c = 0; c < ch; c++, p += 2) s1[c] = rd16(p);
    for (int c = 0; c < ch; c++, p += 2) s2[c] = rd16(p);
    int frames = 2 + (bytes - 7 * ch) * 2 / ch;
    if (frames > a->spb) frames = a->spb;
    // the header holds the first two samples, older one first
    for (int c = 0; c < 2; c++) {
        out[0 + c] = (int16_t)s2[ch == 2 ? c : 0];
        out[2 + c] = (int16_t)s1[ch == 2 ? c : 0];
    }
    int c = 0;   // nibbles alternate channels (stereo); high nibble first
    int nib = 0, total = (frames - 2) * ch;
    for (int k = 0; k < total; k++) {
        int code = nib ? (*p++ & 15) : (*p >> 4);
        nib ^= 1;
        int sc = code >= 8 ? code - 16 : code;
        int predv = (s1[c] * a->coef[pred[c]][0] + s2[c] * a->coef[pred[c]][1]) >> 8;
        int s = clamp16(predv + sc * delta[c]);
        s2[c] = s1[c];
        s1[c] = s;
        delta[c] = (adapt[code] * delta[c]) >> 8;
        if (delta[c] < 16) delta[c] = 16;
        int f = 2 + k / ch;
        if (ch == 2) out[f * 2 + c] = (int16_t)s;
        else out[f * 2] = out[f * 2 + 1] = (int16_t)s;
        c = ch == 2 ? c ^ 1 : 0;
    }
    return frames;
}

int adpcm_read_block(Adpcm *a, int16_t *out) {
    long left = a->data_size - a->data_pos;
    if (left <= 0) return 0;
    int want = left < a->block_align ? (int)left : a->block_align;
    int got = (int)fread(a->block, 1, want, a->f);
    if (got < want) return -1;   // a read error (the position stays, for adpcm_reopen)
    a->data_pos += want;
    return adpcm_decode_block(a, a->block, got, out);
}

int adpcm_reopen(Adpcm *a, const char *path) {
    long pos = a->data_pos - a->data_pos % (a->block_align ? a->block_align : 1);
    adpcm_close(a);
    if (!adpcm_open(a, path)) return 0;
    if (fseek(a->f, a->data_start + pos, SEEK_SET)) { adpcm_close(a); return 0; }
    a->data_pos = pos;
    return 1;
}
