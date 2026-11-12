#include <rzi/lorawan/fuota_trace.h>
/* SPDX-License-Identifier: Apache-2.0 */
/* The receive agent coordinates module calls. Package parsing, FEC, keys and storage stay below. */
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <rzi/lorawan/fuota.h>
#include <rzi/lorawan/lorawan.h>
#include "fuota_agent_internal.h"

LOG_MODULE_DECLARE(rzi_fuota, LOG_LEVEL_INF);

K_MUTEX_DEFINE(agent_lock);
K_MSGQ_DEFINE(agent_events, sizeof(struct rzi_lorawan_backend_event),
	      CONFIG_RZI_LORAWAN_EVENT_QUEUE_SIZE, 4);
static atomic_t started, overflowed, blocked;
static struct rzi_fuota_snapshot snap;
static bool processing;
static const struct rzi_lorawan_fuota_ops *ops;
static enum rzi_fuota_state state;
static int64_t deadline, retry_at, keepalive_at, window_end_at;
static bool defer_class_c_keepalive;
static unsigned int attempts;
static size_t expected_size;
static bool step_failed, complete_notified;

static void publish_notice_locked(const struct fuota_agent_notice *notice)
{
	k_mutex_unlock(&agent_lock);
	fuota_service_on_agent_notice(notice);
	k_mutex_lock(&agent_lock, K_FOREVER);
}

static void emit(enum rzi_fuota_event_type type)
{
	RZI_FUOTA_TRACE(RZI_FUOTA_TRACE_AGENT_EVENT, type, snap.phase, snap.session.generation);
	const struct rzi_fuota_event event = {.type = type, .snapshot = snap};
	const struct fuota_agent_notice notice = {.kind = FUOTA_AGENT_NOTICE_EVENT,
						  .event = &event};

	publish_notice_locked(&notice);
}

static void change_state(enum rzi_fuota_state next)
{
	if (state == next) {
		return;
	}
	state = next;
	struct fuota_agent_notice notice = {
		.kind = FUOTA_AGENT_NOTICE_STATE_CHANGED,
		.state.next = next,
	};

	if (next == RZI_FUOTA_STATE_COMPLETE) {
		notice.state.data = snap.data;
	}
	publish_notice_locked(&notice);
}

static void phase(enum rzi_fuota_phase next)
{
	snap.phase = next;
	emit(RZI_FUOTA_EVENT_PHASE);
}

static void completion(int status)
{
	if (complete_notified) {
		return;
	}
	complete_notified = true;
	const struct fuota_agent_notice notice = {
		.kind = FUOTA_AGENT_NOTICE_TRANSFER_RESULT,
		.transfer_result = status,
	};

	publish_notice_locked(&notice);
}

static void remember_session(const struct rzi_fuota_session_info *session)
{
	snap.session = *session;
	if (session->valid & RZI_FUOTA_SESSION_DEADLINE) {
		window_end_at = session->window_end_ms;
	}
}

static void restart_keepalive_period(int64_t from)
{
	keepalive_at = from + CONFIG_RZI_LORAWAN_FUOTA_KEEPALIVE_INTERVAL_S * 1000LL;
	RZI_FUOTA_TRACE(RZI_FUOTA_TRACE_TIMER, from, keepalive_at, snap.session.generation);
}

#if CONFIG_RZI_LORAWAN_FUOTA_KEEPALIVE_INTERVAL_S > 0
static bool periodic_allowed(void)
{
	return snap.phase == RZI_FUOTA_READY || snap.phase == RZI_FUOTA_PRECONFIG ||
	       (snap.phase == RZI_FUOTA_FINALIZING && snap.exit.settings_restored);
}
#endif

static void recover(enum rzi_fuota_exit_reason reason, int error)
{
	if (snap.phase == RZI_FUOTA_RECOVERING || snap.phase == RZI_FUOTA_FINALIZING ||
	    snap.phase == RZI_FUOTA_READY || snap.phase == RZI_FUOTA_OFFLINE) {
		return;
	}
	defer_class_c_keepalive = snap.phase == RZI_FUOTA_RECEIVING &&
				 snap.session.original_class == RZI_LORAWAN_CLASS_C;
	snap.exit.reason = reason;
	snap.exit.error = error;
	attempts = 0;
	step_failed = false;
	retry_at = 0;
	phase(RZI_FUOTA_RECOVERING);
}

