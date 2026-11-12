/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RZI_FUOTA_AGENT_INTERNAL_H
#define RZI_FUOTA_AGENT_INTERNAL_H

#include <rzi/lorawan/fuota.h>
#include "../../backend/lorawan_backend.h"

enum fuota_agent_notice_kind {
	FUOTA_AGENT_NOTICE_STATE_CHANGED,
	FUOTA_AGENT_NOTICE_SESSION_STARTED,
	FUOTA_AGENT_NOTICE_TRANSFER_RESULT,
	FUOTA_AGENT_NOTICE_EVENT,
};

struct fuota_agent_notice {
	enum fuota_agent_notice_kind kind;
	union {
		struct {
			enum rzi_fuota_state next;
			struct rzi_fuota_data_info data;
		} state;
		int transfer_result;
		const struct rzi_fuota_event *event;
	};
};

/* Synchronous call with agent_lock released. Payloads live until it returns. */
void fuota_service_on_agent_notice(const struct fuota_agent_notice *notice);
int fuota_agent_init(void);
void fuota_agent_on_event(const struct rzi_lorawan_backend_event *event);
bool fuota_agent_business_allowed(void);
bool fuota_agent_can_stop(void);
void fuota_agent_stopped(void);
int fuota_agent_set_expected_size(size_t size);
int fuota_agent_release_data(uint32_t generation);

#endif
