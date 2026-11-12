/* SPDX-License-Identifier: Apache-2.0 */
/* Generate a test input using the existing provider parity helper, not an Agent codec. */
#include "fragmentation_helper_v1.0.0.c"
void fixture_fragment(const uint8_t *source, uint16_t count, uint8_t size, uint16_t counter,
		      uint8_t *out)
{
	if (counter <= count) {
		memcpy(out, source + (counter - 1) * size, size);
		return;
	}
	uint8_t row[(FRAG_MAX_NB + 7) / 8];
	memset(row, 0, sizeof(row));
	memset(out, 0, size);
	FragGetParityMatrixRow(counter - count, count, row);
	for (uint16_t i = 0; i < count; ++i) {
		if (GetParity(i, row)) {
			for (uint8_t k = 0; k < size; ++k) {
				out[k] ^= source[i * size + k];
			}
		}
	}
}
