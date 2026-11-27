/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */

#include <rzi/boot.h>
#include <zephyr/ztest.h>

ZTEST(rzi_boot_state, test_confirmed_image_follows_next_swap)
{
	zassert_equal(rzi_boot_classify(RZI_BOOT_SWAP_NONE, true), RZI_BOOT_STATE_CONFIRMED);
	zassert_equal(rzi_boot_classify(RZI_BOOT_SWAP_TEST, true), RZI_BOOT_STATE_TEST);
	zassert_equal(rzi_boot_classify(RZI_BOOT_SWAP_PERM, true), RZI_BOOT_STATE_PERM);
	zassert_equal(rzi_boot_classify(RZI_BOOT_SWAP_REVERT, true), RZI_BOOT_STATE_REVERT);
	zassert_equal(rzi_boot_classify(RZI_BOOT_SWAP_FAIL, true), RZI_BOOT_STATE_FAIL);
}

ZTEST(rzi_boot_state, test_unconfirmed_image_stays_test)
{
	zassert_equal(rzi_boot_classify(RZI_BOOT_SWAP_NONE, false), RZI_BOOT_STATE_TEST);
	zassert_equal(rzi_boot_classify(RZI_BOOT_SWAP_TEST, false), RZI_BOOT_STATE_TEST);
	zassert_equal(rzi_boot_classify(RZI_BOOT_SWAP_PERM, false), RZI_BOOT_STATE_TEST);
	zassert_equal(rzi_boot_classify(RZI_BOOT_SWAP_REVERT, false), RZI_BOOT_STATE_TEST);
	zassert_equal(rzi_boot_classify(RZI_BOOT_SWAP_FAIL, false), RZI_BOOT_STATE_TEST);
}

ZTEST(rzi_boot_state, test_names)
{
	zassert_str_equal(rzi_boot_state_name(RZI_BOOT_STATE_CONFIRMED), "CONFIRMED");
	zassert_str_equal(rzi_boot_state_name(RZI_BOOT_STATE_TEST), "TEST");
	zassert_str_equal(rzi_boot_swap_name(RZI_BOOT_SWAP_REVERT), "REVERT");
	zassert_str_equal(rzi_boot_swap_name(0), "UNKNOWN");
}

ZTEST(rzi_boot_state, test_query_without_image_manager)
{
	zassert_equal(rzi_boot_get_status(NULL), -RZI_ERR_INVALID);
	zassert_equal(rzi_boot_get_status(&(struct rzi_boot_status){0}), -RZI_ERR_NOT_SUPPORTED);
}

ZTEST_SUITE(rzi_boot_state, NULL, NULL, NULL, NULL, NULL);
