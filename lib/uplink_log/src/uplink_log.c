/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>

#include "uplink_log_port.h"
#include "uplink_log_frame.h"

#define BOOT_MAX_ATTEMPTS (1U + UPLINK_LOG_BOOT_MAX_RETRIES)
#define SEND_RETRY_SEC    1U
#define COOLDOWN_KEY_SZ   5U

enum inflight_kind {
	INFLIGHT_NONE,
	INFLIGHT_BOOT,
	INFLIGHT_PERIODIC,
	INFLIGHT_CUSTOM,
	INFLIGHT_RECORD,
};

/* Event or config record. seq == 0 marks a free slot; lower seq is older. */
struct record {
	uint32_t seq;
	uint32_t enqueued_at;
	uint8_t type;
	/* Event domain or config status. */
	uint8_t nibble;
	/* Event code or config type. */
	uint8_t code;
	uint8_t data[4];
	/* Transmissions so far; the next frame carries this as retry_count. */
	uint8_t tx_count;
	bool urgent;
};

struct cooldown {
	bool used;
	uint8_t type;
	uint8_t key[COOLDOWN_KEY_SZ];
	uint32_t since;
};

static struct {
	bool ready;
	struct uplink_log_port port;
	struct uplink_log_options opts;
	uint32_t periodic_interval_sec;

	bool link_up;
	bool paused;
	bool drain_requested;
	uint32_t send_retry_at;

	enum inflight_kind inflight;
	uint32_t inflight_id;
	uint32_t inflight_at;

	bool boot_pending;
	struct uplink_log_boot boot;
	uint32_t boot_gen;
	uint8_t boot_attempts;
	uint32_t boot_due;

	bool periodic_armed;
	uint32_t periodic_due;
	bool periodic_pending;
	struct uplink_log_periodic periodic;
	uint8_t periodic_count;

	bool custom_pending;
	uint8_t custom[UPLINK_LOG_PAYLOAD_SIZE];

	struct record queue[UPLINK_LOG_QUEUE_DEPTH];
	uint32_t next_seq;
	/* Newest record not yet sent; it goes out before the drain order. */
	uint32_t fresh_seq;
	struct cooldown cooldowns[UPLINK_LOG_QUEUE_DEPTH];
} s;

static void lock(void)
{
	if (s.port.lock != NULL) {
		s.port.lock(s.port.ctx);
	}
}

static void unlock(void)
{
	if (s.port.unlock != NULL) {
		s.port.unlock(s.port.ctx);
	}
}

static uint32_t now_sec(void)
{
	return s.port.uptime_sec(s.port.ctx);
}

static void wakeup(void)
{
	if (s.port.wakeup != NULL) {
		s.port.wakeup(s.port.ctx);
	}
}

/* Wrap-safe "now has reached deadline". */
static bool reached(uint32_t now, uint32_t deadline)
{
	return (int32_t)(now - deadline) >= 0;
}

static bool is_fuota_result(uint8_t domain, uint8_t code)
{
	return domain == UPLINK_LOG_EVENT_DOMAIN_FUOTA && code >= UPLINK_LOG_EVENT_FUOTA_SUCCESS;
}

static uint32_t record_age(const struct record *r, uint32_t now)
{
	return now - r->enqueued_at;
}

static size_t record_count(void)
{
	size_t n = 0;

	for (size_t i = 0; i < UPLINK_LOG_QUEUE_DEPTH; i++) {
		n += s.queue[i].seq != 0U ? 1U : 0U;
	}
	return n;
}

static struct record *find_record(uint32_t seq)
{
	if (seq == 0U) {
		return NULL;
	}
	for (size_t i = 0; i < UPLINK_LOG_QUEUE_DEPTH; i++) {
		if (s.queue[i].seq == seq) {
			return &s.queue[i];
		}
	}
	return NULL;
}

static void remove_record(struct record *r)
{
	if (r->seq == s.fresh_seq) {
		s.fresh_seq = 0U;
	}
	memset(r, 0, sizeof(*r));
}

static struct record *oldest_record(bool urgent)
{
	struct record *oldest = NULL;

