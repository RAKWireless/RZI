/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "fuota_event_log.h"

static void put_le16(uint8_t *dst, uint16_t value)
{
	dst[0] = (uint8_t)value;
	dst[1] = (uint8_t)(value >> 8);
}

bool fuota_event_log_record(const struct rzi_fuota_event *event, struct uplink_log_event *record)
{
	const struct rzi_fuota_snapshot *s = &event->snapshot;
	const struct rzi_fuota_session_info *session = &s->session;
	const struct rzi_fuota_rx_stats *rx = &s->exit.fragments;

	if (!(session->valid & RZI_FUOTA_SESSION_FRAGMENT) || !session->fragment_count ||
	    !session->fragment_size) {
		return false;
	}
	*record = (struct uplink_log_event){.domain = UPLINK_LOG_EVENT_DOMAIN_FUOTA};
	if (event->type == RZI_FUOTA_EVENT_PHASE && s->phase == RZI_FUOTA_RECEIVING) {
		record->code = UPLINK_LOG_EVENT_FUOTA_STARTED;
		put_le16(record->data, session->fragment_count);
		record->data[2] = session->fragment_size;
		record->data[3] = session->fragment_index;
		return true;
	}
	if (event->type != RZI_FUOTA_EVENT_FINISHED || s->data.ready || !rx->valid) {
		return false;
	}
	if (rx->write_failed) {
		/* The application header stores target_version at offsets 12..15. */
		if ((rx->prefix_valid & 0xf000U) != 0xf000U) {
			return false;
		}
		record->code = UPLINK_LOG_EVENT_FUOTA_FLASH_FAIL;
		memcpy(record->data, &rx->prefix[12], sizeof(record->data));
		return true;
	}
	if (s->exit.reason != RZI_FUOTA_EXIT_TIMEOUT || !s->exit.reception_started || rx->error) {
		return false;
	}
	record->code = UPLINK_LOG_EVENT_FUOTA_FRAG_INCOMPLETE;
	put_le16(record->data, rx->received);
	put_le16(record->data + 2, session->fragment_count);
	return true;
}
