/*
 * Streaming JPEG encoder on top of libjpeg(-turbo).
 *
 * Lines arrive a strip at a time straight from the framebuffer copy, so we
 * never hold a whole decoded frame in memory. 24/32-bit framebuffers are fed
 * to libjpeg-turbo untouched using its extended colour spaces; 16-bit formats
 * are expanded one line at a time.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

#include "jpeg.h"

#define MAX_LINE_PIXELS 256

struct jpeg_enc {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    struct jpeg_destination_mgr dest;
    jmp_buf jb;

    uint8_t *out;
    uint32_t cap;
    bool overflow;
    bool active;
    frame_geom g;
    uint8_t row[MAX_LINE_PIXELS * 3];
};

static void on_error_exit(j_common_ptr c)
{
    jpeg_enc *e = (jpeg_enc *)c->client_data;
    longjmp(e->jb, 1);
}

static void on_output_message(j_common_ptr c)
{
    (void)c;
}

static void dest_init(j_compress_ptr c)
{
    jpeg_enc *e = (jpeg_enc *)c->client_data;
    c->dest->next_output_byte = e->out;
    c->dest->free_in_buffer = e->cap;
}

static boolean dest_empty(j_compress_ptr c)
{
    /* The fixed output buffer is full: give up on this frame. */
    jpeg_enc *e = (jpeg_enc *)c->client_data;
    e->overflow = true;
    longjmp(e->jb, 1);
    return FALSE;
}

static void dest_term(j_compress_ptr c)
{
    (void)c;
}

static int create_compress(jpeg_enc *e)
{
    if (setjmp(e->jb))
        return -1;
    jpeg_create_compress(&e->cinfo);
    return 0;
}

jpeg_enc *jpeg_enc_create(void)
{
    jpeg_enc *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;

    e->cinfo.err = jpeg_std_error(&e->jerr);
    e->jerr.error_exit = on_error_exit;
    e->jerr.output_message = on_output_message;
    e->cinfo.client_data = e;
    if (create_compress(e) != 0) {
        free(e);
        return NULL;
    }

    e->dest.init_destination = dest_init;
    e->dest.empty_output_buffer = dest_empty;
    e->dest.term_destination = dest_term;
    e->cinfo.dest = &e->dest;
    return e;
}

void jpeg_enc_destroy(jpeg_enc *e)
{
    if (!e)
        return;
    jpeg_destroy_compress(&e->cinfo);
    free(e);
}

static int fail(jpeg_enc *e)
{
    jpeg_abort_compress(&e->cinfo);
    e->active = false;
    return e->overflow ? ENC_OVERFLOW : ENC_ERROR;
}

int jpeg_enc_begin(jpeg_enc *e, const frame_geom *g, int quality, bool sharp,
                   uint8_t *out, uint32_t cap)
{
    if (g->width <= 0 || g->width > MAX_LINE_PIXELS || g->lines <= 0)
        return ENC_ERROR;

    e->g = *g;
    e->out = out;
    e->cap = cap;
    e->overflow = false;

    struct jpeg_compress_struct *c = &e->cinfo;
    if (setjmp(e->jb))
        return fail(e);

    c->image_width = (JDIMENSION)g->width;
    c->image_height = (JDIMENSION)g->lines;
    switch (g->format) {
    case PIX_RGBA8:
        /* GSP RGBA8 is stored A,B,G,R in memory. */
        c->input_components = 4;
        c->in_color_space = JCS_EXT_ABGR;
        break;
    case PIX_BGR8:
        c->input_components = 3;
        c->in_color_space = JCS_EXT_BGR;
        break;
    default:
        c->input_components = 3;
        c->in_color_space = JCS_RGB;
        break;
    }

    jpeg_set_defaults(c);
    jpeg_set_quality(c, quality, TRUE);
    c->dct_method = JDCT_IFAST;
    c->optimize_coding = FALSE;
    /* jpeg_set_defaults() picks 4:2:0; "sharp" keeps full colour detail,
     * which helps small coloured text at the cost of bandwidth. */
    c->comp_info[0].h_samp_factor = sharp ? 1 : 2;
    c->comp_info[0].v_samp_factor = sharp ? 1 : 2;

    jpeg_start_compress(c, TRUE);
    e->active = true;
    return ENC_OK;
}

static void expand_line(uint8_t *dst, const uint8_t *src, int width, int format)
{
    const uint16_t *px = (const uint16_t *)src;
    for (int i = 0; i < width; i++, dst += 3) {
        unsigned p = px[i], r, g, b;
        switch (format) {
        case PIX_RGB565:
            r = (p >> 11) & 0x1F;
            g = (p >> 5) & 0x3F;
            b = p & 0x1F;
            dst[0] = (uint8_t)((r << 3) | (r >> 2));
            dst[1] = (uint8_t)((g << 2) | (g >> 4));
            dst[2] = (uint8_t)((b << 3) | (b >> 2));
            break;
        case PIX_RGB5A1:
            r = (p >> 11) & 0x1F;
            g = (p >> 6) & 0x1F;
            b = (p >> 1) & 0x1F;
            dst[0] = (uint8_t)((r << 3) | (r >> 2));
            dst[1] = (uint8_t)((g << 3) | (g >> 2));
            dst[2] = (uint8_t)((b << 3) | (b >> 2));
            break;
        default: /* PIX_RGBA4 */
            r = (p >> 12) & 0xF;
            g = (p >> 8) & 0xF;
            b = (p >> 4) & 0xF;
            dst[0] = (uint8_t)(r * 17);
            dst[1] = (uint8_t)(g * 17);
            dst[2] = (uint8_t)(b * 17);
            break;
        }
    }
}

/* Runs under the setjmp armed by jpeg_enc_lines(). */
static int write_lines(jpeg_enc *e, const uint8_t *src, int count)
{
    struct jpeg_compress_struct *c = &e->cinfo;
    const int stride = e->g.stride;
    if (e->g.format == PIX_RGBA8 || e->g.format == PIX_BGR8) {
        JSAMPROW rows[16];
        while (count > 0) {
            int n = count < 16 ? count : 16;
            for (int i = 0; i < n; i++)
                rows[i] = (JSAMPROW)(src + i * stride);
            JDIMENSION done = jpeg_write_scanlines(c, rows, (JDIMENSION)n);
            if (done == 0)
                return fail(e);
            src += done * stride;
            count -= (int)done;
        }
    } else {
        JSAMPROW row = e->row;
        for (; count > 0; count--, src += stride) {
            expand_line(e->row, src, e->g.width, e->g.format);
            if (jpeg_write_scanlines(c, &row, 1) != 1)
                return fail(e);
        }
    }
    return ENC_OK;
}

int jpeg_enc_lines(jpeg_enc *e, const uint8_t *src, int count)
{
    if (!e->active)
        return ENC_ERROR;
    if (setjmp(e->jb))
        return fail(e);
    return write_lines(e, src, count);
}

int jpeg_enc_finish(jpeg_enc *e, uint32_t *out_len)
{
    if (!e->active)
        return ENC_ERROR;
    if (setjmp(e->jb))
        return fail(e);

    jpeg_finish_compress(&e->cinfo);
    e->active = false;
    *out_len = e->cap - (uint32_t)e->dest.free_in_buffer;
    return ENC_OK;
}

void jpeg_enc_abort(jpeg_enc *e)
{
    if (e->active) {
        jpeg_abort_compress(&e->cinfo);
        e->active = false;
    }
}
