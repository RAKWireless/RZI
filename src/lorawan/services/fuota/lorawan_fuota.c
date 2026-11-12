/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Backend-independent LoRaWAN FUOTA coordination.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <rzi/lorawan/fuota.h>
#include <rzi/lorawan/lorawan.h>

#if defined(CONFIG_RZI_POWER)
#include <rzi/power/power.h>
#endif

#include "../../backend/lorawan_backend.h"
#include "../../lorawan_service.h"

#ifdef CONFIG_RZI_LORAWAN_FUOTA_AGENT
#include "fuota_agent_internal.h"
/* Pins a ready generation across application callbacks, copy and reboot. */
static bool data_in_use;
#endif

static __maybe_unused void ignore_result(int result)
{
	ARG_UNUSED(result);
}

#ifdef CONFIG_IMG_MANAGER
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#endif

LOG_MODULE_REGISTER(rzi_fuota, LOG_LEVEL_INF);

#define IMAGE_MAGIC     0x96f3b83dU
#define IMAGE_TLV_MAGIC 0x6907U
#define IMAGE_HDR_SIZE  32U
#define IMAGE_HEAD_LOG  16U
#define APPLY_CHUNK     256U

static K_MUTEX_DEFINE(lock);
static atomic_t active;
static enum rzi_fuota_state state = RZI_FUOTA_STATE_IDLE;
static bool image_ready;
#if defined(CONFIG_RZI_POWER)
static bool power_flash_held;
#endif
static size_t image_size;
static size_t expected_size;
static int last_error;
static struct rzi_fuota_callbacks callbacks;
static const struct rzi_lorawan_fuota_ops *ops;

#if CONFIG_RZI_LORAWAN_FUOTA_KEEPALIVE_INTERVAL_S > 0 && !defined(CONFIG_RZI_LORAWAN_FUOTA_AGENT)
static void keepalive_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(keepalive_work, keepalive_work_handler);
#endif

static bool callbacks_empty(const struct rzi_fuota_callbacks *table)
{
	return table->state_changed == NULL && table->session_started == NULL &&
	       table->session_ended == NULL && table->complete == NULL
#ifdef CONFIG_RZI_LORAWAN_FUOTA_AGENT
	       && table->agent_event == NULL
#endif
		;
}

static const struct rzi_lorawan_fuota_ops *resolve_ops(void)
{
	return rzi_lorawan_backend.fuota;
}

static void set_state_locked(enum rzi_fuota_state next)
{
	struct rzi_fuota_callbacks table = callbacks;

	if (state == next) {
		return;
	}
	state = next;
#if defined(CONFIG_RZI_POWER)
	{
		bool want =
			next == RZI_FUOTA_STATE_TRANSFERRING || next == RZI_FUOTA_STATE_APPLYING;

		if (want && !power_flash_held && rzi_power_lock_get(RZI_POWER_LOCK_FLASH) == 0) {
			power_flash_held = true;
		} else if (!want && power_flash_held) {
			ignore_result(rzi_power_lock_put(RZI_POWER_LOCK_FLASH));
			power_flash_held = false;
		}
	}
#endif
	k_mutex_unlock(&lock);
	if (table.state_changed != NULL) {
		table.state_changed(next, table.user_data);
	}
	k_mutex_lock(&lock, K_FOREVER);
}

static void notify_complete(int status)
{
	struct rzi_fuota_callbacks table = callbacks;

	k_mutex_unlock(&lock);
	if (table.complete != NULL) {
		table.complete(status, table.user_data);
	}
	k_mutex_lock(&lock, K_FOREVER);
}

static void notify_session_started(void)
{
	struct rzi_fuota_callbacks table = callbacks;

	k_mutex_unlock(&lock);
	if (table.session_started != NULL) {
		table.session_started(table.user_data);
	}
	k_mutex_lock(&lock, K_FOREVER);
}

