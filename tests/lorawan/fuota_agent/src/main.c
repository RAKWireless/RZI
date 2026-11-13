/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>
#include <rzi/lorawan/fuota.h>
#include <rzi/lorawan/lorawan.h>
#include "backend/lorawan_backend.h"
#include "callback_log.h"
#include "fuota_event_log.h"
#include "fuota_receive_demo.h"

static rzi_lorawan_event_sink_t sink;
static atomic_t closes, restores, uplinks, releases, sends, downlinks, completions;
static atomic_t gate, pause_callback, periodic_uplinks;
K_SEM_DEFINE(stack_ready, 0, 1);
static bool reserved;
static bool network_joined;
static int64_t periodic_requested_at, restored_at;
K_SEM_DEFINE(paused, 0, 1);
K_SEM_DEFINE(resume_worker, 0, 1);
static void on_agent_event(const struct rzi_fuota_event *event, void *user)
{
	ARG_UNUSED(user);
	if ((event->type == RZI_FUOTA_EVENT_PHASE && event->snapshot.phase == RZI_FUOTA_PRECONFIG &&
	     atomic_cas(&pause_callback, 1, 0)) ||
	    (event->type == RZI_FUOTA_EVENT_TIME && atomic_cas(&pause_callback, 2, 0))) {
		k_sem_give(&paused);
		zassert_ok(k_sem_take(&resume_worker, K_SECONDS(5)));
	}
}
static int close_error, restore_error, uplink_error;
static uint32_t generation;
static uint8_t raw[] = {0x46, 0x55, 0x4c, 0x4c};
static enum rzi_lorawan_class initial_class;
static struct rzi_fuota_session_info session;

static void network(enum rzi_lorawan_backend_event_type type)
{
	const struct rzi_lorawan_backend_event e = {.type = type};
	sink(&e);
}
static int start(enum rzi_lorawan_region r, bool bypass, rzi_lorawan_event_sink_t s)
{
	ARG_UNUSED(r);
	ARG_UNUSED(bypass);
	sink = s;
	network(RZI_LORAWAN_BACKEND_READY);
	return 0;
}
static int join(const struct rzi_lorawan_join_config *c)
{
	ARG_UNUSED(c);
	network_joined = true;
	network(RZI_LORAWAN_BACKEND_JOINED);
	return 0;
}
static int leave(void)
{
	network_joined = false;
	return 0;
}
static int send(uint8_t p, const uint8_t *d, size_t n, enum rzi_lorawan_message_type t)
{
	ARG_UNUSED(p);
	ARG_UNUSED(d);
	ARG_UNUSED(n);
	ARG_UNUSED(t);
	atomic_inc(&sends);
	return 0;
}
static int set_class(enum rzi_lorawan_class c)
{
	initial_class = c;
	return 0;
}
static int get_class(enum rzi_lorawan_class *c)
{
	*c = initial_class;
	return 0;
}
static int joined(bool *v)
{
	*v = network_joined;
	return 0;
}
static int busy(bool *v)
{
	*v = false;
	return 0;
}
static int clock_sync(void)
{
	return 0;
}
static int (*test_image_reader)(uint32_t offset, uint8_t *buffer, size_t size);

static int image_size(size_t *n)
{
	*n = sizeof(raw);
	return 0;
}
static int read_image(uint32_t o, uint8_t *d, size_t n)
{
	if (test_image_reader) {
		return test_image_reader(o, d, n);
	}
	if (o > sizeof(raw) || n > sizeof(raw) - o) {
		return -RZI_ERR_INVALID;
	}
	memcpy(d, raw + o, n);
	return 0;
}
static int get_session(struct rzi_fuota_session_info *out)
{
	*out = session;
	return reserved ? 0 : -RZI_ERR_NOT_FOUND;
}
static struct rzi_fuota_rx_stats rx_stats;

