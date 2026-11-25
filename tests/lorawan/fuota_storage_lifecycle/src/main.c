/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include <rzi/lorawan/fuota.h>
#include <rzi/lorawan/lorawan.h>

#include "backend/lorawan_backend.h"
#include <zephyr/storage/flash_map.h>
#include "smtc_modem_api.h"

K_SEM_DEFINE(fuota_sem, 0, 8);
K_SEM_DEFINE(ready_sem, 0, 1);

struct fuota_stats {
	atomic_t state;
	atomic_t complete;
};

static rzi_lorawan_event_sink_t fake_sink;
static bool session_joined;
static uint8_t fake_image[] = {0x11, 0x22, 0x33, 0x44};

static int fake_init(enum rzi_lorawan_region region, bool join_backoff_bypass,
		     rzi_lorawan_event_sink_t event_sink)
{
	const struct rzi_lorawan_backend_event event = {
		.type = RZI_LORAWAN_BACKEND_READY,
	};

	ARG_UNUSED(region);
	ARG_UNUSED(join_backoff_bypass);
	session_joined = false;
	fake_sink = event_sink;
	fake_sink(&event);
	return 0;
}

static int fake_join(const struct rzi_lorawan_join_config *config)
{
	const struct rzi_lorawan_backend_event event = {
		.type = RZI_LORAWAN_BACKEND_JOINED,
	};

	ARG_UNUSED(config);
	session_joined = true;
	fake_sink(&event);
	return 0;
}

static int fake_leave(void)
{
	session_joined = false;
	return 0;
}

static int fake_stop(void)
{
	session_joined = false;
	return 0;
}

static int fake_deinit(void)
{
	fake_sink = NULL;
	return 0;
}

static int fake_send(uint8_t port, const uint8_t *data, size_t size,
		     enum rzi_lorawan_message_type type)
{
	ARG_UNUSED(port);
	ARG_UNUSED(data);
	ARG_UNUSED(size);
	ARG_UNUSED(type);
	return 0;
}

static enum rzi_lorawan_class observed_class;
static int restore_error;
static int fake_set_class(enum rzi_lorawan_class device_class)
{
	if (restore_error) {
		return restore_error;
	}
	observed_class = device_class;
	return 0;
}

static int fake_is_joined(bool *joined)
{
	*joined = session_joined;
	return 0;
}

static int fake_start_clock_sync(void)
{
	return 0;
}

static int fake_get_image_size(size_t *size)
{
	*size = sizeof(fake_image);
	return 0;
}

static int fake_read_image(uint32_t offset, uint8_t *buffer, size_t size)
{
	if (offset + size > sizeof(fake_image)) {
		return -RZI_ERR_INVALID;
	}
	memcpy(buffer, &fake_image[offset], size);
	return 0;
}

#ifdef FUOTA_TEST_INSTALLER
static atomic_t reboot_calls;
static void reset_observations(void);
#endif

static int fake_reboot(void)
{
#ifdef FUOTA_TEST_INSTALLER
	atomic_inc(&reboot_calls);
#endif
	return -RZI_ERR_IO;
}

static int multicast_stop_error;
static int fake_stop_multicast(uint8_t group)
{
	ARG_UNUSED(group);
	return multicast_stop_error;
}
static const struct rzi_lorawan_fuota_ops fake_fuota_ops = {
	.start_clock_sync = fake_start_clock_sync,
	.stop_multicast = fake_stop_multicast,
	.get_image_size = fake_get_image_size,
	.read_image = fake_read_image,
	.reboot = fake_reboot,
};

static int fake_is_busy(bool *busy)
{
	*busy = false;
	return 0;
}

const struct rzi_lorawan_backend_api rzi_lorawan_backend = {
	.capabilities = RZI_LORAWAN_CAP_CLASS_C | RZI_LORAWAN_CAP_FUOTA,
	.base.init = fake_init,
	.base.stop = fake_stop,
	.base.deinit = fake_deinit,
	.base.join = fake_join,
	.base.leave = fake_leave,
	.base.send = fake_send,
	.base.set_class = fake_set_class,
	.base.is_joined = fake_is_joined,
	.base.is_busy = fake_is_busy,
	.fuota = &fake_fuota_ops,
};