static void notify_session_ended(void)
{
	struct rzi_fuota_callbacks table = callbacks;

	k_mutex_unlock(&lock);
	if (table.session_ended != NULL) {
		table.session_ended(table.user_data);
	}
	k_mutex_lock(&lock, K_FOREVER);
}


#if CONFIG_RZI_LORAWAN_FUOTA_KEEPALIVE_INTERVAL_S > 0 && !defined(CONFIG_RZI_LORAWAN_FUOTA_AGENT)
static __maybe_unused void schedule_keepalive(void)
{
	k_work_schedule(&keepalive_work, K_SECONDS(CONFIG_RZI_LORAWAN_FUOTA_KEEPALIVE_INTERVAL_S));
}

static __maybe_unused void cancel_keepalive(void)
{
	(void)k_work_cancel_delayable(&keepalive_work);
}

static void cancel_keepalive_sync(void)
{
	struct k_work_sync sync;

	(void)k_work_cancel_delayable_sync(&keepalive_work, &sync);
}

static void keepalive_work_handler(struct k_work *work)
{
	static const uint8_t payload[] = {0x00};
	int rc;

	ARG_UNUSED(work);
	if (!atomic_get(&active) || state == RZI_FUOTA_STATE_TRANSFERRING ||
	    state == RZI_FUOTA_STATE_APPLYING) {
		return;
	}
	rc = rzi_lorawan_send(CONFIG_RZI_LORAWAN_FUOTA_KEEPALIVE_PORT, payload, sizeof(payload),
			      RZI_LORAWAN_MSG_UNCONFIRMED);
	if (rc != 0 && rc != -RZI_ERR_BUSY && rc != -RZI_ERR_NOT_READY) {
		LOG_WRN("FUOTA keepalive rejected: %d", rc);
	}
	schedule_keepalive();
}
#else
static __maybe_unused void schedule_keepalive(void)
{
}

static __maybe_unused void cancel_keepalive(void)
{
}

static void cancel_keepalive_sync(void)
{
}
#endif

#ifndef CONFIG_RZI_LORAWAN_FUOTA_AGENT
static void log_image_head(void)
{
	uint8_t prefix[IMAGE_HEAD_LOG];
	size_t request = sizeof(prefix);
	int rc;

	if (ops == NULL || ops->read_image == NULL) {
		return;
	}
	if (image_size != 0U && image_size < request) {
		request = image_size;
	}
	rc = ops->read_image(0U, prefix, request);
	if (rc != 0) {
		LOG_WRN("FUOTA image head unread: %d", rc);
		return;
	}
	LOG_HEXDUMP_INF(prefix, request, "FUOTA image head");
}

static int detect_image_size(size_t reported)
{
	uint8_t header[IMAGE_HDR_SIZE];
	uint8_t tlv[4];
	uint32_t magic;
	uint16_t hdr_size;
	uint32_t payload_size;
	int rc;

	if (expected_size != 0U) {
		image_size = expected_size;
		return 0;
	}
	if (reported != 0U) {
		image_size = reported;
		return 0;
	}
	if (ops != NULL && ops->get_image_size != NULL) {
		size_t measured = 0U;

		rc = ops->get_image_size(&measured);
		if (rc == 0 && measured != 0U) {
			image_size = measured;
			return 0;
		}
	}
	if (ops == NULL || ops->read_image == NULL) {
		return -RZI_ERR_NOT_SUPPORTED;
	}
	rc = ops->read_image(0U, header, sizeof(header));
	if (rc != 0) {
		return rc;
	}
	magic = sys_get_le32(&header[0]);
	if (magic != IMAGE_MAGIC) {
		return -RZI_ERR_NO_DATA;
	}
	hdr_size = sys_get_le16(&header[8]);
	payload_size = sys_get_le32(&header[12]);
	rc = ops->read_image(hdr_size + payload_size, tlv, sizeof(tlv));
	if (rc != 0) {
		return rc;
	}
	if (sys_get_le16(&tlv[0]) != IMAGE_TLV_MAGIC) {
		image_size = (size_t)hdr_size + payload_size;
		return 0;
	}
	image_size = (size_t)hdr_size + payload_size + sys_get_le16(&tlv[2]);
	return 0;
}