	for (size_t i = 0; i < UPLINK_LOG_QUEUE_DEPTH; i++) {
		struct record *r = &s.queue[i];

		if (r->seq != 0U && r->urgent == urgent &&
		    (oldest == NULL || r->seq < oldest->seq)) {
			oldest = r;
		}
	}
	return oldest;
}

/* Urgent first, FIFO within a priority. */
static struct record *next_drain_record(void)
{
	struct record *r = oldest_record(true);

	return r != NULL ? r : oldest_record(false);
}

/* Full queue: drop the oldest normal record, or the oldest urgent one. */
static struct record *alloc_record(void)
{
	struct record *r;

	for (size_t i = 0; i < UPLINK_LOG_QUEUE_DEPTH; i++) {
		if (s.queue[i].seq == 0U) {
			return &s.queue[i];
		}
	}
	r = oldest_record(false);
	if (r == NULL) {
		r = oldest_record(true);
	}
	remove_record(r);
	return r;
}

static struct record *enqueue(uint8_t type, uint8_t nibble, uint8_t code, const uint8_t data[4],
			      bool urgent, uint32_t now)
{
	struct record *r = alloc_record();

	r->seq = s.next_seq++;
	if (s.next_seq == 0U) {
		s.next_seq = 1U;
	}
	r->enqueued_at = now;
	r->type = type;
	r->nibble = nibble;
	r->code = code;
	memcpy(r->data, data, sizeof(r->data));
	r->urgent = urgent;
	return r;
}

static bool cooldown_active(uint8_t type, const uint8_t key[COOLDOWN_KEY_SZ], uint32_t now)
{
	for (size_t i = 0; i < UPLINK_LOG_QUEUE_DEPTH; i++) {
		const struct cooldown *c = &s.cooldowns[i];

		if (c->used && c->type == type && memcmp(c->key, key, COOLDOWN_KEY_SZ) == 0 &&
		    !reached(now, c->since + UPLINK_LOG_COOLDOWN_SEC)) {
			return true;
		}
	}
	return false;
}

static void cooldown_start(uint8_t type, const uint8_t key[COOLDOWN_KEY_SZ], uint32_t now)
{
	struct cooldown *slot = NULL;

	for (size_t i = 0; i < UPLINK_LOG_QUEUE_DEPTH; i++) {
		struct cooldown *c = &s.cooldowns[i];

		if (!c->used || reached(now, c->since + UPLINK_LOG_COOLDOWN_SEC)) {
			slot = c;
			break;
		}
		if (slot == NULL || (int32_t)(c->since - slot->since) < 0) {
			slot = c;
		}
	}
	slot->used = true;
	slot->type = type;
	memcpy(slot->key, key, COOLDOWN_KEY_SZ);
	slot->since = now;
}

static void expire_records(uint32_t now)
{
	for (size_t i = 0; i < UPLINK_LOG_QUEUE_DEPTH; i++) {
		struct record *r = &s.queue[i];

		if (r->seq != 0U && record_age(r, now) >= UPLINK_LOG_GIVE_UP_SEC) {
			remove_record(r);
		}
	}
}

static bool records_sendable(void)
{
	return !s.paused && !s.boot_pending &&
	       (s.fresh_seq != 0U || (s.drain_requested && record_count() > 0U));
}

static bool has_sendable(void)
{
	return s.custom_pending || s.periodic_pending || records_sendable();
}

static void handle_result(enum uplink_log_tx_result result, uint32_t now)
{
	enum inflight_kind kind = s.inflight;
	bool acked = result == UPLINK_LOG_TX_ACKED;

	s.inflight = INFLIGHT_NONE;
	switch (kind) {
	case INFLIGHT_BOOT:
		if (!s.boot_pending || s.inflight_id != s.boot_gen) {
			break;
		}
		if (acked) {
			s.boot_pending = false;
			s.drain_requested = true;
		} else if (s.boot_attempts >= BOOT_MAX_ATTEMPTS) {
			s.boot_pending = false;
			s.drain_requested = true;
		} else {
			s.boot_due = now + UPLINK_LOG_BOOT_RETRY_INTERVAL_SEC;
		}
		break;
	case INFLIGHT_PERIODIC:
	case INFLIGHT_CUSTOM:
		s.drain_requested = true;
		break;
	case INFLIGHT_RECORD:
		if (acked) {
			struct record *r = find_record(s.inflight_id);

			if (r != NULL) {
				remove_record(r);
			}
			s.drain_requested = true;
		} else {
			s.drain_requested = false;
		}
		break;
	case INFLIGHT_NONE:
	default:
		break;
	}
}

