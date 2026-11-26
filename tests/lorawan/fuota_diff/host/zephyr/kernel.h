/* Host stand-in: this test never runs inside an interrupt. */
#ifndef ZEPHYR_KERNEL_H
#define ZEPHYR_KERNEL_H

static inline int k_is_in_isr(void)
{
	return 0;
}

#endif
