/* SPDX-License-Identifier: Apache-2.0 */
/* Executes the real USP v1 package parsers and FEC; modem/radio/flash are test doubles. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "fuota_event_log.h"
#include "uplink_log_port.h"
#include "modem_core.h"
#include "modem_event_utilities.h"
#include "modem_supervisor_light.h"
#include "modem_tx_protocol_manager.h"
#include "lorawan_api.h"
#include "lorawan_cid_request_management.h"
#include "lorawan_class_b_management.h"
#include "lorawan_alcsync.h"
#include "lorawan_fragmentation_package.h"
#include "lorawan_remote_multicast_setup_package.h"
#include "lorawan_fuota_observer.h"
#include "smtc_modem_hal.h"

static uint8_t storage[12800], tx[255], tx_size, tx_port;
static uint32_t tx_target, clock_ms, notice_count[16];
static smodem_task last_task;
static tx_protocol_manager_tx_type_t tx_request_type;
static bool supported = true, occupied, write_fail;
static int storage_error;
static unsigned int key_calls, launches, stops;
static uint32_t launched_frequency;
static unsigned int clock_events;
static stask_manager tasks;
static uint8_t frag_id, mc_id;
static uint8_t (*frag_rx)(lr1_stack_mac_down_data_t *), (*mc_rx)(lr1_stack_mac_down_data_t *);
static void (*frag_launch)(void *), (*frag_update)(void *), (*mc_launch)(void *),
	(*mc_update)(void *);
static void *frag_context, *mc_context;
static unsigned int checks;
#define CHECK(condition)                                                                           \
	do {                                                                                       \
		++checks;                                                                          \
		if (!(condition)) {                                                                \
			fprintf(stderr, "line %d: %s\n", __LINE__, #condition);                    \
			abort();                                                                   \
		}                                                                                  \
	} while (0)

static struct lbm_fuota_info accepted_setup;
static uint8_t report_frame[UPLINK_LOG_FRAME_SIZE];
static bool report_sent;

static uint32_t report_time(void *ctx)
{
	(void)ctx;
	return 0;
}

static int capture_report(void *ctx, const uint8_t *frame, size_t size, bool confirmed)
{
	(void)ctx;
	CHECK(confirmed && size == sizeof(report_frame));
	memcpy(report_frame, frame, size);
	report_sent = true;
	return 0;
}

void smtc_modem_hal_print_trace(const char *fmt, ...)
{
	(void)fmt;
}
_Noreturn void smtc_modem_hal_on_panic(uint8_t *f, uint32_t line, const char *fmt, ...)
{
	(void)fmt;
	fprintf(stderr, "panic %s:%u\n", f, line);
	abort();
}
uint32_t smtc_modem_hal_get_time_in_s(void)
{
	return clock_ms / 1000;
}
uint32_t smtc_modem_hal_get_time_in_ms(void)
{
	return clock_ms;
}
uint32_t smtc_modem_hal_get_random_nb_in_range(uint32_t a, uint32_t b)
{
	(void)a;
	return b;
}
void smtc_modem_hal_context_store(modem_context_type_t t, uint32_t o, const uint8_t *b, uint32_t n)
{
	(void)t;
	CHECK(o + n <= sizeof(storage));
	memcpy(storage + o, b, n);
}
void smtc_modem_hal_context_restore(modem_context_type_t t, uint32_t o, uint8_t *b, uint32_t n)
{
	(void)t;
	CHECK(o + n <= sizeof(storage));
	memcpy(b, storage + o, n);
}
void increment_asynchronous_msgnumber(uint8_t e, uint8_t s, uint8_t stack)
{
	if (e == SMTC_MODEM_EVENT_ALCSYNC_TIME) {
		clock_events++;
	}
	(void)s;
	(void)stack;
}
task_valid_t modem_supervisor_add_task(smodem_task *t)
{
	last_task = *t;
	return TASK_VALID;
}
task_valid_t modem_supervisor_remove_task(uint16_t t)
{
	(void)t;
	return TASK_VALID;
}
stask_manager *modem_supervisor_get_task(void)
{
	return &tasks;
}
uint32_t lorawan_api_next_max_payload_length_get(uint8_t s)
{
	(void)s;
	return 242;
}
status_lorawan_t tx_protocol_manager_request(tx_protocol_manager_tx_type_t t, uint8_t port,
					     bool has_port, const uint8_t *data, uint8_t size,
					     lr1mac_layer_param_t type, uint32_t target, uint8_t s)
{
	tx_request_type = t;
	(void)has_port;
	(void)type;
	(void)s;
	tx_port = port;
	tx_size = size;
	tx_target = target;
	memcpy(tx, data, size);
	return OKLORAWAN;
}
bool tx_protocol_manager_tx_is_aborted(void)
{
	return false;
}
bool lorawan_fuota_context_supported(uint8_t s, uint8_t p, uint8_t i)
{
	(void)s;
	(void)p;
	return supported && i == 0;
}
bool lorawan_fuota_storage_available(uint8_t s)
{
	(void)s;
	return !occupied;
}
uint32_t lorawan_fuota_generation(uint8_t s)
{
	(void)s;
	return 7;
}
void lorawan_fuota_observe(uint8_t s, enum lbm_fuota_notice n, const struct lbm_fuota_info *info)
{
	(void)s;
	CHECK(info->generation == 7);
	notice_count[n]++;
	if (n == LBM_FUOTA_FRAGMENT_SETUP) {
		occupied = true;
		accepted_setup = *info;
	}
}
size_t lorawan_fuota_storage_capacity(void)
{
	return sizeof(storage);
}
int lorawan_fuota_store(uint32_t o, const uint8_t *b, size_t n)
{
	if (write_fail) {
		storage_error = -5;
		return -5;
	}
	CHECK(o + n <= sizeof(storage));
	memcpy(storage + o, b, n);
	return 0;
}
int lorawan_fuota_read(uint32_t o, uint8_t *b, size_t n)
{
	CHECK(o + n <= sizeof(storage));
	memcpy(b, storage + o, n);
	return 0;
}
int lorawan_fuota_storage_status(bool reset)
{
	int rc = storage_error;
	if (reset) {
		storage_error = 0;
	}
	return rc;
}
bool lorawan_api_is_time_valid(uint8_t s)
{
	(void)s;
	return true;
}
bool lorawan_api_convert_rtc_to_gps_epoch_time(uint32_t ms, uint32_t *sec, uint32_t *frac,
					       uint8_t s)
{
	(void)s;
	*sec = 1000 + ms / 1000;
	*frac = 0;
	return true;
}
uint32_t lorawan_api_get_timestamp_last_device_time_ans_s(uint8_t s)
{
	(void)s;
	return 0;
}
bool lorawan_api_get_class_b_status(uint8_t s)
{
	(void)s;
	return false;
}
uint32_t lorawan_api_decode_freq_from_24bits_buf(uint8_t s, uint8_t b[3])
{
	(void)s;
	return ((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16) * 100;
}
status_lorawan_t lorawan_api_is_frequency_valid(uint8_t s, uint32_t f)
{
	(void)s;
	return f >= 860000000 && f <= 930000000 ? OKLORAWAN : ERRORLORAWAN;
}
status_lorawan_t lorawan_api_is_datarate_valid(uint8_t s, uint8_t d)
{
	(void)s;
	return d <= 5 ? OKLORAWAN : ERRORLORAWAN;
}
void lorawan_api_class_c_enabled(bool enabled, uint8_t s)
{
	(void)enabled;
	(void)s;
}
void lorawan_class_b_management_enable(uint8_t s, bool enabled, uint8_t delay)
{
	(void)s;
	(void)enabled;
	(void)delay;
}
lorawan_management_rc_t lorawan_cid_request_add_task(uint8_t s, uint8_t mask, uint32_t delay)
{
	(void)s;
	(void)mask;
	(void)delay;
	return 0;
}
lorawan_multicast_rc_t lorawan_api_multicast_set_group_address(uint8_t g, uint32_t a, uint8_t s)
{
	(void)g;
	(void)a;
	(void)s;
	return LORAWAN_MC_RC_OK;
}
lorawan_multicast_rc_t lorawan_api_multicast_set_fcnt_down_range(uint8_t g, uint32_t lo,
								 uint32_t hi, uint8_t s)
{
	(void)g;
	(void)lo;
	(void)hi;
	(void)s;
	return LORAWAN_MC_RC_OK;
}
lorawan_multicast_rc_t lorawan_api_multicast_derive_group_keys(uint8_t g, const uint8_t k[16],
							       uint8_t s)
{
	(void)g;
	(void)k;
	(void)s;
	key_calls++;
	return LORAWAN_MC_RC_OK;
}
lorawan_multicast_rc_t lorawan_api_multicast_c_start_session(uint8_t g, uint32_t f, uint8_t d,
							     uint8_t s)
{
	(void)g;
	(void)d;
	(void)s;
	launches++;
	launched_frequency = f;
	return LORAWAN_MC_RC_OK;
}
lorawan_multicast_rc_t lorawan_api_multicast_c_stop_session(uint8_t g, uint8_t s)
{
	(void)g;
	(void)s;
	stops++;
	return LORAWAN_MC_RC_OK;
}
lorawan_multicast_rc_t lorawan_api_multicast_b_start_session(uint8_t g, uint32_t f, uint8_t d,
							     uint8_t per, uint8_t s)
{
	(void)g;
	(void)f;
	(void)d;
	(void)per;
	(void)s;
	return LORAWAN_MC_RC_OK;
}
lorawan_multicast_rc_t lorawan_api_multicast_b_stop_session(uint8_t g, uint8_t s)
{
	(void)g;
	(void)s;
	return LORAWAN_MC_RC_OK;
}

static void frag_input(const uint8_t *p, size_t n, receive_win_t window)
{
	lr1_stack_mac_down_data_t rx = {0};
	rx.rx_metadata.rx_fport_present = true;
	rx.rx_metadata.rx_fport = 201;
	rx.rx_metadata.rx_window = window;
	rx.rx_payload_size = n;
	memcpy(rx.rx_payload, p, n);
	tx_size = 0;
	frag_rx(&rx);
}
static void mc_input(const uint8_t *p, size_t n)
{
	lr1_stack_mac_down_data_t rx = {0};
	rx.rx_metadata.rx_fport_present = true;
	rx.rx_metadata.rx_fport = 200;
	rx.rx_metadata.rx_window = RECEIVE_ON_RX1;
	rx.rx_payload_size = n;
	memcpy(rx.rx_payload, p, n);
	tx_size = 0;
	mc_rx(&rx);
}
static void reset(void)
{
	supported = true;
	occupied = false;
	write_fail = false;
	storage_error = 0;
	key_calls = launches = stops = 0;
	clock_ms = 0;
	memset(storage, 0xcc, sizeof(storage));
	memset(notice_count, 0, sizeof(notice_count));
	lorawan_fragmentation_package_services_init(&frag_id, 0, &frag_rx, &frag_launch,
						    &frag_update, &frag_context);
	lorawan_fragmentation_release(0);
	lorawan_remote_multicast_setup_package_services_init(&mc_id, 0, &mc_rx, &mc_launch,
							     &mc_update, &mc_context);
}
static void package_versions(void)
{
	uint8_t p[] = {0};
	reset();
	frag_input(p, 1, RECEIVE_ON_RX1);
	frag_launch(frag_context);
	CHECK(tx_port == 201 && tx_size == 3 && memcmp(tx, (uint8_t[]){0, 3, 1}, 3) == 0);
	frag_update(frag_context);
	mc_input(p, 1);
	mc_launch(mc_context);
	CHECK(tx_port == 200 && tx_size == 3 && memcmp(tx, (uint8_t[]){0, 2, 1}, 3) == 0);
	puts("PASS real TS004/TS005 package versions");
}
static const uint8_t setup[] = {2, 1, 2, 0, 4, 0, 0, 0x04, 0x03, 0x02, 0xf1};
static void setup_retries(void)
{
	reset();
	frag_input(setup, sizeof(setup), RECEIVE_ON_RX1);
	CHECK(last_task.priority == TASK_HIGH_PRIORITY);
	CHECK(last_task.time_to_execute_s == clock_ms / 1000);
	frag_input(setup, sizeof(setup), RECEIVE_ON_RX1);
	frag_launch(frag_context);
	CHECK(tx_size == 2 && tx[0] == 2 && tx[1] == 0);
	CHECK(tx_request_type == TX_PROTOCOL_TRANSMIT_LORA_NO_RANDOM_DELAY);
	/* Another retry while the first transmit is owned by TPM. */
	frag_input(setup, sizeof(setup), RECEIVE_ON_RX1);
	frag_update(frag_context);
	CHECK(lorawan_fragmentation_answer_pending(0));
	frag_launch(frag_context);
	CHECK(tx_size == 2 && tx[1] == 0);
	frag_update(frag_context);
	uint8_t f1[] = {8, 1, 0, 0x11, 0x22, 0x33, 0x44};
	frag_input(f1, sizeof(f1), RECEIVE_ON_RXC_MC_GRP0);
	frag_input(setup, sizeof(setup), RECEIVE_ON_RX1);
	uint8_t changed[sizeof(setup)];
	memcpy(changed, setup, sizeof(changed));
	changed[7] ^= 1;
	frag_input(changed, sizeof(changed), RECEIVE_ON_RX1);
	frag_launch(frag_context);
	CHECK(tx_size == 4 && memcmp(tx, (uint8_t[]){2, 0, 2, 2}, 4) == 0);
	frag_update(frag_context);
	CHECK(notice_count[LBM_FUOTA_FRAGMENT_SETUP] == 1);
	uint8_t f2[] = {8, 2, 0, 0x55, 0x66, 0x77, 0x88};
	frag_input(f2, sizeof(f2), RECEIVE_ON_RXC_MC_GRP0);
	CHECK(notice_count[LBM_FUOTA_COMPLETE] == 1);
	CHECK(memcmp(storage, (uint8_t[]){0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}, 8) == 0);
	puts("PASS setup repeats before launch, in flight and after one fragment; conflicting "
	     "setup isolated");
}
static void transfer(void)
{
	reset();
	frag_input(setup, sizeof(setup), RECEIVE_ON_RX1);
	frag_launch(frag_context);
	CHECK(tx_size == 2 && tx[1] == 0);
	frag_update(frag_context);
	uint8_t f1[] = {8, 1, 0, 0x11, 0x22, 0x33, 0x44}, f2[] = {8, 2, 0, 0x55, 0x66, 0x77, 0x88};
	frag_input(f1, sizeof(f1), RECEIVE_ON_RXC_MC_GRP1);
	CHECK(storage[0] == 0xcc);
	frag_input(f1, sizeof(f1), RECEIVE_ON_RXC_MC_GRP0);
	frag_input(f1, sizeof(f1), RECEIVE_ON_RXC_MC_GRP0); /* duplicate */
	frag_input(f2, sizeof(f2), RECEIVE_ON_RXC_MC_GRP0);
	CHECK(notice_count[LBM_FUOTA_COMPLETE] == 1);
	CHECK(memcmp(storage, (uint8_t[]){0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}, 8) == 0);
	uint8_t status[] = {1, 0};
	frag_input(status, 2, RECEIVE_ON_RX1);
	CHECK(tx_size == 0);
	status[1] = 1;
	frag_input(status, 2, RECEIVE_ON_RX1);
	frag_launch(frag_context);
	CHECK(tx_size == 5 && tx[0] == 1 && tx[3] == 0 && tx[4] == 0);
	CHECK(tx_target == clock_ms);
	CHECK(tx_request_type == TX_PROTOCOL_TRANSMIT_LORA_NO_RANDOM_DELAY);
	frag_update(frag_context);
	CHECK(notice_count[LBM_FUOTA_CONTROL_SENT] == 1);
	uint8_t del[] = {3, 0};
	frag_input(del, 2, RECEIVE_ON_RX1);
	frag_launch(frag_context);
	CHECK(tx_size == 2 && tx[0] == 3 && tx[1] == 0);
	frag_update(frag_context);
	CHECK(storage[0] == 0x11 && storage[7] == 0x88); /* delete retains original bytes */
	frag_input(setup, sizeof(setup), RECEIVE_ON_RX1);
	frag_launch(frag_context);
	CHECK(tx_size == 2 && (tx[1] & 2));
	CHECK(storage[0] == 0x11);
	puts("PASS real TS004 reconstruction, duplicate/group filtering, Participant, "
	     "BlockAckDelay, delete retention");
}
static void status_timing(void)
{
	const receive_win_t unicast[] = {RECEIVE_ON_RX1, RECEIVE_ON_RX2, RECEIVE_ON_RXC};
	const receive_win_t multicast[] = {RECEIVE_ON_RXC_MC_GRP0, RECEIVE_ON_RXB_MC_GRP0};
	uint8_t status[] = {1, 1};
	for (unsigned i = 0; i < sizeof(unicast) / sizeof(unicast[0]); ++i) {
		reset();
		frag_input(setup, sizeof(setup), RECEIVE_ON_RX1);
		frag_launch(frag_context);
		frag_update(frag_context);
		clock_ms = 12345;
		frag_input(status, 2, unicast[i]);
		CHECK(last_task.time_to_execute_s == 12);
		CHECK(last_task.priority == TASK_HIGH_PRIORITY);
		frag_launch(frag_context);
		CHECK(tx_target == 12345);
		CHECK(tx_request_type == TX_PROTOCOL_TRANSMIT_LORA_NO_RANDOM_DELAY);
		CHECK(tx_size == 5 && tx[0] == 1);
		frag_update(frag_context);
	}
	for (unsigned i = 0; i < sizeof(multicast) / sizeof(multicast[0]); ++i) {
		reset();
		frag_input(setup, sizeof(setup), RECEIVE_ON_RX1);
		frag_launch(frag_context);
		frag_update(frag_context);
		clock_ms = 12345;
		frag_input(status, 2, multicast[i]);
		CHECK(last_task.time_to_execute_s == 15);
		CHECK(last_task.priority == TASK_MEDIUM_HIGH_PRIORITY);
		frag_launch(frag_context);
		CHECK(tx_target == 28345);
		CHECK(tx_request_type == TX_PROTOCOL_TRANSMIT_LORA);
		CHECK(tx_size == 5 && tx[0] == 1);
		frag_update(frag_context);
		/* A later unicast request must not inherit the multicast delay. */
		clock_ms = 30000;
		frag_input(status, 2, RECEIVE_ON_RX1);
		frag_launch(frag_context);
		CHECK(tx_target == 30000);
		CHECK(tx_request_type == TX_PROTOCOL_TRANSMIT_LORA_NO_RANDOM_DELAY);
		frag_update(frag_context);
	}
	puts("PASS unicast RX1/RX2/RXC prompt StatusAns; multicast B/C jitter retained; no stale "
	     "delay");
}
static void malformed_and_write_failure(void)
{
	reset();
	for (size_t n = 1; n < sizeof(setup); ++n) {
		frag_input(setup, n, RECEIVE_ON_RX1);
		CHECK(notice_count[LBM_FUOTA_FRAGMENT_SETUP] == 0);
	}
	uint8_t bad[11];
	memcpy(bad, setup, 11);
	bad[4] = 0;
	frag_input(bad, 11, RECEIVE_ON_RX1);
	CHECK(!occupied);
	frag_input(setup, 11, RECEIVE_ON_RX1);
	frag_launch(frag_context);
	frag_update(frag_context);
	uint8_t f[] = {8, 1, 0, 0x11, 0x22, 0x33, 0x44};
	for (size_t n = 1; n < sizeof(f); ++n) {
		frag_input(f, n, RECEIVE_ON_RX1);
		CHECK(storage[0] == 0xcc);
	}
	write_fail = true;
	frag_input(f, sizeof(f), RECEIVE_ON_RX1);
	CHECK(notice_count[LBM_FUOTA_COMPLETE] == 1);
	CHECK(storage[0] == 0xcc);
	puts("PASS malformed TS004 frames and checked-storage failure path under ASan/UBSan");
}
static void class_b_and_groups(void)
{
	reset();
	supported = false;
	uint8_t group[30] = {2};
	group[5] = 0x80;
	group[29] = 0xff;
	mc_input(group, sizeof(group));
	mc_launch(mc_context);
	CHECK(tx_size == 2 && tx[0] == 2 && (tx[1] & 4));
	CHECK(key_calls == 0);
	frag_input(setup, 11, RECEIVE_ON_RX1);
	frag_launch(frag_context);
	CHECK(tx_size == 2 && (tx[1] & 4));
	CHECK(!occupied);
	supported = true;
	mc_input(group, sizeof(group));
	mc_launch(mc_context);
	CHECK(key_calls == 1);
	lorawan_remote_multicast_setup_cancel_class_c(0, 0);
	CHECK(stops > 0);
	puts("PASS standard context-rejection bits, no key mutation on refusal, explicit "
	     "cancellation");
}
static void multicast_windows(void)
{
	reset();
	uint8_t group[30] = {2};
	mc_input(group, 30);
	mc_launch(mc_context);
	uint32_t units = 924600000 / 100;
	uint8_t window[] = {
		4, 0, 0xe9, 3, 0, 0, 4, units & 255, (units >> 8) & 255, (units >> 16) & 255, 4};
	mc_input(window, 11);
	mc_launch(mc_context);
	CHECK(tx[0] == 4 && tx_size == 5);
	mc_update(mc_context);
	clock_ms = 1000;
	mc_update(mc_context);
	lorawan_remote_multicast_setup_cancel_class_c(0, 0);
	mc_launch(mc_context);
	CHECK(launches == 0);
	clock_ms = 0;
	mc_input(window, 11);
	mc_launch(mc_context);
	CHECK(tx_size == 5);
	/* Keep the accepted window while processing independent invalid requests. */
	unsigned int scheduled = notice_count[LBM_FUOTA_WINDOW_SETUP];
	uint8_t extra[sizeof(window)];
	memcpy(extra, window, sizeof(extra));
	extra[1] = 3;
	mc_input(extra, sizeof(extra));
	mc_launch(mc_context);
	CHECK(tx_port == 200 && tx_size == 2 && tx[0] == 4 && tx[1] == 0x13);
	uint8_t compound[sizeof(window) + 1] = {0};
	memcpy(compound, extra, sizeof(extra));
	mc_input(compound, sizeof(compound)); /* Unknown group followed by PackageVersionReq. */
	mc_launch(mc_context);
	CHECK(tx_size == 5 && memcmp(tx, (uint8_t[]){4, 0x13, 0, 2, 1}, 5) == 0);
	extra[1] = 0;
	extra[2] = 0;
	extra[3] = 0;
	mc_input(extra, sizeof(extra));
	mc_launch(mc_context);
	CHECK(tx_size == 5 && memcmp(tx, (uint8_t[]){4, 0, 0, 0, 0}, 5) == 0);
	extra[2] = 0xe8; /* Exactly the current GPS second. */
	extra[3] = 3;
	mc_input(extra, sizeof(extra));
	mc_launch(mc_context);
	CHECK(tx_size == 5 && memcmp(tx, (uint8_t[]){4, 0, 0, 0, 0}, 5) == 0);
	CHECK(notice_count[LBM_FUOTA_WINDOW_SETUP] == scheduled);
	uint8_t bad[11];
	memcpy(bad, window, 11);
	bad[7] = bad[8] = bad[9] = 0;
	mc_input(bad, 11);
	mc_launch(mc_context);
	CHECK(tx_size == 2 && (tx[1] & 8));
	clock_ms = 1000;
	mc_update(mc_context);
	mc_launch(mc_context);
	CHECK(launches == 1 && launched_frequency == 924600000);
	CHECK(notice_count[LBM_FUOTA_ENTERING] == 1 && notice_count[LBM_FUOTA_STARTED] == 1);
	clock_ms = 18000;
	mc_update(mc_context);
	mc_launch(mc_context);
	CHECK(notice_count[LBM_FUOTA_ENDED] == 1);
	uint8_t del[] = {3, 0};
	mc_input(del, 2);
	mc_launch(mc_context);
	CHECK(tx_size == 2 && tx[0] == 3 && tx[1] == 0);
	puts("PASS real TS005 scheduling, cancelled launch, invalid-window isolation, natural end "
	     "and delete");
}
void fixture_fragment(const uint8_t *source, uint16_t count, uint8_t size, uint16_t counter,
		      uint8_t *out);