static bool retry_result(int rc)
{
	if (rc == 0) {
		attempts = 0;
		return true;
	}
	snap.exit.cleanup_error = rc;
	retry_at = k_uptime_get() + CONFIG_RZI_LORAWAN_FUOTA_STEP_RETRY_MS;
	if (++attempts >= CONFIG_RZI_LORAWAN_FUOTA_STEP_ATTEMPTS) {
		step_failed = true;
		emit(RZI_FUOTA_EVENT_ERROR);
	}
	return false;
}

static void finish(int cleanup_error)
{
	snap.exit.cleanup_error = cleanup_error;
	if (!snap.data.ready) {
		int rc = ops->release_data(snap.session.generation);
		if (rc != 0) {
			snap.exit.cleanup_error = rc;
			emit(RZI_FUOTA_EVENT_ERROR);
			step_failed = true;
			return;
		}
	}
	phase(RZI_FUOTA_READY);
	change_state(snap.data.ready ? RZI_FUOTA_STATE_COMPLETE : RZI_FUOTA_STATE_READY);
	emit(RZI_FUOTA_EVENT_FINISHED);
}

static void advance(void)
{
	int64_t now = k_uptime_get();
	if ((snap.phase == RZI_FUOTA_PRECONFIG || snap.phase == RZI_FUOTA_RECEIVING) && deadline &&
	    now >= deadline) {
		recover(RZI_FUOTA_EXIT_TIMEOUT, -RZI_ERR_TIMEOUT);
	}
	if (step_failed || now < retry_at) {
		return;
	}
	if (snap.phase == RZI_FUOTA_RECOVERING) {
		if (!snap.exit.multicast_closed) {
			if (!retry_result(ops->close_multicast(snap.session.generation,
							       &snap.exit.fragments))) {
				return;
			}
			/* A decoder/storage failure can already be pending in the dispatcher
			 * when the local window timer expires. Keep the producer's cause. */
			if (snap.exit.fragments.error) {
				snap.exit.reason = RZI_FUOTA_EXIT_ERROR;
				snap.exit.error = snap.exit.fragments.error;
			}
			snap.exit.multicast_closed = true;
			emit(RZI_FUOTA_EVENT_CLOSED);
		}
		if (!snap.exit.settings_restored) {
			if (!retry_result(ops->restore_settings(snap.session.generation))) {
				return;
			}
			snap.exit.settings_restored = true;
			atomic_clear(&blocked);

			emit(RZI_FUOTA_EVENT_RESTORED);
		}
		if (!snap.data.ready) {
			completion(snap.exit.error ? snap.exit.error : -RZI_ERR_TIMEOUT);
		}
		attempts = 0;
		retry_at = 0;
		now = k_uptime_get();
		deadline =
			MAX(now, window_end_at) + CONFIG_RZI_LORAWAN_FUOTA_STATUS_HOLD_S * 1000LL;
		/* Class C can receive the query directly. Reserve the MAC until the
		 * control answer completes, or the local query window expires.
		 */
		restart_keepalive_period(defer_class_c_keepalive ? deadline : now);
		phase(RZI_FUOTA_FINALIZING);
	}
	if (snap.phase == RZI_FUOTA_FINALIZING) {
		/* After a receive timeout the server may query the missing count
		 * and then delete the same session in a separate downlink.
		 */
		bool retain_for_delete = snap.exit.reason == RZI_FUOTA_EXIT_TIMEOUT;

		if (snap.exit.delete_answered || (snap.exit.status_answered && !retain_for_delete)) {
			finish(0);
		} else if (now >= deadline) {
			finish(snap.exit.status_answered ? 0 : -RZI_ERR_TIMEOUT);
			return;
		}
	}
#if CONFIG_RZI_LORAWAN_FUOTA_KEEPALIVE_INTERVAL_S > 0
	if (periodic_allowed() && now >= keepalive_at) {
		/* Reuse the existing transport-owned keepalive, no Agent packet encoding. */
		RZI_FUOTA_TRACE(RZI_FUOTA_TRACE_DUE, keepalive_at, snap.phase, snap.session.generation);
		int keepalive_rc = ops->cleanup_uplink(0);
		RZI_FUOTA_TRACE(RZI_FUOTA_TRACE_KEEPALIVE_RETURN, keepalive_rc, snap.phase, snap.session.generation);
		ARG_UNUSED(keepalive_rc);
		keepalive_at = now + CONFIG_RZI_LORAWAN_FUOTA_KEEPALIVE_INTERVAL_S * 1000LL;
	}
#endif
}