static int close_mc(uint32_t g, struct rzi_fuota_rx_stats *stats)
{
	if (!close_error && stats) {
		*stats = rx_stats;
	}
	zassert_equal(g, generation);
	atomic_inc(&closes);
	return close_error;
}
static int restore(uint32_t g)
{
	zassert_equal(g, generation);
	atomic_inc(&restores);
	if (!restore_error) {
		atomic_clear(&gate);
		initial_class = session.original_class;
		restored_at = k_uptime_get();
	}
	return restore_error;
}
static int cleanup(uint32_t g)
{
	if (!g) {
		atomic_inc(&periodic_uplinks);
		periodic_requested_at = k_uptime_get();
		return uplink_error;
	}
	zassert_equal(g, generation);
	atomic_inc(&uplinks);
	return uplink_error;
}
static int release(uint32_t g)
{
	zassert_equal(g, generation);
	atomic_inc(&releases);
	reserved = false;
	return 0;
}
static bool blocked(void)
{
	return atomic_get(&gate) != 0;
}
static const struct rzi_lorawan_fuota_ops fuota_ops = {
	.start_clock_sync = clock_sync,
	.get_image_size = image_size,
	.read_image = read_image,
	.get_session_info = get_session,
	.close_multicast = close_mc,
	.restore_settings = restore,
	.cleanup_uplink = cleanup,
	.release_data = release,
	.business_blocked = blocked,
};
const struct rzi_lorawan_backend_api rzi_lorawan_backend = {
	.capabilities = RZI_LORAWAN_CAP_CLASS_C | RZI_LORAWAN_CAP_FUOTA,
	.base = {.init = start,
		 .join = join,
		 .leave = leave,
		 .send = send,
		 .set_class = set_class,
		 .get_class = get_class,
		 .is_joined = joined,
		 .is_busy = busy},
	.fuota = &fuota_ops,
};
static void on_public(const struct rzi_lorawan_event *e, void *u)
{
	ARG_UNUSED(u);
	if (e->type == RZI_LORAWAN_EVENT_READY) {
		k_sem_give(&stack_ready);
	}
	if (e->type == RZI_LORAWAN_EVENT_DOWNDATA) {
		atomic_inc(&downlinks);
	}
}
static void on_complete(int rc, void *u)
{
	ARG_UNUSED(rc);
	ARG_UNUSED(u);
	atomic_inc(&completions);
}
static struct rzi_fuota_snapshot snapshot(void)
{
	struct rzi_fuota_snapshot s;
	zassert_ok(rzi_fuota_get_snapshot(&s));
	return s;
}
static void wait_phase(enum rzi_fuota_phase phase)
{
	for (int i = 0; i < 800 && snapshot().phase != phase; ++i) {
		k_msleep(5);
	}
	zassert_equal(snapshot().phase, phase, "phase got %d expected %d", snapshot().phase, phase);
}
static void notice(enum rzi_lorawan_backend_fuota_kind kind, uint32_t g, bool ok, uint8_t control)
{
	struct rzi_lorawan_backend_event e = {
		.type = RZI_LORAWAN_BACKEND_FUOTA,
		.fuota = {.kind = kind,
			  .generation = g,
			  .session = session,
			  .successful = ok,
			  .image_size = sizeof(raw),
			  .control = control},
	};
	sink(&e);
}
static void setup_session(enum rzi_lorawan_class original)
{
	reserved = true;
	session = (struct rzi_fuota_session_info){
		.generation = ++generation,
		.original_class = original,
		.valid = RZI_FUOTA_SESSION_FRAGMENT | RZI_FUOTA_SESSION_WINDOW |
			 RZI_FUOTA_SESSION_GROUP | RZI_FUOTA_SESSION_SETTINGS |
			 RZI_FUOTA_SESSION_DEADLINE,
		.fragment_count = 1,
		.fragment_size = 4,
		.timeout_s = 1,
		.window_end_ms = k_uptime_get() + 600,
	};
	notice(RZI_LORAWAN_BACKEND_FUOTA_PRECONFIG, generation, true, 0);
	wait_phase(RZI_FUOTA_PRECONFIG);
}
static void receiving(void)
{
	initial_class = RZI_LORAWAN_CLASS_C;
	atomic_set(&gate, 1);
	notice(RZI_LORAWAN_BACKEND_FUOTA_SESSION_STARTED, generation, true, 0);
	wait_phase(RZI_FUOTA_RECEIVING);
}
static void wait_periodic_uplink(void)
{
	for (int i = 0; i < 1000 && !atomic_get(&periodic_uplinks); ++i) {
		k_msleep(5);
	}
	zassert_true(atomic_get(&periodic_uplinks) > 0);
	zassert_equal(atomic_get(&uplinks), 0, "No independent cleanup uplink");
}
static void finish_control(uint8_t control)
{
	notice(RZI_LORAWAN_BACKEND_FUOTA_CONTROL_DONE, generation, true, control);
	wait_phase(RZI_FUOTA_READY);
	zassert_false(snapshot().exit.cleanup_uplink_done);
	zassert_equal(atomic_get(&uplinks), 0);
}
static void verify_gen_app_key_sources(void)
{
	uint8_t app[16], gen[16] = {0}, actual[16], eui[8] = {0};
	struct rzi_lorawan_join_config config;
	memset(app, 0x31, sizeof(app));
	zassert_ok(rzi_lorawan_set_dev_eui(eui, sizeof(eui)));
	zassert_ok(rzi_lorawan_set_app_eui(eui, sizeof(eui)));
	zassert_ok(rzi_lorawan_set_app_key(app, sizeof(app)));
	zassert_ok(rzi_lorawan_set_activation(RZI_LORAWAN_ACTIVATION_OTAA));
	zassert_equal(rzi_lorawan_get_gen_app_key(actual, sizeof(actual)), -RZI_ERR_NO_DATA);
	zassert_ok(rzi_lorawan_credentials_build_config(&config));
	zassert_mem_equal(config.otaa.network_key, app, sizeof(app));
	zassert_mem_equal(config.otaa.application_key, app, sizeof(app));
	/* Explicit zero is a configured independent key, not the unset sentinel. */
	zassert_ok(rzi_lorawan_set_gen_app_key(gen, sizeof(gen)));
	zassert_ok(rzi_lorawan_credentials_build_config(&config));
	zassert_mem_equal(config.otaa.application_key, gen, sizeof(gen));
	zassert_mem_equal(config.otaa.network_key, app, sizeof(app));
	memset(gen, 0x72, sizeof(gen));
	zassert_ok(rzi_lorawan_set_gen_app_key(gen, sizeof(gen)));
	zassert_equal(rzi_lorawan_set_gen_app_key(app, 15), -RZI_ERR_INVALID);
	zassert_equal(rzi_lorawan_set_gen_app_key(NULL, 16), -RZI_ERR_INVALID);
	zassert_ok(rzi_lorawan_credentials_build_config(&config));
	zassert_mem_equal(config.otaa.application_key, gen, sizeof(gen));
	zassert_mem_equal(config.otaa.network_key, app, sizeof(app));
	zassert_ok(rzi_lorawan_get_app_key(actual, sizeof(actual)));
	zassert_mem_equal(actual, app, sizeof(app));
}
static const struct rzi_fuota_callbacks fixture_callbacks = {
	.complete = on_complete,
	.agent_event = on_agent_event,
};

/* End the observation interval after all earlier backend events were dispatched. */
static void check_callbacks(const char *scenario, const char *sequence)
{
	notice(RZI_LORAWAN_BACKEND_FUOTA_CLOCK_SYNCED, 0, true, 0);
	callback_log_expect(scenario, sequence);
}

#define START_SEQUENCE   "phase.PRECONFIG phase.RECEIVING state.TRANSFERRING started "
#define SUCCESS_SEQUENCE "state.COMPLETE data_ready complete.OK phase.RECOVERING "
#define CLOSE_SEQUENCE   "closed ended restored "
#define TIMEOUT_SEQUENCE "phase.RECOVERING " CLOSE_SEQUENCE "complete.TIMEOUT phase.FINALIZING "

