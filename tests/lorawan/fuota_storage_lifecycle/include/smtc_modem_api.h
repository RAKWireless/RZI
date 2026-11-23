/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
struct smtc_modem_fuota_hooks {
	uint8_t (*session_setup)(uint32_t descriptor, uint16_t frag_nb, uint8_t frag_size,
				 uint8_t padding);
	int (*store)(uint32_t offset, const uint8_t *data, uint32_t size);
	int (*restore)(uint32_t offset, uint8_t *data, uint32_t size);
	/** Called after FragSessionDelete clears the session and before the next command. */
	void (*session_delete)(uint32_t generation);
};

void smtc_modem_fuota_set_hooks(const struct smtc_modem_fuota_hooks *);
uint32_t smtc_modem_fuota_generation(void);
