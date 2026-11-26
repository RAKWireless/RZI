/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief FUOTA business header v1 parsing and validation.
 *
 * The layout follows the RZI FUOTA technical design, section 2.3. Field
 * offsets live in this file only; a future header version adds one parse
 * function here and leaves every caller unchanged.
 */
#ifndef RZI_FUOTA_HEADER_H
#define RZI_FUOTA_HEADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Wire bytes 40 52 41 4B, ASCII "@RAK", little-endian. */
#define FUOTA_HDR_MAGIC_V1 0x4B415240U
#define FUOTA_HDR_VERSION_V1 1U
#define FUOTA_HDR_LEN_V1 32U

/** Result of a header check. Kept stable for reporting. */
enum fuota_hdr_result {
	FUOTA_HDR_OK = 0,
	FUOTA_HDR_MAGIC,
	FUOTA_HDR_VERSION,
	FUOTA_HDR_LENGTH,
	FUOTA_HDR_PRODUCT,
	/** Retired. A target version equal to the running version is accepted. */
	FUOTA_HDR_TARGET,
	FUOTA_HDR_BASE,
	FUOTA_HDR_KIND,
	FUOTA_HDR_ALGO,
	FUOTA_HDR_FLAGS,
	FUOTA_HDR_SIZE,
	FUOTA_HDR_CRC,
	FUOTA_HDR_IO,
};

/** Decoded header fields. Versions keep their four wire bytes. */
struct fuota_hdr {
	uint16_t header_version;
	uint16_t header_length;
	uint8_t device_code;
	uint8_t target_version[4];
	uint8_t base_version[4];
	uint8_t package_kind;
	uint8_t diff_algo;
	uint16_t flags;
	uint32_t image_length;
	uint32_t crc32;
};

/** Values the device compares against. */
struct fuota_hdr_expect {
	/** Local device code. Zero skips the model check. */
	uint8_t device_code;
	/** True when current_version is known. */
	bool have_version;
	/** Version bytes of the running image: major, minor, revision, build. */
	uint8_t current_version[4];
	/** Package kind selected by the session descriptor. */
	uint8_t package_kind;
	/** Bytes after the header, as received. */
	size_t payload_length;
};

/** Read payload bytes at an absolute offset from the package start. */
typedef int (*fuota_hdr_read)(void *ctx, size_t offset, void *buf, size_t len);

/** Decode the fixed fields. Does not validate. */
void fuota_hdr_parse(const uint8_t raw[FUOTA_HDR_LEN_V1], struct fuota_hdr *out);

/** Validate the decoded fields. Does not check the CRC. */
enum fuota_hdr_result fuota_hdr_check(const struct fuota_hdr *hdr,
				      const struct fuota_hdr_expect *expect);

/** CRC-32/IEEE 802.3 step. Start with 0xFFFFFFFF, finish with a final xor. */
uint32_t fuota_crc32_update(uint32_t crc, const void *data, size_t len);

/**
 * @brief Validate a header and its CRC.
 *
 * The CRC covers only the ``image_length`` payload bytes after the header.
 * Those bytes are read through @p read. The 32-byte header is not included.
 */
enum fuota_hdr_result fuota_hdr_verify(const uint8_t raw[FUOTA_HDR_LEN_V1],
				       const struct fuota_hdr_expect *expect,
				       fuota_hdr_read read, void *ctx);

#endif /* RZI_FUOTA_HEADER_H */