static void *suite_setup(void)
{
	static const struct rzi_lorawan_callbacks cb = {.on_event = on_public};
	verify_gen_app_key_sources();
	zassert_ok(rzi_lorawan_register_callbacks(&cb));
	zassert_ok(rzi_fuota_register_callbacks(&fixture_callbacks));
	zassert_ok(rzi_lorawan_init(RZI_LORAWAN_REGION_EU_868));
	zassert_ok(k_sem_take(&stack_ready, K_SECONDS(2)));
	zassert_ok(rzi_lorawan_join(1, 0, RZI_LORAWAN_JOIN_INTERVAL_DEFAULT, 0));
	wait_phase(RZI_FUOTA_READY);
	return NULL;
}
static void before(void *data)
{
	ARG_UNUSED(data);
	zassert_ok(rzi_fuota_register_callbacks(&fixture_callbacks));
	test_image_reader = NULL;
	close_error = restore_error = uplink_error = 0;
	rx_stats = (struct rzi_fuota_rx_stats){0};
	struct rzi_fuota_snapshot s = snapshot();
	if (s.phase != RZI_FUOTA_READY || s.session.generation) {
		network(RZI_LORAWAN_BACKEND_JOINED);
		k_msleep(40);
	}
	atomic_clear(&gate);
	atomic_clear(&closes);
	atomic_clear(&restores);
	atomic_clear(&uplinks);
	atomic_clear(&releases);
	atomic_clear(&completions);
	atomic_clear(&periodic_uplinks);
	periodic_requested_at = restored_at = 0;
}
ZTEST(rzi_fuota_agent, test_early_complete_retention_and_repeat)
{
	callback_log_start(&fixture_callbacks, true);
	setup_session(RZI_LORAWAN_CLASS_C);
	receiving();
	notice(RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE, generation, true, 0);
	wait_phase(RZI_FUOTA_FINALIZING);
	struct rzi_fuota_snapshot s = snapshot();
	zassert_true(s.data.ready && s.data.length_valid);
	zassert_equal(s.data.length, sizeof(raw));
	struct rzi_fuota_status public_status;
	zassert_ok(rzi_fuota_set_expected_size(1024));
	zassert_ok(rzi_fuota_get_status(&public_status));
	zassert_true(public_status.image_ready);
	zassert_equal(public_status.image_size, sizeof(raw));
	zassert_equal(rzi_fuota_apply(), -RZI_ERR_BUSY);
	zassert_true(s.exit.multicast_closed && s.exit.settings_restored);
	zassert_equal(atomic_get(&closes), 1);
	zassert_equal(atomic_get(&restores), 1);
	zassert_equal(rzi_fuota_release_data(generation), -RZI_ERR_BUSY);
	notice(RZI_LORAWAN_BACKEND_FUOTA_SESSION_ENDED, generation, true, 0);
	notice(RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE, generation, true, 0);
	finish_control(3);
	zassert_equal(rzi_fuota_apply(), -RZI_ERR_NOT_SUPPORTED);
	uint8_t actual[4];
	zassert_ok(rzi_fuota_read_image(0, actual, sizeof(actual)));
	zassert_mem_equal(actual, raw, sizeof(raw));
	zassert_false(snapshot().exit.lns_confirmed);
	notice(RZI_LORAWAN_BACKEND_FUOTA_SESSION_ENDED, generation, true, 0);
	k_msleep(30);
	zassert_equal(snapshot().phase, RZI_FUOTA_READY);
	zassert_equal(atomic_get(&closes), 1);
	zassert_equal(atomic_get(&restores), 1);
	zassert_equal(atomic_get(&completions), 1);
	zassert_equal(rzi_fuota_read_image(SIZE_MAX, actual, 1), -RZI_ERR_INVALID);
	zassert_equal(rzi_fuota_release_data(generation + 1), -RZI_ERR_NOT_FOUND);
	zassert_ok(rzi_fuota_release_data(generation));
	zassert_equal(rzi_fuota_read_image(0, actual, 1), -RZI_ERR_NOT_FOUND);
	check_callbacks("closed_reregister_and_release", START_SEQUENCE SUCCESS_SEQUENCE
			"closed ended.replacement restored phase.FINALIZING "
			"control.status+delete phase.READY finished ^state.APPLYING "
			"^state.COMPLETE ^state.READY");
	uint32_t old = generation;
	setup_session(RZI_LORAWAN_CLASS_A);
	notice(RZI_LORAWAN_BACKEND_FUOTA_SESSION_ENDED, old, true, 0);
	notice(RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE, old, true, 0);
	k_msleep(30);
	zassert_equal(snapshot().phase, RZI_FUOTA_PRECONFIG);
	zassert_false(snapshot().data.ready);
}
/* A 65-byte candidate exercises two full reads and a one-byte tail. */
static size_t demo_next_offset;
static bool demo_fail_second_read;

static int demo_read_image(uint32_t offset, uint8_t *buffer, size_t size)
{
	zassert_equal(offset, demo_next_offset);
	zassert_true(offset < 65);
	zassert_equal(size, MIN(32U, 65U - offset));
	if (offset == 32 && demo_fail_second_read) {
		return -RZI_ERR_IO;
	}
	memset(buffer, 0xa5, size);
	demo_next_offset += size;
	return 0;
}

ZTEST(rzi_fuota_agent, test_demo_releases_only_after_last_read)
{
	setup_session(RZI_LORAWAN_CLASS_C);
	receiving();
	const struct rzi_lorawan_backend_event done = {
		.type = RZI_LORAWAN_BACKEND_FUOTA,
		.fuota = {.kind = RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE,
			  .generation = generation, .session = session,
			  .successful = true, .image_size = 65},
	};
	sink(&done);
	wait_phase(RZI_FUOTA_FINALIZING);
	zassert_equal(fuota_receive_demo_consume(generation), -RZI_ERR_BUSY);
	finish_control(1);
	test_image_reader = demo_read_image;
	demo_next_offset = 0;
	demo_fail_second_read = true;
	zassert_equal(fuota_receive_demo_consume(generation + 1), -RZI_ERR_NOT_FOUND);
	zassert_equal(fuota_receive_demo_consume(generation), -RZI_ERR_IO);
	zassert_equal(demo_next_offset, 32);
	zassert_true(snapshot().data.ready);
	zassert_equal(atomic_get(&releases), 0);

	demo_next_offset = 0;
	demo_fail_second_read = false;
	zassert_ok(fuota_receive_demo_consume(generation));
	zassert_equal(demo_next_offset, 65);
	zassert_false(snapshot().data.ready);
	zassert_equal(fuota_receive_demo_consume(generation), -RZI_ERR_NOT_FOUND);
	zassert_equal(atomic_get(&releases), 1);
}

