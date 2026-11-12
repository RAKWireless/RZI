#include <rzi/lorawan/fuota_trace.h>
/* SPDX-License-Identifier: Apache-2.0 */
/* Included only for the USP v1 FUOTA backend. All mutable fields use the USP lock. */
#include <zephyr/sys/atomic.h>
#include <lorawan_fuota_observer.h>

static struct rzi_fuota_session_info usp_fuota_session;
static uint32_t usp_fuota_generation_counter;
static bool usp_fuota_in_preconfig;
static bool usp_fuota_context_reserved, usp_fuota_fragment_configured, usp_fuota_multicast_closed,
	usp_fuota_settings_restored;
static uint32_t usp_fuota_pending_cleanup_generation;
static smtc_modem_class_t usp_fuota_original_class;
static atomic_t usp_fuota_business_blocked;

static void usp_fuota_publish_event(enum rzi_lorawan_backend_fuota_kind kind, int error,
				    uint8_t control)
{
	struct rzi_lorawan_backend_event event = {
		.type = RZI_LORAWAN_BACKEND_FUOTA,
		.error = error,
		.fuota = {.kind = kind,
			  .generation = usp_fuota_session.generation,
			  .session = usp_fuota_session,
			  .control = control,
			  .successful = error == 0},
	};
	if (usp_fuota_session.valid & RZI_FUOTA_SESSION_FRAGMENT) {
		event.fuota.image_size = (uint32_t)usp_fuota_session.fragment_count *
						 usp_fuota_session.fragment_size -
					 usp_fuota_session.padding;
	}
	rzi_lorawan_usp_publish(&event);
}

bool lorawan_fuota_context_supported(uint8_t stack_id, uint8_t port, uint8_t index)
{
	/* This product supports one context (0), original Class A/C only. */
	smtc_modem_class_t current;
	if (stack_id != STACK_ID || index != 0 ||
	    smtc_modem_get_class(STACK_ID, &current) != SMTC_MODEM_RC_OK ||
	    current == SMTC_MODEM_CLASS_B) {
		return false;
	}
	if (usp_fuota_context_reserved) {
		/* Retained data cannot be overwritten even after TS004 delete. */
		return !usp_fuota_multicast_closed && !fuota_image_ready &&
		       !atomic_get(&usp_fuota_business_blocked) &&
		       (port != 201 || !usp_fuota_fragment_configured);
	}
	usp_fuota_original_class = current;
	usp_fuota_session = (struct rzi_fuota_session_info){
		.generation = ++usp_fuota_generation_counter,
		.valid = RZI_FUOTA_SESSION_SETTINGS,
		.original_class =
			current == SMTC_MODEM_CLASS_C ? RZI_LORAWAN_CLASS_C : RZI_LORAWAN_CLASS_A,
	};
	if (usp_fuota_generation_counter == 0) {
		usp_fuota_session.generation = ++usp_fuota_generation_counter;
	}
	usp_fuota_context_reserved = true;
	usp_fuota_in_preconfig = true;
	usp_fuota_fragment_configured = usp_fuota_multicast_closed = usp_fuota_settings_restored =
		false;
	return true;
}
bool lorawan_fuota_delete_preconfigured_fragment(uint8_t stack_id, uint32_t generation)
{
	if (stack_id != STACK_ID || !usp_fuota_context_reserved ||
	    generation != usp_fuota_session.generation || !usp_fuota_in_preconfig ||
	    !usp_fuota_fragment_configured || fuota_image_ready) {
		return false;
	}

	/* Called under the modem lock at the same point that TS004 drops its slot.
	 * The RX window of DeleteAns may already carry the next SetupReq. */
	usp_fuota_fragment_configured = false;
	usp_fuota_session.valid &= ~RZI_FUOTA_SESSION_FRAGMENT;
	usp_fuota_session.fragment_index = 0;
	usp_fuota_session.multicast_mask = 0;
	usp_fuota_session.fragment_count = 0;
	usp_fuota_session.fragment_size = 0;
	usp_fuota_session.padding = 0;
	usp_fuota_session.block_ack_delay = 0;
	usp_fuota_session.descriptor = 0;
	if (!(usp_fuota_session.valid & RZI_FUOTA_SESSION_GROUP)) {
		usp_fuota_context_reserved = false;
	}
	usp_fuota_publish_event(RZI_LORAWAN_BACKEND_FUOTA_FRAGMENT_DELETED, 0, 0);
	return true;
}

