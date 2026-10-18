#pragma once

#include "core.h"

#define ENC_OK        0
#define ENC_ERROR    -1
#define ENC_OVERFLOW -2 /* frame did not fit in the output buffer */

typedef struct jpeg_enc jpeg_enc;

jpeg_enc *jpeg_enc_create(void);
void      jpeg_enc_destroy(jpeg_enc *e);

/* Start a frame. Lines are then fed in order with jpeg_enc_lines(). With
 * half, each pair of lines becomes one (averaged), so the picture has half as
 * many lines and takes about half as long; counts passed in must be even. */
int  jpeg_enc_begin(jpeg_enc *e, const frame_geom *g, int quality, bool sharp, bool half,
                    uint8_t *out, uint32_t cap);
int  jpeg_enc_lines(jpeg_enc *e, const uint8_t *src, int count);
int  jpeg_enc_finish(jpeg_enc *e, uint32_t *out_len);
void jpeg_enc_abort(jpeg_enc *e);