ZTEST(rzi_fuota_agent, test_business_two_way_gate)
{
	static const uint8_t byte = 0x42;
	struct rzi_lorawan_backend_event dl = {.type = RZI_LORAWAN_BACKEND_DOWNLINK,
					       .downlink = {.port = 1, .size = 1}};
	setup_session(RZI_LORAWAN_CLASS_A);
	zassert_ok(rzi_lorawan_send(1, &byte, 1, RZI_LORAWAN_MSG_UNCONFIRMED));
	int prior = atomic_get(&downlinks);
	sink(&dl);
	k_msleep(30);
	zassert_equal(atomic_get(&downlinks), prior + 1);
	receiving();
	zassert_equal(rzi_lorawan_send(1, &byte, 1, RZI_LORAWAN_MSG_UNCONFIRMED), -RZI_ERR_BUSY);
	prior = atomic_get(&downlinks);
	sink(&dl);
	k_msleep(30);
	zassert_equal(atomic_get(&downlinks), prior);
	notice(RZI_LORAWAN_BACKEND_FUOTA_SESSION_ENDED, generation, true, 0);
	wait_phase(RZI_FUOTA_FINALIZING);
	zassert_ok(rzi_lorawan_send(1, &byte, 1, RZI_LORAWAN_MSG_UNCONFIRMED));
	sink(&dl);
	k_msleep(30);
	zassert_equal(atomic_get(&downlinks), prior + 1);
	finish_control(2);
}
ZTEST(rzi_fuota_agent, test_repeated_setup_before_window_start)
{
	setup_session(RZI_LORAWAN_CLASS_A);
	notice(RZI_LORAWAN_BACKEND_FUOTA_PRECONFIG, generation, true, 0);
	receiving();
	struct rzi_fuota_snapshot s = snapshot();

	zassert_equal(s.session.generation, generation);
	zassert_true(s.exit.reception_started);
	zassert_equal(s.exit.error, 0);
}

ZTEST(rzi_fuota_agent, test_failed_window_launch_without_fragment_setup)
{
	setup_session(RZI_LORAWAN_CLASS_A);
	session.valid &= ~RZI_FUOTA_SESSION_FRAGMENT;
	notice(RZI_LORAWAN_BACKEND_FUOTA_SESSION_STARTED, generation, false, 0);
	wait_phase(RZI_FUOTA_FINALIZING);
	zassert_equal(snapshot().exit.error, -RZI_ERR_IO);
	finish_control(2);
}

ZTEST(rzi_fuota_agent, test_window_start_without_fragment_setup)
{
	setup_session(RZI_LORAWAN_CLASS_A);
	session.valid &= ~RZI_FUOTA_SESSION_FRAGMENT;
	notice(RZI_LORAWAN_BACKEND_FUOTA_SESSION_STARTED, generation, true, 0);
	wait_phase(RZI_FUOTA_FINALIZING);
	struct rzi_fuota_snapshot s = snapshot();

	zassert_equal(s.exit.error, -RZI_ERR_NO_DATA);
	zassert_false(s.exit.reception_started);
	finish_control(2);
}

