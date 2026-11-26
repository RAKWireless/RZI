/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuota_diff.h"

#define FUOTA_HEADER_SIZE 32U

struct loaded_file {
	uint8_t *data;
	size_t size;
};

static struct loaded_file load_file(const char *path)
{
	struct loaded_file file = {0};
	FILE *input = fopen(path, "rb");
	long length;

	if (input == NULL) {
		fprintf(stderr, "missing %s\n", path);
		exit(1);
	}
	if (fseek(input, 0, SEEK_END) != 0) {
		exit(1);
	}
	length = ftell(input);
	if (length <= 0 || fseek(input, 0, SEEK_SET) != 0) {
		exit(1);
	}
	file.size = (size_t)length;
	file.data = malloc(file.size);
	if (file.data == NULL || fread(file.data, 1, file.size, input) != file.size) {
		exit(1);
	}
	fclose(input);
	return file;
}

int main(void)
{
	const char *old_path = "D:/Git_code/rzi-workspace/build/fuota-key-check/old.signed.bin";
	const char *new_path = "D:/Git_code/rzi-workspace/build/fuota-key-check/new.signed.bin";
	const char *pkg_path = "D:/Git_code/rzi-workspace/build/fuota-key-check/diff/firmware.bin";
	struct loaded_file old = load_file(old_path);
	struct loaded_file package = load_file(pkg_path);
	struct loaded_file expect = load_file(new_path);
	uint8_t *restored = malloc(expect.size);
	const uint8_t *patch = package.data + FUOTA_HEADER_SIZE;
	size_t patch_size = package.size - FUOTA_HEADER_SIZE;
	uint8_t small[16];
	int written;
	int restored_size;

	if (restored == NULL || package.size <= FUOTA_HEADER_SIZE) {
		return 1;
	}
	restored_size = rzi_fuota_diff_patch(old.data, old.size, patch, patch_size, restored, expect.size);
	written = restored_size;
	if (written != (int)expect.size || memcmp(restored, expect.data, expect.size) != 0) {
		fprintf(stderr, "restore mismatch written=%d expect=%zu\n", written, expect.size);
		return 1;
	}
	if (rzi_fuota_diff_patch(NULL, old.size, patch, patch_size, small, sizeof(small)) != -1000) {
		fprintf(stderr, "null input was accepted\n");
		return 1;
	}
	written = rzi_fuota_diff_patch(old.data, 8U, patch, patch_size, restored, expect.size);
	if (written != -1013) {
		fprintf(stderr, "short old image returned %d\n", written);
		return 1;
	}
	written = rzi_fuota_diff_patch(old.data, old.size, patch, 8U, restored, expect.size);
	if (written >= 0) {
		fprintf(stderr, "truncated patch returned %d\n", written);
		return 1;
	}
	if (rzi_fuota_diff_patch(old.data, old.size, patch, patch_size, small, sizeof(small)) != -1009) {
		fprintf(stderr, "small output buffer was not rejected\n");
		return 1;
	}
	printf("restored %d bytes\n", restored_size);
	free(restored);
	free(old.data);
	free(package.data);
	free(expect.data);
	return 0;
}
