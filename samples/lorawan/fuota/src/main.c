/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <zephyr/app_version.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#if defined(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif
#include <rzi/at/at.h>
#include <rzi/at/uart.h>
#include <rzi/lorawan/fuota.h>
#include <rzi/lorawan/lorawan.h>
#include <rzi/version.h>

#ifdef CONFIG_IMG_MANAGER
#include <zephyr/dfu/mcuboot.h>
#endif

#include <uplink_log.h>

#include "uplink_log_rzi.h"

static __maybe_unused void ignore_result(int result)
{
	ARG_UNUSED(result);
}

LOG_MODULE_REGISTER(rzi_fuota_sample, LOG_LEVEL_INF);

#define AT_UART_NODE       DT_ALIAS(rzi_at_uart)
#define RETRY_DELAY_S      5
#define RETRY_DELAY        K_SECONDS(RETRY_DELAY_S)
#define UPLINK_INTERVAL_MS 5000

#if !DT_NODE_EXISTS(AT_UART_NODE)
#error "The board overlay must define the rzi-at-uart alias"
#endif

#if defined(CONFIG_BOARD_RAK3372_F)
#define DEVICE_CODE UPLINK_LOG_MODULE_RAK3172_F
#elif defined(CONFIG_BOARD_RAK4631)
#define DEVICE_CODE UPLINK_LOG_MODULE_RAK4630
#else
#define DEVICE_CODE UPLINK_LOG_MODULE_UNKNOWN
#endif

struct fuota_log_item {
	uint8_t code;
	bool pause_drain;
	bool resume_drain;
	bool has_event;
};

static const struct device *const at_uart = DEVICE_DT_GET(AT_UART_NODE);
static K_SEM_DEFINE(stack_ready, 0, 1);
static K_SEM_DEFINE(join_done, 0, 1);
K_MSGQ_DEFINE(fuota_log_q, sizeof(struct fuota_log_item), 4, 4);
static volatile bool joined;
static enum rzi_fuota_state fuota_state;
static uint32_t fuota_result_generation;

static const char *tx_status_str(enum rzi_lorawan_tx_status status)
{
	switch (status) {
	case RZI_LORAWAN_EVENT_TXDONE_SENT:
		return "TXDONE_SENT";
	case RZI_LORAWAN_EVENT_TXDONE_CONFIRMED:
		return "TXDONE_CONFIRMED";
	case RZI_LORAWAN_EVENT_TXDONE_NOT_SENT:
		return "TXDONE_NOT_SENT";
	default:
		return "TXDONE_UNKNOWN";
	}
}

static void log_uplink_done(const struct rzi_lorawan_tx_result *tx)
{
	if (tx->error != 0) {
		LOG_WRN("%s: %s", tx_status_str(tx->status), rzi_err_str(tx->error));
		return;
	}
	LOG_INF("%s", tx_status_str(tx->status));
}

static const char *fuota_state_str(enum rzi_fuota_state state)
{
	switch (state) {
	case RZI_FUOTA_STATE_IDLE:
		return "idle";
	case RZI_FUOTA_STATE_READY:
		return "ready";
	case RZI_FUOTA_STATE_TRANSFERRING:
		return "transferring";
	case RZI_FUOTA_STATE_COMPLETE:
		return "complete";
	case RZI_FUOTA_STATE_FAILED:
		return "failed";
	case RZI_FUOTA_STATE_APPLYING:
		return "applying";
	default:
		return "unknown";
	}
}

