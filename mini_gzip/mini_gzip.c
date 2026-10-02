/*
 * BSD 2-clause license
 * Copyright (c) 2013 Wojciech A. Koszek <wkoszek@FreeBSD.org>
 * 
 * Based on:
 * 
 * https://github.com/strake/gzip.git
 *
 * I had to rewrite it, since strake's version was powered by UNIX FILE* API,
 * while the key objective was to perform memory-to-memory operations
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#ifdef MINI_GZ_DEBUG
#include <stdio.h>
#endif

#include "miniz.h"
#include "mini_gzip.h"

int
mini_gz_start(struct mini_gzip *gz_ptr, void *mem, size_t mem_len)
{
	uint8_t		*hptr, *hauxptr, *mem8_ptr, *mem_end_ptr;
	uint16_t	fextra_len;

	assert(gz_ptr != NULL);

	mem8_ptr = (uint8_t *)mem;
	mem_end_ptr = mem8_ptr + mem_len;
	hptr = mem8_ptr + 0;		// .gz header
	hauxptr = mem8_ptr + 10;	// auxillary header

	gz_ptr->hdr_ptr = hptr;
	gz_ptr->data_ptr = 0;
	gz_ptr->data_len = 0;
	gz_ptr->total_len = mem_len;
	gz_ptr->chunk_size = 1024;

	if (mem_len < 10) {
		return (-1);
	}
	if (hptr[0] != 0x1F || hptr[1] != 0x8B) {
		GZDBG("hptr[0] = %02x hptr[1] = %02x\n", hptr[0], hptr[1]);
		return (-1);
	}
	if (hptr[2] != 8) {
		return (-2);
	}
	/* Every optional field below is sized by the member itself, so each one
	 * is kept inside [mem8_ptr, mem_end_ptr) before it is walked: the data
	 * comes from a file on disk and a truncated field would otherwise send
	 * the scans past the end of the mapping. */
	if (hptr[3] & 0x4) {
		if (hauxptr + 2 > mem_end_ptr) {
			return (-3);
		}
		fextra_len = hauxptr[1] << 8 | hauxptr[0];
		gz_ptr->fextra_len = fextra_len;
		hauxptr += 2;
		gz_ptr->fextra_ptr = hauxptr;
		/* Skip the extra field itself: without this the deflate stream is
		 * read from the padding instead of from the payload, so a member
		 * carrying FEXTRA (which is what src/pakfile.cc writes when it pads
		 * a patched entry) could not be unpacked. */
		if (hauxptr + fextra_len > mem_end_ptr) {
			return (-3);
		}
		hauxptr += fextra_len;
	}
	if (hptr[3] & 0x8) {
		gz_ptr->fname_ptr = hauxptr;
		while (hauxptr < mem_end_ptr && *hauxptr != '\0') {
			hauxptr++;
		}
		if (hauxptr >= mem_end_ptr) {
			return (-3);
		}
		hauxptr++;
	}
	if (hptr[3] & 0x10) {
		gz_ptr->fcomment_ptr = hauxptr;
		while (hauxptr < mem_end_ptr && *hauxptr != '\0') {
			hauxptr++;
		}
		if (hauxptr >= mem_end_ptr) {
			return (-3);
		}
		hauxptr++;
	}
	if (hptr[3] & 0x2) /* FCRC */ {
		if (hauxptr + 2 > mem_end_ptr) {
			return (-3);
		}
		gz_ptr->fcrc = (*(uint16_t *)hauxptr);
		hauxptr += 2;
	}
	if (hauxptr >= mem_end_ptr) {
		return (-3);
	}
	gz_ptr->data_ptr = hauxptr;
	gz_ptr->data_len = mem_len - (hauxptr - hptr);
	gz_ptr->magic = MINI_GZIP_MAGIC;
	return (0);
}

void
mini_gz_chunksize_set(struct mini_gzip *gz_ptr, int chunk_size)
{

	assert(gz_ptr != 0);
	assert(gz_ptr->magic == MINI_GZIP_MAGIC);
	gz_ptr->chunk_size = chunk_size;
}

void
mini_gz_init(struct mini_gzip *gz_ptr)
{

	memset(gz_ptr, 0xffffffff, sizeof(*gz_ptr));
	gz_ptr->magic = MINI_GZIP_MAGIC;
	mini_gz_chunksize_set(gz_ptr, 1024);
}