static void fec_recovery(void)
{
	reset();
	uint8_t conf[11];
	memcpy(conf, setup, 11);
	conf[2] = 4;
	frag_input(conf, 11, RECEIVE_ON_RX1);
	frag_launch(frag_context);
	frag_update(frag_context);
	const uint8_t original[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
	uint8_t fragment[7] = {8};
	const uint16_t order[] = {3, 2, 4};
	for (unsigned int i = 0; i < 3; ++i) {
		uint16_t n = order[i];
		fragment[1] = n;
		fixture_fragment(original, 4, 4, n, fragment + 3);
		frag_input(fragment, 7, RECEIVE_ON_RX1);
	}
	CHECK(notice_count[LBM_FUOTA_COMPLETE] == 0); /* insufficient with fragment 1 absent */
	for (uint16_t n = 5; n < 40 && notice_count[LBM_FUOTA_COMPLETE] == 0; ++n) {
		fragment[1] = n;
		fixture_fragment(original, 4, 4, n, fragment + 3);
		frag_input(fragment, 7, RECEIVE_ON_RX1);
	}
	CHECK(notice_count[LBM_FUOTA_COMPLETE] == 1);
	CHECK(memcmp(storage, original, 16) == 0);
	puts("PASS real FEC missing-first-fragment/out-of-order vector and insufficient-data "
	     "boundary");
}
static void clocks(void)
{
	uint8_t id = 0;
	void *context;
	uint8_t (*rx)(lr1_stack_mac_down_data_t *);
	void (*launch)(void *), (*update)(void *);
	lorawan_alcsync_services_init(&id, 0, &rx, &launch, &update, &context);
	lorawan_alcsync_set_enabled(0, true);
	lr1_stack_mac_down_data_t input = {0};
	input.rx_metadata.rx_fport_present = true;
	input.rx_metadata.rx_fport = 202;
	input.rx_metadata.rx_window = RECEIVE_ON_RX1;
	input.rx_payload[0] = 0;
	input.rx_payload_size = 1;
	rx(&input);
	launch(context);
	CHECK(tx_port == 202 && tx_size >= 3 && memcmp(tx, (uint8_t[]){0, 1, 1}, 3) == 0);
	CHECK(lorawan_alcsync_request_sync(0, true) == ALC_SYNC_OK);
	launch(context);
	CHECK(tx_port == 202 && tx_size >= 6);
	uint8_t token = tx[tx_size - 1] & 15;
	uint32_t gps;
	CHECK(lorawan_alcsync_get_gps_time_second(0, &gps) != ALC_SYNC_OK);
	input.rx_payload_size = 6;
	memcpy(input.rx_payload, (uint8_t[]){1, 1, 0, 0, 0, (token + 1) & 15}, 6);
	rx(&input);
	update(context);
	CHECK(clock_events == 0);
	CHECK(lorawan_alcsync_get_gps_time_second(0, &gps) != ALC_SYNC_OK);
	input.rx_payload[5] = token;
	rx(&input);
	update(context);
	CHECK(clock_events == 1);
	CHECK(lorawan_alcsync_get_gps_time_second(0, &gps) == ALC_SYNC_OK);
	input.rx_payload_size = 2;
	input.rx_payload[0] = 2;
	input.rx_payload[1] = 0;
	rx(&input);
	launch(context);
	CHECK(tx_port == 202 && tx[0] == 2);
	input.rx_payload[0] = 3;
	input.rx_payload[1] = 1;
	rx(&input);
	update(context);
	launch(context);
	CHECK(tx_port == 202 && tx[0] == 1); /* force resync produces AppTimeReq, no private Ans */
	puts("PASS real TS003 version, no-answer/wrong-token, AppTime, periodicity and forced "
	     "resync");
}
static void check_report(const char *scenario, const struct rzi_fuota_event *event, uint8_t code,
			 const uint8_t data[4])
{
	const struct uplink_log_port port = {.uptime_sec = report_time, .send = capture_report};
	const struct uplink_log_options options = {.device_code = UPLINK_LOG_MODULE_RAK3172_F};
	struct uplink_log_event record;
	uint8_t expected[] = {0x40, 0x03,    0x03,    0,       0,      0x05,
			      code, data[0], data[1], data[2], data[3]};

	CHECK(fuota_event_log_record(event, &record));
	CHECK(uplink_log_init(&port, &options) == UPLINK_LOG_OK);
	CHECK(uplink_log_set_link(true) == UPLINK_LOG_OK);
	CHECK(uplink_log_submit_event(&record) == UPLINK_LOG_OK);
	report_sent = false;
	(void)uplink_log_process();
	CHECK(report_sent && memcmp(report_frame, expected, sizeof(expected)) == 0);
	printf("FPort %u %s:", UPLINK_LOG_FPORT, scenario);
	for (size_t i = 0; i < sizeof(report_frame); ++i) {
		printf(" %02X", report_frame[i]);
	}
	puts("");
}

static void capture_stats(struct rzi_fuota_event *event)
{
	struct lbm_fuota_rx_stats stats;
	CHECK(!lorawan_fragmentation_get_rx_stats(accepted_setup.generation + 1, &stats));
	CHECK(lorawan_fragmentation_get_rx_stats(accepted_setup.generation, &stats));
	struct rzi_fuota_rx_stats *rx = &event->snapshot.exit.fragments;
	rx->valid = true;
	rx->received = stats.received;
	rx->error = stats.decoder_failed ? -1 : 0;
	rx->storage_errno = stats.storage_error;
	rx->write_failed = stats.write_failed;
	rx->prefix_valid = stats.prefix_valid;
	memcpy(rx->prefix, stats.prefix, sizeof(rx->prefix));
}

static void event_data_reports(void)
{
	uint8_t conf[sizeof(setup)];
	uint8_t frag[] = {8, 3, 0, 1, 2, 3, 4};
	struct rzi_fuota_event event = {.type = RZI_FUOTA_EVENT_PHASE};
	struct rzi_fuota_session_info *session = &event.snapshot.session;
	struct uplink_log_event unused;
	reset();
	memcpy(conf, setup, sizeof(conf));
	conf[2] = 8;
	frag_input(conf, sizeof(conf), RECEIVE_ON_RX1);
	frag_launch(frag_context);
	frag_update(frag_context);
	session->valid = RZI_FUOTA_SESSION_FRAGMENT;
	session->fragment_count = accepted_setup.fragment_count;
	session->fragment_size = accepted_setup.fragment_size;
	session->fragment_index = accepted_setup.index;
	event.snapshot.phase = RZI_FUOTA_RECEIVING;
	check_report("started", &event, 0x01, (uint8_t[]){8, 0, 4, 0});
	event.type = RZI_FUOTA_EVENT_FINISHED;
	event.snapshot.phase = RZI_FUOTA_READY;
	event.snapshot.exit.reason = RZI_FUOTA_EXIT_TIMEOUT;
	capture_stats(&event);
	CHECK(!fuota_event_log_record(&event, &unused)); /* PRECONFIG timeout */
	event.snapshot.exit.reception_started = true;
	frag_input(frag, sizeof(frag), RECEIVE_ON_RXC_MC_GRP1);
	frag_input(frag, sizeof(frag) - 1, RECEIVE_ON_RX1);
	capture_stats(&event);
	check_report("zero", &event, 0x08, (uint8_t[]){0, 0, 8, 0});
	frag_input(frag, sizeof(frag), RECEIVE_ON_RXC_MC_GRP0);
	frag[1] = 2; /* out of order, still received */
	frag_input(frag, sizeof(frag), RECEIVE_ON_RXC_MC_GRP0);
	capture_stats(&event);
	check_report("partial", &event, 0x08, (uint8_t[]){2, 0, 8, 0});
	frag_input(frag, sizeof(frag), RECEIVE_ON_RXC_MC_GRP0);
	frag_input(conf, sizeof(conf), RECEIVE_ON_RX1); /* identical SetupReq preserves counters */
	frag[1] = 9;                                    /* redundancy */
	frag_input(frag, sizeof(frag), RECEIVE_ON_RXC_MC_GRP0);
	frag_input(frag, sizeof(frag), RECEIVE_ON_RXC_MC_GRP0);
	capture_stats(&event);
	check_report("duplicates_and_redundancy", &event, 0x08, (uint8_t[]){3, 0, 8, 0});
	reset();
	frag_input(conf, sizeof(conf), RECEIVE_ON_RX1);
	capture_stats(&event);
	check_report("next_setup", &event, 0x08, (uint8_t[]){0, 0, 8, 0});
	frag[1] = 4; /* business header offsets 12..15 */
	write_fail = true;
	frag_input(frag, sizeof(frag), RECEIVE_ON_RX1);
	capture_stats(&event);
	CHECK(event.snapshot.exit.fragments.storage_errno == -5 &&
	      event.snapshot.exit.fragments.write_failed);
	check_report("write_failure", &event, 0x07, (uint8_t[]){1, 2, 3, 4});
	reset();
	frag_input(conf, sizeof(conf), RECEIVE_ON_RX1);
	write_fail = true;
	frag[1] = 1;
	frag_input(frag, sizeof(frag), RECEIVE_ON_RX1);
	capture_stats(&event);
	CHECK(event.snapshot.exit.fragments.write_failed &&
	      !(event.snapshot.exit.fragments.prefix_valid & 0xf000U));
	CHECK(!fuota_event_log_record(&event, &unused));
	puts("PASS write failure before version is available retains errno; no fabricated result");
	lorawan_fragmentation_release(0);
	struct lbm_fuota_rx_stats retired;
	CHECK(!lorawan_fragmentation_get_rx_stats(accepted_setup.generation, &retired));

	/* Cross the byte boundary in both LE16 fields with an actual TS004 session. */
	reset();
	conf[2] = 0x2c;
	conf[3] = 1;
	frag_input(conf, sizeof(conf), RECEIVE_ON_RX1);
	session->fragment_count = accepted_setup.fragment_count;
	event.type = RZI_FUOTA_EVENT_PHASE;
	event.snapshot.phase = RZI_FUOTA_RECEIVING;
	check_report("started_300", &event, 0x01, (uint8_t[]){0x2c, 1, 4, 0});
	for (uint16_t n = 1; n <= 258; ++n) {
		frag[1] = (uint8_t)n;
		frag[2] = (uint8_t)(n >> 8);
		frag_input(frag, sizeof(frag), RECEIVE_ON_RX1);
	}
	event.type = RZI_FUOTA_EVENT_FINISHED;
	event.snapshot.phase = RZI_FUOTA_READY;
	capture_stats(&event);
	check_report("received_258", &event, 0x08, (uint8_t[]){2, 1, 0x2c, 1});
}

int main(void)
{
	event_data_reports();
	setup_retries();
	package_versions();
	transfer();
	status_timing();
	malformed_and_write_failure();
	class_b_and_groups();
	multicast_windows();
	fec_recovery();
	clocks();
	printf("USP protocol/FEC host checks passed: %u assertions\n", checks);
	return 0;
}