static void handle(const struct rzi_lorawan_backend_event *event)
{
	if (event->type == RZI_LORAWAN_BACKEND_JOINED) {
		/* Network recovery never resumes an old fragment generation. */
		if (snap.session.generation) {
			int rc = ops->close_multicast(snap.session.generation, NULL);
			if (!rc) {
				rc = ops->restore_settings(snap.session.generation);
			}
			if (!rc) {
				rc = ops->release_data(snap.session.generation);
			}
			/* NOT_FOUND means the backend invalidated its old network context. */
			if (rc && rc != -RZI_ERR_NOT_FOUND) {
				snap.exit.error = rc;
				atomic_set(&blocked, 1);
				phase(RZI_FUOTA_OFFLINE);
				emit(RZI_FUOTA_EVENT_ERROR);
				return;
			}
		}
		step_failed = false;
		retry_at = 0;
		memset(&snap, 0, sizeof(snap));
		window_end_at = 0;
		atomic_clear(&blocked);

		phase(RZI_FUOTA_READY);
		change_state(RZI_FUOTA_STATE_READY);
		int rc = ops->start_clock_sync ? ops->start_clock_sync() : -RZI_ERR_NOT_SUPPORTED;
		if (rc != 0) {
			snap.exit.error = rc;
			emit(RZI_FUOTA_EVENT_ERROR);
		}
		keepalive_at = k_uptime_get();
		return;
	}
	if (event->type == RZI_LORAWAN_BACKEND_JOIN_FAILED ||
	    (event->type == RZI_LORAWAN_BACKEND_STATE_CHANGED &&
	     event->state == RZI_LORAWAN_STATE_READY)) {
		if (snap.session.generation) {
			int rc = ops->close_multicast(snap.session.generation, NULL);
			if (rc == 0) {
				rc = ops->restore_settings(snap.session.generation);
			}
			if (rc == 0) {
				rc = ops->release_data(snap.session.generation);
			}
			snap.exit.error = rc;
		}
		snap.data.ready = false;
		snap.time_valid = false;
		snap.exit.reason = RZI_FUOTA_EXIT_OFFLINE;
		atomic_set(&blocked, snap.exit.error != 0);

		phase(RZI_FUOTA_OFFLINE);
		change_state(RZI_FUOTA_STATE_IDLE);
		return;
	}
	if (event->type == RZI_LORAWAN_BACKEND_ERROR && snap.session.generation) {
		if (snap.phase == RZI_FUOTA_READY && snap.data.ready) {
			snap.phase = RZI_FUOTA_PRECONFIG;
		}
		snap.data.ready = false;
		snap.exit.error = event->error;
		recover(RZI_FUOTA_EXIT_ERROR, event->error);
		emit(RZI_FUOTA_EVENT_ERROR);
		return;
	}
	if (event->type != RZI_LORAWAN_BACKEND_FUOTA) {
		return;
	}
	if (event->fuota.kind == RZI_LORAWAN_BACKEND_FUOTA_CLOCK_SYNCED) {
		snap.time_source = event->fuota.time_source;
		snap.time_valid = event->fuota.successful;
		if (snap.time_valid) {
			snap.time_updated_ms = k_uptime_get();
		}
		emit(RZI_FUOTA_EVENT_TIME);
		return;
	}
	if (event->fuota.kind == RZI_LORAWAN_BACKEND_FUOTA_PRECONFIG) {
		if (snap.phase == RZI_FUOTA_READY && !snap.data.ready && event->fuota.generation &&
		    event->fuota.generation != snap.session.generation) {
			window_end_at = 0;
			remember_session(&event->fuota.session);
			snap.data =
				(struct rzi_fuota_data_info){.generation = event->fuota.generation,
							     .expected_length = expected_size};
			snap.exit = (struct rzi_fuota_exit_state){0};
			complete_notified = step_failed = false;
			deadline = k_uptime_get() +
				   CONFIG_RZI_LORAWAN_FUOTA_PRECONFIG_TIMEOUT_S * 1000LL;
			phase(RZI_FUOTA_PRECONFIG);
		} else if (snap.phase == RZI_FUOTA_PRECONFIG &&
			   event->fuota.generation == snap.session.generation) {
			remember_session(&event->fuota.session);
		}
		return;
	}
	if (!event->fuota.generation || event->fuota.generation != snap.session.generation) {
		return;
	}
	switch (event->fuota.kind) {
	case RZI_LORAWAN_BACKEND_FUOTA_SESSION_STARTED:
		if (snap.phase != RZI_FUOTA_PRECONFIG) {
			break;
		}
		remember_session(&event->fuota.session);
		if (!event->fuota.successful || event->error) {
			recover(RZI_FUOTA_EXIT_ERROR, event->error ? event->error : -RZI_ERR_IO);
			break;
		}
		atomic_set(&blocked, 1);

		if ((snap.session.valid &
		     (RZI_FUOTA_SESSION_FRAGMENT | RZI_FUOTA_SESSION_WINDOW)) !=
		    (RZI_FUOTA_SESSION_FRAGMENT | RZI_FUOTA_SESSION_WINDOW)) {
			if ((snap.session.valid & RZI_FUOTA_SESSION_WINDOW) &&
			    !(snap.session.valid & RZI_FUOTA_SESSION_FRAGMENT)) {
				LOG_ERR("fuota.preconfig_fail generation=%u valid=0x%x",
					snap.session.generation, snap.session.valid);
			}
			recover(RZI_FUOTA_EXIT_ERROR, -RZI_ERR_NO_DATA);
			break;
		}
		if (!(snap.session.valid & RZI_FUOTA_SESSION_DEADLINE)) {
			/* Backends without a captured GPS deadline can still anchor at START. */
			window_end_at = k_uptime_get() + snap.session.timeout_s * 1000LL;
			snap.session.window_end_ms = window_end_at;
			snap.session.valid |= RZI_FUOTA_SESSION_DEADLINE;
		}
		deadline = window_end_at;
		snap.exit.reception_started = true;
		phase(RZI_FUOTA_RECEIVING);
		change_state(RZI_FUOTA_STATE_TRANSFERRING);
		const struct fuota_agent_notice notice = {
			.kind = FUOTA_AGENT_NOTICE_SESSION_STARTED};

		publish_notice_locked(&notice);
		break;
	case RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE:
		if ((snap.phase != RZI_FUOTA_PRECONFIG && snap.phase != RZI_FUOTA_RECEIVING &&
		     snap.phase != RZI_FUOTA_RECOVERING && snap.phase != RZI_FUOTA_FINALIZING) ||
		    complete_notified || snap.exit.reason == RZI_FUOTA_EXIT_ERROR) {
			break;
		}
		if (event->fuota.successful) {
			size_t length = event->fuota.image_size;
			int rc = length                ? 0
				 : ops->get_image_size ? ops->get_image_size(&length)
						       : -RZI_ERR_NO_DATA;
			snap.data.length_valid = rc == 0 && length != 0;
			snap.data.length = snap.data.length_valid ? length : 0;
			snap.data.ready = true;
			snap.exit.reason = RZI_FUOTA_EXIT_COMPLETE;
			snap.exit.error = 0;
			change_state(RZI_FUOTA_STATE_COMPLETE);
			emit(RZI_FUOTA_EVENT_DATA_READY);
			completion(0);
			recover(RZI_FUOTA_EXIT_COMPLETE, 0);
		} else {
			change_state(RZI_FUOTA_STATE_FAILED);
			recover(RZI_FUOTA_EXIT_ERROR, event->error ? event->error : -RZI_ERR_IO);
		}
		break;
	case RZI_LORAWAN_BACKEND_FUOTA_SESSION_ENDED:
		recover(RZI_FUOTA_EXIT_TIMEOUT, -RZI_ERR_TIMEOUT);
		break;
	case RZI_LORAWAN_BACKEND_FUOTA_CLEANUP_UPLINK_DONE:
		/* Legacy event: no dedicated cleanup transmission is requested. */
		break;
	case RZI_LORAWAN_BACKEND_FUOTA_FRAGMENT_DELETED:
		/* The backend has already discarded this fragment under the modem lock.
		 * A separate TS005 group/window, if configured, still owns its generation. */
		if (snap.phase == RZI_FUOTA_PRECONFIG ||
		    (snap.phase == RZI_FUOTA_RECOVERING && !snap.data.ready)) {
			remember_session(&event->fuota.session);
			if (!(snap.session.valid & RZI_FUOTA_SESSION_GROUP)) {
				snap.exit = (struct rzi_fuota_exit_state){.reason = RZI_FUOTA_EXIT_DELETED};
				step_failed = false;
				retry_at = 0;
				phase(RZI_FUOTA_READY);
				emit(RZI_FUOTA_EVENT_FINISHED);
			}
		}
		break;
	case RZI_LORAWAN_BACKEND_FUOTA_CONTROL_DONE:
		if (snap.phase == RZI_FUOTA_FINALIZING && event->error == 0) {
			snap.exit.status_answered |= (event->fuota.control & 1) != 0;
			snap.exit.delete_answered |= (event->fuota.control & 2) != 0;
			if (defer_class_c_keepalive &&
			    (snap.exit.status_answered || snap.exit.delete_answered)) {
				defer_class_c_keepalive = false;
				restart_keepalive_period(k_uptime_get());
			}
			emit(RZI_FUOTA_EVENT_FRAG_CONTROL);
		}
		break;
	default:
		/* FMP requests do not trigger installation or reboot. */
		break;
	}
}

