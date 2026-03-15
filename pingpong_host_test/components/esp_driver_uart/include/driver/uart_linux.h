/*
 * Linux-specific UART PTY shim extensions.
 *
 * Call uart_linux_set_device_path() before uart_driver_install() to override
 * the Kconfig default PTY path for a given port.
 */

#pragma once

#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set the PTY device path for a UART port.
 *
 * Must be called before uart_driver_install() for the given port.
 *
 * @param uart_num  UART port number (0 .. UART_NUM_MAX-1)
 * @param path      Null-terminated path, e.g. "/tmp/ttyVESP0"
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if uart_num is out of range or path is NULL
 */
esp_err_t uart_linux_set_device_path(uart_port_t uart_num, const char *path);

#ifdef __cplusplus
}
#endif