static bool hold_complete_callback;
K_SEM_DEFINE(release_complete_callback, 0, 1);
K_SEM_DEFINE(complete_callback_entered, 0, 1);
static unsigned complete_count;
static void on_state_changed(enum rzi_fuota_state next, void *user_data)
{
	struct fuota_stats *stats = user_data;

	printk("API_CALLBACK state=%d thread=%p user=%p\n", next, k_current_get(), user_data);
	atomic_set(&stats->state, next);
	k_sem_give(&fuota_sem);
	if (hold_complete_callback && next == RZI_FUOTA_STATE_COMPLETE) {
		k_sem_give(&complete_callback_entered);
		(void)k_sem_take(&release_complete_callback, K_SECONDS(1));
	}
}

static void on_complete(int status, void *user_data)
{
	struct fuota_stats *stats = user_data;

	printk("API_CALLBACK complete=%d thread=%p user=%p\n", status, k_current_get(), user_data);
	complete_count++;
	atomic_set(&stats->complete, status);
	k_sem_give(&fuota_sem);
}

static void on_lorawan_event(const struct rzi_lorawan_event *event, void *user_data)
{
	ARG_UNUSED(user_data);
	if (event->type == RZI_LORAWAN_EVENT_READY) {
		k_sem_give(&ready_sem);
	}
}

static void wait_for(unsigned int count)
{
	for (unsigned int i = 0; i < count; ++i) {
		zassert_ok(k_sem_take(&fuota_sem, K_SECONDS(1)));
	}
}

static const struct smtc_modem_fuota_hooks *hook;
static int erase_error, write_error;
static unsigned erase_calls;
static bool answer_pending;
void smtc_modem_fuota_set_hooks(const struct smtc_modem_fuota_hooks *value)
{
	hook = value;
}
bool lorawan_fragmentation_answer_pending(uint8_t id)
{
	ARG_UNUSED(id);
	return answer_pending;
}
bool lorawan_multicast_answer_pending(uint8_t id)
{
	ARG_UNUSED(id);
	return false;
}
int test_flash_erase(const struct flash_area *area, off_t off, size_t size)
{
	erase_calls++;
	return erase_error ? erase_error : flash_area_erase(area, off, size);
}
int test_flash_write(const struct flash_area *area, off_t off, const void *data, size_t size)
{
	return write_error ? write_error : flash_area_write(area, off, data, size);
}
static struct fuota_stats stats;
static void emit(enum rzi_lorawan_backend_fuota_kind kind, bool good, uint32_t generation,
		 uint8_t control)
{
	struct rzi_lorawan_backend_event event = {.type = RZI_LORAWAN_BACKEND_FUOTA};
	event.fuota.kind = kind;
	event.fuota.successful = good;
	event.fuota.generation = generation;
	event.fuota.control = control;
	event.fuota.original_class = RZI_LORAWAN_CLASS_C;
	event.fuota.multicast_group = 0;
	event.fuota.mc_group_bit_mask = 1;
	fake_sink(&event);
}
static uint32_t setup_round(uint32_t descriptor)
{
	zassert_not_null(hook);
	zassert_equal(hook->session_setup(descriptor, 128, 32, 0), 0);
	struct rzi_fuota_round r;
	zassert_ok(rzi_fuota_get_round(&r));
	emit(RZI_LORAWAN_BACKEND_FUOTA_SETUP_ACCEPTED, true, r.generation, 0);
	emit(RZI_LORAWAN_BACKEND_FUOTA_CLASS_CAPTURED, true, r.generation, 0);
	return r.generation;
}
static void start_round(void)
{
	emit(RZI_LORAWAN_BACKEND_FUOTA_SESSION_STARTED, true, 0, 0);
	wait_for(1);
}
static void before(void *fixture)
{
	ARG_UNUSED(fixture);
#ifdef FUOTA_TEST_INSTALLER
	reset_observations();
#endif
	k_sem_reset(&fuota_sem);
	k_sem_reset(&ready_sem);
	erase_error = write_error = restore_error = multicast_stop_error = 0;
	answer_pending = false;
	complete_count = 0;
	hold_complete_callback = false;
	observed_class = RZI_LORAWAN_CLASS_A;
	k_sem_reset(&release_complete_callback);
	k_sem_reset(&complete_callback_entered);
	const struct rzi_fuota_callbacks cb = {
		.state_changed = on_state_changed, .complete = on_complete, .user_data = &stats};
	const struct rzi_lorawan_callbacks lcb = {.on_event = on_lorawan_event};
	zassert_ok(rzi_fuota_register_callbacks(&cb));
	zassert_ok(rzi_lorawan_register_callbacks(&lcb));
	zassert_ok(rzi_lorawan_init(RZI_LORAWAN_REGION_EU_868));
	zassert_ok(k_sem_take(&ready_sem, K_SECONDS(1)));
	zassert_ok(rzi_lorawan_join(1, 0, RZI_LORAWAN_JOIN_INTERVAL_DEFAULT, 0));
	wait_for(1);
}
static void after(void *fixture)
{
	ARG_UNUSED(fixture);
	answer_pending = false;
	erase_error = write_error = restore_error = 0;
	struct rzi_fuota_status s;
	int rc = rzi_fuota_get_status(&s);
	if (!rc && s.state == RZI_FUOTA_STATE_TRANSFERRING) {
		emit(RZI_LORAWAN_BACKEND_FUOTA_SESSION_ENDED, true, 0, 0);
		k_sleep(K_MSEC(20));
	}
	rc = rzi_lorawan_stop();
	if (rc == -RZI_ERR_BUSY) {
		k_sleep(K_SECONDS(31));
		rc = rzi_lorawan_stop();
	}
	if (rc == -RZI_ERR_BUSY) {
		/* Independent test cleanup after a reproduced stuck finish_wait; no product change.
		 */
		struct rzi_lorawan_backend_event event = {.type = RZI_LORAWAN_BACKEND_JOIN_FAILED};
		fake_sink(&event);
		k_sleep(K_MSEC(20));
		rc = rzi_lorawan_stop();
	}
	printk("SLOT_CLEANUP stop=%d\n", rc);
	zassert_ok(rc);
	rc = rzi_lorawan_deinit();
	printk("SLOT_CLEANUP deinit=%d\n", rc);
	zassert_ok(rc);
}

