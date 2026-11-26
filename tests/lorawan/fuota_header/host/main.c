/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Host checks for the FUOTA business header rules.
 *
 * Build together with ../../../../src/lorawan/services/fuota/fuota_header.c.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fuota_header.h"

#define PAYLOAD 64U

static uint8_t package[FUOTA_HDR_LEN_V1 + PAYLOAD];
static size_t package_len;

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void build(uint8_t kind, uint8_t device, const uint8_t target[4], const uint8_t base[4],
		  uint8_t algo, uint16_t flags, size_t payload, uint32_t crc_delta)
{
	uint32_t crc;

	memset(package, 0, sizeof(package));
	put32(&package[0], FUOTA_HDR_MAGIC_V1);
	put16(&package[4], FUOTA_HDR_VERSION_V1);
	put16(&package[6], FUOTA_HDR_LEN_V1);
	put32(&package[8], device);
	memcpy(&package[12], target, 4);
	memcpy(&package[16], base, 4);
	package[20] = kind;
	package[21] = algo;
	put16(&package[22], flags);
	put32(&package[24], (uint32_t)payload);
	for (size_t i = 0; i < payload; i++) {
		package[FUOTA_HDR_LEN_V1 + i] = (uint8_t)(0xA0U + i);
	}
	crc = fuota_crc32_update(0xFFFFFFFFU, &package[FUOTA_HDR_LEN_V1], payload);
	put32(&package[28], (crc ^ 0xFFFFFFFFU) + crc_delta);
	package_len = FUOTA_HDR_LEN_V1 + payload;
}

static int read_package(void *ctx, size_t offset, void *buf, size_t len)
{
	(void)ctx;
	if (offset > package_len || len > package_len - offset) {
		return -1;
	}
	memcpy(buf, &package[offset], len);
	return 0;
}

static enum fuota_hdr_result check(uint8_t device, const uint8_t current[4], uint8_t kind,
				   size_t payload)
{
	struct fuota_hdr_expect expect;

	memset(&expect, 0, sizeof(expect));
	expect.device_code = device;
	expect.have_version = true;
	memcpy(expect.current_version, current, 4);
	expect.package_kind = kind;
	expect.payload_length = payload;
	return fuota_hdr_verify(package, &expect, read_package, NULL);
}

int main(void)
{
	static const uint8_t current[4] = {1U, 0U, 0U, 1U};
	static const uint8_t target[4] = {1U, 1U, 0U, 2U};
	static const uint8_t base_ok[4] = {1U, 0U, 0U, 1U};
	static const uint8_t base_bad[4] = {9U, 0U, 0U, 0U};
	static const uint8_t zero[4] = {0U, 0U, 0U, 0U};
	int failures = 0;

	/* CRC-32/ISO-HDLC check value for "123456789". */
	if ((fuota_crc32_update(0xFFFFFFFFU, "123456789", 9U) ^ 0xFFFFFFFFU) != 0xCBF43926U) {
		fprintf(stderr, "CRC check value mismatch\n");
		failures++;
	}

	build(0U, 3U, target, zero, 0U, 0U, PAYLOAD, 0U);
	if (check(3U, current, 0U, PAYLOAD) != FUOTA_HDR_OK) {
		fprintf(stderr, "full package rejected\n");
		failures++;
	}
	build(1U, 3U, target, base_ok, 1U, 0U, PAYLOAD, 0U);
	if (check(3U, current, 1U, PAYLOAD) != FUOTA_HDR_OK) {
		fprintf(stderr, "diff package rejected\n");
		failures++;
	}

	build(0U, 3U, target, zero, 0U, 0U, PAYLOAD, 0U);
	package[0] = 0U;
	if (check(3U, current, 0U, PAYLOAD) != FUOTA_HDR_MAGIC) {
		fprintf(stderr, "magic not rejected\n");
		failures++;
	}
	build(0U, 3U, target, zero, 0U, 0U, PAYLOAD, 0U);
	put16(&package[4], 2U);
	if (check(3U, current, 0U, PAYLOAD) != FUOTA_HDR_VERSION) {
		fprintf(stderr, "version not rejected\n");
		failures++;
	}
	build(0U, 3U, target, zero, 0U, 0U, PAYLOAD, 0U);
	put16(&package[6], 33U);
	if (check(3U, current, 0U, PAYLOAD) != FUOTA_HDR_LENGTH) {
		fprintf(stderr, "length not rejected\n");
		failures++;
	}
	build(0U, 2U, target, zero, 0U, 0U, PAYLOAD, 0U);
	if (check(3U, current, 0U, PAYLOAD) != FUOTA_HDR_PRODUCT) {
		fprintf(stderr, "device code not rejected\n");
		failures++;
	}
	build(0U, 3U, current, zero, 0U, 0U, PAYLOAD, 0U);
	if (check(3U, current, 0U, PAYLOAD) != FUOTA_HDR_OK) {
		fprintf(stderr, "same version rejected\n");
		failures++;
	}
	build(0U, 3U, target, zero, 0U, 0U, PAYLOAD, 0U);
	package[12] ^= 0x5AU;
	if (check(3U, current, 0U, PAYLOAD) != FUOTA_HDR_OK) {
		fprintf(stderr, "header byte changed the payload crc\n");
		failures++;
	}
	build(1U, 3U, target, base_bad, 1U, 0U, PAYLOAD, 0U);
	if (check(3U, current, 1U, PAYLOAD) != FUOTA_HDR_BASE) {
		fprintf(stderr, "diff base not rejected\n");
		failures++;
	}
	build(0U, 3U, target, base_bad, 0U, 0U, PAYLOAD, 0U);
	if (check(3U, current, 0U, PAYLOAD) != FUOTA_HDR_BASE) {
		fprintf(stderr, "full base not rejected\n");
		failures++;
	}
	build(0U, 3U, target, zero, 0U, 0U, PAYLOAD, 0U);
	if (check(3U, current, 1U, PAYLOAD) != FUOTA_HDR_KIND) {
		fprintf(stderr, "kind mismatch not rejected\n");
		failures++;
	}
	build(1U, 3U, target, base_ok, 0U, 0U, PAYLOAD, 0U);
	if (check(3U, current, 1U, PAYLOAD) != FUOTA_HDR_ALGO) {
		fprintf(stderr, "algorithm not rejected\n");
		failures++;
	}
	build(0U, 3U, target, zero, 0U, 1U, PAYLOAD, 0U);
	if (check(3U, current, 0U, PAYLOAD) != FUOTA_HDR_FLAGS) {
		fprintf(stderr, "flags not rejected\n");
		failures++;
	}
	build(0U, 3U, target, zero, 0U, 0U, PAYLOAD, 0U);
	if (check(3U, current, 0U, PAYLOAD + 1U) != FUOTA_HDR_SIZE) {
		fprintf(stderr, "size mismatch not rejected\n");
		failures++;
	}
	build(0U, 3U, target, zero, 0U, 0U, PAYLOAD, 1U);
	if (check(3U, current, 0U, PAYLOAD) != FUOTA_HDR_CRC) {
		fprintf(stderr, "crc not rejected\n");
		failures++;
	}

	if (failures == 0) {
		printf("all fuota_header checks passed\n");
		return 0;
	}
	fprintf(stderr, "%d fuota_header checks failed\n", failures);
	return 1;
}
