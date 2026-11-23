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
#include <zephyr/storage/flash_map.h>
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

static __maybe_unused void ignore_result(int result)
{
	ARG_UNUSED(result);
}

#ifdef CONFIG_IMG_MANAGER
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#endif

#if defined(CONFIG_RZI_LORAWAN_BACKEND_USP)
#include <smtc_modem_api.h>

static atomic_t business_blocked;
static atomic_t published_generation;

__weak void smtc_modem_fuota_set_hooks(const struct smtc_modem_fuota_hooks *hooks)
{
	ARG_UNUSED(hooks);
}

bool lorawan_fragmentation_answer_pending(uint8_t stack_id);
bool lorawan_multicast_answer_pending(uint8_t stack_id);

uint32_t smtc_modem_fuota_generation(void)
{
	return (uint32_t)atomic_get(&published_generation);
}

bool smtc_modem_fuota_business_allowed(void)
{
	return atomic_get(&business_blocked) == 0;
}
#endif

#if defined(CONFIG_RZI_LORAWAN_BACKEND_USP) && PARTITION_EXISTS(slot1_partition)
#define FUOTA_DIRECT_SLOT 1
#include <zephyr/drivers/flash.h>
#else
#define FUOTA_DIRECT_SLOT 0
#endif

LOG_MODULE_REGISTER(rzi_fuota, LOG_LEVEL_INF);

#define IMAGE_MAGIC         0x96f3b83dU
#define IMAGE_TLV_MAGIC     0x6907U
#define IMAGE_HDR_SIZE      32U
#define IMAGE_HEAD_LOG      16U
#define APPLY_CHUNK         256U
#define FUOTA_FINISH_WAIT_S 30
#define FUOTA_CONTROL_STATUS BIT(0)
#define FUOTA_CONTROL_DELETE BIT(1)

static K_MUTEX_DEFINE(lock);
static atomic_t active;
#if !defined(CONFIG_RZI_LORAWAN_BACKEND_USP)
static atomic_t business_blocked;
#endif
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

struct fuota_round_state {
	uint32_t generation;
	uint8_t mc_mask;
	bool id_valid;
	bool image_committed;
	bool write_failed;
	int storage_errno;
	bool class_valid;
	enum rzi_lorawan_class original_class;
	uint8_t multicast_group;
	bool group_valid;
	bool status_done;
	bool delete_done;
	bool finish_wait;
	int64_t finish_deadline_ms;
	bool hold_class_c_keepalive;
};

static struct fuota_round_state round;

#if FUOTA_DIRECT_SLOT
/* SessionSetupReq bytes 52 41 4B 30, assembled little-endian by USP. */
#define FUOTA_DESCRIPTOR_DELTA 0x304B4152U

static const struct flash_area *slot;
static size_t session_bytes;
static size_t session_span;
static bool slot_session;
static bool target_upgrade;
static bool reconstructed;
static bool write_failed;
static uint8_t page_buf[4096];
#endif

#if CONFIG_RZI_LORAWAN_FUOTA_KEEPALIVE_INTERVAL_S > 0
static void keepalive_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(keepalive_work, keepalive_work_handler);
#endif

static bool callbacks_empty(const struct rzi_fuota_callbacks *table)
{
	return table->state_changed == NULL && table->session_started == NULL &&
	       table->complete == NULL;
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
	atomic_set(&business_blocked, next == RZI_FUOTA_STATE_TRANSFERRING ||
					      next == RZI_FUOTA_STATE_APPLYING);
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

#if CONFIG_RZI_LORAWAN_FUOTA_KEEPALIVE_INTERVAL_S > 0
static void schedule_keepalive(void)
{
	k_work_schedule(&keepalive_work, K_SECONDS(CONFIG_RZI_LORAWAN_FUOTA_KEEPALIVE_INTERVAL_S));
}

static void cancel_keepalive(void)
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
	    state == RZI_FUOTA_STATE_APPLYING || round.hold_class_c_keepalive) {
		if (round.hold_class_c_keepalive) {
			schedule_keepalive();
		}
		return;
	}
#if defined(CONFIG_RZI_LORAWAN_BACKEND_USP)
	if (lorawan_fragmentation_answer_pending(0) || lorawan_multicast_answer_pending(0)) {
		schedule_keepalive();
		return;
	}