ZTEST(slot, test_delta_lifetime_and_readback)
{
	setup_round(0x304b4152);
	start_round();
	uint8_t input[32], output[32];
	for (unsigned i = 0; i < 32; i++) {
		input[i] = i;
	}
	for (unsigned off = 0; off < 4096; off += 32) {
		zassert_ok(hook->store(off, input, sizeof(input)));
	}
	emit(RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE, true, 0, 0);
	k_sleep(K_MSEC(20));
	struct rzi_fuota_status s;
	zassert_ok(rzi_fuota_get_status(&s));
	zassert_false(s.image_ready);
	zassert_equal(s.state, RZI_FUOTA_STATE_TRANSFERRING);
	emit(RZI_LORAWAN_BACKEND_FUOTA_SESSION_ENDED, true, 0, 0);
	wait_for(2);
	zassert_equal(observed_class, RZI_LORAWAN_CLASS_C);
	zassert_equal(complete_count, 1);
	for (unsigned off = 0; off < 4096; off += 32) {
		zassert_ok(rzi_fuota_read_image(off, output, 32));
		zassert_mem_equal(input, output, 32);
	}
}

ZTEST(slot, test_delete_and_stale_control)
{
	uint32_t old = setup_round(0x304b4152);
	hook->session_delete(old);
	struct rzi_fuota_round r;
	zassert_equal(rzi_fuota_get_round(&r), -RZI_ERR_NOT_FOUND);
	uint32_t next = setup_round(0x304b4152);
	zassert_equal(next, old + 1);
	emit(RZI_LORAWAN_BACKEND_FUOTA_CONTROL_DONE, true, old, 2);
	k_sleep(K_MSEC(20));
	zassert_ok(rzi_fuota_get_round(&r));
	zassert_equal(r.generation, next);
}
static uint32_t full_complete(void)
{
	uint32_t g = setup_round(0);
	start_round();
	emit(RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE, true, 0, 0);
	emit(RZI_LORAWAN_BACKEND_FUOTA_SESSION_ENDED, true, 0, 0);
	wait_for(1);
	/* Observe the normal post-notification path separately from the held-callback test. */
	k_sleep(K_MSEC(20));
	return g;
}
ZTEST(slot, test_full_candidate_protection)
{
	uint32_t g = full_complete();
	hook->session_delete(g);
	struct rzi_fuota_round r;
	zassert_ok(rzi_fuota_get_round(&r));
	zassert_equal(hook->session_setup(0, 128, 32, 0), 2);
	zassert_equal(rzi_lorawan_stop(), -RZI_ERR_BUSY);
}

ZTEST(slot, test_pending_answer_holds_finish)
{
	full_complete();
	answer_pending = true;
	k_sleep(K_SECONDS(31));
	zassert_equal(complete_count, 0);
	answer_pending = false;
	k_sleep(K_SECONDS(2));
	zassert_equal(complete_count, 1);
}

