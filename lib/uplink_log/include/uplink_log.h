/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief LoRa uplink log library: application API.
 *
 * Builds the fixed 11-byte FPort 101 uplink-log frames and owns their
 * confirmation, queueing, de-duplication and retransmission rules. The
 * library is plain C99 and has no platform dependency; the platform side is
 * registered through uplink_log_port.h.
 *
 * State is valid for the current boot only. The library does not use flash;
 * anything that must survive reboot is the application's responsibility.
 *
 * All functions are serialized through the optional port lock. Submit
 * functions never transmit directly: they queue the record and request a
 * call to uplink_log_process() through the port wakeup hook.
 */
#ifndef UPLINK_LOG_H
#define UPLINK_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @name Transport constants
 *  @{
 */
#define UPLINK_LOG_FPORT        101U
#define UPLINK_LOG_FRAME_SIZE   11U
#define UPLINK_LOG_PAYLOAD_SIZE 8U
/** @} */

/** @name Protocol version carried in Byte 0 (2:3:3 bits)
 *  @{
 */
#ifndef UPLINK_LOG_PROTO_VER_MAJOR
#define UPLINK_LOG_PROTO_VER_MAJOR 1U
#endif
#ifndef UPLINK_LOG_PROTO_VER_MINOR
#define UPLINK_LOG_PROTO_VER_MINOR 0U
#endif
#ifndef UPLINK_LOG_PROTO_VER_PATCH
#define UPLINK_LOG_PROTO_VER_PATCH 0U
#endif
/** @} */

/** @name Behaviour limits from the uplink log specification
 *  @{
 */
#define UPLINK_LOG_QUEUE_DEPTH              20U
#define UPLINK_LOG_COOLDOWN_SEC             (30U * 60U)
#define UPLINK_LOG_BOOT_RETRY_INTERVAL_SEC  30U
#define UPLINK_LOG_BOOT_MAX_RETRIES         10U
#define UPLINK_LOG_GIVE_UP_SEC              (24U * 60U * 60U)
#define UPLINK_LOG_PERIODIC_MIN_MINUTES     30U
#define UPLINK_LOG_PERIODIC_MAX_MINUTES     (24U * 60U)
#define UPLINK_LOG_PERIODIC_STEP_MINUTES    30U
#define UPLINK_LOG_PERIODIC_DEFAULT_MINUTES 30U
/** A frame without a transmit result after this long is treated as failed. */
#define UPLINK_LOG_TX_TIMEOUT_SEC           120U
/** @} */

/** Returned by uplink_log_process() when nothing is scheduled. */
#define UPLINK_LOG_NO_DEADLINE UINT32_MAX

/** Library return codes. */
enum uplink_log_err {
	UPLINK_LOG_OK = 0,
	/** An argument or enum value is invalid. */
	UPLINK_LOG_ERR_INVALID = -1,
	/** uplink_log_init() has not succeeded. */
	UPLINK_LOG_ERR_STATE = -2,
	/** The record matched one still in its 30 min cooldown and was dropped. */
	UPLINK_LOG_ERR_SUPPRESSED = -3,
};

/** Byte 1: device or module code. */
enum uplink_log_device_code {
	UPLINK_LOG_MODULE_UNKNOWN = 0x00,
	UPLINK_LOG_MODULE_RAK4630 = 0x01,
	UPLINK_LOG_MODULE_RAK3172 = 0x02,
	UPLINK_LOG_MODULE_RAK3172_F = 0x03,
	UPLINK_LOG_MODULE_RAK9105U = 0x04,
	UPLINK_LOG_MODULE_RAK2461 = 0x05,
};

/** Byte 2: log type. */
enum uplink_log_type {
	UPLINK_LOG_TYPE_BOOT = 0x01,
	UPLINK_LOG_TYPE_PERIODIC = 0x02,
	UPLINK_LOG_TYPE_EVENT = 0x03,
	UPLINK_LOG_TYPE_CONFIG = 0x04,
	UPLINK_LOG_TYPE_CUSTOM = 0xFF,
};

/** Boot report reset reason (4 bits). */
enum uplink_log_reset_reason {
	UPLINK_LOG_RESET_UNKNOWN = 0x0,
	UPLINK_LOG_RESET_POWER_ON = 0x1,
	UPLINK_LOG_RESET_WATCHDOG = 0x2,
	UPLINK_LOG_RESET_FUOTA = 0x3,
};

/** Periodic report power source (2 bits). */
enum uplink_log_power_source {
	UPLINK_LOG_POWER_SOURCE_UNKNOWN = 0x0,
	UPLINK_LOG_POWER_SOURCE_BATTERY = 0x1,
	UPLINK_LOG_POWER_SOURCE_EXTERNAL = 0x2,
	UPLINK_LOG_POWER_SOURCE_SOLAR = 0x3,
};

/** Periodic report charging status (3 bits). */
enum uplink_log_charging_status {
	UPLINK_LOG_CHARGING_UNKNOWN = 0x0,
	UPLINK_LOG_CHARGING_NOT_CHARGING = 0x1,
	UPLINK_LOG_CHARGING_CHARGING = 0x2,
	UPLINK_LOG_CHARGING_FULL = 0x3,
	UPLINK_LOG_CHARGING_FAULT = 0x4,
};

/** @name Periodic report health snapshot bits (1 = healthy)
 *  @{
 */
#define UPLINK_LOG_HEALTH_SENSOR (1U << 0)
#define UPLINK_LOG_HEALTH_POWER  (1U << 1)
#define UPLINK_LOG_HEALTH_SYSTEM (1U << 2)
/** @} */

/** Event domain (4 bits). */
enum uplink_log_event_domain {
	UPLINK_LOG_EVENT_DOMAIN_POWER = 0x1,
	UPLINK_LOG_EVENT_DOMAIN_SENSOR = 0x2,
	UPLINK_LOG_EVENT_DOMAIN_COMMUNICATION = 0x3,
	UPLINK_LOG_EVENT_DOMAIN_SYSTEM = 0x4,
	UPLINK_LOG_EVENT_DOMAIN_FUOTA = 0x5,
};

/** Power domain event codes. */
enum uplink_log_power_event {
	UPLINK_LOG_EVENT_POWER_LOW_VOLTAGE = 0x01,
	UPLINK_LOG_EVENT_POWER_TEMP_HIGH_SHUTDOWN = 0x02,
};

/** Sensor domain event codes. */
enum uplink_log_sensor_event {
	UPLINK_LOG_EVENT_SENSOR_NOT_DETECTED = 0x01,
	UPLINK_LOG_EVENT_SENSOR_VALUE_OUT_OF_RANGE = 0x02,
	UPLINK_LOG_EVENT_SENSOR_INIT_FAILED = 0x03,
	UPLINK_LOG_EVENT_SENSOR_CRC_ERROR = 0x04,
	UPLINK_LOG_EVENT_SENSOR_DISCONNECTED = 0x05,
};

/** Communication domain event codes. */
enum uplink_log_comm_event {
	UPLINK_LOG_EVENT_COMM_DOWNLINK_DECODE_FAIL = 0x01,
};

/** System domain event codes. */
enum uplink_log_system_event {
	UPLINK_LOG_EVENT_SYS_NVM_WRITE_FAIL = 0x01,
	UPLINK_LOG_EVENT_SYS_NVM_CORRUPT = 0x02,
	UPLINK_LOG_EVENT_SYS_TIME_SYNC_FAIL = 0x03,
	UPLINK_LOG_EVENT_SYS_TIME_SYNC_SUCCESS = 0x04,
};

/**
 * FUOTA domain event codes. Codes from UPLINK_LOG_EVENT_FUOTA_SUCCESS upwards
 * are result events and are always treated as urgent for this boot.
 */
enum uplink_log_fuota_event {
	UPLINK_LOG_EVENT_FUOTA_STARTED = 0x01,
	UPLINK_LOG_EVENT_FUOTA_SUCCESS = 0x02,
	UPLINK_LOG_EVENT_FUOTA_FAIL = 0x03,
	UPLINK_LOG_EVENT_FUOTA_ROLLBACK = 0x04,
	UPLINK_LOG_EVENT_FUOTA_DELTA_APPLY_FAIL = 0x05,
	UPLINK_LOG_EVENT_FUOTA_SIGN_FAIL = 0x06,
	UPLINK_LOG_EVENT_FUOTA_FLASH_FAIL = 0x07,
	UPLINK_LOG_EVENT_FUOTA_FRAG_INCOMPLETE = 0x08,
	UPLINK_LOG_EVENT_FUOTA_HDR_INVALID = 0x11,
	UPLINK_LOG_EVENT_FUOTA_HDR_DEVICE = 0x12,
	UPLINK_LOG_EVENT_FUOTA_HDR_VERSION = 0x13,
	UPLINK_LOG_EVENT_FUOTA_HDR_BASE = 0x14,
	UPLINK_LOG_EVENT_FUOTA_HDR_LENGTH = 0x15,
	UPLINK_LOG_EVENT_FUOTA_HDR_CRC = 0x16,
};

/** Config report status (4 bits). */
enum uplink_log_config_status {
	UPLINK_LOG_CONFIG_STATUS_SUCCESS = 0x0,
	UPLINK_LOG_CONFIG_STATUS_FAIL = 0x1,
};

/** Config report item type. */
enum uplink_log_config_type {
	UPLINK_LOG_CONFIG_TYPE_UNDEFINED = 0x00,
	UPLINK_LOG_CONFIG_TYPE_UPLINK_INTERVAL = 0x01,
	UPLINK_LOG_CONFIG_TYPE_SAMPLE_STRATEGY = 0x02,
	UPLINK_LOG_CONFIG_TYPE_LORA_ADR = 0x20,
	UPLINK_LOG_CONFIG_TYPE_LORA_DR = 0x21,
	UPLINK_LOG_CONFIG_TYPE_SENSOR_ENABLE = 0x40,
	UPLINK_LOG_CONFIG_TYPE_SENSOR_BAUDRATE = 0x41,
	UPLINK_LOG_CONFIG_TYPE_SENSOR_READ_INTERVAL = 0x42,
	UPLINK_LOG_CONFIG_TYPE_APP_THRESHOLD_BIZ = 0x60,
	UPLINK_LOG_CONFIG_TYPE_RESERVED = 0xFF,
};

/** Firmware version in the byte order used by FUOTA headers. */
struct uplink_log_version {
	uint8_t major;
	uint8_t minor;
	uint8_t patch;
	uint8_t build;
};

/** Content of a type 0x01 boot report. */
struct uplink_log_boot {
	struct uplink_log_version fw_version;
	/** Filled by the application for this boot report. */
	enum uplink_log_reset_reason reset_reason;
	bool cap_fuota;
	bool cap_delta;
	bool cap_full;
	bool cap_rollback;
	bool cap_host_mcu;
	uint8_t join_fail_count;
};

/** Content of a type 0x02 periodic report. The counter is owned by the library. */
struct uplink_log_periodic {
	enum uplink_log_power_source power_source;
	enum uplink_log_charging_status charging_status;
	/** Supply voltage in mV, or 0 on external power. */
	uint16_t voltage_mv;
	/** RSSI of the last downlink in dBm, clamped to -180..75. */
	int16_t rssi_dbm;
	/** SNR of the last downlink in dB. */
	int8_t snr_db;
	uint8_t tx_fail_count;
	/** Bitwise OR of UPLINK_LOG_HEALTH_*. */
	uint8_t health;
};

/** Content of a type 0x03 event report. */
struct uplink_log_event {
	/** One of enum uplink_log_event_domain. */
	uint8_t domain;
	/** Domain-specific event code. */
	uint8_t code;
	/** Event Data (Byte 7-10); zero when unused. */
	uint8_t data[4];
	/** Urgent records are sent before normal ones. Forced for FUOTA results. */
	bool urgent;
};

/** Content of a type 0x04 config report. */
struct uplink_log_config_report {
	enum uplink_log_config_status status;
	/** One of enum uplink_log_config_type. */
	uint8_t config_type;
	/** Sensor id, channel or profile id; zero when unused. */
	uint8_t object_id;
	/** Main parameter; single-byte enums use the low byte. */
	uint16_t value;
	bool urgent;
};

/**
 * Application callback filling a periodic report when it falls due.
 *
 * @return true to send the snapshot, false to skip this period.
 */
typedef bool (*uplink_log_periodic_fn)(void *user_data, struct uplink_log_periodic *out);

/** Application options passed to uplink_log_init(). */
struct uplink_log_options {
	/** Byte 1 of every frame, one of enum uplink_log_device_code. */
	uint8_t device_code;
	/** Periodic interval in minutes; zero selects the 30 min default. */
	uint16_t periodic_interval_min;
	/** Optional. When NULL, periodic reports come only from uplink_log_submit_periodic(). */
	uplink_log_periodic_fn periodic_snapshot;
	/** Passed to periodic_snapshot. */
	void *user_data;
};

/**
 * @brief Queue a boot report, replacing any boot report still in progress.
 *
 * Sent as a confirmed uplink once the link is up, every 30 s for at most
 * 10 retries. Event and config records wait until the boot report finishes.
 */
int uplink_log_submit_boot(const struct uplink_log_boot *boot);

/** @brief Send an unconfirmed periodic report now, replacing an unsent one. */
int uplink_log_submit_periodic(const struct uplink_log_periodic *periodic);

/**
 * @brief Queue an event report.
 *
 * @retval UPLINK_LOG_OK Record queued.
 * @retval UPLINK_LOG_ERR_SUPPRESSED The Domain/Code pair is in its cooldown.
 */
int uplink_log_submit_event(const struct uplink_log_event *event);

/**
 * @brief Queue a config report.
 *
 * @retval UPLINK_LOG_OK Record queued.
 * @retval UPLINK_LOG_ERR_SUPPRESSED The same type and payload is in its cooldown.
 */
int uplink_log_submit_config(const struct uplink_log_config_report *config);

/** @brief Send an unconfirmed device-specific report now, replacing an unsent one. */
int uplink_log_submit_custom(const uint8_t payload[UPLINK_LOG_PAYLOAD_SIZE]);

/**
 * @brief Change the periodic interval.
 *
 * @param minutes 30 through 1440, a multiple of 30. The next report falls due
 *        one full interval after this call.
 */
int uplink_log_set_periodic_interval(uint16_t minutes);

/** @brief Stop sending event and config records. Submissions still queue. */
int uplink_log_pause_drain(void);

/** @brief Resume sending event and config records. */
int uplink_log_resume_drain(void);

/** @brief Number of queued event and config records. */
size_t uplink_log_queue_count(void);

#ifdef __cplusplus
}
#endif

#endif /* UPLINK_LOG_H */
