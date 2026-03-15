/*
 * UART driver shim for ESP-IDF Linux target.
 *
 * Implements the ESP-IDF UART API on top of POSIX PTY devices so that
 * firmware can talk to external tools (minicom, socat, etc.) when running
 * under the linux host simulator.
 */

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>

#include "driver/uart.h"
#include "driver/uart_linux.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"

static const char *TAG = "uart_linux";

/* ── Per-port runtime state ─────────────────────────────────────────────── */

typedef struct {
    bool              installed;
    int               fd;
    char              device_path[128];
    uart_config_t     config;
    RingbufHandle_t   rx_ring;
    int               rx_buffered_len;
    SemaphoreHandle_t rx_mux;
    QueueHandle_t     event_queue;
    TaskHandle_t      rx_task_handle;
    volatile bool     rx_task_stop;
} uart_obj_t;

static uart_obj_t *port_obj[UART_NUM_MAX];

/* ── Default device paths from Kconfig ──────────────────────────────────── */

static const char *default_paths[UART_NUM_MAX] = {
#ifdef CONFIG_UART_LINUX_PTY0_PATH
    CONFIG_UART_LINUX_PTY0_PATH,
#else
    "/tmp/ttyVESP0",
#endif
#ifdef CONFIG_UART_LINUX_PTY1_PATH
    CONFIG_UART_LINUX_PTY1_PATH,
#else
    "/tmp/ttyVESP1",
#endif
#ifdef CONFIG_UART_LINUX_PTY2_PATH
    CONFIG_UART_LINUX_PTY2_PATH,
#else
    "/tmp/ttyVESP2",
#endif
};

/* Pre-install path overrides (set by uart_linux_set_device_path) */
static char path_override[UART_NUM_MAX][128];

/* ── Helpers ────────────────────────────────────────────────────────────── */

static bool port_valid(uart_port_t num)
{
    return (unsigned)num < UART_NUM_MAX;
}

static speed_t baud_to_speed(int baud)
{
    switch (baud) {
        case 0:       return B0;
        case 50:      return B50;
        case 75:      return B75;
        case 110:     return B110;
        case 134:     return B134;
        case 150:     return B150;
        case 200:     return B200;
        case 300:     return B300;
        case 600:     return B600;
        case 1200:    return B1200;
        case 1800:    return B1800;
        case 2400:    return B2400;
        case 4800:    return B4800;
        case 9600:    return B9600;
        case 19200:   return B19200;
        case 38400:   return B38400;
        case 57600:   return B57600;
        case 115200:  return B115200;
        case 230400:  return B230400;
        case 460800:  return B460800;
        case 500000:  return B500000;
        case 576000:  return B576000;
        case 921600:  return B921600;
        case 1000000: return B1000000;
        case 1152000: return B1152000;
        case 1500000: return B1500000;
        case 2000000: return B2000000;
        case 2500000: return B2500000;
        case 3000000: return B3000000;
        case 3500000: return B3500000;
        case 4000000: return B4000000;
        default:      return B115200;
    }
}

static void apply_termios(int fd, const uart_config_t *cfg)
{
    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    tcgetattr(fd, &tty);
    cfmakeraw(&tty);

    /* Baud */
    speed_t spd = baud_to_speed(cfg->baud_rate);
    cfsetispeed(&tty, spd);
    cfsetospeed(&tty, spd);

    /* Data bits */
    tty.c_cflag &= ~CSIZE;
    switch (cfg->data_bits) {
        case UART_DATA_5_BITS: tty.c_cflag |= CS5; break;
        case UART_DATA_6_BITS: tty.c_cflag |= CS6; break;
        case UART_DATA_7_BITS: tty.c_cflag |= CS7; break;
        default:               tty.c_cflag |= CS8; break;
    }

    /* Stop bits */
    if (cfg->stop_bits == UART_STOP_BITS_2) {
        tty.c_cflag |= CSTOPB;
    } else {
        tty.c_cflag &= ~CSTOPB;
    }

    /* Parity */
    switch (cfg->parity) {
        case UART_PARITY_EVEN:
            tty.c_cflag |= PARENB;
            tty.c_cflag &= ~PARODD;
            break;
        case UART_PARITY_ODD:
            tty.c_cflag |= PARENB;
            tty.c_cflag |= PARODD;
            break;
        default:
            tty.c_cflag &= ~PARENB;
            break;
    }

    /* No modem control, enable receiver */
    tty.c_cflag |= (CLOCAL | CREAD);

    /* Non-blocking reads handled by poll() in RX task */
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    tcsetattr(fd, TCSANOW, &tty);
}

