/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuota_install.h"

#define HEADER_SIZE 32U
#define DESC_DELTA RZI_FUOTA_DESC_DELTA_WIRE
#define DESC_HOST RZI_FUOTA_DESC_HOST_WIRE
#define DESC_BYTE1 0x00000100U

struct buffer {
	uint8_t *data;
	size_t size;
	size_t written;
	int writes;
};

static int read_at(void *ctx, size_t offset, void *buf, size_t len)
{
	struct buffer *buffer = ctx;

	memcpy(buf, buffer->data + offset, len);
	return 0;
}

static int write_at(void *ctx, const void *data, size_t len)
{
	struct buffer *buffer = ctx;

	memcpy(buffer->data + buffer->written, data, len);
	buffer->written += len;
	buffer->writes++;
	return 0;
}

static int flush_ok(void *ctx)
{
	(void)ctx;
	return 0;
}

static struct buffer load_file(const char *path)
{
	struct buffer file = {0};
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

static void fill_header(uint8_t header[HEADER_SIZE])
{
	memset(header, 0, HEADER_SIZE);
	memcpy(header, "@RAK", 4);
	header[6] = HEADER_SIZE;
}

int main(void)
{
	uint8_t payload[] = {0x11, 0x22, 0x33, 0x44};
	uint8_t package[HEADER_SIZE + sizeof(payload)];
	uint8_t slot1[sizeof(payload)];
	struct buffer pkg = {.data = package + HEADER_SIZE, .size = sizeof(package)};
	struct buffer out = {.data = slot1, .size = sizeof(slot1)};
	struct rzi_fuota_install_io io;
	const char *old_path = "D:/Git_code/rzi-workspace/build/fuota-key-check/old.signed.bin";
	const char *new_path = "D:/Git_code/rzi-workspace/build/fuota-key-check/new.signed.bin";
	const char *pkg_path = "D:/Git_code/rzi-workspace/build/fuota-key-check/diff/firmware.bin";
	struct buffer old;
	struct buffer diff_pkg;
	struct buffer expect;
	struct buffer restored;

	fill_header(package);
	memcpy(package + HEADER_SIZE, payload, sizeof(payload));
	memset(&io, 0, sizeof(io));
	io.read_pkg = read_at;
	io.pkg_ctx = &pkg;
	io.pkg_size = pkg.size;
	io.write_slot1 = write_at;
	io.flush_slot1 = flush_ok;
	io.slot1_ctx = &out;
	io.slot1_cap = sizeof(slot1);
	if (rzi_fuota_install(0U, HEADER_SIZE, &io) != 0 || out.written != sizeof(payload) ||
	    memcmp(slot1, payload, sizeof(payload)) != 0) {
		fprintf(stderr, "full package was not copied\n");
		return 1;
	}
	out.written = 0;
	out.writes = 0;
	if (rzi_fuota_install(0U, 0U, &io) != -1013 || out.writes != 0) {
		fprintf(stderr, "zero header length was accepted\n");
		return 1;
	}
	fill_header(package);
	memcpy(package + HEADER_SIZE, payload, sizeof(payload));
	if (rzi_fuota_install(DESC_BYTE1, HEADER_SIZE, &io) != -1000 || out.writes != 0) {
		fprintf(stderr, "byte1-style descriptor was accepted\n");
		return 1;
	}
	if (rzi_fuota_install(DESC_HOST, HEADER_SIZE, &io) != -1000 || out.writes != 0) {
		fprintf(stderr, "RAK1 was installed into slot1\n");
		return 1;
	}
	if (rzi_fuota_classify_descriptor(0U, false, false) != RZI_FUOTA_DESC_FULL ||
	    rzi_fuota_classify_descriptor(DESC_DELTA, true, false) != RZI_FUOTA_DESC_DELTA ||
	    rzi_fuota_classify_descriptor(DESC_DELTA, false, false) != RZI_FUOTA_DESC_REJECT ||
	    rzi_fuota_classify_descriptor(DESC_HOST, false, true) != RZI_FUOTA_DESC_HOST ||
	    rzi_fuota_classify_descriptor(DESC_HOST, false, false) != RZI_FUOTA_DESC_REJECT ||
	    rzi_fuota_classify_descriptor(DESC_BYTE1, true, true) != RZI_FUOTA_DESC_REJECT) {
		fprintf(stderr, "descriptor classification mismatch\n");
		return 1;
	}
	if (rzi_fuota_install(0x00000200U, HEADER_SIZE, &io) != -1000 || out.writes != 0) {
		fprintf(stderr, "unknown descriptor was installed\n");
		return 1;
	}

	old = load_file(old_path);
	diff_pkg = load_file(pkg_path);
	expect = load_file(new_path);
	/* Installation checks the magic only. Rewrite a package built with the previous magic. */
	if (diff_pkg.size < HEADER_SIZE) {
		fprintf(stderr, "diff package is shorter than the header\n");
		return 1;
	}
	memcpy(diff_pkg.data, "@RAK", 4);
	restored.data = malloc(expect.size);
	restored.size = expect.size;
	restored.written = 0U;
	restored.writes = 0;
	if (restored.data == NULL) {
		return 1;
	}
	struct buffer diff_payload = {.data = diff_pkg.data + HEADER_SIZE, .size = diff_pkg.size};

	memset(&io, 0, sizeof(io));
	io.read_pkg = read_at;
	io.pkg_ctx = &diff_payload;
	io.pkg_size = diff_pkg.size;
	io.read_slot0 = read_at;
	io.slot0_ctx = &old;
	io.slot0_size = old.size;
	io.write_slot1 = write_at;
	io.flush_slot1 = flush_ok;
	io.slot1_ctx = &restored;
	io.slot1_cap = restored.size;
	if (rzi_fuota_install(DESC_DELTA, HEADER_SIZE, &io) != 0 || restored.written != expect.size ||
	    memcmp(restored.data, expect.data, expect.size) != 0) {
		fprintf(stderr, "diff restore mismatch written=%zu\n", restored.written);
		return 1;
	}
	printf("installed full %zu bytes and diff %zu bytes\n", sizeof(payload), restored.written);
	free(old.data);
	free(diff_pkg.data);
	free(expect.data);
	free(restored.data);
	return 0;
}
