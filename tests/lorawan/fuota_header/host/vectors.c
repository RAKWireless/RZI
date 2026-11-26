/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */

/* Replay the generated rejection fixtures through the device header check. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuota_header.h"

struct blob {
	uint8_t *data;
	size_t size;
};

static int load(const char *path, struct blob *out)
{
	FILE *file = fopen(path, "rb");
	long size;

	if (file == NULL) {
		return -1;
	}
	if (fseek(file, 0L, SEEK_END) != 0 || (size = ftell(file)) <= 0L ||
	    fseek(file, 0L, SEEK_SET) != 0) {
		fclose(file);
		return -1;
	}
	out->data = malloc((size_t)size);
	out->size = (size_t)size;
	if (out->data == NULL || fread(out->data, 1U, out->size, file) != out->size) {
		free(out->data);
		fclose(file);
		return -1;
	}
	fclose(file);
	return 0;
}

static int read_payload(void *ctx, size_t offset, void *buf, size_t len)
{
	struct blob *blob = ctx;

	if (offset < FUOTA_HDR_LEN_V1 || offset + len > blob->size) {
		return -1;
	}
	memcpy(buf, blob->data + offset, len);
	return 0;
}

static enum fuota_hdr_result verify(struct blob *blob, const uint8_t current[4])
{
	struct fuota_hdr_expect expect;

	memset(&expect, 0, sizeof(expect));
	expect.device_code = 3U;
	expect.have_version = true;
	memcpy(expect.current_version, current, 4U);
	expect.package_kind = 0U;
	expect.payload_length = blob->size - FUOTA_HDR_LEN_V1;
	return fuota_hdr_verify(blob->data, &expect, read_payload, blob);
}

int main(int argc, char **argv)
{
	static const uint8_t current[4] = {1U, 1U, 0U, 2U};
	static const struct {
		const char *file;
		enum fuota_hdr_result want;
	} cases[] = {
		{"firmware.bin", FUOTA_HDR_OK},
		{"vectors/same_version.bin", FUOTA_HDR_OK},
		{"vectors/device_mismatch.bin", FUOTA_HDR_PRODUCT},
		{"vectors/header_crc.bin", FUOTA_HDR_CRC},
		{"vectors/base_mismatch.bin", FUOTA_HDR_BASE},
	};
	char path[512];
	int failures = 0;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <vector-dir>\n", argv[0]);
		return 2;
	}
	for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct blob blob = {0};
		enum fuota_hdr_result got;

		snprintf(path, sizeof(path), "%s/%s", argv[1], cases[i].file);
		if (load(path, &blob) != 0) {
			fprintf(stderr, "cannot read %s\n", path);
			return 2;
		}
		got = verify(&blob, current);
		free(blob.data);
		if (got != cases[i].want) {
			fprintf(stderr, "%s: got %d, want %d\n", cases[i].file, (int)got,
				(int)cases[i].want);
			failures++;
		}
	}
	if (failures != 0) {
		return 1;
	}
	printf("vector replay passed: %zu cases\n", sizeof(cases) / sizeof(cases[0]));
	return 0;
}