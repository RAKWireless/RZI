/* SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless */
/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file
 * @brief Interrupt-driven UART I/O adapter for the RZI AT service.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>

#if defined(CONFIG_SOC_SERIES_STM32WLX) && DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(lpuart1))
#include <stm32_ll_bus.h>
#include <stm32_ll_usart.h>
#endif

#include <rzi/at/at.h>
#include <rzi/at/uart.h>

static const struct device *active_uart;
static uint32_t current_baud = 115200U;

static void ignore_result(int result)
{
	ARG_UNUSED(result);
}

static int uart_write(const uint8_t *data, size_t size, void *user_data)
{
	const struct device *uart = user_data;

	for (size_t i = 0; i < size; ++i) {
		uart_poll_out(uart, data[i]);
	}
	return 0;
}

#if defined(CONFIG_SOC_SERIES_STM32WLX) && DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(lpuart1))
static void pause_spins(uint32_t spins)
{
	for (uint32_t i = 0U; i < spins; i++) {
		__NOP();
	}
}

/*
 * Called with the UART IRQ still masked, before the system clock is restored.
 * The waking byte is already in the FIFO; keep reading until the line is quiet.
 */
void stm32wl_lpuart_wakeup_drain(void)
{
	uint32_t quiet = 0U;
	uint32_t total = 0U;

	if (active_uart != DEVICE_DT_GET(DT_NODELABEL(lpuart1)) ||
	    !LL_APB1_GRP2_IsEnabledClock(LL_APB1_GRP2_PERIPH_LPUART1)) {
		return;
	}
	if (!LL_USART_IsActiveFlag_WKUP(LPUART1) && !LL_USART_IsActiveFlag_RXNE(LPUART1)) {
		return;
	}
	for (;;) {
		if (LL_USART_IsActiveFlag_RXNE(LPUART1)) {
			uint8_t byte = (uint8_t)LL_USART_ReceiveData8(LPUART1);

			if (LL_USART_IsActiveFlag_ORE(LPUART1)) {
				LL_USART_ClearFlag_ORE(LPUART1);
			}
			ignore_result(rzi_at_receive(&byte, 1U));
			quiet = 0U;
			continue;
		}
		if (quiet >= 100000U || total >= 2000000U) {
			break;
		}
		pause_spins(1U);
		quiet++;
		total++;
	}
	LL_USART_ClearFlag_WKUP(LPUART1);
}
#endif

static void uart_isr(const struct device *uart, void *user_data)
{
	uint8_t buffer[32];

	ARG_UNUSED(user_data);
	uart_irq_update(uart);
	while (uart_irq_rx_ready(uart)) {
		int count = uart_fifo_read(uart, buffer, sizeof(buffer));

		if (count <= 0) {
			break;
		}
		ignore_result(rzi_at_receive(buffer, (size_t)count));
	}
}

int rzi_at_uart_init(const struct device *uart)
{
	struct rzi_at_io io = {
		.write = uart_write,
		.user_data = (void *)uart,
	};
	int rc;

	if (uart == NULL || !device_is_ready(uart)) {
		return -RZI_ERR_NO_DEVICE;
	}
	if (active_uart != NULL) {
		return -RZI_ERR_ALREADY;
	}
	rc = uart_irq_callback_user_data_set(uart, uart_isr, NULL);
	if (rc != 0) {
		return rzi_err_from_errno(rc);
	}
	active_uart = uart;
	rc = rzi_at_init(&io);
	if (rc != 0) {
		active_uart = NULL;
		(void)uart_irq_callback_user_data_set(uart, NULL, NULL);
		return rc;
	}
	uart_irq_rx_enable(uart);
	return 0;
}

int rzi_at_uart_get_baud(uint32_t *baud_rate)
{
	if (baud_rate == NULL) {
		return -RZI_ERR_INVALID;
	}
	if (active_uart == NULL) {
		return -RZI_ERR_NOT_READY;
	}
	*baud_rate = current_baud;
	return 0;
}

int rzi_at_uart_set_baud(uint32_t baud_rate)
{
	struct uart_config config;
	int rc;

	if (baud_rate == 0U) {
		return -RZI_ERR_INVALID;
	}
	if (active_uart == NULL) {
		return -RZI_ERR_NOT_READY;
	}
	rc = uart_config_get(active_uart, &config);
	if (rc != 0) {
		return rzi_err_from_errno(rc);
	}
	config.baudrate = baud_rate;
	rc = uart_configure(active_uart, &config);
	if (rc != 0) {
		return rzi_err_from_errno(rc);
	}
	current_baud = baud_rate;
	return 0;
}