#endif
	rc = rzi_lorawan_send(CONFIG_RZI_LORAWAN_FUOTA_KEEPALIVE_PORT, payload, sizeof(payload),
			      RZI_LORAWAN_MSG_UNCONFIRMED);
	if (rc != 0 && rc != -RZI_ERR_BUSY && rc != -RZI_ERR_NOT_READY) {
		LOG_WRN("FUOTA keepalive rejected: %d", rc);
	}
	schedule_keepalive();
}
#else
static void schedule_keepalive(void)
{
}

static void cancel_keepalive(void)
{
}

static void cancel_keepalive_sync(void)
{
}
#endif

static int read_stored(uint32_t offset, uint8_t *buffer, size_t size)
{
#if FUOTA_DIRECT_SLOT
	if (slot != NULL) {
		if ((uint64_t)offset + size > slot->fa_size) {
			return -RZI_ERR_INVALID;
		}
		return rzi_err_from_errno(flash_area_read(slot, offset, buffer, size));
	}
#endif
	if (ops == NULL || ops->read_image == NULL) {
		return -RZI_ERR_NOT_SUPPORTED;
	}
	return ops->read_image(offset, buffer, size);
}

static void log_image_head(void)
{
	uint8_t prefix[IMAGE_HEAD_LOG];
	size_t request = sizeof(prefix);
	int rc;

	if (image_size != 0U && image_size < request) {
		request = image_size;
	}
	rc = read_stored(0U, prefix, request);
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
	rc = read_stored(0U, header, sizeof(header));
	if (rc != 0) {
		return rc;
	}
	magic = sys_get_le32(&header[0]);
	if (magic != IMAGE_MAGIC) {
		return -RZI_ERR_NO_DATA;
	}
	hdr_size = sys_get_le16(&header[8]);
	payload_size = sys_get_le32(&header[12]);
	rc = read_stored(hdr_size + payload_size, tlv, sizeof(tlv));
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

#if FUOTA_DIRECT_SLOT
static int slot_program(uint32_t offset, const uint8_t *data, size_t size)
{
	const struct device *flash = flash_area_get_device(slot);

	while (size > 0U) {
		struct flash_pages_info info;
		uint32_t page_off;
		uint32_t page_end;
		uint32_t limit;
		uint32_t unit;
		uint32_t unit_off;
		uint32_t align;
		size_t page_index;
		size_t count;
		bool rewrite = false;
		int rc = flash_get_page_info_by_offs(flash, slot->fa_off + offset, &info);

		if (rc != 0 || info.size == 0U || info.start_offset < slot->fa_off) {
			return rc == 0 ? -RZI_ERR_NO_RESOURCE : rzi_err_from_errno(rc);
		}
		page_off = (uint32_t)(info.start_offset - slot->fa_off);
		if (offset < page_off || offset >= slot->fa_size) {
			return -RZI_ERR_INVALID;
		}
		/* The SPI NOR layout page is 64 KB. Program in 4 KB sectors, which
		 * this chip can erase and which fit in page_buf.
		 */
		unit = info.size;
		if (unit > sizeof(page_buf)) {
			if ((info.size % sizeof(page_buf)) != 0U) {
				return -RZI_ERR_NO_RESOURCE;
			}
			unit = sizeof(page_buf);
		}
		page_end = page_off + info.size;
		limit = page_end < slot->fa_size ? page_end : slot->fa_size;
		unit_off = page_off + ((offset - page_off) / unit) * unit;
		if ((uint64_t)unit_off + unit > limit) {
			return -RZI_ERR_INVALID;
		}
		page_index = offset - unit_off;
		count = MIN(unit - page_index, size);
		rc = flash_area_read(slot, unit_off, page_buf, unit);
		if (rc != 0) {
			return rzi_err_from_errno(rc);
		}
		align = flash_area_align(slot);
		if (align == 0U || (align & (align - 1U)) != 0U || align > unit) {
			return -RZI_ERR_NO_RESOURCE;
		}
		{
			uint32_t aligned = offset & ~(align - 1U);
			uint32_t aligned_end =
				((uint32_t)(offset + count) + align - 1U) & ~(align - 1U);

			if (aligned < unit_off) {
				aligned = unit_off;
			}
			if (aligned_end > unit_off + unit) {
				aligned_end = unit_off + unit;
			}
			for (uint32_t i = aligned; i < aligned_end; i++) {
				if (page_buf[i - unit_off] != 0xFFU) {
					rewrite = true;
					break;
				}
			}
		}
		memcpy(&page_buf[page_index], data, count);
		if (rewrite) {
			rc = flash_area_erase(slot, unit_off, unit);
			if (rc == 0) {
				rc = flash_area_write(slot, unit_off, page_buf, unit);
			}
		} else {
			uint32_t start = offset & ~(align - 1U);
			uint32_t end = (uint32_t)(offset + count + align - 1U) & ~(align - 1U);

			if (end > unit_off + unit) {
				end = unit_off + unit;
			}
			rc = flash_area_write(slot, start, &page_buf[start - unit_off], end - start);
		}
		if (rc != 0) {
			return rzi_err_from_errno(rc);
		}
		offset += count;
		data += count;
		size -= count;
	}
	return 0;
}

static bool control_tx_running(void)
{
#if defined(CONFIG_RZI_LORAWAN_BACKEND_USP)
	return lorawan_fragmentation_answer_pending(0) || lorawan_multicast_answer_pending(0);
#else
	return false;
#endif
}

static void finish_try_apply(void);

static void finish_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	k_mutex_lock(&lock, K_FOREVER);
	finish_try_apply();
	k_mutex_unlock(&lock);
}