ZTEST(rzi_fuota_agent, test_timeout_and_cleanup_hold)
{
	callback_log_start(&fixture_callbacks, false);
	setup_session(RZI_LORAWAN_CLASS_A);
	wait_phase(RZI_FUOTA_FINALIZING); /* setup deadline, never entered multicast */
	zassert_equal(snapshot().exit.reason, RZI_FUOTA_EXIT_TIMEOUT);
	zassert_false(snapshot().data.ready);
	wait_phase(RZI_FUOTA_READY);
	zassert_equal(snapshot().exit.cleanup_error, -RZI_ERR_TIMEOUT);
	zassert_false(snapshot().exit.lns_confirmed);
	zassert_equal(atomic_get(&releases), 1);
	check_callbacks("preconfig_timeout",
			"phase.PRECONFIG " TIMEOUT_SEQUENCE "phase.READY finished");
}
ZTEST(rzi_fuota_agent, test_restore_failure_bounded)
{
	callback_log_start(&fixture_callbacks, false);
	setup_session(RZI_LORAWAN_CLASS_C);
	receiving();
	restore_error = -RZI_ERR_IO;
	notice(RZI_LORAWAN_BACKEND_FUOTA_SESSION_ENDED, generation, true, 0);
	k_msleep(250);
	zassert_equal(snapshot().phase, RZI_FUOTA_RECOVERING);
	zassert_false(snapshot().exit.settings_restored);
	zassert_equal(atomic_get(&closes), 1);
	zassert_equal(atomic_get(&restores), CONFIG_RZI_LORAWAN_FUOTA_STEP_ATTEMPTS);
	zassert_equal(atomic_get(&uplinks), 0);
	zassert_equal(rzi_fuota_release_data(generation), -RZI_ERR_BUSY);
	check_callbacks("restore_failure", START_SEQUENCE "phase.RECOVERING closed ended error");
}
ZTEST(rzi_fuota_agent, test_no_install_and_time_source)
{
	zassert_equal(rzi_fuota_apply(), -RZI_ERR_NOT_FOUND);
	struct rzi_lorawan_backend_event e = {
		.type = RZI_LORAWAN_BACKEND_FUOTA,
		.fuota = {.kind = RZI_LORAWAN_BACKEND_FUOTA_CLOCK_SYNCED,
			  .successful = true,
			  .time_source = RZI_FUOTA_TIME_MAC}};
	sink(&e);
	k_msleep(30);
	zassert_equal(snapshot().time_source, RZI_FUOTA_TIME_MAC);
	e.fuota.time_source = RZI_FUOTA_TIME_ALCSYNC;
	sink(&e);
	k_msleep(30);
	zassert_equal(snapshot().time_source, RZI_FUOTA_TIME_ALCSYNC);
	zassert_true(snapshot().time_valid);
}
ZTEST(rzi_fuota_agent, test_close_failure_and_stale_controls)
{
	callback_log_start(&fixture_callbacks, false);
	setup_session(RZI_LORAWAN_CLASS_A);
	receiving();
	close_error = -RZI_ERR_IO;
	notice(RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE, generation, true, 0);
	k_msleep(250);
	zassert_false(snapshot().exit.multicast_closed);
	zassert_equal(atomic_get(&closes), CONFIG_RZI_LORAWAN_FUOTA_STEP_ATTEMPTS);
	zassert_equal(atomic_get(&restores), 0);
	notice(RZI_LORAWAN_BACKEND_FUOTA_CONTROL_DONE, generation - 1, true, 3);
	k_msleep(30);
	zassert_false(snapshot().exit.status_answered || snapshot().exit.delete_answered);
	check_callbacks("close_failure", START_SEQUENCE SUCCESS_SEQUENCE "error");
}
ZTEST(rzi_fuota_agent, test_periodic_failure_retains_context_until_hold_deadline)
{
	setup_session(RZI_LORAWAN_CLASS_A);
	receiving();
	uplink_error = -RZI_ERR_IO;
	notice(RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE, generation, true, 0);
	wait_phase(RZI_FUOTA_READY);
	zassert_equal(atomic_get(&uplinks), 0);
	zassert_true(atomic_get(&periodic_uplinks) > 0);
	zassert_equal(snapshot().exit.cleanup_error, -RZI_ERR_TIMEOUT);
	zassert_false(snapshot().exit.cleanup_uplink_done || snapshot().exit.lns_confirmed);
	zassert_true(snapshot().data.ready);
	zassert_ok(rzi_fuota_release_data(generation));
}
ZTEST(rzi_fuota_agent, test_lost_events_never_report_complete)
{
	atomic_set(&pause_callback, 1);
	reserved = true;
	session = (struct rzi_fuota_session_info){.generation = ++generation,
						  .valid = RZI_FUOTA_SESSION_SETTINGS,
						  .original_class = RZI_LORAWAN_CLASS_A};
	notice(RZI_LORAWAN_BACKEND_FUOTA_PRECONFIG, generation, true, 0);
	zassert_ok(k_sem_take(&paused, K_SECONDS(1)));
	for (int i = 0; i < CONFIG_RZI_LORAWAN_EVENT_QUEUE_SIZE + 8; ++i) {
		notice(RZI_LORAWAN_BACKEND_FUOTA_CLOCK_SYNCED, 0, true, 0);
		k_msleep(2);
	}
	notice(RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE, generation, true, 0);
	k_msleep(10);
	k_sem_give(&resume_worker);
	wait_phase(RZI_FUOTA_FINALIZING);
	zassert_false(snapshot().data.ready);
	zassert_equal(snapshot().exit.error, -RZI_ERR_OVERFLOW);
	finish_control(2);
}
ZTEST(rzi_fuota_agent, test_offline_invalidates_data_and_can_rejoin)
{
	setup_session(RZI_LORAWAN_CLASS_C);
	receiving();
	network(RZI_LORAWAN_BACKEND_JOIN_FAILED);
	wait_phase(RZI_FUOTA_OFFLINE);
	zassert_false(snapshot().data.ready);
	network(RZI_LORAWAN_BACKEND_JOINED);
	wait_phase(RZI_FUOTA_READY);
	uint32_t old = generation;
	setup_session(RZI_LORAWAN_CLASS_A);
	notice(RZI_LORAWAN_BACKEND_FUOTA_SESSION_STARTED, old, true, 0);
	k_msleep(30);
	zassert_equal(snapshot().phase, RZI_FUOTA_PRECONFIG);
	const uint8_t byte = 0;
	zassert_ok(rzi_lorawan_send(1, &byte, 1, RZI_LORAWAN_MSG_UNCONFIRMED));
}
ZTEST(rzi_fuota_agent, test_unicast_completion_cancels_future_window)
{
	setup_session(RZI_LORAWAN_CLASS_A);
	notice(RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE, generation, true, 0);
	wait_phase(RZI_FUOTA_FINALIZING);
	zassert_true(snapshot().data.ready);
	zassert_equal(atomic_get(&closes), 1);
	zassert_equal(atomic_get(&restores), 1);
	finish_control(2);
	zassert_ok(rzi_fuota_release_data(generation));
}
ZTEST(rzi_fuota_agent, test_idle_overflow_does_not_recover_a_released_generation)
{
	reserved = false;
	atomic_set(&pause_callback, 2);
	notice(RZI_LORAWAN_BACKEND_FUOTA_CLOCK_SYNCED, 0, true, 0);
	zassert_ok(k_sem_take(&paused, K_SECONDS(1)));
	for (int i = 0; i < CONFIG_RZI_LORAWAN_EVENT_QUEUE_SIZE + 8; ++i) {
		notice(RZI_LORAWAN_BACKEND_FUOTA_CLOCK_SYNCED, 0, true, 0);
		k_msleep(2);
	}
	k_sem_give(&resume_worker);
	k_msleep(50);
	zassert_equal(snapshot().phase, RZI_FUOTA_READY);
	zassert_equal(snapshot().exit.error, -RZI_ERR_OVERFLOW);
	zassert_equal(atomic_get(&closes), 0);
}
ZTEST(rzi_fuota_agent, test_keepalive_continues_without_cleanup_txdone)
{
	setup_session(RZI_LORAWAN_CLASS_A);
	receiving();
	notice(RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE, generation, true, 0);
	wait_phase(RZI_FUOTA_FINALIZING);
	wait_periodic_uplink();
	int prior = atomic_get(&periodic_uplinks);
	for (int i = 0; i < 400 && atomic_get(&periodic_uplinks) == prior; ++i) {
		k_msleep(5);
	}
	zassert_true(atomic_get(&periodic_uplinks) > prior);
	zassert_equal(snapshot().phase, RZI_FUOTA_FINALIZING);
	zassert_false(snapshot().exit.status_answered);
	finish_control(3);
	zassert_ok(rzi_fuota_release_data(generation));
}
/* These tests exercise the user-visible ordering, for original A and C.
 * Millisecond deadlines keep tests fast without changing the production clock.
 */