bool lorawan_fuota_storage_available(uint8_t stack_id)
{
	return stack_id == STACK_ID && !usp_fuota_fragment_configured && !fuota_image_ready;
}
uint32_t lorawan_fuota_generation(uint8_t stack_id)
{
	return stack_id == STACK_ID && usp_fuota_context_reserved ? usp_fuota_session.generation
								  : 0;
}
/* Convert the accepted GPS end once into the Agent's monotonic clock.
 * MAC fractions in this USP API are milliseconds; ALCSync has whole seconds.
 * Refresh at START so delayed event dispatch does not extend the original window.
 */
static void usp_fuota_update_window_deadline(void)
{
	uint32_t gps, fraction_ms = 0;
	if (smtc_modem_get_lorawan_mac_time(STACK_ID, &gps, &fraction_ms) != SMTC_MODEM_RC_OK) {
		if (smtc_modem_get_alcsync_time(STACK_ID, &gps) != SMTC_MODEM_RC_OK) {
			return; /* Keep a previously captured deadline if time is unavailable. */
		}
		fraction_ms = 0;
	}
	int64_t remaining =
		((int64_t)usp_fuota_session.session_time_gps + usp_fuota_session.timeout_s - gps) *
			1000LL -
		fraction_ms;
	usp_fuota_session.window_end_ms = k_uptime_get() + MAX(remaining, 0);
	usp_fuota_session.valid |= RZI_FUOTA_SESSION_DEADLINE;
}
void lorawan_fuota_observe(uint8_t stack_id, enum lbm_fuota_notice notice,
			   const struct lbm_fuota_info *info)
{
	if (stack_id != STACK_ID || !usp_fuota_context_reserved ||
	    info->generation != usp_fuota_session.generation) {
		return;
	}
	switch (notice) {
	case LBM_FUOTA_FRAGMENT_SETUP:
		usp_fuota_fragment_configured = true;
		usp_fuota_session.fragment_index = info->index;
		usp_fuota_session.fragment_count = info->fragment_count;
		usp_fuota_session.fragment_size = info->fragment_size;
		usp_fuota_session.padding = info->padding;
		usp_fuota_session.multicast_mask = info->multicast_mask;
		usp_fuota_session.descriptor = info->descriptor;
		usp_fuota_session.block_ack_delay = info->block_ack_delay;
		usp_fuota_session.valid |= RZI_FUOTA_SESSION_FRAGMENT;
		usp_fuota_publish_event(RZI_LORAWAN_BACKEND_FUOTA_PRECONFIG, 0, 0);
		break;
	case LBM_FUOTA_GROUP_SETUP:
		usp_fuota_session.multicast_group = info->index;
		usp_fuota_session.valid |= RZI_FUOTA_SESSION_GROUP;
		usp_fuota_publish_event(RZI_LORAWAN_BACKEND_FUOTA_PRECONFIG, 0, 0);
		break;
	case LBM_FUOTA_WINDOW_SETUP:
		usp_fuota_session.multicast_group = info->index;
		usp_fuota_session.valid |= RZI_FUOTA_SESSION_GROUP;
		usp_fuota_session.session_time_gps = info->session_time;
		usp_fuota_session.timeout_s = info->timeout_s;
		usp_fuota_session.frequency_hz = info->frequency;
		usp_fuota_session.data_rate = info->data_rate;
		usp_fuota_session.valid |= RZI_FUOTA_SESSION_WINDOW;
		usp_fuota_update_window_deadline();
		usp_fuota_publish_event(RZI_LORAWAN_BACKEND_FUOTA_PRECONFIG, 0, 0);
		break;
	case LBM_FUOTA_ENTERING:
		usp_fuota_in_preconfig = false;
		atomic_set(&usp_fuota_business_blocked, 1);
		break;
	case LBM_FUOTA_STARTED:
		usp_fuota_update_window_deadline();
		usp_fuota_publish_event(RZI_LORAWAN_BACKEND_FUOTA_SESSION_STARTED,
					info->successful ? 0 : -RZI_ERR_IO, 0);
		break;
	case LBM_FUOTA_ENDED:
		usp_fuota_in_preconfig = false;
		usp_fuota_publish_event(RZI_LORAWAN_BACKEND_FUOTA_SESSION_ENDED, 0, 0);
		break;
	case LBM_FUOTA_COMPLETE:
		usp_fuota_in_preconfig = false;
		fuota_image_ready = info->successful;
		usp_fuota_publish_event(RZI_LORAWAN_BACKEND_FUOTA_TRANSFER_DONE,
					info->successful ? 0
					: info->error    ? rzi_err_from_errno(info->error)
							 : -RZI_ERR_IO,
					0);
		break;
	case LBM_FUOTA_CONTROL_SENT:
		/* Supervisor completion is a local package transaction result.
		 * Remote LNS receipt is never inferred here. */
		usp_fuota_publish_event(RZI_LORAWAN_BACKEND_FUOTA_CONTROL_DONE,
					info->successful ? 0 : -RZI_ERR_IO, info->control);
		break;
	default:
		break;
	}
}