static K_WORK_DELAYABLE_DEFINE(finish_work, finish_work_handler);

static void finish_try_apply(void)
{
	int rc;

	if (!round.finish_wait || !round.image_committed || !target_upgrade) {
		return;
	}
	if (!round.status_done && k_uptime_get() < round.finish_deadline_ms) {
		return;
	}
	if (control_tx_running()) {
		k_work_schedule(&finish_work, K_SECONDS(1));
		return;
	}
	(void)k_work_cancel_delayable(&finish_work);
	round.finish_wait = false;
	round.hold_class_c_keepalive = false;
	set_state_locked(RZI_FUOTA_STATE_APPLYING);
	notify_complete(0);
	k_mutex_unlock(&lock);
	rc = rzi_fuota_apply();
	k_mutex_lock(&lock, K_FOREVER);
	if (rc != 0) {
		LOG_ERR("FUOTA install failed: %d", rc);
		last_error = rc;
		set_state_locked(RZI_FUOTA_STATE_COMPLETE);
	}
}

static void stop_fuota_multicast(bool group_valid, uint8_t group, uint8_t mc_mask)
{
	int rc;

	if (!group_valid) {
		group = 0U;
		for (uint8_t index = 0U; index < 4U; index++) {
			if ((mc_mask & BIT(index)) != 0U) {
				group = index;
				break;
			}
		}
	}
	if (ops == NULL || ops->stop_multicast == NULL) {
		return;
	}
	rc = ops->stop_multicast(group);
	if (rc == -RZI_ERR_BUSY) {
		LOG_WRN("FUOTA multicast group %u still active", group);
	} else if (rc != 0) {
		LOG_WRN("FUOTA multicast stop failed: %d", rc);
	}
}