static void run(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	struct rzi_lorawan_backend_event event;
	k_timeout_t timeout = K_FOREVER;
	for (;;) {
		int rc = k_msgq_get(&agent_events, &event, timeout);
		k_mutex_lock(&agent_lock, K_FOREVER);
		if (atomic_get(&started)) {
			processing = true;
			if (atomic_set(&overflowed, 0)) {
				/* Do not trust queued COMPLETE after an event has been lost. */
				k_msgq_purge(&agent_events);
				struct rzi_fuota_session_info current;
				int current_rc = ops->get_session_info(&current);
				if (current_rc == 0) {
					remember_session(&current);
					snap.data.generation = current.generation;
				}
				snap.data.ready = false;
				snap.exit.error = -RZI_ERR_OVERFLOW;
				if (current_rc != -RZI_ERR_NOT_FOUND) {
					if (snap.phase == RZI_FUOTA_READY &&
					    snap.session.generation) {
						snap.phase = RZI_FUOTA_PRECONFIG;
					}
					recover(RZI_FUOTA_EXIT_ERROR, -RZI_ERR_OVERFLOW);
				}
				emit(RZI_FUOTA_EVENT_ERROR);
			} else if (rc == 0) {
				handle(&event);
			}
			advance();
			processing = false;
		}
		/* Sleep until a real event or the next policy deadline. */
		int64_t wake = INT64_MAX;
		if (snap.phase == RZI_FUOTA_PRECONFIG || snap.phase == RZI_FUOTA_RECEIVING) {
			wake = deadline;
		} else if (snap.phase == RZI_FUOTA_RECOVERING && !step_failed) {
			wake = retry_at;
		} else if (snap.phase == RZI_FUOTA_FINALIZING && !step_failed) {
			wake = deadline;
		}
#if CONFIG_RZI_LORAWAN_FUOTA_KEEPALIVE_INTERVAL_S > 0
		if (periodic_allowed()) {
			wake = MIN(wake, keepalive_at);
		}
#endif
		timeout = wake == INT64_MAX ? K_FOREVER : K_MSEC(MAX(1, wake - k_uptime_get()));
		k_mutex_unlock(&agent_lock);
	}
}
K_THREAD_DEFINE(fuota_agent_thread, 3072, run, NULL, NULL, NULL, 9, 0, 0);