ZTEST(slot, test_setup_during_complete_callback)
{
	static uint8_t candidate[4096], actual[4096];
	for (unsigned i = 0; i < sizeof(candidate); i++) {
		candidate[i] = (uint8_t)(37 * i + 11);
	}
	hold_complete_callback = true;
	uint32_t generation = setup_round(0);
	zassert_ok(hook->store(0, candidate, sizeof(candidate)));
	start_round();
	emit(RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE, true, 0, 0);
	emit(RZI_LORAWAN_BACKEND_FUOTA_SESSION_ENDED, true, 0, 0);
	wait_for(1);
	/* The dispatcher is held in COMPLETE while the protocol-side caller tries Setup. */
	zassert_ok(k_sem_take(&complete_callback_entered, K_SECONDS(1)));
	unsigned erases_before = erase_calls;
	int rc = hook->session_setup(0, 128, 32, 0);
	unsigned erases_after = erase_calls;
	struct rzi_fuota_round round_after;
	int round_rc = rzi_fuota_get_round(&round_after);
	const struct flash_area *area;
	int open_rc = flash_area_open(PARTITION_ID(slot1_partition), &area);
	int read_rc = open_rc ? open_rc : flash_area_read(area, 0, actual, sizeof(actual));
	if (!open_rc) {
		flash_area_close(area);
	}
	unsigned mismatches = 0;
	if (!read_rc) {
		for (unsigned i = 0; i < sizeof(actual); i++) {
			mismatches += actual[i] != candidate[i];
		}
	}
	hold_complete_callback = false;
	k_sem_give(&release_complete_callback);
	k_sleep(K_MSEC(20));
	printk("CANDIDATE_RACE status=%d generation_before=%u generation_after=%u erases_before=%u "
	       "erases_after=%u compared=%u mismatches=%u\n",
	       rc, generation, round_rc ? 0 : round_after.generation, erases_before, erases_after,
	       (unsigned)sizeof(actual), mismatches);
	zassert_equal(rc, 2, "Setup must reject during COMPLETE notification");
	zassert_ok(round_rc);
	zassert_equal(round_after.generation, generation);
	zassert_equal(erases_after, erases_before, "Rejected Setup must not erase the candidate");
	zassert_ok(read_rc);
	zassert_mem_equal(candidate, actual, sizeof(actual));
}
ZTEST(slot, test_decode_failure_not_flash_error)
{
	setup_round(0x304b4152);
	start_round();
	emit(RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE, false, 0, 0);
	emit(RZI_LORAWAN_BACKEND_FUOTA_SESSION_ENDED, true, 0, 0);
	wait_for(2);
	struct rzi_fuota_round r;
	zassert_ok(rzi_fuota_get_round(&r));
	printk("SLOT_DECODE_FAILURE write_failed=%d storage_errno=%d\n", r.write_failed,
	       r.storage_errno);
	zassert_false(r.write_failed,
		      "Decoder failure without a Flash error must not imply Flash failure");
	zassert_equal(r.storage_errno, 0);
	zassert_equal(complete_count, 1);
	struct rzi_fuota_status status;
	zassert_ok(rzi_fuota_get_status(&status));
	zassert_false(status.image_ready);
	zassert_equal(status.state, RZI_FUOTA_STATE_FAILED);
}
ZTEST(slot, test_first_storage_error_sticky)
{
	setup_round(0x304b4152);
	uint8_t data[32] = {0};
	write_error = -EIO;
	zassert_equal(hook->store(0, data, 32), -1);
	struct rzi_fuota_round r;
	zassert_ok(rzi_fuota_get_round(&r));
	int first = r.storage_errno;
	zassert_true(r.write_failed);
	zassert_not_equal(first, 0);
	write_error = -ENOSPC;
	zassert_equal(hook->store(32, data, 32), -1);
	zassert_ok(rzi_fuota_get_round(&r));
	zassert_equal(r.storage_errno, first);
}
ZTEST(slot, test_bounds_no_fabricated_errno)
{
	setup_round(0x304b4152);
	uint8_t data[32] = {0};
	zassert_equal(hook->store(4090, data, 32), -1);
	struct rzi_fuota_round r;
	zassert_ok(rzi_fuota_get_round(&r));
	zassert_true(r.write_failed);
	zassert_equal(r.storage_errno, 0);
}

