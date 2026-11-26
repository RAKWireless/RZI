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
#include "fuota_header.h"
#include "fuota_install.h"

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
#define FUOTA_HDR_LEN 32U

/* The first 4 KB of image-scratch stages one sector; the second 4 KB holds
 * the 32-byte business header. MCUboot only touches scratch while swapping,
 * and by then the header is no longer needed.
 */
#if PARTITION_EXISTS(scratch_partition)
#define FUOTA_HDR_AREA_ID PARTITION_ID(scratch_partition)
#define FUOTA_HDR_AREA_OFF 4096U
#define FUOTA_HDR_AREA_SIZE 4096U
#else
#define FUOTA_HDR_AREA_ID PARTITION_ID(storage_partition)
#define FUOTA_HDR_AREA_OFF CONFIG_RZI_LORAWAN_FUOTA_HEADER_OFFSET
#define FUOTA_HDR_AREA_SIZE 4096U
#endif
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
/* Host MCU firmware has no 32-byte business header. */
#define FUOTA_DESCRIPTOR_HOST 0x314B4152U

static const struct flash_area *slot;
static size_t session_bytes;
static size_t session_span;
static bool slot_session;
static bool target_upgrade;
static bool reconstructed;
static bool write_failed;
static bool session_has_header = true;
/* Boards without image-scratch keep the sector image in RAM. */
#if !PARTITION_EXISTS(scratch_partition)
static uint8_t page_buf[4096];
#endif
static uint8_t local_device_code;
int rzi_fuota_set_device_code(uint8_t device_code)
{
	local_device_code = device_code;
	return 0;
}
static enum fuota_hdr_result last_hdr_result;
static enum rzi_fuota_reason last_reason;

static enum rzi_fuota_reason reason_from_header(enum fuota_hdr_result result)
{
	switch (result) {
	case FUOTA_HDR_PRODUCT:
		return RZI_FUOTA_REASON_HDR_DEVICE;
	case FUOTA_HDR_VERSION:
		return RZI_FUOTA_REASON_HDR_VERSION;
	case FUOTA_HDR_BASE:
		return RZI_FUOTA_REASON_HDR_BASE;
	case FUOTA_HDR_LENGTH:
		return RZI_FUOTA_REASON_HDR_LENGTH;
	case FUOTA_HDR_CRC:
		return RZI_FUOTA_REASON_HDR_CRC;
	case FUOTA_HDR_IO:
		return RZI_FUOTA_REASON_FLASH_FAIL;
	default:
		return RZI_FUOTA_REASON_HDR_INVALID;
	}
}

static int hdr_area_open(const struct flash_area **area)
{
	int rc = flash_area_open(FUOTA_HDR_AREA_ID, area);

	return rc == 0 ? 0 : rzi_err_from_errno(rc);
}

static int hdr_store(uint32_t offset, const uint8_t *data, size_t size)
{
	const struct flash_area *area = NULL;
	int rc;

	if ((uint64_t)offset + size > FUOTA_HDR_LEN) {
		return -RZI_ERR_BAD_MESSAGE;
	}
	rc = hdr_area_open(&area);
	if (rc != 0) {
		return rc;
	}
	rc = flash_area_write(area, FUOTA_HDR_AREA_OFF + offset, data, size);
	flash_area_close(area);
	return rc == 0 ? 0 : rzi_err_from_errno(rc);
}

static int hdr_read(uint32_t offset, uint8_t *data, size_t size)
{
	const struct flash_area *area = NULL;
	int rc;

	if ((uint64_t)offset + size > FUOTA_HDR_LEN) {
		return -RZI_ERR_BAD_MESSAGE;
	}
	rc = hdr_area_open(&area);
	if (rc != 0) {
		return rc;
	}
	rc = flash_area_read(area, FUOTA_HDR_AREA_OFF + offset, data, size);
	flash_area_close(area);
	return rc == 0 ? 0 : rzi_err_from_errno(rc);
}

