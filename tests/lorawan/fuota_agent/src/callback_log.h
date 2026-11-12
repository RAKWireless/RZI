/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CALLBACK_LOG_H
#define CALLBACK_LOG_H

#include <rzi/lorawan/fuota.h>

/* Register a test listener that forwards to the fixture callbacks. */
void callback_log_start(const struct rzi_fuota_callbacks *fixture, bool replace_at_close);

/* Inject a TIME event before calling this to mark the end of the observed interval. */
void callback_log_expect(const char *scenario, const char *expected);

#endif
