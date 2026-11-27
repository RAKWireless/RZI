/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief MCUboot test, confirm, and rollback status.
 */
#ifndef RZI_BOOT_H
#define RZI_BOOT_H

#include <stdbool.h>
#include <zephyr/toolchain.h>

#include <rzi/err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup rzi_boot RZI MCUboot image state
 * @brief Query the running MCUboot image state.
 *
 * Swap type values match MCUboot `BOOT_SWAP_TYPE_*`.
 *
 * @since 0.1.0
 * @version 0.1.0
 * @{
 */

/** No swap on the next reset. */
#define RZI_BOOT_SWAP_NONE   1
/** Secondary slot is requested as a one-shot test. */
#define RZI_BOOT_SWAP_TEST   2
/** Secondary slot is requested as a permanent upgrade. */
#define RZI_BOOT_SWAP_PERM   3
/** Primary slot will be reverted on the next reset. */
#define RZI_BOOT_SWAP_REVERT 4
/** The pending image failed validation. */
#define RZI_BOOT_SWAP_FAIL   5

/** Reported image state. */
enum rzi_boot_state {
	/** Running image is confirmed and no test or revert is pending. */
	RZI_BOOT_STATE_CONFIRMED,
	/** A test image is running, or a test swap is pending. */
	RZI_BOOT_STATE_TEST,
	/** Next reset permanently installs the secondary image. */
	RZI_BOOT_STATE_PERM,
	/** Next reset reverts a confirmed image. */
	RZI_BOOT_STATE_REVERT,
	/** Next reset will not install the pending image. */
	RZI_BOOT_STATE_FAIL,
};

/** Snapshot of the running image and the next MCUboot action. */
struct rzi_boot_status {
	/** Derived state. An unconfirmed image is always TEST. */
	enum rzi_boot_state state;
	/** MCUboot swap type for the next reset. */
	int next_swap;
	/** True when the running image has image_ok set, or has no trailer. */
	bool confirmed;
};

/**
 * @brief Classify a swap type and confirm flag.
 *
 * An unconfirmed image stays TEST for every next-swap value, including a
 * reset that has not written image_ok. Confirmation is never inferred from
 * a reboot.
 *
 * @param next_swap BOOT_SWAP_TYPE_* value.
 * @param confirmed True when the running image is confirmed.
 *
 * @return Derived image state.
 */
enum rzi_boot_state rzi_boot_classify(int next_swap, bool confirmed);

/**
 * @brief Static name for a derived image state.
 *
 * @param state Image state.
 *
 * @return `CONFIRMED`, `TEST`, `PERM`, `REVERT`, `FAIL`, or `UNKNOWN`.
 */
const char *rzi_boot_state_name(enum rzi_boot_state state);

/**
 * @brief Static name for an MCUboot swap type.
 *
 * @param next_swap BOOT_SWAP_TYPE_* value.
 *
 * @return `NONE`, `TEST`, `PERM`, `REVERT`, `FAIL`, or `UNKNOWN`.
 */
const char *rzi_boot_swap_name(int next_swap);

/**
 * @brief Read the running image state.
 *
 * @param status Output snapshot.
 *
 * @retval 0 Status written.
 * @retval -RZI_ERR_INVALID status is NULL.
 * @retval -RZI_ERR_NOT_SUPPORTED MCUboot image management is not enabled.
 * @retval -RZI_ERR_IO The trailer could not be read.
 */
__must_check int rzi_boot_get_status(struct rzi_boot_status *status);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* RZI_BOOT_H */
