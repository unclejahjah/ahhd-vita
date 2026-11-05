// Reader for the game's sound files: RIFF WAV with MS ADPCM (format 2), mono or stereo.
// The PC data names them .adpcm. Decodes to interleaved stereo int16 (mono is doubled).
#ifndef AH_MSADPCM_H
#define AH_MSADPCM_H

#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE *f;
    int channels, rate, block_align, spb;   // spb: sample frames per block
    int ncoef;
    int16_t coef[32][2];
    long data_start, data_size, data_pos;    // data_pos: bytes of the data chunk read so far
    uint8_t *block;
} Adpcm;

int adpcm_open(Adpcm *a, const char *path);   // 1 on success
void adpcm_close(Adpcm *a);
void adpcm_rewind(Adpcm *a);
// Decodes the next block into out (room for spb stereo frames). Returns frames, 0 at the end,
// -1 if the file couldn't be read.
int adpcm_read_block(Adpcm *a, int16_t *out);
// Opens the file again and carries on from the same place (the old handle can go bad, e.g.
// while the Vita had the app suspended). 0 if it can't.
int adpcm_reopen(Adpcm *a, const char *path);
// Decodes one block from memory (for tests and whole-file decoding).
int adpcm_decode_block(const Adpcm *a, const uint8_t *in, int bytes, int16_t *out);
long adpcm_total_frames(const Adpcm *a);

#endif
