/* jpeg_decoder_source.c */

#include "jpeg_roi_decoder.h"
#include <string.h>

/* ============================================================
 *  FILE* backend
 * ============================================================ */

static size_t file_read(void *ctx, uint8_t *buf, size_t nbyte)
{
    FILE *fp = (FILE *)ctx;
    if (buf)
        return fread(buf, 1, nbyte, fp);
    /* buf == NULL means skip forward */
    return fseek(fp, (long)nbyte, SEEK_CUR) == 0 ? nbyte : 0;
}

static int file_seek(void *ctx, size_t offset)
{
    return fseek((FILE *)ctx, (long)offset, SEEK_SET);
}

jpeg_source_t jpeg_decoder_source_from_file(FILE *fp)
{
    /* FILE* has no embedded state — safe to return by value */
    return (jpeg_source_t){
        .read = file_read,
        .seek = file_seek,
        .ctx  = fp,
    };
}

/* ============================================================
 *  Buffer backend
 *
 *  Uses an init-function (not a factory) so that ctx can point
 *  into the struct itself. The struct must already be in its
 *  final location before this is called — do not copy the
 *  jpeg_source_t afterwards or ctx becomes dangling.
 * ============================================================ */

static size_t buf_read(void *ctx, uint8_t *buf, size_t nbyte)
{
    /* ctx points to src._buf inside the caller's jpeg_source_t */
    __typeof__(((jpeg_source_t*)0)->_buf) *bc = ctx;

    size_t remaining = bc->len - bc->pos;
    if (nbyte > remaining)
        nbyte = remaining;

    if (buf)
        memcpy(buf, bc->data + bc->pos, nbyte);

    bc->pos += nbyte;
    return nbyte;
}

static int buf_seek(void *ctx, size_t offset)
{
    __typeof__(((jpeg_source_t*)0)->_buf) *bc = ctx;

    if (offset > bc->len)
        return -1;

    bc->pos = offset;
    return 0;
}

void jpeg_decoder_source_from_buffer(jpeg_source_t *src,
                                      const uint8_t *data,
                                      size_t         len)
{
    src->read      = buf_read;
    src->seek      = buf_seek;
    src->_buf.data = data;
    src->_buf.len  = len;
    src->_buf.pos  = 0;
    src->ctx       = &src->_buf;   /* safe — src is already in final location */
}