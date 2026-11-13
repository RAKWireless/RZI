/* SPDX-License-Identifier: Apache-2.0 */
#include <rzi/lorawan/fuota.h>
#include <zephyr/sys/util.h>

#include "fuota_receive_demo.h"

int fuota_receive_demo_consume(uint32_t generation)
{
	struct rzi_fuota_snapshot current;
	uint8_t chunk[32];
	int rc = rzi_fuota_get_snapshot(&current);

	if (rc) {
		return rc;
	}
	if (!current.data.ready || current.data.generation != generation) {
		return -RZI_ERR_NOT_FOUND;
	}
	if (current.phase != RZI_FUOTA_READY) {
		return -RZI_ERR_BUSY;
	}
	if (!current.data.length_valid || current.data.length == 0) {
		return -RZI_ERR_NO_DATA;
	}
	for (size_t offset = 0; offset < current.data.length;) {
		size_t count = MIN(sizeof(chunk), current.data.length - offset);

		rc = rzi_fuota_read_image(offset, chunk, count);
		if (rc) {
			return rc;
		}
		offset += count;
	}
	return rzi_fuota_release_data(generation);
}
