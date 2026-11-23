/* SPDX-License-Identifier: Apache-2.0 */
/* Include the real translation unit to exercise its private bit predicate. */
#include <assert.h>
#include <stdio.h>
#include "fragmentation_helper_v1.0.0.c"

static uint8_t storage[4096];

static int8_t store(uint32_t offset, uint8_t *data, uint32_t size)
{
	assert(offset <= sizeof(storage) && size <= sizeof(storage) - offset);
	memcpy(storage + offset, data, size);
	return 0;
}

static int8_t restore(uint32_t offset, uint8_t *data, uint32_t size)
{
	assert(offset <= sizeof(storage) && size <= sizeof(storage) - offset);
	memcpy(data, storage + offset, size);
	return 0;
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	assert(!IsPowerOfTwo(0));
	assert(IsPowerOfTwo(1));
	assert(IsPowerOfTwo(2));
	assert(!IsPowerOfTwo(3));
	assert(IsPowerOfTwo(UINT32_C(0x80000000)));
	assert(!IsPowerOfTwo(UINT32_C(0x80000001)));
	assert(!IsPowerOfTwo(UINT32_MAX));
	puts("FEC_BOUNDARIES_PASS cases=7");

	FragDecoderCallbacks_t callbacks = {.FragDecoderWrite = store, .FragDecoderRead = restore};
	FragDecoderInit(128, 32, &callbacks);
	FILE *queue = fopen(argv[1], "rb");
	assert(queue != NULL);
	uint8_t packet[35];
	unsigned count = 0;
	int length;
	int32_t result = FRAG_SESSION_ONGOING;
	while ((length = fgetc(queue)) != EOF) {
		assert(length == sizeof(packet));
		assert(fread(packet, 1, length, queue) == (size_t)length);
		assert(packet[0] == 0x08);
		uint16_t counter = (packet[1] | ((uint16_t)packet[2] << 8)) & 0x3fff;
		result = FragDecoderProcess(counter, packet + 3);
		count++;
		if (result >= 0) {
			break;
		}
	}
	fclose(queue);
	assert(result >= 0);
	for (unsigned i = 0; i < sizeof(storage); i++) {
		assert(storage[i] == (uint8_t)(37 * i + 11));
	}
	printf("FEC_RECONSTRUCTION_PASS fed=%u compared=%zu mismatches=0\n", count,
	       sizeof(storage));
	return 0;
}