static int usp_fuota_check_generation(uint32_t generation)
{
	return usp_fuota_context_reserved && generation == usp_fuota_session.generation
		       ? 0
		       : -RZI_ERR_NOT_FOUND;
}
static int usp_fuota_close_multicast(uint32_t generation, struct rzi_fuota_rx_stats *stats)
{
	int rc = rzi_lorawan_usp_enter();
	if (rc) {
		return rc;
	}
	rc = usp_fuota_check_generation(generation);
	if (!rc && !usp_fuota_multicast_closed) {
		usp_fuota_in_preconfig = false;
		lorawan_remote_multicast_setup_cancel_class_c(STACK_ID,
							      usp_fuota_session.multicast_group);
		bool active;
		uint32_t frequency;
		uint8_t dr;
		rc = rzi_usp_result(smtc_modem_multicast_class_c_get_session_status(
			STACK_ID, usp_fuota_session.multicast_group, &active, &frequency, &dr));
		if (!rc && active) {
			rc = -RZI_ERR_BUSY;
		}
		if (!rc) {
			usp_fuota_multicast_closed = true;
		}
	}
	if (!rc && stats) {
		struct lbm_fuota_rx_stats rx;
		if (lorawan_fragmentation_get_rx_stats(generation, &rx)) {
			*stats = (struct rzi_fuota_rx_stats){
				.received = rx.received,
				.valid = true,
				.prefix_valid = rx.prefix_valid,
				.write_failed = rx.write_failed,
				.storage_errno = rx.storage_error,
				.error = rx.storage_error ? rzi_err_from_errno(rx.storage_error)
							  : (rx.decoder_failed ? -RZI_ERR_IO : 0),
			};
			memcpy(stats->prefix, rx.prefix, sizeof(stats->prefix));
		}
	}
	return rzi_lorawan_usp_finish(rc);
}
static int usp_fuota_restore_settings(uint32_t generation)
{
	int rc = rzi_lorawan_usp_enter();
	if (rc) {
		return rc;
	}
	rc = usp_fuota_check_generation(generation);
	if (!rc && !usp_fuota_multicast_closed) {
		rc = -RZI_ERR_BUSY;
	}
	if (!rc && !usp_fuota_settings_restored) {
		/* Multicast owns separate RX frequency/DR fields. Stopping it restores
		 * unicast parameters; always cycle the receiver even for original C. */
		rc = rzi_usp_result(smtc_modem_set_class(STACK_ID, SMTC_MODEM_CLASS_A));
		if (!rc) {
			rc = rzi_usp_result(
				smtc_modem_set_class(STACK_ID, usp_fuota_original_class));
		}
		smtc_modem_class_t actual;
		if (!rc) {
			rc = rzi_usp_result(smtc_modem_get_class(STACK_ID, &actual));
		}
		if (!rc && actual != usp_fuota_original_class) {
			rc = -RZI_ERR_IO;
		}
		if (!rc) {
			usp_fuota_settings_restored = true;
			atomic_clear(&usp_fuota_business_blocked);
		}
	}
	return rzi_lorawan_usp_finish(rc);
}
static int usp_fuota_request_keepalive(uint32_t generation)
{
#if CONFIG_RZI_LORAWAN_FUOTA_KEEPALIVE_INTERVAL_S > 0
	static const uint8_t keepalive[] = {0x00}; /* existing service keepalive */
	int rc = rzi_lorawan_usp_enter();
	if (rc) {
		return rc;
	}
	if (generation) {
		rc = usp_fuota_check_generation(generation);
		if (!rc && !usp_fuota_settings_restored) {
			rc = -RZI_ERR_BUSY;
		}
	}
	if (!rc && (tx_pending || usp_fuota_pending_cleanup_generation ||
		    lorawan_fragmentation_answer_pending(STACK_ID) ||
		    lorawan_multicast_answer_pending(STACK_ID))) {
		rc = -RZI_ERR_BUSY;
	}
	if (!rc) {
		RZI_FUOTA_TRACE(RZI_FUOTA_TRACE_API_ENTER, CONFIG_RZI_LORAWAN_FUOTA_KEEPALIVE_PORT, generation, 0);
		rc = rzi_usp_result(
			smtc_modem_request_uplink(STACK_ID, CONFIG_RZI_LORAWAN_FUOTA_KEEPALIVE_PORT,
						  false, keepalive, sizeof(keepalive)));
		RZI_FUOTA_TRACE(RZI_FUOTA_TRACE_API_RETURN, CONFIG_RZI_LORAWAN_FUOTA_KEEPALIVE_PORT, rc, generation);
		if (!rc) {
			tx_pending = true;
			usp_fuota_pending_cleanup_generation = generation;
		}
	}
	return rzi_lorawan_usp_finish(rc);
#else
	ARG_UNUSED(generation);
	return -RZI_ERR_NOT_SUPPORTED;
#endif
}
static void usp_fuota_complete_cleanup_uplink(int error)
{
	if (usp_fuota_pending_cleanup_generation) {
		if (usp_fuota_pending_cleanup_generation == usp_fuota_session.generation) {
			usp_fuota_publish_event(RZI_LORAWAN_BACKEND_FUOTA_CLEANUP_UPLINK_DONE,
						error, 0);
		}
		usp_fuota_pending_cleanup_generation = 0;
	}
}
static int usp_fuota_release_data(uint32_t generation)
{
	int rc = rzi_lorawan_usp_enter();
	if (rc) {
		return rc;
	}
	rc = usp_fuota_check_generation(generation);
	if (!rc && (!usp_fuota_multicast_closed || !usp_fuota_settings_restored ||
		    usp_fuota_pending_cleanup_generation)) {
		rc = -RZI_ERR_BUSY;
	}
	if (!rc) {
		lorawan_fragmentation_release(STACK_ID);
		usp_fuota_context_reserved = usp_fuota_fragment_configured = fuota_image_ready =
			false;
	}
	return rzi_lorawan_usp_finish(rc);
}
static bool usp_fuota_is_business_blocked(void)
{
	return atomic_get(&usp_fuota_business_blocked) != 0;
}
static int usp_fuota_get_fragmented_image_size(size_t *size)
{
	int rc = rzi_lorawan_usp_enter();
	if (rc) {
		return rc;
	}
	if (!size) {
		rc = -RZI_ERR_INVALID;
	} else if (!(usp_fuota_session.valid & RZI_FUOTA_SESSION_FRAGMENT)) {
		rc = -RZI_ERR_NO_DATA;
	} else {
		*size = (uint32_t)usp_fuota_session.fragment_count *
				usp_fuota_session.fragment_size -
			usp_fuota_session.padding;
	}
	return rzi_lorawan_usp_finish(rc);
}
static int usp_fuota_read_received_image(uint32_t offset, uint8_t *buffer, size_t size)
{
	int rc = rzi_lorawan_usp_enter();
	if (rc) {
		return rc;
	}
	size_t length =
		(uint32_t)usp_fuota_session.fragment_count * usp_fuota_session.fragment_size -
		usp_fuota_session.padding;
	if (!usp_fuota_context_reserved || !fuota_image_ready) {
		rc = -RZI_ERR_NOT_FOUND;
	} else if (!buffer || !size || offset > length || size > length - offset) {
		rc = -RZI_ERR_INVALID;
	} else {
		rc = rzi_err_from_errno(lorawan_fuota_read(offset, buffer, size));
	}
	return rzi_lorawan_usp_finish(rc);
}

bool lorawan_fuota_business_allowed(uint8_t stack)
{
	return stack == STACK_ID && !atomic_get(&usp_fuota_business_blocked);
}

static int usp_fuota_get_session_info(struct rzi_fuota_session_info *out)
{
	int rc = rzi_lorawan_usp_enter();
	if (rc) {
		return rc;
	}
	if (!out) {
		rc = -RZI_ERR_INVALID;
	} else if (!usp_fuota_context_reserved) {
		rc = -RZI_ERR_NOT_FOUND;
	} else {
		*out = usp_fuota_session;
	}
	return rzi_lorawan_usp_finish(rc);
}

static bool usp_fuota_is_holding_settings(void)
{
	return usp_fuota_context_reserved && !usp_fuota_settings_restored;
}

bool lorawan_fuota_class_b_supported(uint8_t stack)
{
	(void)stack;
	return false;
}
