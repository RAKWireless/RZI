/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief FUOTA business header v1 parsing and validation.
 */

#include "fuota_header.h"

#include <string.h>

#define FUOTA_CRC_CHUNK 256U
#define FUOTA_CRC_INIT 0xFFFFFFFFU
#define FUOTA_CRC_POLY 0xEDB88320U

static uint16_t load_le16(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t load_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
	       ((uint32_t)bytes[3] << 24);
}

static bool is_zero4(const uint8_t value[4])
{
	return value[0] == 0U && value[1] == 0U && value[2] == 0U && value[3] == 0U;
}

void fuota_hdr_parse(const uint8_t raw[FUOTA_HDR_LEN_V1], struct fuota_hdr *out)
{
	memset(out, 0, sizeof(*out));
	out->header_version = load_le16(&raw[4]);
	out->header_length = load_le16(&raw[6]);
	out->device_code = raw[8];
	memcpy(out->target_version, &raw[12], sizeof(out->target_version));
	memcpy(out->base_version, &raw[16], sizeof(out->base_version));
	out->package_kind = raw[20];
	out->diff_algo = raw[21];
	out->flags = load_le16(&raw[22]);
	out->image_length = load_le32(&raw[24]);
	out->crc32 = load_le32(&raw[28]);
}

enum fuota_hdr_result fuota_hdr_check(const struct fuota_hdr *hdr,
				      const struct fuota_hdr_expect *expect)
{
	if (hdr->header_version != FUOTA_HDR_VERSION_V1) {
		return FUOTA_HDR_VERSION;
	}
	if (hdr->header_length != FUOTA_HDR_LEN_V1) {
		return FUOTA_HDR_LENGTH;
	}
	if (hdr->package_kind > 1U) {
		return FUOTA_HDR_KIND;
	}
	if (hdr->package_kind != expect->package_kind) {
		return FUOTA_HDR_KIND;
	}
	if (hdr->package_kind == 0U && hdr->diff_algo != 0U) {
		return FUOTA_HDR_ALGO;
	}
	if (hdr->package_kind == 1U && hdr->diff_algo != 1U) {
		return FUOTA_HDR_ALGO;
	}
	if (hdr->flags != 0U) {
		return FUOTA_HDR_FLAGS;
	}
	if (hdr->image_length != expect->payload_length) {
		return FUOTA_HDR_SIZE;
	}
	if (expect->device_code != 0U && hdr->device_code != expect->device_code) {
		return FUOTA_HDR_PRODUCT;
	}
	if (expect->have_version) {
		if (hdr->package_kind == 1U) {
			if (memcmp(hdr->base_version, expect->current_version, 4) != 0) {
				return FUOTA_HDR_BASE;
			}
		} else if (!is_zero4(hdr->base_version)) {
			return FUOTA_HDR_BASE;
		}
	}
	return FUOTA_HDR_OK;
}

uint32_t fuota_crc32_update(uint32_t crc, const void *data, size_t len)
{
	const uint8_t *bytes = data;

	for (size_t i = 0U; i < len; i++) {
		crc ^= bytes[i];
		for (uint8_t bit = 0U; bit < 8U; bit++) {
			crc = (crc >> 1) ^ (FUOTA_CRC_POLY & (0U - (crc & 1U)));
		}
	}
	return crc;
}

enum fuota_hdr_result fuota_hdr_verify(const uint8_t raw[FUOTA_HDR_LEN_V1],
				       const struct fuota_hdr_expect *expect,
				       fuota_hdr_read read, void *ctx)
{
	struct fuota_hdr hdr;
	uint8_t chunk[FUOTA_CRC_CHUNK];
	uint32_t crc;
	size_t done = 0U;

	if (load_le32(raw) != FUOTA_HDR_MAGIC_V1) {
		return FUOTA_HDR_MAGIC;
	}
	/* product_id bytes 1..3 are reserved and must be zero. */
	if ((raw[9] | raw[10] | raw[11]) != 0U) {
		return FUOTA_HDR_PRODUCT;
	}
	fuota_hdr_parse(raw, &hdr);
	enum fuota_hdr_result result = fuota_hdr_check(&hdr, expect);

	if (result != FUOTA_HDR_OK) {
		return result;
	}
	if (read == NULL) {
		return FUOTA_HDR_IO;
	}
	crc = FUOTA_CRC_INIT;
	while (done < hdr.image_length) {
		size_t request = hdr.image_length - done;

		if (request > sizeof(chunk)) {
			request = sizeof(chunk);
		}
		if (read(ctx, hdr.header_length + done, chunk, request) != 0) {
			return FUOTA_HDR_IO;
		}
		crc = fuota_crc32_update(crc, chunk, request);
		done += request;
	}
	return (crc ^ FUOTA_CRC_INIT) == hdr.crc32 ? FUOTA_HDR_OK : FUOTA_HDR_CRC;
}
