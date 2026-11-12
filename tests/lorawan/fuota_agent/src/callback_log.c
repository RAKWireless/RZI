/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/ztest.h>
#include "callback_log.h"

struct listener {
	const char *name;
};

struct observation {
	const char *label;
	const struct listener *listener;
	k_tid_t thread;
	int result;
};

static const struct listener first = {"first"}, replacement = {"replacement"};
static const struct rzi_fuota_callbacks *fixture_callbacks;
static struct rzi_fuota_callbacks observer;
static bool replace_on_closed;
static k_tid_t scenario_thread;
K_MSGQ_DEFINE(observations, sizeof(struct observation), 48, __alignof__(struct observation));

static void record(const char *label, int result, void *user)
{
	struct observation entry = {label, user, k_current_get(), result};

	zassert_ok(k_msgq_put(&observations, &entry, K_NO_WAIT), "callback recorder overflow");
}

static void state_changed(enum rzi_fuota_state state, void *user)
{
	static const char *const labels[] = {
		[RZI_FUOTA_STATE_IDLE] = "state.IDLE",
		[RZI_FUOTA_STATE_READY] = "state.READY",
		[RZI_FUOTA_STATE_TRANSFERRING] = "state.TRANSFERRING",
		[RZI_FUOTA_STATE_COMPLETE] = "state.COMPLETE",
		[RZI_FUOTA_STATE_FAILED] = "state.FAILED",
		[RZI_FUOTA_STATE_APPLYING] = "state.APPLYING",
	};

	zassert_true((unsigned int)state < ARRAY_SIZE(labels) && labels[state]);
	record(labels[state], 0, user);
}

static void session_started(void *user)
{
	record("started", 0, user);
}

static void session_ended(void *user)
{
	record("ended", 0, user);
}

static void replacement_ended(void *user)
{
	zassert_equal_ptr(user, &replacement);
	record("ended.replacement", 0, user);
}

static void complete(int result, void *user)
{
	const char *label = "complete.other";

	if (result == 0) {
		label = "complete.OK";
	} else if (result == -RZI_ERR_IO) {
		label = "complete.IO";
	} else if (result == -RZI_ERR_TIMEOUT) {
		label = "complete.TIMEOUT";
	}
	record(label, result, user);
	fixture_callbacks->complete(result, fixture_callbacks->user_data);
}

static const char *event_label(const struct rzi_fuota_event *event)
{
	static const char *const phases[] = {
		[RZI_FUOTA_OFFLINE] = "phase.OFFLINE",
		[RZI_FUOTA_READY] = "phase.READY",
		[RZI_FUOTA_PRECONFIG] = "phase.PRECONFIG",
		[RZI_FUOTA_RECEIVING] = "phase.RECEIVING",
		[RZI_FUOTA_RECOVERING] = "phase.RECOVERING",
		[RZI_FUOTA_FINALIZING] = "phase.FINALIZING",
	};

	switch (event->type) {
	case RZI_FUOTA_EVENT_PHASE:
		zassert_true((unsigned int)event->snapshot.phase < ARRAY_SIZE(phases));
		return phases[event->snapshot.phase];
	case RZI_FUOTA_EVENT_DATA_READY:
		return "data_ready";
	case RZI_FUOTA_EVENT_CLOSED:
		return "closed";
	case RZI_FUOTA_EVENT_RESTORED:
		return "restored";
	case RZI_FUOTA_EVENT_FRAG_CONTROL:
		if (event->snapshot.exit.status_answered && event->snapshot.exit.delete_answered) {
			return "control.status+delete";
		}
		return event->snapshot.exit.status_answered ? "control.status" : "control.other";
	case RZI_FUOTA_EVENT_FINISHED:
		return "finished";
	case RZI_FUOTA_EVENT_ERROR:
		return "error";
	case RZI_FUOTA_EVENT_TIME:
		return "barrier";
	default:
		return "unexpected";
	}
}

static void agent_event(const struct rzi_fuota_event *event, void *user)
{
	record(event_label(event), 0, user);
	if (event->type == RZI_FUOTA_EVENT_CLOSED && replace_on_closed) {
		struct rzi_fuota_status status;
		struct rzi_fuota_snapshot snapshot;
		struct rzi_fuota_callbacks next = observer;

		zassert_equal_ptr(user, &first);
		zassert_ok(rzi_fuota_get_status(&status));
		zassert_ok(rzi_fuota_get_snapshot(&snapshot));
		zassert_equal(snapshot.session.generation, event->snapshot.session.generation);
		next.session_ended = replacement_ended;
		next.user_data = (void *)&replacement;
		zassert_ok(rzi_fuota_register_callbacks(&next));
	}
	fixture_callbacks->agent_event(event, fixture_callbacks->user_data);
}

void callback_log_start(const struct rzi_fuota_callbacks *fixture, bool replace_at_close)
{
	fixture_callbacks = fixture;
	replace_on_closed = replace_at_close;
	scenario_thread = k_current_get();
	k_msgq_purge(&observations);
	observer = (struct rzi_fuota_callbacks){
		.state_changed = state_changed,
		.session_started = session_started,
		.session_ended = session_ended,
		.complete = complete,
		.agent_event = agent_event,
		.user_data = (void *)&first,
	};
	zassert_ok(rzi_fuota_register_callbacks(&observer));
}

void callback_log_expect(const char *scenario, const char *expected)
{
	struct observation entry;
	const struct listener *active_listener = &first;
	unsigned int count = 0;
	k_tid_t worker = NULL;

	for (;;) {
		zassert_ok(k_msgq_get(&observations, &entry, K_SECONDS(1)), "missing TIME barrier");
		if (!strcmp(entry.label, "barrier")) {
			break;
		}
		const char *end = strchr(expected, ' ');
		size_t len = end ? (size_t)(end - expected) : strlen(expected);
		bool caller = entry.thread == scenario_thread;
		/* A caret marks the synchronous callback made by explicit release_data(). */
		bool expect_caller = expected[0] == '^';
		const char *label = expected + (expect_caller ? 1 : 0);

		printk("CB_BASELINE %s %u %s %s %s %d\n", scenario, ++count, entry.label,
		       entry.listener->name, caller ? "caller" : "worker", entry.result);
		zassert_true(len > 0 && strlen(entry.label) == len - (expect_caller ? 1U : 0U) &&
				     !strncmp(entry.label, label, strlen(entry.label)),
			     "%s #%u got %s, remaining: %s", scenario, count, entry.label,
			     expected);
		zassert_equal(caller, expect_caller, "%s #%u caller context", scenario, count);
		if (!caller) {
			if (!worker) {
				worker = entry.thread;
			}
			zassert_equal_ptr(entry.thread, worker);
		}
		zassert_equal_ptr(entry.listener, active_listener);
		if (replace_on_closed && !strcmp(entry.label, "closed")) {
			active_listener = &replacement;
		}
		expected += len + (end ? 1U : 0U);
	}
	zassert_equal(*expected, '\0', "%s missing callbacks: %s", scenario, expected);
	printk("CB_TOTAL %s %u\n", scenario, count);
	zassert_ok(rzi_fuota_register_callbacks(fixture_callbacks));
}
