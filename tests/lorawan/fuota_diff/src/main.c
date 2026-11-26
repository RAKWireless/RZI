/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <rzi/err.h>

#include "backend/lorawan_backend.h"

#include "fuota_diff.h"

#define FUOTA_HEADER_SIZE 32U

const struct rzi_lorawan_backend_api rzi_lorawan_backend = {0};

struct loaded_file {
	uint8_t *data;
	size_t size;
};

static struct loaded_file load_file(const char *path)
{
	struct loaded_file file = {0};
	FILE *input = fopen(path, "rb");
	long length;

	zassert_not_null(input, "missing %s", path);
	zassert_equal(fseek(input, 0, SEEK_END), 0);
	length = ftell(input);
	zassert_true(length > 0, "empty %s", path);
	zassert_equal(fseek(input, 0, SEEK_SET), 0);
	file.size = (size_t)length;
	file.data = malloc(file.size);
	zassert_not_null(file.data);
	zassert_equal(fread(file.data, 1, file.size, input), file.size);
	zassert_equal(fclose(input), 0);
	return file;
}

static void free_file(struct loaded_file *file)
{
	free(file->data);
	file->data = NULL;
	file->size = 0U;
}

ZTEST(rzi_lorawan_fuota_diff, test_patch_matches_the_new_signed_image)
{
	struct loaded_file old = load_file(RZI_FUOTA_DIFF_OLD);
	struct loaded_file package = load_file(RZI_FUOTA_DIFF_PKG);
	struct loaded_file expect = load_file(RZI_FUOTA_DIFF_NEW);
	uint8_t *restored;
	int written;

	zassert_true(package.size > FUOTA_HEADER_SIZE);
	restored = malloc(expect.size);
	zassert_not_null(restored);
	written = rzi_fuota_diff_patch(old.data, old.size, package.data + FUOTA_HEADER_SIZE,
				       package.size - FUOTA_HEADER_SIZE, restored, expect.size);
	zassert_equal(written, (int)expect.size);
	zassert_mem_equal(restored, expect.data, expect.size);

	free(restored);
	free_file(&old);
	free_file(&package);
	free_file(&expect);
}

ZTEST(rzi_lorawan_fuota_diff, test_rejected_inputs_do_not_report_success)
{
	struct loaded_file old = load_file(RZI_FUOTA_DIFF_OLD);
	struct loaded_file package = load_file(RZI_FUOTA_DIFF_PKG);
	uint8_t small[16];
	uint8_t *out = malloc(old.size);
	const uint8_t *patch = package.data + FUOTA_HEADER_SIZE;
	size_t patch_size = package.size - FUOTA_HEADER_SIZE;

	zassert_not_null(out);
	zassert_equal(rzi_fuota_diff_patch(NULL, old.size, patch, patch_size, small, sizeof(small)),
		      -RZI_ERR_INVALID);
	zassert_equal(rzi_fuota_diff_patch(old.data, 8U, patch, patch_size, out, old.size),
		      -RZI_ERR_BAD_MESSAGE);
	zassert_true(rzi_fuota_diff_patch(old.data, old.size, patch, 8U, out, old.size) < 0);
	zassert_equal(rzi_fuota_diff_patch(old.data, old.size, patch, patch_size, small, sizeof(small)),
		      -RZI_ERR_TOO_LARGE);
	free(out);

	free_file(&old);
	free_file(&package);
}

ZTEST_SUITE(rzi_lorawan_fuota_diff, NULL, NULL, NULL, NULL, NULL);