static void try_send(uint32_t now)
{
	uint8_t frame[UPLINK_LOG_FRAME_SIZE];
	enum inflight_kind kind;
	struct record *r = NULL;
	bool confirmed;

	if (s.boot_pending && reached(now, s.boot_due)) {
		uplink_log_frame_boot(frame, s.opts.device_code, &s.boot);
		kind = INFLIGHT_BOOT;
		confirmed = true;
	} else if (s.custom_pending) {
		uplink_log_frame_custom(frame, s.opts.device_code, s.custom);
		kind = INFLIGHT_CUSTOM;
		confirmed = false;
	} else if (s.periodic_pending) {
		uplink_log_frame_periodic(frame, s.opts.device_code, s.periodic_count, &s.periodic);
		kind = INFLIGHT_PERIODIC;
		confirmed = false;
	} else if (records_sendable()) {
		r = find_record(s.fresh_seq);
		if (r == NULL) {
			r = next_drain_record();
		}
		if (r == NULL) {
			s.drain_requested = false;
			return;
		}
		uplink_log_frame_record(frame, s.opts.device_code, r->type, record_age(r, now),
					r->tx_count, r->nibble, r->code, r->data);
		kind = INFLIGHT_RECORD;
		confirmed = true;
	} else {
		return;
	}

	if (s.port.send(s.port.ctx, frame, sizeof(frame), confirmed) != 0) {
		s.send_retry_at = now + SEND_RETRY_SEC;
		return;
	}

	s.inflight = kind;
	s.inflight_at = now;
	switch (kind) {
	case INFLIGHT_BOOT:
		s.inflight_id = s.boot_gen;
		s.boot_attempts++;
		break;
	case INFLIGHT_CUSTOM:
		s.custom_pending = false;
		break;
	case INFLIGHT_PERIODIC:
		s.periodic_pending = false;
		s.periodic_count++;
		break;
	case INFLIGHT_RECORD:
		s.inflight_id = r->seq;
		if (r->seq == s.fresh_seq) {
			s.fresh_seq = 0U;
		}
		if (r->tx_count < UINT8_MAX) {
			r->tx_count++;
		}
		break;
	default:
		break;
	}
}

static void consider(uint32_t *next, uint32_t now, uint32_t deadline)
{
	uint32_t wait = reached(now, deadline) ? 0U : deadline - now;

	if (wait < *next) {
		*next = wait;
	}
}

static uint32_t next_deadline(uint32_t now)
{
	uint32_t next = UPLINK_LOG_NO_DEADLINE;

	for (size_t i = 0; i < UPLINK_LOG_QUEUE_DEPTH; i++) {
		const struct record *r = &s.queue[i];

		if (r->seq != 0U) {
			consider(&next, now, r->enqueued_at + UPLINK_LOG_GIVE_UP_SEC);
		}
	}
	if (!s.link_up) {
		return next;
	}
	if (s.inflight != INFLIGHT_NONE) {
		consider(&next, now, s.inflight_at + UPLINK_LOG_TX_TIMEOUT_SEC);
	} else {
		if (has_sendable()) {
			consider(&next, now, s.send_retry_at);
		}
		if (s.boot_pending) {
			/* A refused send also holds back a due boot report. */
			consider(&next, now,
				 (int32_t)(s.send_retry_at - s.boot_due) > 0 ? s.send_retry_at
									     : s.boot_due);
		}
	}
	if (s.periodic_armed) {
		consider(&next, now, s.periodic_due);
	}
	return next;
}

