/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Copy or restore a received FUOTA package into the secondary slot.
 */

#include "fuota_install.h"

#include <string.h>

#include <zephyr/kernel.h>

#include <rzi/err.h>

#include "fuota_header.h"

#if defined(CONFIG_RZI_LORAWAN_FUOTA_DIFF) || defined(RZI_FUOTA_DIFF_HOST)
#include "fuota_diff.h"
#define FUOTA_INSTALL_DIFF 1
#endif

#define FUOTA_HEADER_MIN 32U
#define FUOTA_COPY_CHUNK 256U

struct pkg_window {
	const struct rzi_fuota_install_io *io;
	size_t base;
};

enum rzi_fuota_desc rzi_fuota_classify_descriptor(uint32_t descriptor, bool diff_supported,
						  bool host_supported)
{
	if (descriptor == RZI_FUOTA_DESC_FULL_WIRE) {
		return RZI_FUOTA_DESC_FULL;
	}
	if (descriptor == RZI_FUOTA_DESC_DELTA_WIRE) {
		return diff_supported ? RZI_FUOTA_DESC_DELTA : RZI_FUOTA_DESC_REJECT;
	}
	if (descriptor == RZI_FUOTA_DESC_HOST_WIRE) {
		return host_supported ? RZI_FUOTA_DESC_HOST : RZI_FUOTA_DESC_REJECT;
	}
	return RZI_FUOTA_DESC_REJECT;
}

static uint32_t load_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
	       ((uint32_t)bytes[3] << 24);
}

static uint16_t load_le16(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static int read_exact(const struct rzi_fuota_install_io *io, size_t offset, void *buf, size_t len)
{
	if (offset > io->pkg_size || len > io->pkg_size - offset) {
		return -RZI_ERR_BAD_MESSAGE;
	}
	return io->read_pkg(io->pkg_ctx, offset, buf, len);
}

static int copy_payload(const struct rzi_fuota_install_io *io, size_t offset, size_t length)
{
	uint8_t chunk[FUOTA_COPY_CHUNK];
	size_t done = 0U;

	if (length > io->slot1_cap) {
		return -RZI_ERR_TOO_LARGE;
	}
	while (done < length) {
		size_t request = length - done;

		if (request > sizeof(chunk)) {
			request = sizeof(chunk);
		}
		int rc = read_exact(io, offset + done, chunk, request);

		if (rc != 0) {
			return rc;
		}
		rc = io->write_slot1(io->slot1_ctx, chunk, request);
		if (rc != 0) {
			return rc;
		}
		done += request;
	}
	return io->flush_slot1(io->slot1_ctx);
}

#ifdef FUOTA_INSTALL_DIFF
static int read_patch_window(void *ctx, size_t offset, void *buf, size_t len)
{
	struct pkg_window *window = ctx;

	return window->io->read_pkg(window->io->pkg_ctx, window->base + offset, buf, len);
}

static int restore_patch(const struct rzi_fuota_install_io *io, size_t offset, size_t length)
{
	struct pkg_window window;
	struct rzi_fuota_diff_io diff;

	if (io->read_slot0 == NULL || io->slot0_size == 0U) {
		return -RZI_ERR_NOT_SUPPORTED;
	}
	window.io = io;
	window.base = offset;
	memset(&diff, 0, sizeof(diff));
	diff.read_old = io->read_slot0;
	diff.old_ctx = io->slot0_ctx;
	diff.old_size = io->slot0_size;
	diff.read_patch = read_patch_window;
	diff.patch_ctx = &window;
	diff.patch_size = length;
	diff.write_new = io->write_slot1;
	diff.out_ctx = io->slot1_ctx;
	diff.out_cap = io->slot1_cap;

	int written = rzi_fuota_diff_patch_io(&diff);

	if (written < 0) {
		return written;
	}
	return io->flush_slot1(io->slot1_ctx);
}
#endif

int rzi_fuota_install(uint32_t descriptor, uint16_t header_length,
		      const struct rzi_fuota_install_io *io)
{
	if (k_is_in_isr()) {
		return -RZI_ERR_ISR;
	}
	if (io == NULL || io->read_pkg == NULL || io->write_slot1 == NULL || io->flush_slot1 == NULL ||
	    io->pkg_size == 0U || io->slot1_cap == 0U) {
		return -RZI_ERR_INVALID;
	}
	if (descriptor != RZI_FUOTA_DESC_FULL_WIRE && descriptor != RZI_FUOTA_DESC_DELTA_WIRE) {
		return -RZI_ERR_INVALID;
	}
	if (header_length == 0U || header_length >= io->pkg_size) {
		return -RZI_ERR_BAD_MESSAGE;
	}

	if (descriptor == RZI_FUOTA_DESC_FULL_WIRE) {
		return copy_payload(io, 0U, io->pkg_size - header_length);
	}
#ifdef FUOTA_INSTALL_DIFF
	return restore_patch(io, 0U, io->pkg_size - header_length);
#else
	return -RZI_ERR_NOT_SUPPORTED;
#endif
}