static void finish_window(void)
{
	bool committed;
	bool install;
	bool class_c;
	bool group_valid;
	enum rzi_lorawan_class original;
	uint8_t group;
	uint8_t mc_mask;
	size_t bytes;
	int rc;

	k_mutex_lock(&lock, K_FOREVER);
	committed = slot_session && round.image_committed && session_bytes != 0U;
	install = target_upgrade;
	class_c = round.class_valid && round.original_class == RZI_LORAWAN_CLASS_C;
	original = round.class_valid ? round.original_class : RZI_LORAWAN_CLASS_A;
	group_valid = round.group_valid;
	group = round.multicast_group;
	mc_mask = round.mc_mask;
	bytes = session_bytes;
	k_mutex_unlock(&lock);

	stop_fuota_multicast(group_valid, group, mc_mask);
	rc = rzi_lorawan_set_class(original);
	if (rc != 0) {
		LOG_WRN("FUOTA class restore failed: %d", rc);
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (!committed) {
		image_ready = false;
		last_error = -RZI_ERR_IO;
		slot_session = false;
		round.finish_wait = false;
		set_state_locked(RZI_FUOTA_STATE_FAILED);
		notify_complete(-RZI_ERR_IO);
		schedule_keepalive();
		k_mutex_unlock(&lock);
		return;
	}
	image_ready = true;
	image_size = bytes;
	last_error = 0;
	LOG_INF("FUOTA image stored, %u bytes", (unsigned int)bytes);
	log_image_head();
	if (!install) {
		slot_session = false;
		round.finish_wait = false;
		set_state_locked(RZI_FUOTA_STATE_COMPLETE);
		notify_complete(0);
		schedule_keepalive();
		k_mutex_unlock(&lock);
		LOG_INF("FUOTA delta stored, upgrade not requested");
		return;
	}
	round.finish_wait = true;
	round.finish_deadline_ms = k_uptime_get() + (int64_t)FUOTA_FINISH_WAIT_S * 1000;
	round.hold_class_c_keepalive = class_c;
	/* COMPLETE callbacks run unlocked and may race with the next Setup. */
	set_state_locked(RZI_FUOTA_STATE_COMPLETE);
	if (!class_c) {
		schedule_keepalive();
	}
	k_work_schedule(&finish_work, K_SECONDS(FUOTA_FINISH_WAIT_S));
	finish_try_apply();
	k_mutex_unlock(&lock);
}

static uint8_t fuota_area_for_descriptor(uint32_t descriptor, uint8_t *area_id, bool *upgrade)
{
	if (descriptor == 0U) {
		*area_id = PARTITION_ID(slot1_partition);
		*upgrade = true;
		return 0;
	}
#if PARTITION_EXISTS(fuota_delta_partition)
	if (descriptor == FUOTA_DESCRIPTOR_DELTA) {
		*area_id = PARTITION_ID(fuota_delta_partition);
		*upgrade = false;
		return 0;
	}
#endif
	return 0x08;
}

static uint8_t fuota_session_setup(uint32_t descriptor, uint16_t frag_nb, uint8_t frag_size,
				   uint8_t padding)
{
	const struct flash_area *area = NULL;
	const char *name;
	uint8_t area_id;
	uint8_t status;
	uint32_t raw;
	uint32_t bytes;
	bool upgrade;
	int rc;

	if (frag_nb == 0U || frag_size == 0U || padding > frag_size) {
		return 0x02;
	}
	raw = (uint32_t)frag_nb * frag_size;
	if (padding >= raw) {
		return 0x02;
	}
	bytes = raw - padding;
	status = fuota_area_for_descriptor(descriptor, &area_id, &upgrade);
	if (status != 0U) {
		LOG_WRN("FUOTA descriptor 0x%08x rejected", descriptor);
		return status;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if ((round.finish_wait && round.image_committed) || state == RZI_FUOTA_STATE_APPLYING) {
		k_mutex_unlock(&lock);
		return 0x02;
	}
	rc = flash_area_open(area_id, &area);
	if (rc != 0) {
		k_mutex_unlock(&lock);
		return 0x02;
	}
	if (raw > area->fa_size) {
		flash_area_close(area);
		k_mutex_unlock(&lock);
		return 0x02;
	}
#if defined(CONFIG_RZI_POWER)
	if (!power_flash_held && rzi_power_lock_get(RZI_POWER_LOCK_FLASH) == 0) {
		power_flash_held = true;
	}
#endif
	rc = flash_area_erase(area, 0, area->fa_size);
	if (rc != 0) {
		flash_area_close(area);
		slot_session = false;
		k_mutex_unlock(&lock);
		LOG_ERR("FUOTA erase failed: %d", rc);
		return 0x02;
	}
	if (slot != NULL) {
		flash_area_close(slot);
	}
	slot = area;
	session_bytes = bytes;
	session_span = raw;
	target_upgrade = upgrade;
	slot_session = true;
	reconstructed = false;
	write_failed = false;
	image_ready = false;
	image_size = bytes;
	round.generation++;
	if (round.generation == 0U) {
		round.generation = 1U;
	}
	atomic_set(&published_generation, (atomic_val_t)round.generation);
	round.id_valid = true;
	round.mc_mask = 0U;
	round.image_committed = false;
	round.write_failed = false;
	round.storage_errno = 0;
	round.class_valid = false;
	round.group_valid = false;
	round.status_done = false;
	round.delete_done = false;
	round.finish_wait = false;
	round.hold_class_c_keepalive = false;
	k_mutex_unlock(&lock);
	name = upgrade ? "slot1" : "fuota_delta";
	LOG_INF("FUOTA descriptor 0x%08x -> %s, %u bytes", descriptor, name, bytes);
	return 0;
}

static int fuota_store(uint32_t offset, const uint8_t *data, uint32_t size)
{
	int rc;

	k_mutex_lock(&lock, K_FOREVER);
	if (!slot_session || slot == NULL || data == NULL ||
	    (uint64_t)offset + size > session_span) {
		write_failed = true;
		round.write_failed = true;
		k_mutex_unlock(&lock);
		return -1;
	}
	rc = slot_program(offset, data, size);
	if (rc != 0) {
		write_failed = true;
		round.write_failed = true;
		if (round.storage_errno == 0) {
			round.storage_errno = rc;
		}
		LOG_ERR("FUOTA flash write failed: %d", rc);
	}
	k_mutex_unlock(&lock);
	return rc == 0 ? 0 : -1;
}

static void fuota_session_delete(uint32_t generation)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (generation == round.generation && !round.image_committed) {
		round.id_valid = false;
		slot_session = false;
	}
	k_mutex_unlock(&lock);
}

static int fuota_restore(uint32_t offset, uint8_t *data, uint32_t size)
{
	int rc;

	k_mutex_lock(&lock, K_FOREVER);
	if (!slot_session || slot == NULL || data == NULL ||
	    (uint64_t)offset + size > slot->fa_size) {
		k_mutex_unlock(&lock);
		return -1;
	}
	rc = flash_area_read(slot, offset, data, size);
	k_mutex_unlock(&lock);
	return rc == 0 ? 0 : -1;
}

static void fuota_register_hooks(void)
{
	static const struct smtc_modem_fuota_hooks hooks = {
		.session_setup = fuota_session_setup,
		.store = fuota_store,
		.restore = fuota_restore,
		.session_delete = fuota_session_delete,
	};

	smtc_modem_fuota_set_hooks(&hooks);
}
#elif defined(CONFIG_RZI_LORAWAN_BACKEND_USP)
static uint8_t fuota_session_setup(uint32_t descriptor, uint16_t frag_nb, uint8_t frag_size,
				   uint8_t padding)
{
	ARG_UNUSED(descriptor);
	ARG_UNUSED(frag_nb);
	ARG_UNUSED(frag_size);
	ARG_UNUSED(padding);
	return 0x02;
}

static int fuota_store(uint32_t offset, const uint8_t *data, uint32_t size)
{
	ARG_UNUSED(offset);
	ARG_UNUSED(data);
	ARG_UNUSED(size);
	return -1;
}

static int fuota_restore(uint32_t offset, uint8_t *data, uint32_t size)
{
	ARG_UNUSED(offset);
	ARG_UNUSED(data);
	ARG_UNUSED(size);
	return -1;
}

static void fuota_register_hooks(void)
{
	static const struct smtc_modem_fuota_hooks hooks = {
		.session_setup = fuota_session_setup,
		.store = fuota_store,
		.restore = fuota_restore,
	};

	smtc_modem_fuota_set_hooks(&hooks);
}
#endif

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
#if FUOTA_DIRECT_SLOT
		if (slot_session) {
			k_mutex_unlock(&lock);
			finish_window();
			k_mutex_lock(&lock, K_FOREVER);
			break;
		}
#endif
		if (state == RZI_FUOTA_STATE_TRANSFERRING && !image_ready) {
			schedule_keepalive();
			set_state_locked(RZI_FUOTA_STATE_READY);
		}
		break;
	case RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE:
#if FUOTA_DIRECT_SLOT
		if (slot_session) {
			reconstructed = event->fuota.successful;
			round.write_failed = write_failed;
			round.image_committed = reconstructed && !write_failed;
			LOG_INF("FUOTA reconstruction %s", reconstructed ? "complete" : "failed");
			break;
		}
#endif
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
	case RZI_LORAWAN_BACKEND_FUOTA_SETUP_ACCEPTED:
		if (event->fuota.generation == round.generation && round.id_valid) {
			round.mc_mask = event->fuota.mc_group_bit_mask;
		}
		break;
	case RZI_LORAWAN_BACKEND_FUOTA_CLASS_CAPTURED:
		if (!round.class_valid) {
			round.original_class = event->fuota.original_class;
			round.class_valid = true;
			round.multicast_group = event->fuota.multicast_group;
			round.group_valid = true;
		}
		break;
	case RZI_LORAWAN_BACKEND_FUOTA_CONTROL_DONE:
		if (event->fuota.generation != round.generation) {
			break;
		}
		if ((event->fuota.control & FUOTA_CONTROL_STATUS) != 0U) {
			round.status_done = true;
		}
		if ((event->fuota.control & FUOTA_CONTROL_DELETE) != 0U) {
			round.delete_done = true;
			if (!round.image_committed) {
				round.id_valid = false;
#if FUOTA_DIRECT_SLOT
				slot_session = false;
#endif
			}
		}
#if FUOTA_DIRECT_SLOT
		if (round.finish_wait) {
			k_work_schedule(&finish_work, K_NO_WAIT);
		}
#endif
		break;
	}
}

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