ZTEST(slot, test_decode_failure_preserves_storage_error)
{
	setup_round(0x304b4152);
	start_round();
	uint8_t data[32] = {0};
	write_error = -EIO;
	zassert_equal(hook->store(0, data, sizeof(data)), -1);
	write_error = -ENOSPC;
	zassert_equal(hook->store(32, data, sizeof(data)), -1);
	emit(RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE, false, 0, 0);
	emit(RZI_LORAWAN_BACKEND_FUOTA_SESSION_ENDED, true, 0, 0);
	wait_for(2);
	struct rzi_fuota_round r;
	zassert_ok(rzi_fuota_get_round(&r));
	zassert_true(r.write_failed);
	zassert_equal(r.storage_errno, -RZI_ERR_IO);
	struct rzi_fuota_status status;
	zassert_ok(rzi_fuota_get_status(&status));
	zassert_false(status.image_ready);
	zassert_equal(complete_count, 1);
}

ZTEST(slot, test_missing_timeout_not_flash_error)
{
	setup_round(0x304b4152);
	start_round();
	emit(RZI_LORAWAN_BACKEND_FUOTA_SESSION_ENDED, true, 0, 0);
	wait_for(2);
	struct rzi_fuota_round r;
	zassert_ok(rzi_fuota_get_round(&r));
	zassert_false(r.write_failed);
	zassert_equal(r.storage_errno, 0);
	zassert_equal(atomic_get(&stats.complete), -RZI_ERR_IO);
}
#ifdef FUOTA_TEST_INSTALLER
/* Public apply gates with the real Service and instrumented Boot calls. */

static atomic_t boot_calls;
static int boot_result;
static bool hold_first_boot;
K_SEM_DEFINE(boot_entered, 0, 1);
K_SEM_DEFINE(boot_release, 0, 1);

int boot_request_upgrade(int permanent)
{
	unsigned call = atomic_inc(&boot_calls) + 1;
	zassert_equal(permanent, 0, "The existing call must request TEST");
	printk("BOOT_DOUBLE call=%u answer_pending=%d thread=%p\n", call,
	       answer_pending, k_current_get());
	if (hold_first_boot && call == 1) {
		k_sem_give(&boot_entered);
		zassert_ok(k_sem_take(&boot_release, K_SECONDS(3)));
	}
	return boot_result;
}

struct flash_img_context;
int flash_img_init(struct flash_img_context *ctx)
{
	ARG_UNUSED(ctx);
	zassert_unreachable("Direct-slot cases must not copy through flash_img");
	return -ENOTSUP;
}
int flash_img_buffered_write(struct flash_img_context *ctx, const uint8_t *data,
			     size_t len, bool flush)
{
	ARG_UNUSED(ctx); ARG_UNUSED(data); ARG_UNUSED(len); ARG_UNUSED(flush);
	zassert_unreachable("Direct-slot cases must not copy through flash_img");
	return -ENOTSUP;
}

static void reset_observations(void)
{
	atomic_clear(&boot_calls);
	atomic_clear(&reboot_calls);
	boot_result = -EIO;
	hold_first_boot = false;
	k_sem_reset(&boot_entered);
	k_sem_reset(&boot_release);
}

static void wait_state(enum rzi_fuota_state wanted)
{
	for (unsigned i = 0; i < 1000; ++i) {
		struct rzi_fuota_status s;
		zassert_ok(rzi_fuota_get_status(&s));
		if (s.state == wanted) {
			return;
		}
		k_sleep(K_MSEC(1));
	}
	zassert_unreachable("Timed out waiting for Service state %d", wanted);
}

static uint8_t candidate[4096], readback[4096];
static uint32_t complete_candidate(void)
{
	uint32_t generation = setup_round(0);
	for (unsigned i = 0; i < sizeof(candidate); ++i) {
		candidate[i] = (uint8_t)(37 * i + 11);
	}
	zassert_ok(hook->store(0, candidate, sizeof(candidate)));
	start_round();
	emit(RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE, true, 0, 0);
	emit(RZI_LORAWAN_BACKEND_FUOTA_SESSION_ENDED, true, 0, 0);
	wait_state(RZI_FUOTA_STATE_COMPLETE);
	/* Let dispatcher finish its normal return, without calling apply in a callback. */
	k_sleep(K_MSEC(20));
	return generation;
}