int uplink_log_init(const struct uplink_log_port *port, const struct uplink_log_options *options)
{
	uint16_t minutes;

	if (port == NULL || options == NULL || port->uptime_sec == NULL || port->send == NULL ||
	    (port->lock == NULL) != (port->unlock == NULL)) {
		return UPLINK_LOG_ERR_INVALID;
	}
	minutes = options->periodic_interval_min != 0U ? options->periodic_interval_min
						       : UPLINK_LOG_PERIODIC_DEFAULT_MINUTES;
	if (minutes < UPLINK_LOG_PERIODIC_MIN_MINUTES ||
	    minutes > UPLINK_LOG_PERIODIC_MAX_MINUTES ||
	    minutes % UPLINK_LOG_PERIODIC_STEP_MINUTES != 0U) {
		return UPLINK_LOG_ERR_INVALID;
	}

	if (port->lock != NULL) {
		port->lock(port->ctx);
	}
	memset(&s, 0, sizeof(s));
	s.port = *port;
	s.opts = *options;
	s.periodic_interval_sec = (uint32_t)minutes * 60U;
	s.next_seq = 1U;
	s.ready = true;
	unlock();
	return UPLINK_LOG_OK;
}

int uplink_log_set_link(bool up)
{
	uint32_t now;

	lock();
	if (!s.ready) {
		unlock();
		return UPLINK_LOG_ERR_STATE;
	}
	now = now_sec();
	if (up && !s.link_up) {
		s.link_up = true;
		s.drain_requested = true;
		s.send_retry_at = now;
		if (s.opts.periodic_snapshot != NULL) {
			s.periodic_armed = true;
			s.periodic_due = now + s.periodic_interval_sec;
		}
	} else if (!up && s.link_up) {
		s.link_up = false;
		s.periodic_armed = false;
		if (s.inflight != INFLIGHT_NONE) {
			handle_result(UPLINK_LOG_TX_FAILED, now);
		}
	}
	unlock();
	wakeup();
	return UPLINK_LOG_OK;
}

int uplink_log_on_tx_result(enum uplink_log_tx_result result)
{
	lock();
	if (!s.ready) {
		unlock();
		return UPLINK_LOG_ERR_STATE;
	}
	if (s.inflight != INFLIGHT_NONE) {
		handle_result(result, now_sec());
	}
	unlock();
	wakeup();
	return UPLINK_LOG_OK;
}

uint32_t uplink_log_process(void)
{
	uint32_t now;
	uint32_t next;

	lock();
	if (!s.ready) {
		unlock();
		return UPLINK_LOG_NO_DEADLINE;
	}
	now = now_sec();
	expire_records(now);
	if (s.inflight != INFLIGHT_NONE &&
	    reached(now, s.inflight_at + UPLINK_LOG_TX_TIMEOUT_SEC)) {
		handle_result(UPLINK_LOG_TX_FAILED, now);
	}
	if (s.link_up && s.periodic_armed && reached(now, s.periodic_due)) {
		struct uplink_log_periodic snapshot = {0};

		s.periodic_due = now + s.periodic_interval_sec;
		if (s.opts.periodic_snapshot(s.opts.user_data, &snapshot)) {
			s.periodic = snapshot;
			s.periodic_pending = true;
		}
	}
	if (s.link_up && s.inflight == INFLIGHT_NONE && reached(now, s.send_retry_at)) {
		try_send(now);
	}
	next = next_deadline(now);
	unlock();
	return next;
}

int uplink_log_submit_boot(const struct uplink_log_boot *boot)
{
	if (boot == NULL || (uint32_t)boot->reset_reason > 0xFU) {
		return UPLINK_LOG_ERR_INVALID;
	}
	lock();
	if (!s.ready) {
		unlock();
		return UPLINK_LOG_ERR_STATE;
	}
	s.boot = *boot;
	s.boot_gen++;
	s.boot_pending = true;
	s.boot_attempts = 0U;
	s.boot_due = now_sec();
	unlock();
	wakeup();
	return UPLINK_LOG_OK;
}

int uplink_log_submit_periodic(const struct uplink_log_periodic *periodic)
{
	if (periodic == NULL) {
		return UPLINK_LOG_ERR_INVALID;
	}
	lock();
	if (!s.ready) {
		unlock();
		return UPLINK_LOG_ERR_STATE;
	}
	s.periodic = *periodic;
	s.periodic_pending = true;
	unlock();
	wakeup();
	return UPLINK_LOG_OK;
}

int uplink_log_submit_custom(const uint8_t payload[UPLINK_LOG_PAYLOAD_SIZE])
{
	if (payload == NULL) {
		return UPLINK_LOG_ERR_INVALID;
	}
	lock();
	if (!s.ready) {
		unlock();
		return UPLINK_LOG_ERR_STATE;
	}
	memcpy(s.custom, payload, sizeof(s.custom));
	s.custom_pending = true;
	unlock();
	wakeup();
	return UPLINK_LOG_OK;
}