static void on_event(const struct rzi_lorawan_event *event, void *user_data)
{
	ARG_UNUSED(user_data);
	uplink_log_rzi_on_lorawan_event(event);
	switch (event->type) {
	case RZI_LORAWAN_EVENT_READY:
		k_sem_give(&stack_ready);
		break;
	case RZI_LORAWAN_EVENT_STATE_CHANGED:
		if (event->state == RZI_LORAWAN_STATE_READY) {
			k_sem_give(&stack_ready);
		}
		break;
	case RZI_LORAWAN_EVENT_JOINED:
		joined = true;
		k_sem_give(&join_done);
		break;
	case RZI_LORAWAN_EVENT_JOIN_FAILED:
		joined = false;
		k_sem_give(&join_done);
		break;
	case RZI_LORAWAN_EVENT_TXDONE:
		log_uplink_done(&event->tx);
		break;
	case RZI_LORAWAN_EVENT_DOWNDATA:
		LOG_INF("Downlink: port %u, %u bytes, RSSI %d dBm, SNR %d/4 dB",
			event->downlink.port, (unsigned int)event->downlink.size,
			event->downlink.rssi_dbm, event->downlink.snr_quarter_db);
		break;
	case RZI_LORAWAN_EVENT_ERROR:
		LOG_ERR("Error: %s", rzi_err_str(event->error));
		break;
	default:
		break;
	}
}

static void queue_fuota_log(const struct fuota_log_item *item)
{
	if (k_msgq_put(&fuota_log_q, item, K_NO_WAIT) != 0) {
		LOG_WRN("FUOTA log queue full");
	}
	uplink_log_rzi_wakeup();
}

static void queue_fuota_result(const struct rzi_fuota_round *round, bool have_round)
{
	struct fuota_log_item item = {
		.resume_drain = true,
		.has_event = true,
	};

	if (have_round && round->generation != 0U && round->generation == fuota_result_generation) {
		item.has_event = false;
		queue_fuota_log(&item);
		return;
	}
	if (have_round && round->generation != 0U) {
		fuota_result_generation = round->generation;
	}
	item.code = have_round && round->write_failed ? UPLINK_LOG_EVENT_FUOTA_FLASH_FAIL
						       : UPLINK_LOG_EVENT_FUOTA_FRAG_INCOMPLETE;
	queue_fuota_log(&item);
}

static void on_fuota_state(enum rzi_fuota_state state, void *user_data)
{
	ARG_UNUSED(user_data);
	LOG_INF("FUOTA %s", fuota_state_str(state));
	/* The session ended before reconstruction reported a result. */
	if (fuota_state == RZI_FUOTA_STATE_TRANSFERRING && state == RZI_FUOTA_STATE_READY) {
		struct rzi_fuota_round round;
		bool have_round = rzi_fuota_get_round(&round) == 0;

		queue_fuota_result(have_round ? &round : NULL, have_round);
	}
	fuota_state = state;
}

static void on_fuota_session_started(void *user_data)
{
	const struct fuota_log_item item = {
		.code = UPLINK_LOG_EVENT_FUOTA_STARTED,
		.pause_drain = true,
		.has_event = true,
	};

	ARG_UNUSED(user_data);
	LOG_INF("FUOTA transfer started");
	queue_fuota_log(&item);
}

static void on_fuota_complete(int status, void *user_data)
{
	struct rzi_fuota_round round;
	bool have_round = rzi_fuota_get_round(&round) == 0;

	ARG_UNUSED(user_data);
	if (status != 0) {
		LOG_ERR("FUOTA failed: %s", rzi_err_str(status));
		queue_fuota_result(have_round ? &round : NULL, have_round);
		return;
	}
	LOG_INF("FUOTA finished");
	queue_fuota_log(&(struct fuota_log_item){.resume_drain = true});
}

static const struct rzi_lorawan_callbacks callbacks = {
	.on_event = on_event,
};

static const struct rzi_fuota_callbacks fuota_callbacks = {
	.state_changed = on_fuota_state,
	.session_started = on_fuota_session_started,
	.complete = on_fuota_complete,
};