/* ── RX reader task ─────────────────────────────────────────────────────── */

static void rx_task(void *arg)
{
    uart_port_t num = (uart_port_t)(uintptr_t)arg;
    uart_obj_t *obj = port_obj[num];
    uint8_t buf[256];

    /* Use a non-blocking duplicate fd for reads so that the main fd
       stays blocking for write operations. */
    int rx_fd = dup(obj->fd);
    if (rx_fd < 0) {
        ESP_LOGE(TAG, "UART%d: dup() failed: %s", num, strerror(errno));
        vTaskDelete(NULL);
        return;
    }
    int fl = fcntl(rx_fd, F_GETFL, 0);
    fcntl(rx_fd, F_SETFL, fl | O_NONBLOCK);

    while (!obj->rx_task_stop) {
        ssize_t n = read(rx_fd, buf, sizeof(buf));
        if (n > 0) {
            xSemaphoreTake(obj->rx_mux, portMAX_DELAY);
            if (xRingbufferSend(obj->rx_ring, buf, (size_t)n, 0) == pdTRUE) {
                obj->rx_buffered_len += (int)n;
            }
            xSemaphoreGive(obj->rx_mux);

            if (obj->event_queue) {
                uart_event_t evt = {
                    .type = UART_DATA,
                    .size = (size_t)n,
                    .timeout_flag = false,
                };
                xQueueSend(obj->event_queue, &evt, 0);
            }
        } else {
            /* No data available — yield via FreeRTOS (not POSIX poll/sleep)
               so the Linux FreeRTOS scheduler can run other tasks. */
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    close(rx_fd);
    vTaskDelete(NULL);
}

/* ── Linux-specific API ─────────────────────────────────────────────────── */

esp_err_t uart_linux_set_device_path(uart_port_t uart_num, const char *path)
{
    if (!port_valid(uart_num) || !path) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(path_override[uart_num], path, sizeof(path_override[0]) - 1);
    path_override[uart_num][sizeof(path_override[0]) - 1] = '\0';
    return ESP_OK;
}

/* ── Core driver functions ──────────────────────────────────────────────── */

esp_err_t uart_driver_install(uart_port_t uart_num, int rx_buffer_size, int tx_buffer_size,
                              int queue_size, QueueHandle_t *uart_queue, int intr_alloc_flags)
{
    (void)tx_buffer_size;
    (void)intr_alloc_flags;

    if (!port_valid(uart_num)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (port_obj[uart_num] && port_obj[uart_num]->installed) {
        ESP_LOGW(TAG, "UART%d already installed", uart_num);
        return ESP_FAIL;
    }

    uart_obj_t *obj = calloc(1, sizeof(uart_obj_t));
    if (!obj) {
        return ESP_ERR_NO_MEM;
    }

    /* Determine device path */
    const char *path = (path_override[uart_num][0] != '\0')
                           ? path_override[uart_num]
                           : default_paths[uart_num];
    strncpy(obj->device_path, path, sizeof(obj->device_path) - 1);

    /* Open PTY device */
    obj->fd = open(obj->device_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (obj->fd < 0) {
        ESP_LOGE(TAG, "UART%d: failed to open %s: %s", uart_num, obj->device_path, strerror(errno));
        free(obj);
        return ESP_FAIL;
    }

    /* Clear O_NONBLOCK after open — we use poll() for timed reads */
    int flags = fcntl(obj->fd, F_GETFL, 0);
    fcntl(obj->fd, F_SETFL, flags & ~O_NONBLOCK);

    /* Apply raw mode */
    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    tcgetattr(obj->fd, &tty);
    cfmakeraw(&tty);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;
    tcsetattr(obj->fd, TCSANOW, &tty);

    /* RX ring buffer */
    if (rx_buffer_size < 128) {
        rx_buffer_size = 128;
    }
    obj->rx_ring = xRingbufferCreate(rx_buffer_size, RINGBUF_TYPE_BYTEBUF);
    if (!obj->rx_ring) {
        close(obj->fd);
        free(obj);
        return ESP_ERR_NO_MEM;
    }

    obj->rx_mux = xSemaphoreCreateMutex();
    if (!obj->rx_mux) {
        vRingbufferDelete(obj->rx_ring);
        close(obj->fd);
        free(obj);
        return ESP_ERR_NO_MEM;
    }

    /* Event queue */
    if (queue_size > 0) {
        obj->event_queue = xQueueCreate(queue_size, sizeof(uart_event_t));
        if (uart_queue) {
            *uart_queue = obj->event_queue;
        }
    } else {
        obj->event_queue = NULL;
        if (uart_queue) {
            *uart_queue = NULL;
        }
    }

    /* Publish the object before starting the RX task — on the Linux
       FreeRTOS port xTaskCreate may run the new task immediately. */
    obj->installed = true;
    port_obj[uart_num] = obj;

    /* Start RX reader task */
    obj->rx_task_stop = false;
    int stack_size = 4096;
    int prio = 5;
#ifdef CONFIG_UART_LINUX_RX_TASK_STACK
    stack_size = CONFIG_UART_LINUX_RX_TASK_STACK;
#endif
#ifdef CONFIG_UART_LINUX_RX_TASK_PRIO
    prio = CONFIG_UART_LINUX_RX_TASK_PRIO;
#endif
    char name[16];
    snprintf(name, sizeof(name), "uart%d_rx", uart_num);
    BaseType_t xret = xTaskCreate(rx_task, name, stack_size,
                                  (void *)(uintptr_t)uart_num, prio,
                                  &obj->rx_task_handle);
    if (xret != pdPASS) {
        port_obj[uart_num] = NULL;
        obj->installed = false;
        if (obj->event_queue) vQueueDelete(obj->event_queue);
        vSemaphoreDelete(obj->rx_mux);
        vRingbufferDelete(obj->rx_ring);
        close(obj->fd);
        free(obj);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "UART%d installed on %s (fd=%d)", uart_num, obj->device_path, obj->fd);
    return ESP_OK;
}

esp_err_t uart_driver_delete(uart_port_t uart_num)
{
    if (!port_valid(uart_num) || !port_obj[uart_num] || !port_obj[uart_num]->installed) {
        return ESP_FAIL;
    }

    uart_obj_t *obj = port_obj[uart_num];

    /* Signal RX task to stop and wait for it */
    obj->rx_task_stop = true;
    /* Give the task time to notice (it polls every 50 ms) */
    vTaskDelay(pdMS_TO_TICKS(120));

    if (obj->fd >= 0) {
        close(obj->fd);
        obj->fd = -1;
    }

    if (obj->event_queue) {
        vQueueDelete(obj->event_queue);
    }
    vSemaphoreDelete(obj->rx_mux);
    vRingbufferDelete(obj->rx_ring);

    obj->installed = false;
    free(obj);
    port_obj[uart_num] = NULL;

    ESP_LOGI(TAG, "UART%d driver deleted", uart_num);
    return ESP_OK;
}

bool uart_is_driver_installed(uart_port_t uart_num)
{
    if (!port_valid(uart_num)) return false;
    return port_obj[uart_num] && port_obj[uart_num]->installed;
}

/* ── Configuration ──────────────────────────────────────────────────────── */

esp_err_t uart_param_config(uart_port_t uart_num, const uart_config_t *uart_config)
{
    if (!port_valid(uart_num) || !uart_config) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Store config — we might not be installed yet */
    if (port_obj[uart_num]) {
        port_obj[uart_num]->config = *uart_config;
        apply_termios(port_obj[uart_num]->fd, uart_config);
    }
    /* If not installed, the config is only used for logging — save a static copy */

    ESP_LOGI(TAG, "UART%d configured: baud=%d, data=%d, parity=%d, stop=%d",
             uart_num, uart_config->baud_rate, uart_config->data_bits,
             uart_config->parity, uart_config->stop_bits);
    return ESP_OK;
}

esp_err_t uart_intr_config(uart_port_t uart_num, const uart_intr_config_t *intr_conf)
{
    (void)uart_num; (void)intr_conf;
    return ESP_OK; /* no-op */
}

/* ── Data transfer ──────────────────────────────────────────────────────── */

int uart_write_bytes(uart_port_t uart_num, const void *src, size_t size)
{
    if (!port_valid(uart_num) || !port_obj[uart_num] || !port_obj[uart_num]->installed) {
        return -1;
    }
    int fd = port_obj[uart_num]->fd;
    const uint8_t *p = (const uint8_t *)src;
    size_t remaining = size;

    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            ESP_LOGE(TAG, "UART%d write error: %s", uart_num, strerror(errno));
            return -1;
        }
        p += n;
        remaining -= (size_t)n;
    }
    return (int)size;
}

int uart_write_bytes_with_break(uart_port_t uart_num, const void *src, size_t size, int brk_len)
{
    (void)brk_len;
    int ret = uart_write_bytes(uart_num, src, size);
    if (ret > 0 && port_obj[uart_num]) {
        tcsendbreak(port_obj[uart_num]->fd, 0);
    }
    return ret;
}

int uart_tx_chars(uart_port_t uart_num, const char *buffer, uint32_t len)
{
    if (!port_valid(uart_num) || !port_obj[uart_num] || !port_obj[uart_num]->installed) {
        return -1;
    }
    /* Non-blocking write */
    ssize_t n = write(port_obj[uart_num]->fd, buffer, len);
    return (n < 0) ? -1 : (int)n;
}

int uart_read_bytes(uart_port_t uart_num, void *buf, uint32_t length, TickType_t ticks_to_wait)
{
    if (!port_valid(uart_num) || !port_obj[uart_num] || !port_obj[uart_num]->installed) {
        return -1;
    }
    uart_obj_t *obj = port_obj[uart_num];

    size_t got = 0;
    size_t item_size = 0;
    void *data = xRingbufferReceiveUpTo(obj->rx_ring, &item_size, ticks_to_wait, length);
    if (data && item_size > 0) {
        memcpy(buf, data, item_size);
        got = item_size;
        vRingbufferReturnItem(obj->rx_ring, data);

        xSemaphoreTake(obj->rx_mux, portMAX_DELAY);
        obj->rx_buffered_len -= (int)item_size;
        if (obj->rx_buffered_len < 0) obj->rx_buffered_len = 0;
        xSemaphoreGive(obj->rx_mux);
    }
    return (int)got;
}

/* ── TX wait / flush ────────────────────────────────────────────────────── */

esp_err_t uart_wait_tx_done(uart_port_t uart_num, TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    if (!port_valid(uart_num) || !port_obj[uart_num] || !port_obj[uart_num]->installed) {
        return ESP_FAIL;
    }
    tcdrain(port_obj[uart_num]->fd);
    return ESP_OK;
}

esp_err_t uart_flush(uart_port_t uart_num)
{
    return uart_flush_input(uart_num);
}

esp_err_t uart_flush_input(uart_port_t uart_num)
{
    if (!port_valid(uart_num) || !port_obj[uart_num] || !port_obj[uart_num]->installed) {
        return ESP_FAIL;
    }
    uart_obj_t *obj = port_obj[uart_num];

    /* Drain ring buffer */
    xSemaphoreTake(obj->rx_mux, portMAX_DELAY);
    size_t item_size;
    while (1) {
        void *data = xRingbufferReceiveUpTo(obj->rx_ring, &item_size, 0, 1024);
        if (!data) break;
        vRingbufferReturnItem(obj->rx_ring, data);
    }
    obj->rx_buffered_len = 0;
    xSemaphoreGive(obj->rx_mux);

    tcflush(obj->fd, TCIFLUSH);
    return ESP_OK;
}

/* ── Buffered data ──────────────────────────────────────────────────────── */

esp_err_t uart_get_buffered_data_len(uart_port_t uart_num, size_t *size)
{
    if (!port_valid(uart_num) || !size) return ESP_ERR_INVALID_ARG;
    if (!port_obj[uart_num] || !port_obj[uart_num]->installed) {
        *size = 0;
        return ESP_OK;
    }
    *size = (size_t)(port_obj[uart_num]->rx_buffered_len > 0
                         ? port_obj[uart_num]->rx_buffered_len : 0);
    return ESP_OK;
}

esp_err_t uart_get_tx_buffer_free_size(uart_port_t uart_num, size_t *size)
{
    if (!port_valid(uart_num) || !size) return ESP_ERR_INVALID_ARG;
    /* We don't use a TX ring buffer; report "always free" */
    *size = 4096;
    return ESP_OK;
}

/* ── Baud rate ──────────────────────────────────────────────────────────── */

esp_err_t uart_set_baudrate(uart_port_t uart_num, uint32_t baudrate)
{
    if (!port_valid(uart_num) || !port_obj[uart_num] || !port_obj[uart_num]->installed) {
        return ESP_FAIL;
    }
    port_obj[uart_num]->config.baud_rate = (int)baudrate;
    apply_termios(port_obj[uart_num]->fd, &port_obj[uart_num]->config);
    return ESP_OK;
}

esp_err_t uart_get_baudrate(uart_port_t uart_num, uint32_t *baudrate)
{
    if (!port_valid(uart_num) || !baudrate) return ESP_ERR_INVALID_ARG;
    if (!port_obj[uart_num] || !port_obj[uart_num]->installed) return ESP_FAIL;
    *baudrate = (uint32_t)port_obj[uart_num]->config.baud_rate;
    return ESP_OK;
}

/* ── Line parameters ────────────────────────────────────────────────────── */

esp_err_t uart_set_word_length(uart_port_t uart_num, uart_word_length_t data_bit)
{
    if (!port_valid(uart_num) || !port_obj[uart_num]) return ESP_FAIL;
    port_obj[uart_num]->config.data_bits = data_bit;
    apply_termios(port_obj[uart_num]->fd, &port_obj[uart_num]->config);
    return ESP_OK;
}

esp_err_t uart_get_word_length(uart_port_t uart_num, uart_word_length_t *data_bit)
{
    if (!port_valid(uart_num) || !data_bit) return ESP_FAIL;
    if (!port_obj[uart_num]) return ESP_FAIL;
    *data_bit = port_obj[uart_num]->config.data_bits;
    return ESP_OK;
}

esp_err_t uart_set_stop_bits(uart_port_t uart_num, uart_stop_bits_t stop_bits)
{
    if (!port_valid(uart_num) || !port_obj[uart_num]) return ESP_FAIL;
    port_obj[uart_num]->config.stop_bits = stop_bits;
    apply_termios(port_obj[uart_num]->fd, &port_obj[uart_num]->config);
    return ESP_OK;
}

esp_err_t uart_get_stop_bits(uart_port_t uart_num, uart_stop_bits_t *stop_bits)
{
    if (!port_valid(uart_num) || !stop_bits) return ESP_FAIL;
    if (!port_obj[uart_num]) return ESP_FAIL;
    *stop_bits = port_obj[uart_num]->config.stop_bits;
    return ESP_OK;
}

esp_err_t uart_set_parity(uart_port_t uart_num, uart_parity_t parity_mode)
{
    if (!port_valid(uart_num) || !port_obj[uart_num]) return ESP_FAIL;
    port_obj[uart_num]->config.parity = parity_mode;
    apply_termios(port_obj[uart_num]->fd, &port_obj[uart_num]->config);
    return ESP_OK;
}

esp_err_t uart_get_parity(uart_port_t uart_num, uart_parity_t *parity_mode)
{
    if (!port_valid(uart_num) || !parity_mode) return ESP_FAIL;
    if (!port_obj[uart_num]) return ESP_FAIL;
    *parity_mode = port_obj[uart_num]->config.parity;
    return ESP_OK;
}

esp_err_t uart_get_sclk_freq(uart_sclk_t sclk, uint32_t *out_freq_hz)
{
    (void)sclk;
    if (!out_freq_hz) return ESP_ERR_INVALID_ARG;
    *out_freq_hz = 80000000; /* pretend APB = 80 MHz */
    return ESP_OK;
}

/* ── Stubs — all return ESP_OK (or appropriate neutral value) ───────────── */

esp_err_t uart_set_hw_flow_ctrl(uart_port_t uart_num, uart_hw_flowcontrol_t flow_ctrl, uint8_t rx_thresh)
{ (void)uart_num; (void)flow_ctrl; (void)rx_thresh; return ESP_OK; }

esp_err_t uart_get_hw_flow_ctrl(uart_port_t uart_num, uart_hw_flowcontrol_t *flow_ctrl)
{
    if (!flow_ctrl) return ESP_ERR_INVALID_ARG;
    *flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    return ESP_OK;
}

esp_err_t uart_set_sw_flow_ctrl(uart_port_t uart_num, bool enable, uint8_t rx_thresh_xon, uint8_t rx_thresh_xoff)
{ (void)uart_num; (void)enable; (void)rx_thresh_xon; (void)rx_thresh_xoff; return ESP_OK; }

esp_err_t uart_set_pin(uart_port_t uart_num, int tx_io_num, int rx_io_num, int rts_io_num, int cts_io_num)
{ (void)uart_num; (void)tx_io_num; (void)rx_io_num; (void)rts_io_num; (void)cts_io_num; return ESP_OK; }

esp_err_t uart_set_rts(uart_port_t uart_num, int level)
{ (void)uart_num; (void)level; return ESP_OK; }

esp_err_t uart_set_dtr(uart_port_t uart_num, int level)
{ (void)uart_num; (void)level; return ESP_OK; }

esp_err_t uart_set_line_inverse(uart_port_t uart_num, uint32_t inverse_mask)
{ (void)uart_num; (void)inverse_mask; return ESP_OK; }

esp_err_t uart_set_tx_idle_num(uart_port_t uart_num, uint16_t idle_num)
{ (void)uart_num; (void)idle_num; return ESP_OK; }

esp_err_t uart_clear_intr_status(uart_port_t uart_num, uint32_t clr_mask)
{ (void)uart_num; (void)clr_mask; return ESP_OK; }

esp_err_t uart_enable_intr_mask(uart_port_t uart_num, uint32_t enable_mask)
{ (void)uart_num; (void)enable_mask; return ESP_OK; }

esp_err_t uart_disable_intr_mask(uart_port_t uart_num, uint32_t disable_mask)
{ (void)uart_num; (void)disable_mask; return ESP_OK; }

esp_err_t uart_enable_rx_intr(uart_port_t uart_num)
{ (void)uart_num; return ESP_OK; }

esp_err_t uart_disable_rx_intr(uart_port_t uart_num)
{ (void)uart_num; return ESP_OK; }

esp_err_t uart_disable_tx_intr(uart_port_t uart_num)
{ (void)uart_num; return ESP_OK; }

esp_err_t uart_enable_tx_intr(uart_port_t uart_num, int enable, int thresh)
{ (void)uart_num; (void)enable; (void)thresh; return ESP_OK; }

esp_err_t uart_enable_pattern_det_baud_intr(uart_port_t uart_num, char pattern_chr, uint8_t chr_num,
                                            int chr_tout, int post_idle, int pre_idle)
{ (void)uart_num; (void)pattern_chr; (void)chr_num; (void)chr_tout; (void)post_idle; (void)pre_idle; return ESP_OK; }

esp_err_t uart_disable_pattern_det_intr(uart_port_t uart_num)
{ (void)uart_num; return ESP_OK; }

int uart_pattern_pop_pos(uart_port_t uart_num)
{ (void)uart_num; return -1; }

int uart_pattern_get_pos(uart_port_t uart_num)
{ (void)uart_num; return -1; }

esp_err_t uart_pattern_queue_reset(uart_port_t uart_num, int queue_length)
{ (void)uart_num; (void)queue_length; return ESP_OK; }

esp_err_t uart_set_mode(uart_port_t uart_num, uart_mode_t mode)
{ (void)uart_num; (void)mode; return ESP_OK; }

esp_err_t uart_set_rx_full_threshold(uart_port_t uart_num, int threshold)
{ (void)uart_num; (void)threshold; return ESP_OK; }

esp_err_t uart_set_tx_empty_threshold(uart_port_t uart_num, int threshold)
{ (void)uart_num; (void)threshold; return ESP_OK; }

esp_err_t uart_set_rx_timeout(uart_port_t uart_num, const uint8_t tout_thresh)
{ (void)uart_num; (void)tout_thresh; return ESP_OK; }

esp_err_t uart_get_collision_flag(uart_port_t uart_num, bool *collision_flag)
{
    if (!collision_flag) return ESP_ERR_INVALID_ARG;
    *collision_flag = false;
    return ESP_OK;
}

esp_err_t uart_set_wakeup_threshold(uart_port_t uart_num, int wakeup_threshold)
{ (void)uart_num; (void)wakeup_threshold; return ESP_OK; }

esp_err_t uart_get_wakeup_threshold(uart_port_t uart_num, int *out_wakeup_threshold)
{
    if (!out_wakeup_threshold) return ESP_ERR_INVALID_ARG;
    *out_wakeup_threshold = 3;
    return ESP_OK;
}

esp_err_t uart_wait_tx_idle_polling(uart_port_t uart_num)
{
    return uart_wait_tx_done(uart_num, portMAX_DELAY);
}

esp_err_t uart_set_loop_back(uart_port_t uart_num, bool loop_back_en)
{ (void)uart_num; (void)loop_back_en; return ESP_OK; }

void uart_set_always_rx_timeout(uart_port_t uart_num, bool always_rx_timeout_en)
{ (void)uart_num; (void)always_rx_timeout_en; }

esp_err_t uart_detect_bitrate_start(uart_port_t uart_num, const uart_bitrate_detect_config_t *config)
{ (void)uart_num; (void)config; return ESP_ERR_NOT_SUPPORTED; }

esp_err_t uart_detect_bitrate_stop(uart_port_t uart_num, bool deinit, uart_bitrate_res_t *ret_res)
{ (void)uart_num; (void)deinit; (void)ret_res; return ESP_ERR_NOT_SUPPORTED; }