void fuota_agent_on_event(const struct rzi_lorawan_backend_event *event)
{
	if (!atomic_get(&started)) {
		return;
	}
	if (event->type != RZI_LORAWAN_BACKEND_FUOTA && event->type != RZI_LORAWAN_BACKEND_JOINED &&
	    event->type != RZI_LORAWAN_BACKEND_JOIN_FAILED &&
	    event->type != RZI_LORAWAN_BACKEND_STATE_CHANGED &&
	    event->type != RZI_LORAWAN_BACKEND_ERROR) {
		return;
	}
	if (k_msgq_put(&agent_events, event, K_NO_WAIT) != 0) {
		atomic_set(&overflowed, 1);
	}
}

bool fuota_agent_business_allowed(void)
{
	return !atomic_get(&blocked) && !(ops && ops->business_blocked && ops->business_blocked());
}

int fuota_agent_init(void)
{
	if (k_is_in_isr()) {
		return -RZI_ERR_ISR;
	}
	k_mutex_lock(&agent_lock, K_FOREVER);
	ops = rzi_lorawan_backend.fuota;
	int rc = 0;
	if (!ops || !ops->get_session_info || !ops->close_multicast || !ops->restore_settings ||
	    !ops->cleanup_uplink || !ops->release_data) {
		rc = -RZI_ERR_NOT_SUPPORTED;
	} else {
		atomic_set(&started, 1);
	}
	k_mutex_unlock(&agent_lock);
	return rc;
}