static void prompt_exit(bool success)
{
	const enum rzi_lorawan_class classes[] = {RZI_LORAWAN_CLASS_A, RZI_LORAWAN_CLASS_C};
	for (size_t i = 0; i < ARRAY_SIZE(classes); ++i) {
		before(NULL);
		callback_log_start(&fixture_callbacks, false);
		setup_session(classes[i]);
		session.window_end_ms = k_uptime_get() + 1500;
		notice(RZI_LORAWAN_BACKEND_FUOTA_PRECONFIG, generation, true, 0);
		receiving();
		int64_t end = snapshot().session.window_end_ms;
		notice(RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE, generation, success, 0);
		wait_phase(RZI_FUOTA_FINALIZING);
		zassert_true(snapshot().exit.settings_restored);
		zassert_equal(initial_class, classes[i]);
		zassert_equal(atomic_get(&closes), 1);
		zassert_equal(atomic_get(&restores), 1);
		zassert_equal(snapshot().data.ready, success);
		zassert_equal(snapshot().exit.reason,
			      success ? RZI_FUOTA_EXIT_COMPLETE : RZI_FUOTA_EXIT_ERROR);
		int prior = atomic_get(&periodic_uplinks);
		/* Neither class sends an overdue keepalive on restore. */
		k_msleep(200);
		zassert_equal(atomic_get(&uplinks), 0);
		zassert_equal(atomic_get(&periodic_uplinks), prior);
		zassert_ok(rzi_lorawan_send(1, raw, 1, RZI_LORAWAN_MSG_UNCONFIRMED));
		if (classes[i] == RZI_LORAWAN_CLASS_C) {
			k_msleep(1100);
			zassert_equal(atomic_get(&periodic_uplinks), prior);
			int64_t answer = k_uptime_get();
			finish_control(1);
			wait_periodic_uplink();
			zassert_true(periodic_requested_at >= answer + 1000);
			zassert_true(periodic_requested_at < answer + 1200);
		} else {
			wait_periodic_uplink();
			zassert_true(periodic_requested_at < end);
			zassert_true(periodic_requested_at - restored_at >= 1000);
			zassert_true(periodic_requested_at - restored_at < 1200);
			finish_control(1);
		}
		zassert_equal(atomic_get(&uplinks), 0);
		if (success) {
			zassert_ok(rzi_fuota_release_data(generation));
			check_callbacks(classes[i] == RZI_LORAWAN_CLASS_C ? "complete_c"
									  : "complete_a",
					START_SEQUENCE SUCCESS_SEQUENCE CLOSE_SEQUENCE
					"phase.FINALIZING control.status phase.READY finished "
					"^state.READY");
		} else {
			check_callbacks(
				classes[i] == RZI_LORAWAN_CLASS_C ? "failure_c" : "failure_a",
				START_SEQUENCE "state.FAILED phase.RECOVERING " CLOSE_SEQUENCE
					       "complete.IO phase.FINALIZING control.status "
					       "phase.READY state.READY finished");
		}
	}
}
ZTEST(rzi_fuota_agent, test_success_restores_then_resumes_periodic_by_class)
{
	prompt_exit(true);
}
ZTEST(rzi_fuota_agent, test_failure_restores_then_resumes_periodic_by_class)
{
	prompt_exit(false);
}
ZTEST(rzi_fuota_agent, test_receive_timeout_restores_a_c_then_waits_periodic_timer)
{
	const enum rzi_lorawan_class classes[] = {RZI_LORAWAN_CLASS_A, RZI_LORAWAN_CLASS_C};
	for (size_t i = 0; i < ARRAY_SIZE(classes); ++i) {
		before(NULL);
		callback_log_start(&fixture_callbacks, false);
		setup_session(classes[i]);
		receiving();
		int64_t end = snapshot().session.window_end_ms;
		wait_phase(RZI_FUOTA_FINALIZING);
		int prior = atomic_get(&periodic_uplinks);
		k_msleep(200);
		zassert_equal(atomic_get(&periodic_uplinks), prior);
		zassert_equal(atomic_get(&uplinks), 0);
		int64_t resume = restored_at;
		if (classes[i] == RZI_LORAWAN_CLASS_C) {
			notice(RZI_LORAWAN_BACKEND_FUOTA_CONTROL_DONE, generation - 1, true, 1);
			notice(RZI_LORAWAN_BACKEND_FUOTA_CLEANUP_UPLINK_DONE, generation, true, 0);
			k_msleep(1000);
			zassert_equal(atomic_get(&periodic_uplinks), prior);
			resume = k_uptime_get();
			notice(RZI_LORAWAN_BACKEND_FUOTA_CONTROL_DONE, generation, true, 1);
		}
		wait_periodic_uplink();
		zassert_equal(snapshot().exit.reason, RZI_FUOTA_EXIT_TIMEOUT);
		zassert_false(snapshot().data.ready);
		zassert_equal(initial_class, classes[i]);
		zassert_equal(atomic_get(&closes), 1);
		zassert_equal(atomic_get(&restores), 1);
		zassert_true(periodic_requested_at >= end);
		zassert_true(periodic_requested_at - resume >= 1000);
		zassert_true(periodic_requested_at - resume < 1200);
		/* A status-only server still gets bounded cleanup at the deadline. */
		finish_control(1);
		zassert_true(snapshot().exit.status_answered);
		zassert_false(snapshot().exit.delete_answered);
		zassert_equal(snapshot().exit.error, -RZI_ERR_TIMEOUT);
		zassert_equal(snapshot().exit.cleanup_error, 0);
		zassert_equal(atomic_get(&releases), 1);
		if (classes[i] == RZI_LORAWAN_CLASS_C) {
			check_callbacks(
				"deadline_timeout_c", START_SEQUENCE TIMEOUT_SEQUENCE
				"control.status control.status phase.READY state.READY finished");
		} else {
			check_callbacks("deadline_timeout_a", START_SEQUENCE TIMEOUT_SEQUENCE
					"control.status phase.READY state.READY finished");
		}
	}
}
ZTEST(rzi_fuota_agent, test_status_hold_starts_at_window_end_not_early_restore)
{
	setup_session(RZI_LORAWAN_CLASS_A);
	session.window_end_ms = k_uptime_get() + 3500;
	notice(RZI_LORAWAN_BACKEND_FUOTA_PRECONFIG, generation, true, 0);
	receiving();
	notice(RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE, generation, true, 0);
	wait_phase(RZI_FUOTA_FINALIZING);
	k_msleep(CONFIG_RZI_LORAWAN_FUOTA_STATUS_HOLD_S * 1000 + 100);
	zassert_equal(snapshot().phase, RZI_FUOTA_FINALIZING);
	zassert_equal(atomic_get(&uplinks), 0);
	finish_control(1);
	zassert_ok(rzi_fuota_release_data(generation));
}
ZTEST(rzi_fuota_agent, test_class_c_no_query_resumes_one_period_after_hold)
{
	callback_log_start(&fixture_callbacks, false);
	setup_session(RZI_LORAWAN_CLASS_C);
	session.window_end_ms = k_uptime_get() + 1500;
	notice(RZI_LORAWAN_BACKEND_FUOTA_PRECONFIG, generation, true, 0);
	receiving();
	int64_t expiry = snapshot().session.window_end_ms +
			 CONFIG_RZI_LORAWAN_FUOTA_STATUS_HOLD_S * 1000LL;
	notice(RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE, generation, true, 0);
	wait_phase(RZI_FUOTA_FINALIZING);
	k_msleep(CONFIG_RZI_LORAWAN_FUOTA_STATUS_HOLD_S * 1000);
	zassert_equal(snapshot().phase, RZI_FUOTA_FINALIZING);
	wait_phase(RZI_FUOTA_READY);
	zassert_equal(atomic_get(&periodic_uplinks), 0);
	wait_periodic_uplink();
	zassert_true(periodic_requested_at >= expiry + 1000);
	zassert_true(periodic_requested_at < expiry + 1200);
	zassert_ok(rzi_fuota_release_data(generation));
	check_callbacks("class_c_no_query", START_SEQUENCE SUCCESS_SEQUENCE CLOSE_SEQUENCE
			"phase.FINALIZING phase.READY finished ^state.READY");
}

