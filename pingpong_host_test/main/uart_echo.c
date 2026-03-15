/*
 * UART Echo demo — reads bytes from a PTY and echoes them back.
 *
 * 1. Start socat:   ./scripts/socat_pty.sh
 * 2. Build & run:   idf.py build && ./build/pingpong_host_test.elf
 * 3. Test:          echo "hello" > /tmp/ttyEXT0   (or use minicom -D /tmp/ttyEXT0)
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/uart_linux.h"

#define UART_PORT       UART_NUM_0
#define BUF_SIZE        256
#define RX_BUF_SIZE     1024

static const char *TAG = "uart_echo";

void app_main(void)
{
    ESP_LOGI(TAG, "UART echo demo starting...");

    /* Set PTY path before installing the driver */
    uart_linux_set_device_path(UART_PORT, "/tmp/ttyVESP0");

    /* Configure UART parameters */
    uart_config_t uart_config = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    QueueHandle_t uart_queue;
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, RX_BUF_SIZE, 0, 16, &uart_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_config));

    ESP_LOGI(TAG, "UART echo ready — send data to /tmp/ttyEXT0");

    uint8_t buf[BUF_SIZE];
    while (1) {
        int len = uart_read_bytes(UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (len > 0) {
            ESP_LOGI(TAG, "RX %d bytes", len);
            ESP_LOG_BUFFER_HEXDUMP(TAG, buf, len, ESP_LOG_INFO);

            /* Echo back */
            uart_write_bytes(UART_PORT, buf, len);
        }
    }
}
