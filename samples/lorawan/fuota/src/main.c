/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#if defined(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif
#include <rzi/lorawan/fuota.h>
#include <rzi/lorawan/lorawan.h>

#include <uplink_log.h>

#include "uplink_log_rzi.h"

static __maybe_unused void ignore_result(int result)
{
	ARG_UNUSED(result);
}

LOG_MODULE_REGISTER(rzi_fuota_sample, LOG_LEVEL_INF);

#define USER_NODE          DT_PATH(zephyr_user)
#define LORAWAN_REGION     RZI_LORAWAN_REGION_EU_868
#define RETRY_DELAY_S      5
#define RETRY_DELAY        K_SECONDS(RETRY_DELAY_S)
#define UPLINK_INTERVAL_MS 5000

/* Firmware version reported in the boot report (major.minor.patch.build). */
#define FW_VERSION_MAJOR 0
#define FW_VERSION_MINOR 1
#define FW_VERSION_PATCH 0
#define FW_VERSION_BUILD 0

#if defined(CONFIG_BOARD_RAK3372_F)
#define DEVICE_CODE UPLINK_LOG_MODULE_RAK3172_F
#elif defined(CONFIG_BOARD_RAK4631)
#define DEVICE_CODE UPLINK_LOG_MODULE_RAK4630
#else
#define DEVICE_CODE UPLINK_LOG_MODULE_UNKNOWN
#endif

/* FUOTA session milestones handed from the dispatcher thread to main. */
#define FUOTA_SESSION_STARTED BIT(0)
#define FUOTA_SESSION_ENDED   BIT(1)
#define FUOTA_FRAG_INCOMPLETE BIT(2)

static K_SEM_DEFINE(stack_ready, 0, 1);
static K_SEM_DEFINE(join_done, 0, 1);
static volatile bool joined;
static atomic_t fuota_milestones;
static enum rzi_fuota_state fuota_state;

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

static void post_fuota_milestone(atomic_val_t milestone)
{
	atomic_or(&fuota_milestones, milestone);
	uplink_log_rzi_wakeup();
}

static void on_fuota_state(enum rzi_fuota_state state, void *user_data)
{
	ARG_UNUSED(user_data);
	LOG_INF("FUOTA %s", fuota_state_str(state));
	/* The session ended before the image was reconstructed. */
	if (fuota_state == RZI_FUOTA_STATE_TRANSFERRING && state == RZI_FUOTA_STATE_READY) {
		post_fuota_milestone(FUOTA_FRAG_INCOMPLETE | FUOTA_SESSION_ENDED);
	}
	fuota_state = state;
}

static void on_fuota_session_started(void *user_data)
{
	ARG_UNUSED(user_data);
	LOG_INF("FUOTA transfer started");
	post_fuota_milestone(FUOTA_SESSION_STARTED);
}

static void on_fuota_complete(int status, void *user_data)
{
	ARG_UNUSED(user_data);
	if (status != 0) {
		LOG_ERR("FUOTA failed: %s", rzi_err_str(status));
		post_fuota_milestone(FUOTA_FRAG_INCOMPLETE | FUOTA_SESSION_ENDED);
		return;
	}
	LOG_INF("FUOTA finished");
	post_fuota_milestone(FUOTA_SESSION_ENDED);
}

static const struct rzi_lorawan_callbacks callbacks = {
	.on_event = on_event,
};

static const struct rzi_fuota_callbacks fuota_callbacks = {
	.state_changed = on_fuota_state,
	.session_started = on_fuota_session_started,
	.complete = on_fuota_complete,
};

static void submit_fuota_event(uint8_t code)
{
	struct uplink_log_event event = {
		.domain = UPLINK_LOG_EVENT_DOMAIN_FUOTA,
		.code = code,
	};
	int rc = uplink_log_submit_event(&event);

	if (rc != UPLINK_LOG_OK) {
		LOG_WRN("FUOTA log event 0x%02x not queued: %d", code, rc);
	}
}

/*
 * The FUOTA service does not expose fragment counts or the target version
 * yet, so Event Data stays zero.
 */
static void handle_fuota_milestones(void)
{
	atomic_val_t milestones = atomic_clear(&fuota_milestones);

	if ((milestones & FUOTA_SESSION_STARTED) != 0) {
		submit_fuota_event(UPLINK_LOG_EVENT_FUOTA_STARTED);
		ignore_result(uplink_log_pause_drain());
	}
	if ((milestones & FUOTA_FRAG_INCOMPLETE) != 0) {
		submit_fuota_event(UPLINK_LOG_EVENT_FUOTA_FRAG_INCOMPLETE);
	}
	if ((milestones & FUOTA_SESSION_ENDED) != 0) {
		ignore_result(uplink_log_resume_drain());
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
		.fw_version = {FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH,
			       FW_VERSION_BUILD},
		.reset_reason = reset_reason,
		.cap_fuota = true,
		.cap_full = true,
		.join_fail_count = (uint8_t)MIN(join_failures, UINT8_MAX),
	};
	int rc = uplink_log_submit_boot(&boot);

	if (rc != UPLINK_LOG_OK) {
		LOG_WRN("Boot report not queued: %d", rc);
	}
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
	static const uint8_t dev_eui[] = DT_PROP(USER_NODE, user_lorawan_device_eui);
	static const uint8_t join_eui[] = DT_PROP(USER_NODE, user_lorawan_join_eui);
	static const uint8_t app_key[] = DT_PROP(USER_NODE, user_lorawan_app_key);
	static const uint8_t gen_app_key[] = DT_PROP(USER_NODE, user_lorawan_gen_app_key);
	static const uint8_t payload[] = "RZI FUOTA";
	const struct uplink_log_options log_options = {
		.device_code = DEVICE_CODE,
		.periodic_snapshot = periodic_snapshot,
	};
	enum uplink_log_reset_reason reset_reason = read_reset_reason();
	uint32_t join_failures = 0U;
	int64_t next_uplink;
	int rc;

	LOG_INF("RZI LoRaWAN FUOTA sample %d.%d.%d.%d", FW_VERSION_MAJOR, FW_VERSION_MINOR,
		FW_VERSION_PATCH, FW_VERSION_BUILD);
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
	rc = rzi_lorawan_set_dev_eui(dev_eui, sizeof(dev_eui));
	if (rc != 0) {
		LOG_ERR("Credentials failed: %s", rzi_err_str(rc));
		return rc;
	}
	rc = rzi_lorawan_set_app_eui(join_eui, sizeof(join_eui));
	if (rc != 0) {
		LOG_ERR("Credentials failed: %s", rzi_err_str(rc));
		return rc;
	}
	rc = rzi_lorawan_set_app_key(app_key, sizeof(app_key));
	if (rc != 0) {
		LOG_ERR("Credentials failed: %s", rzi_err_str(rc));
		return rc;
	}
	rc = rzi_lorawan_set_gen_app_key(gen_app_key, sizeof(gen_app_key));
	if (rc != 0) {
		LOG_ERR("Credentials failed: %s", rzi_err_str(rc));
		return rc;
	}
	rc = rzi_lorawan_set_activation(RZI_LORAWAN_ACTIVATION_OTAA);
	if (rc != 0) {
		LOG_ERR("Credentials failed: %s", rzi_err_str(rc));
		return rc;
	}
	rc = rzi_lorawan_init(LORAWAN_REGION);
	if (rc != 0) {
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
		handle_fuota_milestones();
		uplink_log_rzi_poll((uint32_t)MAX(next_uplink - k_uptime_get(), 0));
	}
}
