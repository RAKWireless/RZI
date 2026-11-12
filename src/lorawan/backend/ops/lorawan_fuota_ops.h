/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Backend operations for LoRaWAN FUOTA image access.
 */
#ifndef RZI_LORAWAN_FUOTA_OPS_H
#define RZI_LORAWAN_FUOTA_OPS_H

#include <stddef.h>
#include <stdint.h>
#include <rzi/lorawan/fuota_agent.h>

/** Optional FUOTA operations. NULL members return -RZI_ERR_NOT_SUPPORTED. */
struct rzi_lorawan_fuota_ops {
	/** Start ALCSync and optionally request MAC DeviceTime. */
	int (*start_clock_sync)(void);
	/** Return the reconstructed image size, or a negative RZI_ERR_*. */
	int (*get_image_size)(size_t *size);
	/** Read reconstructed image bytes from backend storage. */
	int (*read_image)(uint32_t offset, uint8_t *buffer, size_t size);
	/** Request a cold reboot. Does not return on success. */
	int (*reboot)(void);
	/** Managed FUOTA receive operations: zero means the operation actually completed, not queued.
	 * Generation mismatch must return NOT_FOUND without changing resources. */
	int (*get_session_info)(struct rzi_fuota_session_info *out);
	/** Optionally return final statistics for this generation before releasing the modem lock.
	 */
	int (*close_multicast)(uint32_t generation, struct rzi_fuota_rx_stats *stats);
	int (*restore_settings)(uint32_t generation);
	int (*cleanup_uplink)(uint32_t generation);
	int (*release_data)(uint32_t generation);
	/** Nonblocking gate, also covers the interval before START is dispatched. */
	bool (*business_blocked)(void);
};

#endif /* RZI_LORAWAN_FUOTA_OPS_H */
