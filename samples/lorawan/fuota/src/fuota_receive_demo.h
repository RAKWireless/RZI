/* SPDX-License-Identifier: Apache-2.0 */
#ifndef FUOTA_RECEIVE_DEMO_H
#define FUOTA_RECEIVE_DEMO_H

#include <stdint.h>

/* Application thread only. Read the entire settled generation before releasing it. */
int fuota_receive_demo_consume(uint32_t generation);

#endif