#endif

#ifdef CONFIG_IMG_MANAGER
static int copy_image_to_slot(void)
{
	struct flash_img_context ctx;
	uint8_t chunk[APPLY_CHUNK];
	size_t remaining = image_size;
	size_t offset = 0U;
	int rc;

	if (ops == NULL || ops->read_image == NULL) {
		return -RZI_ERR_NOT_SUPPORTED;
	}
	rc = flash_img_init(&ctx);
	if (rc != 0) {
		return rzi_err_from_errno(rc);
	}
	while (remaining > 0U) {
		size_t request = MIN(remaining, sizeof(chunk));
		bool flush = remaining <= sizeof(chunk);

		rc = ops->read_image((uint32_t)offset, chunk, request);
		if (rc != 0) {
			return rc;
		}
		rc = flash_img_buffered_write(&ctx, chunk, request, flush);
		if (rc != 0) {
			return rzi_err_from_errno(rc);
		}
		offset += request;
		remaining -= request;
	}
	return rzi_err_from_errno(boot_request_upgrade(BOOT_UPGRADE_TEST));
}
#endif

#ifndef CONFIG_RZI_LORAWAN_FUOTA_AGENT
static void handle_joined(void)
{
	int rc = 0;

	image_ready = false;
	image_size = expected_size;
	last_error = 0;
	if (ops != NULL && ops->start_clock_sync != NULL) {
		rc = ops->start_clock_sync();
	}
	if (rc != 0) {
		last_error = rc;
		LOG_WRN("FUOTA clock sync failed: %d", rc);
	}
	set_state_locked(RZI_FUOTA_STATE_READY);
	schedule_keepalive();
}

static void handle_fuota_event(const struct rzi_lorawan_backend_event *event)
{
	switch (event->fuota.kind) {
	case RZI_LORAWAN_BACKEND_FUOTA_CLOCK_SYNCED:
		LOG_INF("FUOTA clock synchronized");
		break;
	case RZI_LORAWAN_BACKEND_FUOTA_SESSION_STARTED:
		cancel_keepalive();
		set_state_locked(RZI_FUOTA_STATE_TRANSFERRING);
		notify_session_started();
		break;
	case RZI_LORAWAN_BACKEND_FUOTA_SESSION_ENDED:
		if (state == RZI_FUOTA_STATE_TRANSFERRING && !image_ready) {
			schedule_keepalive();
			set_state_locked(RZI_FUOTA_STATE_READY);
		}
		notify_session_ended();
		break;
	case RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE:
		if (event->fuota.successful) {
			int rc = detect_image_size(event->fuota.image_size);

			image_ready = true;
			last_error = rc == -RZI_ERR_NO_DATA ? 0 : rc;
			if (rc == 0) {
				LOG_INF("FUOTA image ready, %u bytes", (unsigned int)image_size);
			} else {
				LOG_INF("FUOTA image ready, size unknown");
			}
			log_image_head();
			set_state_locked(RZI_FUOTA_STATE_COMPLETE);
			notify_complete(0);
		} else {
			image_ready = false;
			last_error = -RZI_ERR_IO;
			set_state_locked(RZI_FUOTA_STATE_FAILED);
			notify_complete(-RZI_ERR_IO);
			schedule_keepalive();
		}
		break;
	case RZI_LORAWAN_BACKEND_FUOTA_REBOOT_REQUESTED:
		if (image_ready) {
			LOG_INF("FMP reboot requested; call rzi_fuota_apply()");
		}
		break;
	case RZI_LORAWAN_BACKEND_FUOTA_PRECONFIG:
	case RZI_LORAWAN_BACKEND_FUOTA_FRAGMENT_DELETED:
	case RZI_LORAWAN_BACKEND_FUOTA_CONTROL_DONE:
	case RZI_LORAWAN_BACKEND_FUOTA_CLEANUP_UPLINK_DONE:
		break;
	}
}

