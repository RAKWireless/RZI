/* SPDX-License-Identifier: Apache-2.0 */
#ifndef FUOTA_EVENT_LOG_H
#define FUOTA_EVENT_LOG_H

#include <rzi/lorawan/fuota_agent.h>
#include <uplink_log.h>

/* Convert a managed receiver notification into a report for FPort 101. */
bool fuota_event_log_record(const struct rzi_fuota_event *event, struct uplink_log_event *record);

#endif