int
mini_gz_unpack(struct mini_gzip *gz_ptr, void *mem_out, size_t mem_out_len)
{
	z_stream s;
	int	ret, in_bytes_avail, bytes_to_read;

	assert(gz_ptr != 0);
	assert(gz_ptr->data_len > 0);
	assert(gz_ptr->magic == MINI_GZIP_MAGIC);

	memset (&s, 0, sizeof (z_stream));
	inflateInit2(&s, -MZ_DEFAULT_WINDOW_BITS);
	in_bytes_avail = gz_ptr->data_len;
	s.avail_out = mem_out_len;
	s.next_in = gz_ptr->data_ptr;
	s.next_out = mem_out;
	for (;;) {
		bytes_to_read = MINI_GZ_MIN(gz_ptr->chunk_size, in_bytes_avail);
		s.avail_in += bytes_to_read;
		ret = mz_inflate(&s, MZ_SYNC_FLUSH);
		in_bytes_avail -= bytes_to_read;
		if (s.avail_out == 0 && in_bytes_avail != 0) {
			return (-3);
		}
		assert(ret != MZ_BUF_ERROR);
		if (ret == MZ_PARAM_ERROR) {
			return (-1);
		}
		if (ret == MZ_DATA_ERROR) {
			return (-2);
		}
		if (ret == MZ_STREAM_END) {
			break;
		}
	}
	ret = inflateEnd(&s);
	if (ret != Z_OK) {
		return (-4);
	}
	return (s.total_out);
}

#include <stdlib.h>

#define GZIP_HEADER_SIZE	10
#define GZIP_TRAILER_SIZE	8

static void
gzip_put_le32(uint8_t *dst, uint32_t value)
{
	dst[0] = (uint8_t)(value & 0xFF);
	dst[1] = (uint8_t)((value >> 8) & 0xFF);
	dst[2] = (uint8_t)((value >> 16) & 0xFF);
	dst[3] = (uint8_t)((value >> 24) & 0xFF);
}

/*
 * Compresses `data` into one complete gzip member:
 *
 *     10-byte header | raw deflate stream | CRC-32 | ISIZE
 *
 * The consumer (src/pakfile.cc) stores the result back into the pak entry and
 * relies on that exact framing: it copies the first 10 bytes as the member
 * header, turns on FEXTRA, and relocates everything from offset 10 on behind
 * the extra field, so the header must be 10 bytes and the payload must be raw
 * deflate. mz_compress2()/mz_deflateInit() would emit a zlib stream instead
 * ("\x78\x01" + deflate + adler-32), which is not a gzip member at all.
 */
void* gzip_compress(uint8_t* data, size_t len, size_t* out_len) {
    mz_stream stream;
    mz_ulong bound;
    size_t deflate_len;
    int status;
    uint8_t* out_buf;

    if (!data || len == 0 || !out_len) return NULL;
    /* avail_in/total_in are 32-bit, so the whole input must fit in one pass. */
    if ((uint64_t)len > 0xFFFFFFFFu) return NULL;

    bound = mz_deflateBound(NULL, (mz_ulong)len);
    out_buf = (uint8_t*)malloc(GZIP_HEADER_SIZE + (size_t)bound +
                               GZIP_TRAILER_SIZE);
    if (!out_buf) return NULL;

    memset(&stream, 0, sizeof(stream));
    stream.next_in = data;
    stream.avail_in = (mz_uint32)len;
    stream.next_out = out_buf + GZIP_HEADER_SIZE;
    stream.avail_out = (mz_uint32)bound;

    /* A negative window size selects raw deflate: no zlib header/adler-32,
     * because the gzip header and trailer below replace them. */
    status = mz_deflateInit2(&stream, 6, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS,
                             9, MZ_DEFAULT_STRATEGY);
    if (status != MZ_OK) {
        free(out_buf);
        return NULL;
    }

    status = mz_deflate(&stream, MZ_FINISH);
    if (status != MZ_STREAM_END) {
        mz_deflateEnd(&stream);
        free(out_buf);
        return NULL;
    }
    deflate_len = (size_t)stream.total_out;
    mz_deflateEnd(&stream);

    /* ID1 ID2 CM FLG MTIME(4) XFL OS. FLG stays 0 here; the pak writer turns
     * on FEXTRA itself when it pads the entry to its original slot length. */
    out_buf[0] = 0x1F;	/* ID1 */
    out_buf[1] = 0x8B;	/* ID2 */
    out_buf[2] = 8;	/* CM: deflate */
    out_buf[3] = 0;	/* FLG */
    out_buf[4] = 0;	/* MTIME */
    out_buf[5] = 0;
    out_buf[6] = 0;
    out_buf[7] = 0;
    out_buf[8] = 0;	/* XFL */
    out_buf[9] = 0xFF;	/* OS: unknown */

    /* CRC-32 of the uncompressed data, then ISIZE (size modulo 2^32). */
    gzip_put_le32(out_buf + GZIP_HEADER_SIZE + deflate_len,
                  (uint32_t)mz_crc32(MZ_CRC32_INIT,
                                     (const unsigned char *)data, len));
    gzip_put_le32(out_buf + GZIP_HEADER_SIZE + deflate_len + 4,
                  (uint32_t)((uint64_t)len & 0xFFFFFFFFu));

    *out_len = GZIP_HEADER_SIZE + deflate_len + GZIP_TRAILER_SIZE;
    return out_buf;
}