int rzi_fuota_get_round(struct rzi_fuota_round *out)
{
	if (k_is_in_isr()) {
		return -RZI_ERR_ISR;
	}
	if (out == NULL) {
		return -RZI_ERR_INVALID;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (!round.id_valid || round.generation == 0U) {
		k_mutex_unlock(&lock);
		return -RZI_ERR_NOT_FOUND;
	}
	out->generation = round.generation;
	out->write_failed = round.write_failed;
	out->storage_errno = round.storage_errno;
	k_mutex_unlock(&lock);
	return 0;
}

bool rzi_fuota_uplink_allowed(void)
{
	bool allowed;

	if (k_is_in_isr()) {
		return false;
	}
	k_mutex_lock(&lock, K_FOREVER);
	allowed = state != RZI_FUOTA_STATE_TRANSFERRING && state != RZI_FUOTA_STATE_APPLYING;
	k_mutex_unlock(&lock);
	if (!allowed) {
		return false;
	}
#if defined(CONFIG_RZI_LORAWAN_BACKEND_USP)
	if (lorawan_fragmentation_answer_pending(0) || lorawan_multicast_answer_pending(0)) {
		return false;
	}
#endif
	return true;
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
	if (image_ready && image_size == 0U) {
		image_size = size;
	}
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
	} else {
		rc = read_stored((uint32_t)offset, buffer, size);
	}
	k_mutex_unlock(&lock);
	return rc;
}

