/* SPDX-License-Identifier: Apache-2.0 */
/* Diagnostic records capture boot milliseconds at the source, never UART time.
 * No payload/keys, allocation, waiting or printing in producer/IRQ context.
 * Sequence gaps and cumulative lost count make queue loss detectable.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <rzi/lorawan/fuota_trace.h>
struct trace_record {
	uint32_t ms, seq, event, a, b, c;
};
K_MSGQ_DEFINE(trace_queue, sizeof(struct trace_record), 64, 4);
static atomic_t sequence, lost;
void rzi_fuota_trace(uint32_t event, uint32_t a, uint32_t b, uint32_t c)
{
	struct trace_record r = {
		k_uptime_get_32(), (uint32_t)atomic_inc(&sequence), event, a, b, c};
	if (k_msgq_put(&trace_queue, &r, K_NO_WAIT)) {
		atomic_inc(&lost);
	}
}
static void trace_output(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	struct trace_record r;
	for (;;) {
		k_msgq_get(&trace_queue, &r, K_FOREVER);
		printk("RZI_FUOTA_TRACE ms=%u seq=%u e=%u a=%u b=%u c=%u lost=%u\n", r.ms, r.seq, r.event,
		       r.a, r.b, r.c, (unsigned)atomic_get(&lost));
	}
}
K_THREAD_DEFINE(trace_thread, 1024, trace_output, NULL, NULL, NULL, 14, 0, 0);
