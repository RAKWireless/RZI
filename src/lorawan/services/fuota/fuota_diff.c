/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief In-memory HPatchLite decoder used by differential FUOTA.
 *
 * The cache split matches hpatchi -s-256: 192 bytes for the patcher and
 * 64 bytes for the tinyuz input cache. The dictionary is the size stored
 * in the patch, and a dictionary above 512 bytes is rejected.
 */

#include "fuota_diff.h"

#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>

#include <rzi/err.h>

#include "hpatch_lite.h"
#include "tuz_dec.h"

#define RZI_FUOTA_DIFF_DICT_MAX 512U
#define RZI_FUOTA_DIFF_CACHE    256U
#define RZI_FUOTA_DIFF_PATCH_CACHE (((RZI_FUOTA_DIFF_CACHE) + 1U) * 3U / 4U)
#define RZI_FUOTA_DIFF_DEC_CACHE ((RZI_FUOTA_DIFF_CACHE) - (RZI_FUOTA_DIFF_PATCH_CACHE))

struct diff_cursor {
	const struct rzi_fuota_diff_io *io;
	size_t pos;
};

struct patch_job {
	hpatchi_listener_t listener;
	const struct rzi_fuota_diff_io *io;
	size_t written;
};

static hpi_BOOL read_patch(hpi_TInputStreamHandle handle, hpi_byte *out_data, hpi_size_t *data_size)
{
	struct diff_cursor *cursor = handle;
	size_t want = *data_size;
	size_t left = cursor->io->patch_size - cursor->pos;
	size_t take = want < left ? want : left;

	if (take > 0U && cursor->io->read_patch(cursor->io->patch_ctx, cursor->pos, out_data, take) != 0) {
		return hpi_FALSE;
	}
	cursor->pos += take;
	*data_size = (hpi_size_t)take;
	return hpi_TRUE;
}

static hpi_BOOL read_old(struct hpatchi_listener_t *listener, hpi_pos_t read_from_pos,
			 hpi_byte *out_data, hpi_size_t data_size)
{
	struct patch_job *job = (struct patch_job *)listener;
	const struct rzi_fuota_diff_io *io = job->io;

	if ((size_t)read_from_pos > io->old_size || (size_t)data_size > io->old_size - (size_t)read_from_pos) {
		return hpi_FALSE;
	}
	return io->read_old(io->old_ctx, (size_t)read_from_pos, out_data, data_size) == 0;
}

static hpi_BOOL write_new(struct hpatchi_listener_t *listener, const hpi_byte *data, hpi_size_t data_size)
{
	struct patch_job *job = (struct patch_job *)listener;

	if ((size_t)data_size > job->io->out_cap - job->written) {
		return hpi_FALSE;
	}
	if (job->io->write_new(job->io->out_ctx, data, data_size) != 0) {
		return hpi_FALSE;
	}
	job->written += data_size;
	return hpi_TRUE;
}

static hpi_BOOL read_decompressed(hpi_TInputStreamHandle handle, hpi_byte *out_data, hpi_size_t *data_size)
{
	tuz_TResult result = tuz_TStream_decompress_partial(handle, out_data, data_size);

	return result == tuz_OK || result == tuz_STREAM_END;
}

static int patch_with_io(const struct rzi_fuota_diff_io *io)
{
	struct diff_cursor cursor;
	struct patch_job job;
	tuz_TStream tuz;
	hpi_compressType compress_type = hpi_compressType_no;
	hpi_pos_t new_size = 0;
	hpi_pos_t uncompressed_size = 0;
	tuz_size_t dict_size;
	uint8_t tuz_mem[RZI_FUOTA_DIFF_DICT_MAX + RZI_FUOTA_DIFF_DEC_CACHE];
	uint8_t patch_cache[RZI_FUOTA_DIFF_PATCH_CACHE];

	cursor.io = io;
	cursor.pos = 0U;
	if (!hpatch_lite_open(&cursor, read_patch, &compress_type, &new_size, &uncompressed_size)) {
		return -RZI_ERR_BAD_MESSAGE;
	}
	if (compress_type != hpi_compressType_tuz) {
		return -RZI_ERR_NOT_SUPPORTED;
	}
	if (new_size > io->out_cap || new_size > (hpi_pos_t)INT32_MAX) {
		return -RZI_ERR_TOO_LARGE;
	}

	dict_size = tuz_TStream_read_dict_size(&cursor, read_patch);
	if (dict_size == 0U || dict_size > RZI_FUOTA_DIFF_DICT_MAX) {
		return -RZI_ERR_TOO_LARGE;
	}
	if (tuz_TStream_open(&tuz, &cursor, read_patch, tuz_mem, dict_size, RZI_FUOTA_DIFF_DEC_CACHE) !=
	    tuz_OK) {
		return -RZI_ERR_BAD_MESSAGE;
	}

	memset(&job, 0, sizeof(job));
	job.listener.diff_data = &tuz;
	job.listener.read_diff = read_decompressed;
	job.listener.read_old = read_old;
	job.listener.write_new = write_new;
	job.io = io;
	if (!hpatch_lite_patch(&job.listener, new_size, patch_cache, RZI_FUOTA_DIFF_PATCH_CACHE) ||
	    job.written != (size_t)new_size) {
		return -RZI_ERR_BAD_MESSAGE;
	}
	return (int)new_size;
}

struct memory_span {
	const uint8_t *data;
	size_t size;
};

struct memory_out {
	uint8_t *data;
	size_t written;
};

static int memory_read(void *ctx, size_t offset, void *buf, size_t len)
{
	struct memory_span *span = ctx;

	memcpy(buf, span->data + offset, len);
	return 0;
}

static int memory_write(void *ctx, const void *data, size_t len)
{
	struct memory_out *out = ctx;

	memcpy(out->data + out->written, data, len);
	out->written += len;
	return 0;
}

int rzi_fuota_diff_patch_io(const struct rzi_fuota_diff_io *io)
{
	if (k_is_in_isr()) {
		return -RZI_ERR_ISR;
	}
	if (io == NULL || io->read_old == NULL || io->read_patch == NULL || io->write_new == NULL ||
	    io->old_size == 0U || io->patch_size == 0U || io->out_cap == 0U) {
		return -RZI_ERR_INVALID;
	}
	return patch_with_io(io);
}

int rzi_fuota_diff_patch(const void *old_image, size_t old_size, const void *patch, size_t patch_size,
			 void *out, size_t out_cap)
{
	struct memory_span old_span;
	struct memory_span patch_span;
	struct memory_out output;
	struct rzi_fuota_diff_io io;

	if (old_image == NULL || patch == NULL || out == NULL) {
		return -RZI_ERR_INVALID;
	}
	old_span.data = old_image;
	old_span.size = old_size;
	patch_span.data = patch;
	patch_span.size = patch_size;
	output.data = out;
	output.written = 0U;
	memset(&io, 0, sizeof(io));
	io.read_old = memory_read;
	io.old_ctx = &old_span;
	io.old_size = old_size;
	io.read_patch = memory_read;
	io.patch_ctx = &patch_span;
	io.patch_size = patch_size;
	io.write_new = memory_write;
	io.out_ctx = &output;
	io.out_cap = out_cap;
	return rzi_fuota_diff_patch_io(&io);
}