int rzi_fuota_get_snapshot(struct rzi_fuota_snapshot *out)
{
	if (k_is_in_isr()) {
		return -RZI_ERR_ISR;
	}
	if (!out) {
		return -RZI_ERR_INVALID;
	}
	k_mutex_lock(&agent_lock, K_FOREVER);
	*out = snap;
	k_mutex_unlock(&agent_lock);
	return 0;
}
int rzi_fuota_get_session_info(struct rzi_fuota_session_info *out)
{
	struct rzi_fuota_snapshot value;
	if (!out) {
		return -RZI_ERR_INVALID;
	}
	int rc = rzi_fuota_get_snapshot(&value);
	if (!rc) {
		*out = value.session;
	}
	return rc;
}
int rzi_fuota_get_data_info(struct rzi_fuota_data_info *out)
{
	struct rzi_fuota_snapshot value;
	if (!out) {
		return -RZI_ERR_INVALID;
	}
	int rc = rzi_fuota_get_snapshot(&value);
	if (!rc) {
		*out = value.data;
	}
	return rc;
}
int fuota_agent_set_expected_size(size_t size)
{
	/* Keep the caller hint separate from authoritative TS004 length. */
	if (k_is_in_isr()) {
		return -RZI_ERR_ISR;
	}
	if (!size) {
		return -RZI_ERR_INVALID;
	}
	k_mutex_lock(&agent_lock, K_FOREVER);
	expected_size = size;
	snap.data.expected_length = size;
	k_mutex_unlock(&agent_lock);
	return 0;
}
int fuota_agent_release_data(uint32_t generation)
{
	if (k_is_in_isr()) {
		return -RZI_ERR_ISR;
	}
	k_mutex_lock(&agent_lock, K_FOREVER);
	int rc = !generation || generation != snap.data.generation ? -RZI_ERR_NOT_FOUND
		 : snap.phase != RZI_FUOTA_READY                   ? -RZI_ERR_BUSY
		 : !snap.data.ready                                ? -RZI_ERR_NOT_FOUND
								   : ops->release_data(generation);
	if (!rc) {
		snap.data.ready = false;
		change_state(RZI_FUOTA_STATE_READY);
	}
	k_mutex_unlock(&agent_lock);
	return rc;
}

bool fuota_agent_can_stop(void)
{
	k_mutex_lock(&agent_lock, K_FOREVER);
	bool idle =
		!processing && (snap.phase == RZI_FUOTA_READY || snap.phase == RZI_FUOTA_OFFLINE);
	k_mutex_unlock(&agent_lock);
	return idle;
}

void fuota_agent_stopped(void)
{
	k_mutex_lock(&agent_lock, K_FOREVER);
	atomic_clear(&started);
	atomic_clear(&blocked);
	atomic_clear(&overflowed);
	k_msgq_purge(&agent_events);

	snap = (struct rzi_fuota_snapshot){0};
	state = RZI_FUOTA_STATE_IDLE;
	step_failed = false;
	expected_size = 0;
	k_mutex_unlock(&agent_lock);
}