static void submit_fuota_item(const struct fuota_log_item *item)
{
	if (item->has_event) {
		const struct uplink_log_event event = {
			.domain = UPLINK_LOG_EVENT_DOMAIN_FUOTA,
			.code = item->code,
		};

		if (uplink_log_submit_event(&event) != UPLINK_LOG_OK) {
			LOG_WRN("FUOTA log event 0x%02x not queued", item->code);
		}
	}
	if (item->pause_drain) {
		ignore_result(uplink_log_pause_drain());
	}
	if (item->resume_drain) {
		ignore_result(uplink_log_resume_drain());
	}
}

static void handle_fuota_log(void)
{
	struct fuota_log_item item;

	while (k_msgq_get(&fuota_log_q, &item, K_NO_WAIT) == 0) {
		submit_fuota_item(&item);
	}
}

static bool periodic_snapshot(void *user_data, struct uplink_log_periodic *out)
{
	ARG_UNUSED(user_data);
	out->power_source = UPLINK_LOG_POWER_SOURCE_UNKNOWN;
	out->charging_status = UPLINK_LOG_CHARGING_UNKNOWN;
	out->voltage_mv = 0U;
	out->rssi_dbm = uplink_log_rzi_last_rssi();
	out->snr_db = uplink_log_rzi_last_snr();
	out->tx_fail_count = uplink_log_rzi_tx_fail_count();
	out->health = UPLINK_LOG_HEALTH_SENSOR | UPLINK_LOG_HEALTH_POWER | UPLINK_LOG_HEALTH_SYSTEM;
	return true;
}

static enum uplink_log_reset_reason read_reset_reason(void)
{
#if defined(CONFIG_HWINFO)
	uint32_t cause;

	if (hwinfo_get_reset_cause(&cause) == 0) {
		ignore_result(hwinfo_clear_reset_cause());
		if ((cause & RESET_WATCHDOG) != 0U) {
			return UPLINK_LOG_RESET_WATCHDOG;
		}
		if ((cause & RESET_POR) != 0U) {
			return UPLINK_LOG_RESET_POWER_ON;
		}
	}
#endif
	return UPLINK_LOG_RESET_UNKNOWN;
}

static void submit_boot_report(enum uplink_log_reset_reason reset_reason, uint32_t join_failures)
{
	const struct uplink_log_boot boot = {
		.fw_version = {APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_PATCHLEVEL, APP_TWEAK},
		.reset_reason = reset_reason,
		.cap_fuota = true,
		.cap_full = true,
		.cap_rollback = IS_ENABLED(CONFIG_MCUBOOT_BOOTLOADER_MODE_SWAP_SCRATCH),
		.join_fail_count = (uint8_t)MIN(join_failures, UINT8_MAX),
	};
	int rc = uplink_log_submit_boot(&boot);

	if (rc != UPLINK_LOG_OK) {
		LOG_WRN("Boot report not queued: %d", rc);
	}
}

static void confirm_running_image(void)
{
#ifdef CONFIG_IMG_MANAGER
	int rc;

	if (boot_is_img_confirmed()) {
		return;
	}
	rc = boot_write_img_confirmed();
	if (rc != 0) {
		LOG_ERR("Image confirm failed: %d", rc);
		return;
	}
	LOG_INF("Running image confirmed");
#endif
}

static int start_at(void)
{
	int rc;

	if (!device_is_ready(at_uart)) {
		return -RZI_ERR_NO_DEVICE;
	}

	/* USB CDC waits until the host opens the port. A plain UART returns. */
	if (DT_NODE_HAS_COMPAT(AT_UART_NODE, zephyr_cdc_acm_uart)) {
		uint32_t dtr = 0;

		for (int i = 0; i < 100 && dtr == 0; i++) {
			(void)uart_line_ctrl_get(at_uart, UART_LINE_CTRL_DTR, &dtr);
			k_msleep(100);
		}
	}

	rc = rzi_at_uart_init(at_uart);
	if (rc != 0) {
		return rc;
	}
	return rzi_at_publish_event("BOOT:RZI_%s_%s", rzi_version_get_string(), CONFIG_BOARD);
}