#else
static void notify_agent_event(const struct rzi_fuota_event *event)
{
	struct rzi_fuota_callbacks table = callbacks;

	k_mutex_unlock(&lock);
	if (table.agent_event != NULL) {
		table.agent_event(event, table.user_data);
	}
	k_mutex_lock(&lock, K_FOREVER);
}

void fuota_service_on_agent_notice(const struct fuota_agent_notice *notice)
{
	k_mutex_lock(&lock, K_FOREVER);
	switch (notice->kind) {
	case FUOTA_AGENT_NOTICE_STATE_CHANGED:
		image_ready = notice->state.data.ready;
		image_size = notice->state.data.length_valid ? notice->state.data.length : 0;
		if (notice->state.next == RZI_FUOTA_STATE_READY) {
			last_error = 0;
		}
		set_state_locked(notice->state.next);
		break;
	case FUOTA_AGENT_NOTICE_SESSION_STARTED:
		notify_session_started();
		break;
	case FUOTA_AGENT_NOTICE_TRANSFER_RESULT:
		last_error = notice->transfer_result;
		notify_complete(notice->transfer_result);
		break;
	case FUOTA_AGENT_NOTICE_EVENT:
		notify_agent_event(notice->event);
		if (notice->event->type == RZI_FUOTA_EVENT_CLOSED) {
			/* CLOSED may have registered a new listener for session_ended. */
			notify_session_ended();
		}
		break;
	}
	k_mutex_unlock(&lock);
}
#endif

int rzi_fuota_register_callbacks(const struct rzi_fuota_callbacks *table)
{
	if (k_is_in_isr()) {
		return -RZI_ERR_ISR;
	}
	if (table == NULL || callbacks_empty(table)) {
		return -RZI_ERR_INVALID;
	}

	k_mutex_lock(&lock, K_FOREVER);
	callbacks = *table;
	k_mutex_unlock(&lock);
	return 0;
}