ZTEST(slot, test_incomplete_and_early_reconstruction)
{
	reset_observations();
	setup_round(0);
	start_round();
	wait_state(RZI_FUOTA_STATE_TRANSFERRING);
	int incomplete = rzi_fuota_apply();
	emit(RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE, true, 0, 0);
	k_sleep(K_MSEC(20));
	int early = rzi_fuota_apply();
	zassert_equal(incomplete, -RZI_ERR_NOT_FOUND);
	zassert_equal(early, -RZI_ERR_NOT_FOUND);
	zassert_equal(atomic_get(&boot_calls), 0);
	printk("APPLY_GATE early incomplete=%d reconstructed_before_end=%d boot_calls=0\n",
	       incomplete, early);
}

ZTEST(slot, test_auto_wait_guards_and_failed_test_retention)
{
	reset_observations();
	answer_pending = true;
	uint32_t generation = complete_candidate();
	unsigned erases_before = erase_calls;
	hook->session_delete(generation);
	int setup_rc = hook->session_setup(0, 128, 32, 0);
	struct rzi_fuota_round r;
	zassert_ok(rzi_fuota_get_round(&r));
	zassert_equal(setup_rc, 2);
	zassert_equal(r.generation, generation);
	zassert_equal(erase_calls, erases_before);
	emit(RZI_LORAWAN_BACKEND_FUOTA_CONTROL_DONE, true, generation - 1, 1);
	k_sleep(K_MSEC(20));
	zassert_equal(atomic_get(&boot_calls), 0);
	emit(RZI_LORAWAN_BACKEND_FUOTA_CONTROL_DONE, true, generation, 1);
	/* Reach the real 30-second finish deadline while the answer remains pending. */
	k_sleep(K_SECONDS(31));
	zassert_equal(atomic_get(&boot_calls), 0, "Pending answer must hold automatic TEST");
	answer_pending = false;
	for (unsigned i = 0; i < 2000 && atomic_get(&boot_calls) == 0; ++i) {
		k_sleep(K_MSEC(1));
	}
	zassert_equal(atomic_get(&boot_calls), 1);
	wait_state(RZI_FUOTA_STATE_COMPLETE);
	zassert_ok(rzi_fuota_read_image(0, readback, sizeof(readback)));
	zassert_mem_equal(candidate, readback, sizeof(candidate));
	int retry_rc = rzi_fuota_apply();
	zassert_equal(retry_rc, -RZI_ERR_IO);
	zassert_equal(atomic_get(&boot_calls), 2);
	zassert_equal(erase_calls, erases_before);
	zassert_equal(atomic_get(&reboot_calls), 0);
	printk("APPLY_GATE automatic setup=%d unchanged_generation=1 unchanged_erases=1 "
	       "pending_boot_calls=0 released_boot_calls=1 failed_TEST_readback=4096 "
	       "retry_boot_calls=2 reboot_calls=0\n", setup_rc);
}

ZTEST(slot, test_application_thread_apply_during_finish_wait)
{
	reset_observations();
	answer_pending = true;
	complete_candidate();
	zassert_equal(atomic_get(&boot_calls), 0);
	boot_result = 0;
	int rc = rzi_fuota_apply();
	/* The public entry must not bypass the automatic finish gate. */
	zassert_equal(atomic_get(&boot_calls), 0);
	zassert_equal(atomic_get(&reboot_calls), 0);
	zassert_equal(rc, -RZI_ERR_BUSY);
	printk("APPLY_GATE pending=1 boot_calls=0 reboot_calls=0 "
	       "caller=test_application_thread callback=0 apply_rc=%d\n", rc);
}

ZTEST(slot, test_concurrent_application_apply)
{
	reset_observations();
	answer_pending = false;
	complete_candidate();
	hold_first_boot = true;
	/* The unmodified automatic path owns the first TEST after its normal deadline. */
	zassert_ok(k_sem_take(&boot_entered, K_SECONDS(35)));
	struct rzi_fuota_status s;
	zassert_ok(rzi_fuota_get_status(&s));
	zassert_equal(s.state, RZI_FUOTA_STATE_APPLYING);
	int second = rzi_fuota_apply();
	k_sem_give(&boot_release);
	k_sleep(K_MSEC(20));
	wait_state(RZI_FUOTA_STATE_COMPLETE);
	zassert_equal(atomic_get(&boot_calls), 1);
	zassert_equal(second, -RZI_ERR_BUSY);
	printk("APPLY_GATE concurrent=1 boot_calls=1 second_rc=%d "
	       "first_caller=automatic second_caller=application callback=0 "
	       "answer_pending=0 boot_first_call_deliberately_held=1\n", second);
}



#endif

ZTEST_SUITE(slot, NULL, NULL, before, after, NULL);
