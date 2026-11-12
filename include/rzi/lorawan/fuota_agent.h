/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RZI_FUOTA_AGENT_H
#define RZI_FUOTA_AGENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** FUOTA receive phases. DATA_READY is independent of recovery and cleanup. */
enum rzi_fuota_phase {
	RZI_FUOTA_OFFLINE,
	RZI_FUOTA_READY,
	RZI_FUOTA_PRECONFIG,
	RZI_FUOTA_RECEIVING,
	RZI_FUOTA_RECOVERING,
	RZI_FUOTA_FINALIZING,
};
enum rzi_fuota_time_source {
	RZI_FUOTA_TIME_UNKNOWN,
	RZI_FUOTA_TIME_MAC,
	RZI_FUOTA_TIME_ALCSYNC,
};
enum rzi_fuota_exit_reason {
	RZI_FUOTA_EXIT_NONE,
	RZI_FUOTA_EXIT_COMPLETE,
	RZI_FUOTA_EXIT_TIMEOUT,
	RZI_FUOTA_EXIT_ERROR,
	RZI_FUOTA_EXIT_OFFLINE,
	RZI_FUOTA_EXIT_DELETED, /* Fragment-only preconfiguration cancelled by TS004. */
};
enum rzi_fuota_event_type {
	RZI_FUOTA_EVENT_PHASE,
	RZI_FUOTA_EVENT_TIME,
	RZI_FUOTA_EVENT_DATA_READY,
	RZI_FUOTA_EVENT_CLOSED,
	RZI_FUOTA_EVENT_RESTORED,
	RZI_FUOTA_EVENT_CLEANUP_UPLINK,
	RZI_FUOTA_EVENT_FRAG_CONTROL,
	RZI_FUOTA_EVENT_FINISHED,
	RZI_FUOTA_EVENT_ERROR,
};
/** Known session fields; zero is a valid value, not an unknown marker. */
#define RZI_FUOTA_SESSION_GROUP    (1U << 0)
#define RZI_FUOTA_SESSION_FRAGMENT (1U << 1)
#define RZI_FUOTA_SESSION_WINDOW   (1U << 2)
#define RZI_FUOTA_SESSION_SETTINGS (1U << 3)
#define RZI_FUOTA_SESSION_DEADLINE (1U << 4)

/** Value-owned transport metadata; contains no keys or private backend pointers. */
struct rzi_fuota_session_info {
	uint32_t generation;
	uint32_t valid;
	uint8_t multicast_group;
	uint8_t fragment_index;
	uint8_t multicast_mask;
	uint16_t fragment_count;
	uint8_t fragment_size;
	uint8_t padding;
	uint8_t block_ack_delay;
	uint32_t descriptor;
	uint32_t session_time_gps;
	uint32_t timeout_s;
	/** Original window end on k_uptime_get() clock; valid with DEADLINE.
	 * Preserved when multicast is cancelled early. Never restarted at completion.
	 */
	int64_t window_end_ms;
	uint32_t frequency_hz;
	uint8_t data_rate;
	uint8_t original_class; /* enum rzi_lorawan_class */
};
/** Data remains owned by the backend until explicit release, including after delete. */
struct rzi_fuota_data_info {
	uint32_t generation;
	size_t length;
	/** Optional application hint; never replaces transport metadata. */
	size_t expected_length;
	bool length_valid;
	bool ready;
};
/** Frozen under the modem lock when multicast reception closes. */
struct rzi_fuota_rx_stats {
	uint16_t received;     /* Distinct accepted fragment numbers, including redundancy. */
	uint8_t prefix[16];    /* Received data bytes at logical offsets 0..15. */
	uint16_t prefix_valid; /* Bit i is set when prefix[i] is available. */
	bool valid;
	bool write_failed;
	int storage_errno; /* Original negative errno from checked storage, or zero. */
	int error;         /* Decoder or checked-storage result in RZI error space. */
};

/** Local facts only. A transmitted answer never implies remote LNS confirmation. */
struct rzi_fuota_exit_state {
	enum rzi_fuota_exit_reason reason;
	bool multicast_closed;
	bool settings_restored;
	bool cleanup_uplink_done; /* legacy field: false for keepalive-only recovery */
	bool status_answered;
	bool delete_answered;
	bool lns_confirmed; /* reserved for a future externally verified result */
	int error;
	int cleanup_error;
	bool reception_started;
	struct rzi_fuota_rx_stats fragments;
};
struct rzi_fuota_snapshot {
	enum rzi_fuota_phase phase;
	struct rzi_fuota_session_info session;
	struct rzi_fuota_data_info data;
	struct rzi_fuota_exit_state exit;
	enum rzi_fuota_time_source time_source;
	bool time_valid;
	int64_t time_updated_ms;
};
/** Callback-scoped immutable copy. Copy the value for deferred application work. */
struct rzi_fuota_event {
	enum rzi_fuota_event_type type;
	struct rzi_fuota_snapshot snapshot;
};

/** Thread context only; NULL -> INVALID, ISR -> ISR. Requires the FUOTA receive agent. */
int rzi_fuota_get_snapshot(struct rzi_fuota_snapshot *out);
int rzi_fuota_get_session_info(struct rzi_fuota_session_info *out);
int rzi_fuota_get_data_info(struct rzi_fuota_data_info *out);
/** Release through the backend, after recovery/cleanup; stale generation -> NOT_FOUND.
 * BUSY while active, while recovery failed, or until local cleanup settles.
 * After success all read handles for that generation are invalid. No implicit erase.
 */
int rzi_fuota_release_data(uint32_t generation);

#ifdef __cplusplus
}
#endif
#endif
