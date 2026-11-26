/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Restore one HPatchLite+tinyuz patch in memory.
 *
 * The caller supplies the old signed image and a patch with the 32-byte
 * FUOTA header already removed. This function does not write a flash slot
 * and does not call the bootloader.
 */
#ifndef RZI_FUOTA_DIFF_H
#define RZI_FUOTA_DIFF_H

#include <stddef.h>
#include <zephyr/toolchain.h>

/**
 * @brief Restore a new signed image from an old image and a tinyuz patch.
 *
 * @param old_image Old signed image. Read only.
 * @param old_size Size of @p old_image in bytes.
 * @param patch Patch bytes produced by hdiffi -c-tuz-512, without a FUOTA header.
 * @param patch_size Size of @p patch in bytes.
 * @param out Buffer that receives the restored image.
 * @param out_cap Capacity of @p out in bytes.
 *
 * @retval >0 Number of bytes written to @p out. This is the new image size.
 * @retval -RZI_ERR_INVALID A pointer is NULL, or a size is zero.
 * @retval -RZI_ERR_NOT_SUPPORTED The patch is not tinyuz.
 * @retval -RZI_ERR_TOO_LARGE The new image does not fit, or the dictionary exceeds 512 bytes.
 * @retval -RZI_ERR_BAD_MESSAGE The patch, old image, or output write failed.
 * @retval -RZI_ERR_ISR Called from an ISR.
 */
__must_check int rzi_fuota_diff_patch(const void *old_image, size_t old_size, const void *patch,
				      size_t patch_size, void *out, size_t out_cap);

/**
 * @brief Byte source and sink used to restore a patch without holding it in RAM.
 *
 * Read callbacks copy exactly @p len bytes and return 0, or a negative error.
 * @ref rzi_fuota_diff_patch_io calls read_patch sequentially and read_old at
 * arbitrary offsets. write_new appends the restored image.
 */
struct rzi_fuota_diff_io {
	/** Read the old signed image at @p offset. */
	int (*read_old)(void *ctx, size_t offset, void *buf, size_t len);
	/** Context for read_old. */
	void *old_ctx;
	/** Size of the old signed image in bytes. */
	size_t old_size;
	/** Read the patch, without a FUOTA header, at @p offset. */
	int (*read_patch)(void *ctx, size_t offset, void *buf, size_t len);
	/** Context for read_patch. */
	void *patch_ctx;
	/** Size of the patch in bytes. */
	size_t patch_size;
	/** Append restored image bytes. */
	int (*write_new)(void *ctx, const void *data, size_t len);
	/** Context for write_new. */
	void *out_ctx;
	/** Maximum number of bytes write_new may accept. */
	size_t out_cap;
};

/**
 * @brief Restore a patch through @ref rzi_fuota_diff_io.
 *
 * Return values match @ref rzi_fuota_diff_patch. A NULL callback or a zero
 * size returns -RZI_ERR_INVALID.
 */
__must_check int rzi_fuota_diff_patch_io(const struct rzi_fuota_diff_io *io);

#endif /* RZI_FUOTA_DIFF_H */