ZTEST(rzi_fuota_agent, test_offline_cancels_deferred_keepalive)
{
	setup_session(RZI_LORAWAN_CLASS_C);
	receiving();
	notice(RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE, generation, true, 0);
	wait_phase(RZI_FUOTA_FINALIZING);
	network(RZI_LORAWAN_BACKEND_JOIN_FAILED);
	wait_phase(RZI_FUOTA_OFFLINE);
	k_msleep(750);
	zassert_equal(atomic_get(&uplinks), 0);
	zassert_equal(snapshot().phase, RZI_FUOTA_OFFLINE);
}
ZTEST(rzi_fuota_agent, test_timeout_status_then_delete_before_first_keepalive)
{
	callback_log_start(&fixture_callbacks, false);
	setup_session(RZI_LORAWAN_CLASS_C);
	receiving();
	notice(RZI_LORAWAN_BACKEND_FUOTA_SESSION_ENDED, generation, true, 0);
	wait_phase(RZI_FUOTA_FINALIZING);
	zassert_equal(atomic_get(&periodic_uplinks), 0);
	notice(RZI_LORAWAN_BACKEND_FUOTA_CONTROL_DONE, generation, true, 1);
	for (int i = 0; i < 100 && !snapshot().exit.status_answered; ++i) {
		k_msleep(5);
	}
	zassert_true(snapshot().exit.status_answered);
	zassert_equal(snapshot().phase, RZI_FUOTA_FINALIZING);
	zassert_equal(atomic_get(&releases), 0);
	zassert_false(snapshot().data.ready);
	finish_control(2);
	zassert_true(snapshot().exit.delete_answered);
	zassert_equal(snapshot().exit.cleanup_error, 0);
	zassert_equal(atomic_get(&periodic_uplinks), 0);
	zassert_equal(atomic_get(&releases), 1);
	check_callbacks("timeout_status_then_delete", START_SEQUENCE TIMEOUT_SEQUENCE
			"control.status control.status+delete phase.READY state.READY finished");
}
ZTEST(rzi_fuota_agent, test_legacy_cleanup_event_does_not_finish_or_unlock_timer)
{
	setup_session(RZI_LORAWAN_CLASS_A);
	receiving();
	notice(RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE, generation, true, 0);
	wait_phase(RZI_FUOTA_FINALIZING);
	notice(RZI_LORAWAN_BACKEND_FUOTA_CLEANUP_UPLINK_DONE, generation, true, 0);
	k_msleep(50);
	zassert_equal(snapshot().phase, RZI_FUOTA_FINALIZING);
	zassert_false(snapshot().exit.cleanup_uplink_done);
	zassert_equal(atomic_get(&periodic_uplinks), 0);
	finish_control(1);
	zassert_ok(rzi_fuota_release_data(generation));
}
ZTEST(rzi_fuota_agent, test_delete_preconfigured_fragment_preserves_group)
{
	callback_log_start(&fixture_callbacks, false);
	setup_session(RZI_LORAWAN_CLASS_C);
	session.valid &= ~RZI_FUOTA_SESSION_FRAGMENT;
	notice(RZI_LORAWAN_BACKEND_FUOTA_FRAGMENT_DELETED, generation, true, 0);
	for (int i = 0; i < 100 && (snapshot().session.valid & RZI_FUOTA_SESSION_FRAGMENT); ++i) {
		k_msleep(5);
	}
	zassert_equal(snapshot().session.valid, session.valid);
	zassert_equal(snapshot().phase, RZI_FUOTA_PRECONFIG);
	zassert_equal(atomic_get(&closes), 0);

	check_callbacks("preconfig_delete_keep_group", "phase.PRECONFIG");

	/* Replace only the TS004 parameters under the existing TS005 window. */
	session.valid |= RZI_FUOTA_SESSION_FRAGMENT;
	notice(RZI_LORAWAN_BACKEND_FUOTA_PRECONFIG, generation, true, 0);
	receiving();
	zassert_equal(snapshot().session.generation, generation);
}