/* Common path for event and config records. Caller holds the lock. */
static int submit_record(uint8_t type, uint8_t nibble, uint8_t code, const uint8_t data[4],
			 bool urgent, bool dedup, const uint8_t key[COOLDOWN_KEY_SZ])
{
	uint32_t now = now_sec();
	struct record *r;

	if (dedup && cooldown_active(type, key, now)) {
		s.drain_requested = true;
		return UPLINK_LOG_ERR_SUPPRESSED;
	}
	r = enqueue(type, nibble, code, data, urgent, now);
	if (dedup) {
		cooldown_start(type, key, now);
	}
	if (s.fresh_seq != 0U) {
		s.drain_requested = true;
	}
	s.fresh_seq = r->seq;
	return UPLINK_LOG_OK;
}

int uplink_log_submit_event(const struct uplink_log_event *event)
{
	uint8_t key[COOLDOWN_KEY_SZ] = {0};
	bool fuota;
	bool result;
	int rc;

	if (event == NULL || event->domain == 0U || event->domain > 0xFU) {
		return UPLINK_LOG_ERR_INVALID;
	}
	fuota = event->domain == UPLINK_LOG_EVENT_DOMAIN_FUOTA;
	result = is_fuota_result(event->domain, event->code);
	key[0] = event->domain;
	key[1] = event->code;

	lock();
	if (!s.ready) {
		unlock();
		return UPLINK_LOG_ERR_STATE;
	}
	rc = submit_record(UPLINK_LOG_TYPE_EVENT, event->domain, event->code, event->data,
			   event->urgent || result, !fuota, key);
	unlock();
	wakeup();
	return rc;
}

int uplink_log_submit_config(const struct uplink_log_config_report *config)
{
	uint8_t data[4];
	uint8_t key[COOLDOWN_KEY_SZ];
	int rc;

	if (config == NULL || (uint32_t)config->status > 0xFU) {
		return UPLINK_LOG_ERR_INVALID;
	}
	data[0] = config->object_id;
	data[1] = 0U;
	data[2] = (uint8_t)(config->value & 0xFFU);
	data[3] = (uint8_t)(config->value >> 8);
	key[0] = config->config_type;
	memcpy(&key[1], data, sizeof(data));

	lock();
	if (!s.ready) {
		unlock();
		return UPLINK_LOG_ERR_STATE;
	}
	rc = submit_record(UPLINK_LOG_TYPE_CONFIG, (uint8_t)config->status, config->config_type,
			   data, config->urgent, true, key);
	unlock();
	wakeup();
	return rc;
}

int uplink_log_set_periodic_interval(uint16_t minutes)
{
	if (minutes < UPLINK_LOG_PERIODIC_MIN_MINUTES ||
	    minutes > UPLINK_LOG_PERIODIC_MAX_MINUTES ||
	    minutes % UPLINK_LOG_PERIODIC_STEP_MINUTES != 0U) {
		return UPLINK_LOG_ERR_INVALID;
	}
	lock();
	if (!s.ready) {
		unlock();
		return UPLINK_LOG_ERR_STATE;
	}
	s.periodic_interval_sec = (uint32_t)minutes * 60U;
	if (s.periodic_armed) {
		s.periodic_due = now_sec() + s.periodic_interval_sec;
	}
	unlock();
	wakeup();
	return UPLINK_LOG_OK;
}

static int set_paused(bool paused)
{
	lock();
	if (!s.ready) {
		unlock();
		return UPLINK_LOG_ERR_STATE;
	}
	if (s.paused && !paused) {
		s.drain_requested = true;
	}
	s.paused = paused;
	unlock();
	wakeup();
	return UPLINK_LOG_OK;
}

int uplink_log_pause_drain(void)
{
	return set_paused(true);
}

int uplink_log_resume_drain(void)
{
	return set_paused(false);
}

size_t uplink_log_queue_count(void)
{
	size_t n;

	lock();
	n = s.ready ? record_count() : 0U;
	unlock();
	return n;
}