int rzi_fuota_apply(void)
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
	int rc;

#if FUOTA_DIRECT_SLOT
	if (slot != NULL && !target_upgrade) {
		k_mutex_lock(&lock, K_FOREVER);
		last_error = -RZI_ERR_NOT_SUPPORTED;
		set_state_locked(RZI_FUOTA_STATE_COMPLETE);
		k_mutex_unlock(&lock);
		return -RZI_ERR_NOT_SUPPORTED;
	}
	rc = slot != NULL ? rzi_err_from_errno(boot_request_upgrade(BOOT_UPGRADE_TEST))
			  : copy_image_to_slot();
#else
	rc = copy_image_to_slot();
#endif

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

static void fuota_on_event(const struct rzi_lorawan_backend_event *event)
{
	if (event == NULL || !atomic_get(&active)) {
		return;
	}

	k_mutex_lock(&lock, K_FOREVER);
	switch (event->type) {
	case RZI_LORAWAN_BACKEND_JOINED:
		handle_joined();
		break;
	case RZI_LORAWAN_BACKEND_JOIN_FAILED:
		cancel_keepalive();
#if FUOTA_DIRECT_SLOT
		round.finish_wait = false;
		(void)k_work_cancel_delayable(&finish_work);
#endif
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
}

static void fuota_on_initialized(void)
{
#if defined(CONFIG_RZI_LORAWAN_BACKEND_USP)
	fuota_register_hooks();
#endif
	ignore_result(rzi_fuota_init());
}

static int fuota_on_stopping(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (state == RZI_FUOTA_STATE_APPLYING || state == RZI_FUOTA_STATE_TRANSFERRING ||
	    round.finish_wait) {
		k_mutex_unlock(&lock);
		return -RZI_ERR_BUSY;
	}
	k_mutex_unlock(&lock);
	return 0;
}

static void fuota_on_stopped(void)
{
	atomic_clear(&active);
	cancel_keepalive_sync();
#if FUOTA_DIRECT_SLOT
	{
		struct k_work_sync sync;

		(void)k_work_cancel_delayable_sync(&finish_work, &sync);
	}
#endif
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
	memset(&round, 0, sizeof(round));
#if FUOTA_DIRECT_SLOT
	slot_session = false;
	target_upgrade = false;
	reconstructed = false;
	write_failed = false;
	session_bytes = 0U;
	session_span = 0U;
#endif
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
	};

	return rzi_lorawan_register_service(&service);
}

SYS_INIT(fuota_register, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
