/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief MCUboot image status query.
 */

#include <rzi/boot.h>

#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
#include <zephyr/dfu/mcuboot.h>
/* boot_swap_type_multi() uses this for a trailer read failure. It is not a negative errno. */
#define RZI_BOOT_SWAP_PANIC 0xff
#endif

enum rzi_boot_state rzi_boot_classify(int next_swap, bool confirmed)
{
	if (!confirmed) {
		return RZI_BOOT_STATE_TEST;
	}
	switch (next_swap) {
	case RZI_BOOT_SWAP_TEST:
		return RZI_BOOT_STATE_TEST;
	case RZI_BOOT_SWAP_PERM:
		return RZI_BOOT_STATE_PERM;
	case RZI_BOOT_SWAP_REVERT:
		return RZI_BOOT_STATE_REVERT;
	case RZI_BOOT_SWAP_FAIL:
		return RZI_BOOT_STATE_FAIL;
	default:
		return RZI_BOOT_STATE_CONFIRMED;
	}
}

const char *rzi_boot_state_name(enum rzi_boot_state state)
{
	switch (state) {
	case RZI_BOOT_STATE_CONFIRMED:
		return "CONFIRMED";
	case RZI_BOOT_STATE_TEST:
		return "TEST";
	case RZI_BOOT_STATE_PERM:
		return "PERM";
	case RZI_BOOT_STATE_REVERT:
		return "REVERT";
	case RZI_BOOT_STATE_FAIL:
		return "FAIL";
	default:
		return "UNKNOWN";
	}
}

const char *rzi_boot_swap_name(int next_swap)
{
	switch (next_swap) {
	case RZI_BOOT_SWAP_NONE:
		return "NONE";
	case RZI_BOOT_SWAP_TEST:
		return "TEST";
	case RZI_BOOT_SWAP_PERM:
		return "PERM";
	case RZI_BOOT_SWAP_REVERT:
		return "REVERT";
	case RZI_BOOT_SWAP_FAIL:
		return "FAIL";
	default:
		return "UNKNOWN";
	}
}

int rzi_boot_get_status(struct rzi_boot_status *status)
{
	if (status == NULL) {
		return -RZI_ERR_INVALID;
	}
#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
	int next_swap = mcuboot_swap_type();

	if (next_swap < 0) {
		return rzi_err_from_errno(next_swap);
	}
	if (next_swap == RZI_BOOT_SWAP_PANIC) {
		return -RZI_ERR_IO;
	}
	status->confirmed = boot_is_img_confirmed();
	status->next_swap = next_swap;
	status->state = rzi_boot_classify(next_swap, status->confirmed);
	return 0;
#else
	return -RZI_ERR_NOT_SUPPORTED;
#endif
}