int rzi_fuota_init(void)
{
	if (k_is_in_isr()) {
		return -RZI_ERR_ISR;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (atomic_get(&active)) {
		k_mutex_unlock(&lock);
		return 0;
	}
	ops = resolve_ops();
	if (ops == NULL && (rzi_lorawan_get_capabilities() & RZI_LORAWAN_CAP_FUOTA) == 0U) {
		k_mutex_unlock(&lock);
		return -RZI_ERR_NOT_SUPPORTED;
	}
#ifdef CONFIG_RZI_LORAWAN_FUOTA_AGENT
	int rc = fuota_agent_init();
	if (rc != 0) {
		k_mutex_unlock(&lock);
		return rc;
	}
#endif
	atomic_set(&active, 1);
	k_mutex_unlock(&lock);
	LOG_INF("FUOTA coordination initialized");
	return 0;
}

int rzi_fuota_get_status(struct rzi_fuota_status *status)
{
	if (k_is_in_isr()) {
		return -RZI_ERR_ISR;
	}
	if (status == NULL) {
		return -RZI_ERR_INVALID;
	}

	k_mutex_lock(&lock, K_FOREVER);
	status->state = state;
	status->image_ready = image_ready;
	status->image_size = image_size;
	status->last_error = last_error;
	k_mutex_unlock(&lock);
	return 0;
}

int rzi_fuota_set_expected_size(size_t size)
{
	if (k_is_in_isr()) {
		return -RZI_ERR_ISR;
	}
	if (size == 0U) {
		return -RZI_ERR_INVALID;
	}

	k_mutex_lock(&lock, K_FOREVER);
	expected_size = size;
#ifdef CONFIG_RZI_LORAWAN_FUOTA_AGENT
	(void)fuota_agent_set_expected_size(size);
#else
	if (image_ready && image_size == 0U) {
		image_size = size;
	}
#endif
	k_mutex_unlock(&lock);
	return 0;
}

int rzi_fuota_read_image(size_t offset, void *buffer, size_t size)
{
	int rc;

	if (k_is_in_isr()) {
		return -RZI_ERR_ISR;
	}
	if (buffer == NULL || size == 0U) {
		return -RZI_ERR_INVALID;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (!image_ready) {
		rc = -RZI_ERR_NOT_FOUND;
	}
#ifdef CONFIG_RZI_LORAWAN_FUOTA_AGENT
	else if (!image_size) {
		rc = -RZI_ERR_NO_DATA;
	}
	else if (offset > image_size || size > image_size - offset) {
		rc = -RZI_ERR_INVALID;
	}
#endif
	else if (ops == NULL || ops->read_image == NULL) {
		rc = -RZI_ERR_NOT_SUPPORTED;
	} else {
		rc = ops->read_image((uint32_t)offset, buffer, size);
	}
	k_mutex_unlock(&lock);
	return rc;
}

static int install_ready_image(void)
{
	if (k_is_in_isr()) {
		return -RZI_ERR_ISR;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (!image_ready) {
		k_mutex_unlock(&lock);
		return -RZI_ERR_NOT_FOUND;
	}
	if (image_size == 0U) {
		k_mutex_unlock(&lock);
		return -RZI_ERR_NO_DATA;
	}
	set_state_locked(RZI_FUOTA_STATE_APPLYING);
	k_mutex_unlock(&lock);

#ifndef CONFIG_IMG_MANAGER
	ARG_UNUSED(ops);
	k_mutex_lock(&lock, K_FOREVER);
	last_error = -RZI_ERR_NOT_SUPPORTED;
	set_state_locked(RZI_FUOTA_STATE_COMPLETE);
	k_mutex_unlock(&lock);
	return -RZI_ERR_NOT_SUPPORTED;
#else
	int rc = copy_image_to_slot();

	if (rc != 0) {
		k_mutex_lock(&lock, K_FOREVER);
		last_error = rc;
		set_state_locked(RZI_FUOTA_STATE_COMPLETE);
		k_mutex_unlock(&lock);
		return rc;
	}
	if (ops != NULL && ops->reboot != NULL) {
		return ops->reboot();
	}
	return -RZI_ERR_NOT_SUPPORTED;
#endif
}

int rzi_fuota_apply(void)
{
#ifdef CONFIG_RZI_LORAWAN_FUOTA_AGENT
	struct rzi_fuota_snapshot current;
	if (k_is_in_isr()) {
		return -RZI_ERR_ISR;
	}
	k_mutex_lock(&lock, K_FOREVER);
	if (data_in_use) {
		k_mutex_unlock(&lock);
		return -RZI_ERR_BUSY;
	}
	data_in_use = true;
	k_mutex_unlock(&lock);
	int rc = rzi_fuota_get_snapshot(&current);
	if (rc == 0) {
		rc = current.data.ready && current.phase != RZI_FUOTA_READY ? -RZI_ERR_BUSY
									    : install_ready_image();
	}
	k_mutex_lock(&lock, K_FOREVER);
	data_in_use = false;
	k_mutex_unlock(&lock);
	return rc;
#else
	return install_ready_image();
#endif
}

#ifdef CONFIG_RZI_LORAWAN_FUOTA_AGENT
int rzi_fuota_release_data(uint32_t generation)
{
	if (k_is_in_isr()) {
		return -RZI_ERR_ISR;
	}
	k_mutex_lock(&lock, K_FOREVER);
	if (data_in_use) {
		k_mutex_unlock(&lock);
		return -RZI_ERR_BUSY;
	}
	data_in_use = true;
	k_mutex_unlock(&lock);
	int rc = fuota_agent_release_data(generation);
	k_mutex_lock(&lock, K_FOREVER);
	data_in_use = false;
	k_mutex_unlock(&lock);
	return rc;
}
#else
int rzi_fuota_get_snapshot(struct rzi_fuota_snapshot *out)
{
	ARG_UNUSED(out);
	return k_is_in_isr() ? -RZI_ERR_ISR : -RZI_ERR_NOT_SUPPORTED;
}
int rzi_fuota_get_session_info(struct rzi_fuota_session_info *out)
{
	ARG_UNUSED(out);
	return k_is_in_isr() ? -RZI_ERR_ISR : -RZI_ERR_NOT_SUPPORTED;
}
int rzi_fuota_get_data_info(struct rzi_fuota_data_info *out)
{
	ARG_UNUSED(out);
	return k_is_in_isr() ? -RZI_ERR_ISR : -RZI_ERR_NOT_SUPPORTED;
}
int rzi_fuota_release_data(uint32_t generation)
{
	ARG_UNUSED(generation);
	return k_is_in_isr() ? -RZI_ERR_ISR : -RZI_ERR_NOT_SUPPORTED;
}
#endif

static void fuota_on_event(const struct rzi_lorawan_backend_event *event)
{
	if (event == NULL || !atomic_get(&active)) {
		return;
	}

#ifdef CONFIG_RZI_LORAWAN_FUOTA_AGENT
	fuota_agent_on_event(event);
#else
	k_mutex_lock(&lock, K_FOREVER);
	switch (event->type) {
	case RZI_LORAWAN_BACKEND_JOINED:
		handle_joined();
		break;
	case RZI_LORAWAN_BACKEND_JOIN_FAILED:
		cancel_keepalive();
		image_ready = false;
		set_state_locked(RZI_FUOTA_STATE_IDLE);
		break;
	case RZI_LORAWAN_BACKEND_STATE_CHANGED:
		if (event->state == RZI_LORAWAN_STATE_READY && state != RZI_FUOTA_STATE_IDLE) {
			cancel_keepalive();
			image_ready = false;
			set_state_locked(RZI_FUOTA_STATE_IDLE);
		}
		break;
	case RZI_LORAWAN_BACKEND_FUOTA:
		handle_fuota_event(event);
		break;
	default:
		break;
	}
	k_mutex_unlock(&lock);
#endif
}

static void fuota_on_initialized(void)
{
	ignore_result(rzi_fuota_init());
}

static int fuota_on_stopping(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (state == RZI_FUOTA_STATE_APPLYING
#ifdef CONFIG_RZI_LORAWAN_FUOTA_AGENT
	    || data_in_use || !fuota_agent_can_stop()
#endif
	) {
		k_mutex_unlock(&lock);
		return -RZI_ERR_BUSY;
	}
	k_mutex_unlock(&lock);
	return 0;
}

static void fuota_on_stopped(void)
{
	atomic_clear(&active);
#ifdef CONFIG_RZI_LORAWAN_FUOTA_AGENT
	fuota_agent_stopped();
#endif
	cancel_keepalive_sync();
	k_mutex_lock(&lock, K_FOREVER);
#if defined(CONFIG_RZI_POWER)
	if (power_flash_held) {
		ignore_result(rzi_power_lock_put(RZI_POWER_LOCK_FLASH));
		power_flash_held = false;
	}
#endif
	state = RZI_FUOTA_STATE_IDLE;
	image_ready = false;
	image_size = expected_size;
	last_error = 0;
	ops = NULL;
	k_mutex_unlock(&lock);
}

static void fuota_on_deinit(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	memset(&callbacks, 0, sizeof(callbacks));
	k_mutex_unlock(&lock);
}

static int fuota_register(void)
{
	static const struct rzi_lorawan_service service = {
		.on_initialized = fuota_on_initialized,
		.on_stopping = fuota_on_stopping,
		.on_stopped = fuota_on_stopped,
		.on_deinit = fuota_on_deinit,
		.on_event = fuota_on_event,
#ifdef CONFIG_RZI_LORAWAN_FUOTA_AGENT
		.business_allowed = fuota_agent_business_allowed,
#endif
	};

	return rzi_lorawan_register_service(&service);
}

SYS_INIT(fuota_register, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