static bool credentials_stored(void)
{
	uint8_t stored[16];

	return rzi_lorawan_get_dev_eui(stored, 8) == 0 && rzi_lorawan_get_app_eui(stored, 8) == 0 &&
	       rzi_lorawan_get_app_key(stored, 16) == 0;
}

static int request_join(void)
{
	int rc;

	joined = false;
	k_sem_reset(&join_done);
	rc = rzi_lorawan_join(1, 0, RZI_LORAWAN_JOIN_INTERVAL_DEFAULT, 0);
	if (rc != 0) {
		LOG_WRN("Join rejected: %s", rzi_err_str(rc));
		return rc;
	}
	LOG_INF("Joining");
	k_sem_take(&join_done, K_FOREVER);
	if (!joined) {
		LOG_WRN("Join failed, retrying in %d s", RETRY_DELAY_S);
		return -RZI_ERR_NOT_READY;
	}
	return 0;
}

int main(void)
{
	static const uint8_t payload[] = "RZI FUOTA";
	const struct uplink_log_options log_options = {
		.device_code = DEVICE_CODE,
		.periodic_snapshot = periodic_snapshot,
	};
	enum uplink_log_reset_reason reset_reason = read_reset_reason();
	enum rzi_lorawan_region region;
	uint32_t join_failures = 0U;
	int64_t next_uplink;
	int rc;

	LOG_INF("RZI LoRaWAN FUOTA sample %d.%d.%d.%d", APP_VERSION_MAJOR, APP_VERSION_MINOR,
		APP_PATCHLEVEL, APP_TWEAK);
	rc = uplink_log_rzi_init(&log_options);
	if (rc != UPLINK_LOG_OK) {
		LOG_ERR("Uplink log init failed: %d", rc);
		return -EINVAL;
	}
	rc = rzi_fuota_register_callbacks(&fuota_callbacks);
	if (rc != 0) {
		LOG_ERR("FUOTA callback registration failed: %s", rzi_err_str(rc));
		return rc;
	}
	rc = rzi_lorawan_register_callbacks(&callbacks);
	if (rc != 0) {
		LOG_ERR("Callback registration failed: %s", rzi_err_str(rc));
		return rc;
	}
	rc = start_at();
	if (rc != 0) {
		LOG_ERR("AT start failed: %s", rzi_err_str(rc));
		return rc;
	}
	/* AT+DEVEUI, AT+APPEUI and AT+APPKEY must already be stored. */
	while (!credentials_stored()) {
		k_msleep(500);
	}
	rc = rzi_lorawan_get_region(&region);
	if (rc != 0) {
		LOG_ERR("Region read failed: %s", rzi_err_str(rc));
		return rc;
	}
	rc = rzi_lorawan_init(region);
	if (rc != 0 && rc != -RZI_ERR_ALREADY) {
		LOG_ERR("LoRaWAN init failed: %s", rzi_err_str(rc));
		return rc;
	}

	k_sem_take(&stack_ready, K_FOREVER);

	while (request_join() != 0) {
		join_failures++;
		ignore_result(rzi_lorawan_leave());
		k_sleep(RETRY_DELAY);
	}
	LOG_INF("Joined");
	confirm_running_image();
	submit_boot_report(reset_reason, join_failures);

	next_uplink = k_uptime_get();
	for (;;) {
		int64_t now = k_uptime_get();

		if (now >= next_uplink) {
			if (rzi_fuota_uplink_allowed()) {
				rc = rzi_lorawan_send(1, payload, sizeof(payload) - 1,
						      RZI_LORAWAN_MSG_UNCONFIRMED);
				if (rc != 0 && rc != -RZI_ERR_BUSY) {
					LOG_WRN("Uplink rejected: %s", rzi_err_str(rc));
				}
			}
			next_uplink = now + UPLINK_INTERVAL_MS;
		}
		handle_fuota_log();
		uplink_log_rzi_poll((uint32_t)MAX(next_uplink - k_uptime_get(), 0));
	}
}