static void hdr_erase(void)
{
	const struct flash_area *area = NULL;
	uint32_t sector = FUOTA_HDR_AREA_OFF & ~(FUOTA_HDR_AREA_SIZE - 1U);

	if (hdr_area_open(&area) == 0) {
		ignore_result(flash_area_erase(area, sector, FUOTA_HDR_AREA_SIZE));
		flash_area_close(area);
	}
}
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
	atomic_set(&business_blocked, next == RZI_FUOTA_STATE_RECEIVING ||
					      next == RZI_FUOTA_STATE_INSTALLING);
#if defined(CONFIG_RZI_POWER)
	{
		bool want =
			next == RZI_FUOTA_STATE_RECEIVING || next == RZI_FUOTA_STATE_INSTALLING;

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
	if (!atomic_get(&active) || state == RZI_FUOTA_STATE_RECEIVING ||
	    state == RZI_FUOTA_STATE_INSTALLING || round.hold_class_c_keepalive) {
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
/* First 4 KB of image-scratch. MCUboot's swap trailer stays in the rest. */
#define SCRATCH_PAGE_SIZE 4096U

#if PARTITION_EXISTS(scratch_partition)
BUILD_ASSERT(PARTITION_SIZE(scratch_partition) >= SCRATCH_PAGE_SIZE);

static int copy_flash_area(const struct flash_area *src, uint32_t src_off,
			   const struct flash_area *dst, uint32_t dst_off, size_t len)
{
	uint8_t chunk[APPLY_CHUNK];

	while (len > 0U) {
		size_t n = MIN(len, sizeof(chunk));
		int rc = flash_area_read(src, src_off, chunk, n);

		if (rc != 0) {
			return rzi_err_from_errno(rc);
		}
		rc = flash_area_write(dst, dst_off, chunk, n);
		if (rc != 0) {
			return rzi_err_from_errno(rc);
		}
		src_off += n;
		dst_off += n;
		len -= n;
	}
	return 0;
}

/* Erase image-scratch, stage one sector there, erase the target, copy it back. */
static int program_unit_via_scratch(uint32_t unit_off, uint32_t unit, size_t page_index,
				    const uint8_t *data, size_t count)
{
	const struct flash_area *scratch = NULL;
	uint8_t chunk[APPLY_CHUNK];
	size_t off = 0U;
	int rc = flash_area_open(PARTITION_ID(scratch_partition), &scratch);
	int erase_rc;

	if (rc != 0) {
		return rzi_err_from_errno(rc);
	}
	if (scratch->fa_size < unit) {
		flash_area_close(scratch);
		return -RZI_ERR_NO_RESOURCE;
	}
	rc = flash_area_erase(scratch, 0, MIN(scratch->fa_size, SCRATCH_PAGE_SIZE));
	if (rc != 0) {
		flash_area_close(scratch);
		return rzi_err_from_errno(rc);
	}
	while (off < unit && rc == 0) {
		size_t n = MIN(unit - off, sizeof(chunk));

		rc = flash_area_read(slot, unit_off + off, chunk, n);
		if (rc != 0) {
			rc = rzi_err_from_errno(rc);
			break;
		}
		if (page_index < off + n && page_index + count > off) {
			size_t from = MAX(page_index, off);
			size_t to = MIN(page_index + count, off + n);

			memcpy(&chunk[from - off], data + (from - page_index), to - from);
		}
		rc = flash_area_write(scratch, off, chunk, n);
		if (rc != 0) {
			rc = rzi_err_from_errno(rc);
			break;
		}
		off += n;
	}
	if (rc == 0) {
		rc = flash_area_erase(slot, unit_off, unit);
		if (rc != 0) {
			rc = rzi_err_from_errno(rc);
		}
	}
	if (rc == 0) {
		rc = copy_flash_area(scratch, 0, slot, unit_off, unit);
	}
	erase_rc = flash_area_erase(scratch, 0, scratch->fa_size);
	flash_area_close(scratch);
	if (rc != 0) {
		return rc;
	}
	return erase_rc == 0 ? 0 : rzi_err_from_errno(erase_rc);
}

static int program_blank_span(uint32_t offset, const uint8_t *data, size_t count, uint32_t unit_off,
			      uint32_t unit, uint32_t align)
{
	uint8_t chunk[APPLY_CHUNK];
	uint32_t start = offset & ~(align - 1U);
	uint32_t end = ((uint32_t)(offset + count) + align - 1U) & ~(align - 1U);

	if (start < unit_off) {
		start = unit_off;
	}
	if (end > unit_off + unit) {
		end = unit_off + unit;
	}
	while (start < end) {
		size_t n = MIN((size_t)(end - start), sizeof(chunk));
		int rc;

		if (align > 1U && n > align) {
			n &= ~(size_t)(align - 1U);
		}
		if (n == 0U) {
			return -RZI_ERR_NO_RESOURCE;
		}
		memset(chunk, 0xFF, n);
		if ((uint32_t)offset < start + n && offset + count > start) {
			size_t from = MAX((size_t)offset, (size_t)start);
			size_t to = MIN(offset + count, (size_t)start + n);

			memcpy(&chunk[from - start], data + (from - offset), to - from);
		}
		rc = flash_area_write(slot, start, chunk, n);
		if (rc != 0) {
			return rzi_err_from_errno(rc);
		}
		start += n;
	}
	return 0;
}
#endif /* PARTITION_EXISTS(scratch_partition) */

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
		 * this chip can erase. image-scratch holds that sector while it is
		 * rewritten; boards without scratch keep it in page_buf.
		 */
#if PARTITION_EXISTS(scratch_partition)
		unit = info.size;
		if (unit > SCRATCH_PAGE_SIZE) {
			if ((info.size % SCRATCH_PAGE_SIZE) != 0U) {
				return -RZI_ERR_NO_RESOURCE;
			}
			unit = SCRATCH_PAGE_SIZE;
		}
#else
		unit = info.size;
		if (unit > sizeof(page_buf)) {
			if ((info.size % sizeof(page_buf)) != 0U) {
				return -RZI_ERR_NO_RESOURCE;
			}
			unit = sizeof(page_buf);
		}
#endif
		page_end = page_off + info.size;
		limit = page_end < slot->fa_size ? page_end : slot->fa_size;
		unit_off = page_off + ((offset - page_off) / unit) * unit;
		if ((uint64_t)unit_off + unit > limit) {
			return -RZI_ERR_INVALID;
		}
		page_index = offset - unit_off;
		count = MIN(unit - page_index, size);
#if !PARTITION_EXISTS(scratch_partition)
		rc = flash_area_read(slot, unit_off, page_buf, unit);
		if (rc != 0) {
			return rzi_err_from_errno(rc);
		}
#endif
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
#if PARTITION_EXISTS(scratch_partition)
			uint8_t probe[APPLY_CHUNK];

			for (uint32_t i = aligned; i < aligned_end && !rewrite;) {
				size_t n = MIN((size_t)(aligned_end - i), sizeof(probe));

				rc = flash_area_read(slot, i, probe, n);
				if (rc != 0) {
					return rzi_err_from_errno(rc);
				}
				for (size_t byte = 0U; byte < n; byte++) {
					if (probe[byte] != 0xFFU) {
						rewrite = true;
						break;
					}
				}
				i += n;
			}
			/* A write block larger than the copy chunk is staged with the sector. */
			if (align > APPLY_CHUNK) {
				rewrite = true;
			}
#else
			for (uint32_t i = aligned; i < aligned_end; i++) {
				if (page_buf[i - unit_off] != 0xFFU) {
					rewrite = true;
					break;
				}
			}
#endif
		}
#if PARTITION_EXISTS(scratch_partition)
		if (rewrite) {
			rc = program_unit_via_scratch(unit_off, unit, page_index, data, count);
		} else {
			rc = program_blank_span(offset, data, count, unit_off, unit, align);
		}
#else
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
#endif
		if (rc != 0) {
#if PARTITION_EXISTS(scratch_partition)
			return rc;
#else
			return rzi_err_from_errno(rc);
#endif
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

static int apply_image(bool claimed);

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
	last_reason = RZI_FUOTA_REASON_NONE;
	set_state_locked(RZI_FUOTA_STATE_INSTALLING);
	notify_complete(0);
	k_mutex_unlock(&lock);
	rc = apply_image(true);
	k_mutex_lock(&lock, K_FOREVER);
	if (rc != 0) {
		LOG_ERR("FUOTA install failed: %d", rc);
		last_error = rc;
		set_state_locked(RZI_FUOTA_STATE_FAILED);
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

struct hdr_payload_source {
	const struct flash_area *area;
};

static int hdr_payload_read(void *ctx, size_t offset, void *buf, size_t len)
{
	struct hdr_payload_source *source = ctx;
	int rc;

	if (offset < FUOTA_HDR_LEN) {
		return -1;
	}
	rc = flash_area_read(source->area, (off_t)(offset - FUOTA_HDR_LEN), buf, len);
	return rc == 0 ? 0 : -1;
}

static int read_current_version(uint8_t version[4])
{
	const struct flash_area *area = NULL;
	uint8_t raw[IMAGE_HDR_SIZE];
	uint32_t magic;
	int rc = flash_area_open(PARTITION_ID(slot0_partition), &area);

	if (rc != 0) {
		return rzi_err_from_errno(rc);
	}
	rc = flash_area_read(area, 0, raw, sizeof(raw));
	flash_area_close(area);
	if (rc != 0) {
		return rzi_err_from_errno(rc);
	}
	magic = sys_get_le32(&raw[0]);
	if (magic != IMAGE_MAGIC) {
		return -RZI_ERR_NOT_FOUND;
	}
	version[0] = raw[20];
	version[1] = raw[21];
	version[2] = raw[22];
	version[3] = raw[24];
	return 0;
}

static enum fuota_hdr_result verify_stored(const struct flash_area *area, size_t pkg_size, bool upgrade)
{
	struct fuota_hdr_expect expect;
	struct hdr_payload_source source;
	struct fuota_hdr header;
	uint8_t raw[FUOTA_HDR_LEN];
	int rc;

	if (area == NULL || pkg_size <= FUOTA_HDR_LEN) {
		return FUOTA_HDR_LENGTH;
	}
	rc = hdr_read(0U, raw, sizeof(raw));
	if (rc != 0) {
		return FUOTA_HDR_IO;
	}
	fuota_hdr_parse(raw, &header);
	if (header.header_length != FUOTA_HDR_LEN || header.header_length >= pkg_size) {
		return FUOTA_HDR_LENGTH;
	}
	memset(&expect, 0, sizeof(expect));
	expect.device_code = local_device_code;
	expect.package_kind = upgrade ? 0U : 1U;
	expect.payload_length = pkg_size - header.header_length;
	source.area = area;
	if (read_current_version(expect.current_version) == 0) {
		expect.have_version = true;
	}
	return fuota_hdr_verify(raw, &expect, hdr_payload_read, &source);
}

static int delta_area_read(void *ctx, size_t offset, void *buf, size_t len)
{
	const struct flash_area *area = ctx;
	int rc = flash_area_read(area, (off_t)offset, buf, len);

	return rc == 0 ? 0 : rzi_err_from_errno(rc);
}

static int delta_slot1_write(void *ctx, const void *data, size_t len)
{
	struct flash_img_context *img = ctx;
	int rc = flash_img_buffered_write(img, data, len, false);

	return rc == 0 ? 0 : rzi_err_from_errno(rc);
}

static int delta_slot1_flush(void *ctx)
{
	struct flash_img_context *img = ctx;
	int rc = flash_img_buffered_write(img, NULL, 0U, true);

	return rc == 0 ? 0 : rzi_err_from_errno(rc);
}

static void erase_area(const struct flash_area *area)
{
	if (area != NULL) {
		ignore_result(flash_area_erase(area, 0, area->fa_size));
	}
}

static void erase_partition(uint8_t area_id)
{
	const struct flash_area *area = NULL;

	if (flash_area_open(area_id, &area) == 0) {
		erase_area(area);
		flash_area_close(area);
	}
}

static int install_delta_pkg(size_t pkg_size)
{
	struct flash_img_context img;
	struct rzi_fuota_install_io io;
	const struct flash_area *slot0 = NULL;
	int rc;

	if (slot == NULL || pkg_size <= FUOTA_HDR_LEN) {
		return -RZI_ERR_NO_DATA;
	}
	erase_partition(PARTITION_ID(slot1_partition));
	rc = flash_area_open(PARTITION_ID(slot0_partition), &slot0);
	if (rc != 0) {
		return rzi_err_from_errno(rc);
	}
	rc = flash_img_init(&img);
	if (rc != 0) {
		flash_area_close(slot0);
		return rzi_err_from_errno(rc);
	}
	memset(&io, 0, sizeof(io));
	io.read_pkg = delta_area_read;
	io.pkg_ctx = (void *)slot;
	io.pkg_size = pkg_size;
	io.read_slot0 = delta_area_read;
	io.slot0_ctx = (void *)slot0;
	io.slot0_size = slot0->fa_size;
	io.write_slot1 = delta_slot1_write;
	io.flush_slot1 = delta_slot1_flush;
	io.slot1_ctx = &img;
	io.slot1_cap = FIXED_PARTITION_SIZE(slot1_partition);
	rc = rzi_fuota_install(FUOTA_DESCRIPTOR_DELTA, FUOTA_HDR_LEN, &io);
	flash_area_close(slot0);
	if (rc != 0 && img.flash_area != NULL) {
		flash_area_close(img.flash_area);
		img.flash_area = NULL;
	}
	return rc;
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
		last_reason = RZI_FUOTA_REASON_FRAG_INCOMPLETE;
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
	set_state_locked(RZI_FUOTA_STATE_VERIFYING);
	if (last_hdr_result != FUOTA_HDR_OK) {
		LOG_WRN("FUOTA header rejected: %d", (int)last_hdr_result);
		last_reason = reason_from_header(last_hdr_result);
		erase_area(slot);
		image_ready = false;
		last_error = -RZI_ERR_BAD_MESSAGE;
		slot_session = false;
		round.finish_wait = false;
		set_state_locked(RZI_FUOTA_STATE_FAILED);
		notify_complete(-RZI_ERR_BAD_MESSAGE);
		schedule_keepalive();
		k_mutex_unlock(&lock);
		return;
	}
	if (!install) {
		slot_session = false;
		round.finish_wait = false;
		k_mutex_unlock(&lock);
		rc = install_delta_pkg(bytes);
		if (rc == 0) {
			rc = rzi_err_from_errno(boot_request_upgrade(BOOT_UPGRADE_TEST));
		}
		k_mutex_lock(&lock, K_FOREVER);
		if (rc != 0) {
			last_error = rc;
			last_reason = RZI_FUOTA_REASON_DELTA_APPLY_FAIL;
			erase_partition(PARTITION_ID(slot1_partition));
			image_ready = false;
			set_state_locked(RZI_FUOTA_STATE_FAILED);
			notify_complete(rc);
			schedule_keepalive();
			k_mutex_unlock(&lock);
			return;
		}
		set_state_locked(RZI_FUOTA_STATE_INSTALLING);
		notify_complete(0);
		k_mutex_unlock(&lock);
		LOG_INF("FUOTA delta installed, rebooting");
		if (ops != NULL && ops->reboot != NULL) {
			(void)ops->reboot();
		}
		return;
	}
	round.finish_wait = true;
	round.finish_deadline_ms = k_uptime_get() + (int64_t)FUOTA_FINISH_WAIT_S * 1000;
	round.hold_class_c_keepalive = class_c;
	/* COMPLETE callbacks run unlocked and may race with the next Setup. */
	set_state_locked(RZI_FUOTA_STATE_INSTALLING);
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
	session_has_header = (descriptor != FUOTA_DESCRIPTOR_HOST);
	status = fuota_area_for_descriptor(descriptor, &area_id, &upgrade);
	if (status != 0U) {
		LOG_WRN("FUOTA descriptor 0x%08x rejected", descriptor);
		return status;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if ((round.finish_wait && round.image_committed) || state == RZI_FUOTA_STATE_INSTALLING) {
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
	hdr_erase();
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
	last_reason = RZI_FUOTA_REASON_NONE;
	k_mutex_unlock(&lock);
	name = upgrade ? "slot1" : "fuota_delta";
	LOG_INF("FUOTA descriptor 0x%08x -> %s, %u bytes", descriptor, name, bytes);
	return 0;
}

static int fuota_store(uint32_t offset, const uint8_t *data, uint32_t size)
{
	int rc;
	uint32_t shift = session_has_header ? FUOTA_HDR_LEN : 0U;

	k_mutex_lock(&lock, K_FOREVER);
	if (!slot_session || slot == NULL || data == NULL ||
	    (uint64_t)offset + size > session_span) {
		write_failed = true;
		round.write_failed = true;
		k_mutex_unlock(&lock);
		return -1;
	}
	if (offset == 0U && state == RZI_FUOTA_STATE_PRECONFIG) {
		set_state_locked(RZI_FUOTA_STATE_RECEIVING);
	}
	if (session_has_header && offset < FUOTA_HDR_LEN) {
		size_t head = MIN(size, FUOTA_HDR_LEN - offset);

		rc = hdr_store(offset, data, head);
		if (rc != 0) {
			write_failed = true;
			round.write_failed = true;
			k_mutex_unlock(&lock);
			return -1;
		}
		offset += head;
		data += head;
		size -= head;
	}
	if (size == 0U) {
		k_mutex_unlock(&lock);
		return 0;
	}
	if ((uint64_t)(offset - shift) + size > slot->fa_size) {
		write_failed = true;
		round.write_failed = true;
		k_mutex_unlock(&lock);
		return -1;
	}
	rc = slot_program(offset - shift, data, size);
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
	if (offset == 0U && state == RZI_FUOTA_STATE_PRECONFIG) {
		set_state_locked(RZI_FUOTA_STATE_RECEIVING);
	}
	if (session_has_header && offset < FUOTA_HDR_LEN) {
		size_t head = MIN(size, FUOTA_HDR_LEN - offset);

		rc = hdr_read(offset, data, head);
		if (rc != 0) {
			k_mutex_unlock(&lock);
			return -1;
		}
		offset += head;
		data += head;
		size -= head;
	}
	if (size > 0U) {
		uint32_t shift = session_has_header ? FUOTA_HDR_LEN : 0U;

		rc = flash_area_read(slot, (off_t)(offset - shift), data, size);
	} else {
		rc = 0;
	}
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
	set_state_locked(RZI_FUOTA_STATE_PRECONFIG);
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
		set_state_locked(RZI_FUOTA_STATE_RECEIVING);
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
		if (state == RZI_FUOTA_STATE_RECEIVING && !image_ready) {
			schedule_keepalive();
			set_state_locked(RZI_FUOTA_STATE_PRECONFIG);
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
			set_state_locked(RZI_FUOTA_STATE_INSTALLING);
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
	status->reason = last_reason;
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
	allowed = state != RZI_FUOTA_STATE_RECEIVING && state != RZI_FUOTA_STATE_INSTALLING;
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

/* The finish path claims APPLYING before its completion callbacks. */
static int apply_image(bool claimed)
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
	if (!claimed && state == RZI_FUOTA_STATE_INSTALLING) {
		k_mutex_unlock(&lock);
		return -RZI_ERR_BUSY;
	}
#if FUOTA_DIRECT_SLOT
	if (!claimed && (round.finish_wait || control_tx_running())) {
		k_mutex_unlock(&lock);
		return -RZI_ERR_BUSY;
	}
#endif
	set_state_locked(RZI_FUOTA_STATE_INSTALLING);
	k_mutex_unlock(&lock);

#ifndef CONFIG_IMG_MANAGER
	ARG_UNUSED(ops);
	k_mutex_lock(&lock, K_FOREVER);
	last_error = -RZI_ERR_NOT_SUPPORTED;
	set_state_locked(RZI_FUOTA_STATE_FAILED);
	k_mutex_unlock(&lock);
	return -RZI_ERR_NOT_SUPPORTED;
#else
	int rc;

#if FUOTA_DIRECT_SLOT
	if (slot != NULL && !target_upgrade) {
		k_mutex_lock(&lock, K_FOREVER);
		last_error = -RZI_ERR_NOT_SUPPORTED;
		set_state_locked(RZI_FUOTA_STATE_FAILED);
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
		set_state_locked(RZI_FUOTA_STATE_FAILED);
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
	return apply_image(false);
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
	if (state == RZI_FUOTA_STATE_INSTALLING || state == RZI_FUOTA_STATE_RECEIVING ||
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