ZTEST(rzi_fuota_agent, test_delete_fragment_only_preconfig_allows_new_generation)
{
	callback_log_start(&fixture_callbacks, false);
	reserved = true;
	session = (struct rzi_fuota_session_info){
		.generation = ++generation,
		.valid = RZI_FUOTA_SESSION_SETTINGS | RZI_FUOTA_SESSION_FRAGMENT,
		.original_class = RZI_LORAWAN_CLASS_A,
	};
	notice(RZI_LORAWAN_BACKEND_FUOTA_PRECONFIG, generation, true, 0);
	wait_phase(RZI_FUOTA_PRECONFIG);
	session.valid = RZI_FUOTA_SESSION_SETTINGS;
	reserved = false;
	notice(RZI_LORAWAN_BACKEND_FUOTA_FRAGMENT_DELETED, generation, true, 0);
	wait_phase(RZI_FUOTA_READY);
	zassert_equal(snapshot().exit.reason, RZI_FUOTA_EXIT_DELETED);
	zassert_false(snapshot().exit.delete_answered);
	zassert_equal(atomic_get(&releases), 0);
	check_callbacks("preconfig_delete_last", "phase.PRECONFIG phase.READY finished");
	uint32_t old = generation;
	setup_session(RZI_LORAWAN_CLASS_C);
	notice(RZI_LORAWAN_BACKEND_FUOTA_FRAGMENT_DELETED, old, true, 0);
	notice(RZI_LORAWAN_BACKEND_FUOTA_CONTROL_DONE, old, true, 2);
	receiving();
	zassert_equal(snapshot().session.generation, generation);
	zassert_false(snapshot().exit.delete_answered);
}

K_MSGQ_DEFINE(observed_reports, sizeof(struct uplink_log_event), 4, 1);
K_SEM_DEFINE(report_barrier, 0, 1);

static void collect_report(const struct rzi_fuota_event *event, void *user)
{
	struct uplink_log_event record;

	ARG_UNUSED(user);
	if (fuota_event_log_record(event, &record)) {
		zassert_ok(k_msgq_put(&observed_reports, &record, K_NO_WAIT));
	}
	if (event->type == RZI_FUOTA_EVENT_TIME) {
		k_sem_give(&report_barrier);
	}
}

ZTEST(rzi_fuota_agent, test_event_reports_from_agent_notifications)
{
	const struct rzi_fuota_callbacks callbacks = {.agent_event = collect_report};
	struct uplink_log_event record;
	const uint8_t started[] = {0x2c, 1, 32, 2};
	const uint8_t incomplete[] = {2, 1, 0x2c, 1};

	zassert_ok(rzi_fuota_register_callbacks(&callbacks));
	setup_session(RZI_LORAWAN_CLASS_C);
	session.fragment_count = 300;
	session.fragment_size = 32;
	session.fragment_index = 2;
	receiving();
	rx_stats = (struct rzi_fuota_rx_stats){.valid = true, .received = 258};
	notice(RZI_LORAWAN_BACKEND_FUOTA_SESSION_ENDED, generation, true, 0);
	wait_phase(RZI_FUOTA_FINALIZING);
	finish_control(2);
	notice(RZI_LORAWAN_BACKEND_FUOTA_CLOCK_SYNCED, 0, true, 0);
	zassert_ok(k_sem_take(&report_barrier, K_SECONDS(1)));
	zassert_equal(k_msgq_num_used_get(&observed_reports), 2);
	zassert_ok(k_msgq_get(&observed_reports, &record, K_NO_WAIT));
	zassert_equal(record.code, UPLINK_LOG_EVENT_FUOTA_STARTED);
	zassert_mem_equal(record.data, started, 4);
	zassert_ok(k_msgq_get(&observed_reports, &record, K_NO_WAIT));
	zassert_equal(record.code, UPLINK_LOG_EVENT_FUOTA_FRAG_INCOMPLETE);
	zassert_mem_equal(record.data, incomplete, 4);
	zassert_ok(rzi_fuota_register_callbacks(&fixture_callbacks));
}

ZTEST(rzi_fuota_agent, test_window_timeout_preserves_pending_write_failure)
{
	setup_session(RZI_LORAWAN_CLASS_C);
	receiving();
	rx_stats = (struct rzi_fuota_rx_stats){
		.valid = true,
		.received = 1,
		.storage_errno = -ENOSPC,
		.error = -RZI_ERR_IO,
		.write_failed = true,
	};
	/* The decoder has already failed, but its completion has not reached the worker. */
	notice(RZI_LORAWAN_BACKEND_FUOTA_SESSION_ENDED, generation, true, 0);
	wait_phase(RZI_FUOTA_FINALIZING);
	struct rzi_fuota_snapshot s = snapshot();
	zassert_equal(s.exit.reason, RZI_FUOTA_EXIT_ERROR);
	zassert_equal(s.exit.error, rx_stats.error);
	zassert_equal(s.exit.fragments.storage_errno, -ENOSPC);
	zassert_true(s.exit.reception_started && s.exit.fragments.write_failed);
	zassert_equal(s.exit.fragments.received, 1);
	finish_control(1);
	zassert_equal(snapshot().exit.fragments.storage_errno, -ENOSPC);
}

ZTEST_SUITE(rzi_fuota_agent, NULL, suite_setup, before, NULL, NULL);
