/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Install one received FUOTA package into the secondary slot.
 *
 * The TS004 descriptor selects the path. This file does not check the
 * business header CRC, device code, or version, and it does not reboot.
 */
#ifndef RZI_FUOTA_INSTALL_H
#define RZI_FUOTA_INSTALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

/** Descriptor 0x00000000: a full image for this module. */
#define RZI_FUOTA_DESC_FULL_WIRE 0x00000000U
/** Descriptor bytes 52 41 4B 30, ASCII "RAK0": a differential patch. */
#define RZI_FUOTA_DESC_DELTA_WIRE 0x304B4152U
/** Descriptor bytes 52 41 4B 31, ASCII "RAK1": a package for the host MCU. */
#define RZI_FUOTA_DESC_HOST_WIRE 0x314B4152U

/** How a FragSessionSetup descriptor is handled on this firmware. */
enum rzi_fuota_desc {
	/** Reject the session. Nothing is erased. */
	RZI_FUOTA_DESC_REJECT = 0,
	/** Full image. Fragments land in slot1 and a test upgrade follows. */
	RZI_FUOTA_DESC_FULL,
	/** Differential patch. Fragments land in fuota_delta. */
	RZI_FUOTA_DESC_DELTA,
	/** Host MCU package. Stored, then left for a forwarder. No reboot. */
	RZI_FUOTA_DESC_HOST,
};

/**
 * @brief Classify a TS004 descriptor for this firmware's capabilities.
 *
 * @param diff_supported True when the differential decoder and partition exist.
 * @param host_supported True when this firmware accepts a host MCU package.
 */
enum rzi_fuota_desc rzi_fuota_classify_descriptor(uint32_t descriptor, bool diff_supported,
						  bool host_supported);

/**
 * @brief Callbacks for one installation.
 *
 * read_pkg addresses the stored package, including its business header.
 * read_slot0 addresses the running signed image. write_slot1 appends bytes
 * that belong in the secondary slot. flush_slot1 commits the last buffered
 * bytes. Every callback returns 0 or a negative RZI_ERR_*.
 */
struct rzi_fuota_install_io {
	/** Read the received package at @p offset. */
	int (*read_pkg)(void *ctx, size_t offset, void *buf, size_t len);
	/** Context for read_pkg. */
	void *pkg_ctx;
	/** Size of the received package in bytes. */
	size_t pkg_size;
	/** Read the running signed image at @p offset. NULL when unused. */
	int (*read_slot0)(void *ctx, size_t offset, void *buf, size_t len);
	/** Context for read_slot0. */
	void *slot0_ctx;
	/** Size of the running signed image in bytes. */
	size_t slot0_size;
	/** Append bytes to the secondary slot. */
	int (*write_slot1)(void *ctx, const void *data, size_t len);
	/** Commit bytes still buffered for the secondary slot. */
	int (*flush_slot1)(void *ctx);
	/** Context for the secondary-slot callbacks. */
	void *slot1_ctx;
	/** Capacity of the secondary slot in bytes. */
	size_t slot1_cap;
};

/**
 * @brief Write a full image, or restore a differential image, into slot1.
 *
 * @param descriptor FragSessionSetup descriptor: 0 for a full image,
 *        0x304B4152 (ASCII "RAK0") for a differential patch.
 * @param header_length Bytes before the payload. The payload starts at
 *        offset 0 in @ref rzi_fuota_install_io.read_pkg. 0x314B4152
 *        (ASCII "RAK1") is not installed here.
 *
 * @retval 0 The secondary slot contains the signed image.
 * @retval -RZI_ERR_INVALID The descriptor or an I/O pointer is unusable.
 * @retval -RZI_ERR_NOT_SUPPORTED Diff support is not linked into this firmware.
 * @retval -RZI_ERR_BAD_MESSAGE The business header cannot be skipped.
 * @retval -RZI_ERR_TOO_LARGE The restored image does not fit in the slot.
 * @retval -RZI_ERR_ISR Called from an ISR.
 */
__must_check int rzi_fuota_install(uint32_t descriptor, uint16_t header_length,
					const struct rzi_fuota_install_io *io);

#endif /* RZI_FUOTA_INSTALL_H */
