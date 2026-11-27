/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>

#include "uplink_log_frame.h"

#define RSSI_OFFSET_DBM 180

uint8_t uplink_log_frame_version_byte(void)
{
	return (uint8_t)(((UPLINK_LOG_PROTO_VER_MAJOR & 0x3U) << 6) |
			 ((UPLINK_LOG_PROTO_VER_MINOR & 0x7U) << 3) |
			 (UPLINK_LOG_PROTO_VER_PATCH & 0x7U));
}

static uint8_t *frame_header(uint8_t frame[UPLINK_LOG_FRAME_SIZE], uint8_t device_code,
			     uint8_t type)
{
	memset(frame, 0, UPLINK_LOG_FRAME_SIZE);
	frame[0] = uplink_log_frame_version_byte();
	frame[1] = device_code;
	frame[2] = type;
	return &frame[3];
}

void uplink_log_frame_boot(uint8_t frame[UPLINK_LOG_FRAME_SIZE], uint8_t device_code,
			   const struct uplink_log_boot *boot)
{
	uint8_t *p = frame_header(frame, device_code, UPLINK_LOG_TYPE_BOOT);

	p[0] = boot->fw_version.major;
	p[1] = boot->fw_version.minor;
	p[2] = boot->fw_version.patch;
	p[3] = boot->fw_version.build;
	p[4] = (uint8_t)(((uint8_t)boot->reset_reason & 0xFU) << 4);
	p[4] |= boot->cap_fuota ? (1U << 3) : 0U;
	p[4] |= boot->cap_delta ? (1U << 2) : 0U;
	p[4] |= boot->cap_full ? (1U << 1) : 0U;
	p[4] |= boot->cap_rollback ? (1U << 0) : 0U;
	p[5] = boot->cap_host_mcu ? 1U : 0U;
	p[6] = 0U;
	p[7] = boot->join_fail_count;
}

uint8_t uplink_log_frame_rssi(int16_t rssi_dbm)
{
	int32_t raw = (int32_t)rssi_dbm + RSSI_OFFSET_DBM;

	if (raw < 0) {
		return 0U;
	}
	if (raw > 0xFF) {
		return 0xFFU;
	}
	return (uint8_t)raw;
}

void uplink_log_frame_periodic(uint8_t frame[UPLINK_LOG_FRAME_SIZE], uint8_t device_code,
			       uint8_t count, const struct uplink_log_periodic *periodic)
{
	uint8_t *p = frame_header(frame, device_code, UPLINK_LOG_TYPE_PERIODIC);

	p[0] = count;
	p[1] = (uint8_t)((((uint8_t)periodic->power_source & 0x3U) << 6) |
			 (((uint8_t)periodic->charging_status & 0x7U) << 3));
	p[2] = (uint8_t)(periodic->voltage_mv & 0xFFU);
	p[3] = (uint8_t)(periodic->voltage_mv >> 8);
	p[4] = uplink_log_frame_rssi(periodic->rssi_dbm);
	p[5] = (uint8_t)periodic->snr_db;
	p[6] = periodic->tx_fail_count;
	p[7] = (uint8_t)(periodic->health & (UPLINK_LOG_HEALTH_SENSOR | UPLINK_LOG_HEALTH_POWER |
					     UPLINK_LOG_HEALTH_SYSTEM));
}

void uplink_log_frame_record(uint8_t frame[UPLINK_LOG_FRAME_SIZE], uint8_t device_code,
			     uint8_t type, uint32_t elapsed_sec, uint8_t retry, uint8_t nibble,
			     uint8_t code, const uint8_t data[4])
{
	uint8_t *p = frame_header(frame, device_code, type);
	uint16_t elapsed = elapsed_sec > UPLINK_LOG_ELAPSED_MAX ? (uint16_t)UPLINK_LOG_ELAPSED_MAX
								: (uint16_t)elapsed_sec;

	if (retry > UPLINK_LOG_RETRY_MAX) {
		retry = UPLINK_LOG_RETRY_MAX;
	}
	p[0] = (uint8_t)(elapsed & 0xFFU);
	p[1] = (uint8_t)(elapsed >> 8);
	p[2] = (uint8_t)((retry << 4) | (nibble & 0xFU));
	p[3] = code;
	memcpy(&p[4], data, 4);
}

void uplink_log_frame_custom(uint8_t frame[UPLINK_LOG_FRAME_SIZE], uint8_t device_code,
			     const uint8_t payload[UPLINK_LOG_PAYLOAD_SIZE])
{
	uint8_t *p = frame_header(frame, device_code, UPLINK_LOG_TYPE_CUSTOM);

	memcpy(p, payload, UPLINK_LOG_PAYLOAD_SIZE);
}
