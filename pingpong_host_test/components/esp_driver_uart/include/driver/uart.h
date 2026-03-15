/*
 * Self-contained UART driver header for the ESP-IDF Linux target.
 *
 * Redefines all UART types locally so we never pull in hal/uart_types.h
 * (which requires soc_periph_uart_clk_src_legacy_t — absent on linux).
 * API signatures match the real ESP-IDF driver 1-to-1.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*  Types — self-contained, no HAL/SOC dependency                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief UART port number
 */
typedef enum {
    UART_NUM_0 = 0,
    UART_NUM_1 = 1,
    UART_NUM_2 = 2,
    UART_NUM_MAX = 3,
} uart_port_t;

/**
 * @brief UART mode selection
 */
typedef enum {
    UART_MODE_UART                  = 0x00,
    UART_MODE_RS485_HALF_DUPLEX     = 0x01,
    UART_MODE_IRDA                  = 0x02,
    UART_MODE_RS485_COLLISION_DETECT = 0x03,
    UART_MODE_RS485_APP_CTRL        = 0x04,
} uart_mode_t;

/**
 * @brief UART word length constants
 */
typedef enum {
    UART_DATA_5_BITS   = 0x0,
    UART_DATA_6_BITS   = 0x1,
    UART_DATA_7_BITS   = 0x2,
    UART_DATA_8_BITS   = 0x3,
    UART_DATA_BITS_MAX = 0x4,
} uart_word_length_t;

/**
 * @brief UART stop bits number
 */
typedef enum {
    UART_STOP_BITS_1   = 0x1,
    UART_STOP_BITS_1_5 = 0x2,
    UART_STOP_BITS_2   = 0x3,
    UART_STOP_BITS_MAX = 0x4,
} uart_stop_bits_t;

/**
 * @brief UART parity constants
 */
typedef enum {
    UART_PARITY_DISABLE = 0x0,
    UART_PARITY_EVEN    = 0x2,
    UART_PARITY_ODD     = 0x3,
} uart_parity_t;

/**
 * @brief UART hardware flow control modes
 */
typedef enum {
    UART_HW_FLOWCTRL_DISABLE = 0x0,
    UART_HW_FLOWCTRL_RTS     = 0x1,
    UART_HW_FLOWCTRL_CTS     = 0x2,
    UART_HW_FLOWCTRL_CTS_RTS = 0x3,
    UART_HW_FLOWCTRL_MAX     = 0x4,
} uart_hw_flowcontrol_t;

/**
 * @brief UART signal bit map
 */
typedef enum {
    UART_SIGNAL_INV_DISABLE = 0,
    UART_SIGNAL_IRDA_TX_INV = (0x1 << 0),
    UART_SIGNAL_IRDA_RX_INV = (0x1 << 1),
    UART_SIGNAL_RXD_INV     = (0x1 << 2),
    UART_SIGNAL_CTS_INV     = (0x1 << 3),
    UART_SIGNAL_DSR_INV     = (0x1 << 4),
    UART_SIGNAL_TXD_INV     = (0x1 << 5),
    UART_SIGNAL_RTS_INV     = (0x1 << 6),
    UART_SIGNAL_DTR_INV     = (0x1 << 7),
} uart_signal_inv_t;

/**
 * @brief UART source clock (stub — no real clock tree on Linux)
 */
typedef enum {
    UART_SCLK_DEFAULT = 1,
    UART_SCLK_APB     = 1,
} uart_sclk_t;

/**
 * @brief UART configuration parameters
 */
typedef struct {
    int baud_rate;
    uart_word_length_t data_bits;
    uart_parity_t parity;
    uart_stop_bits_t stop_bits;
    uart_hw_flowcontrol_t flow_ctrl;
    uint8_t rx_flow_ctrl_thresh;
    uart_sclk_t source_clk;
    struct {
        uint32_t allow_pd : 1;
        uint32_t backup_before_sleep : 1;
    } flags;
} uart_config_t;

/**
 * @brief UART interrupt configuration (no-op on Linux)
 */
typedef struct {
    uint32_t intr_enable_mask;
    uint8_t  rx_timeout_thresh;
    uint8_t  txfifo_empty_intr_thresh;
    uint8_t  rxfifo_full_thresh;
} uart_intr_config_t;

/**
 * @brief UART event types
 */
typedef enum {
    UART_DATA,
    UART_BREAK,
    UART_BUFFER_FULL,
    UART_FIFO_OVF,
    UART_FRAME_ERR,
    UART_PARITY_ERR,
    UART_DATA_BREAK,
    UART_PATTERN_DET,
    UART_EVENT_MAX,
} uart_event_type_t;

/**
 * @brief Event structure used in UART event queue
 */
typedef struct {
    uart_event_type_t type;
    size_t size;
    bool timeout_flag;
} uart_event_t;

/**
 * @brief UART AT cmd char configuration
 */
typedef struct {
    uint8_t  cmd_char;
    uint8_t  char_num;
    uint32_t gap_tout;
    uint32_t pre_idle;
    uint32_t post_idle;
} uart_at_cmd_t;

/**
 * @brief UART software flow control configuration
 */
typedef struct {
    uint8_t xon_char;
    uint8_t xoff_char;
    uint8_t xon_thrd;
    uint8_t xoff_thrd;
} uart_sw_flowctrl_t;

/**
 * @brief UART bitrate detection configuration (stub)
 */
typedef struct {
    int rx_io_num;
    uart_sclk_t source_clk;
} uart_bitrate_detect_config_t;

/**
 * @brief UART bitrate detection result (stub)
 */
typedef struct {
    uint32_t low_period;
    uint32_t high_period;
    uint32_t pos_period;
    uint32_t neg_period;
    uint32_t edge_cnt;
    uint32_t clk_freq_hz;
} uart_bitrate_res_t;

typedef void *uart_isr_handle_t;

#define UART_PIN_NO_CHANGE      (-1)
#define UART_HW_FIFO_LEN(n)    (128)
#define UART_BITRATE_MAX        (5000000)

/* -------------------------------------------------------------------------- */
/*  Function declarations — match IDF signatures exactly                      */
/* -------------------------------------------------------------------------- */

/* Driver lifecycle */
esp_err_t uart_driver_install(uart_port_t uart_num, int rx_buffer_size, int tx_buffer_size,
                              int queue_size, QueueHandle_t *uart_queue, int intr_alloc_flags);
esp_err_t uart_driver_delete(uart_port_t uart_num);
bool      uart_is_driver_installed(uart_port_t uart_num);

/* Configuration */
esp_err_t uart_param_config(uart_port_t uart_num, const uart_config_t *uart_config);
esp_err_t uart_intr_config(uart_port_t uart_num, const uart_intr_config_t *intr_conf);

/* Data transfer */
int       uart_write_bytes(uart_port_t uart_num, const void *src, size_t size);
int       uart_write_bytes_with_break(uart_port_t uart_num, const void *src, size_t size, int brk_len);
int       uart_tx_chars(uart_port_t uart_num, const char *buffer, uint32_t len);
int       uart_read_bytes(uart_port_t uart_num, void *buf, uint32_t length, TickType_t ticks_to_wait);

/* TX wait / flush */
esp_err_t uart_wait_tx_done(uart_port_t uart_num, TickType_t ticks_to_wait);
esp_err_t uart_flush(uart_port_t uart_num);
esp_err_t uart_flush_input(uart_port_t uart_num);

/* Buffered data */
esp_err_t uart_get_buffered_data_len(uart_port_t uart_num, size_t *size);
esp_err_t uart_get_tx_buffer_free_size(uart_port_t uart_num, size_t *size);

/* Baud rate */
esp_err_t uart_set_baudrate(uart_port_t uart_num, uint32_t baudrate);
esp_err_t uart_get_baudrate(uart_port_t uart_num, uint32_t *baudrate);

/* Line parameters */
esp_err_t uart_set_word_length(uart_port_t uart_num, uart_word_length_t data_bit);
esp_err_t uart_get_word_length(uart_port_t uart_num, uart_word_length_t *data_bit);
esp_err_t uart_set_stop_bits(uart_port_t uart_num, uart_stop_bits_t stop_bits);
esp_err_t uart_get_stop_bits(uart_port_t uart_num, uart_stop_bits_t *stop_bits);
esp_err_t uart_set_parity(uart_port_t uart_num, uart_parity_t parity_mode);
esp_err_t uart_get_parity(uart_port_t uart_num, uart_parity_t *parity_mode);
esp_err_t uart_get_sclk_freq(uart_sclk_t sclk, uint32_t *out_freq_hz);

/* Flow control */
esp_err_t uart_set_hw_flow_ctrl(uart_port_t uart_num, uart_hw_flowcontrol_t flow_ctrl, uint8_t rx_thresh);
esp_err_t uart_get_hw_flow_ctrl(uart_port_t uart_num, uart_hw_flowcontrol_t *flow_ctrl);
esp_err_t uart_set_sw_flow_ctrl(uart_port_t uart_num, bool enable, uint8_t rx_thresh_xon, uint8_t rx_thresh_xoff);

/* Pin / signal control */
esp_err_t uart_set_pin(uart_port_t uart_num, int tx_io_num, int rx_io_num, int rts_io_num, int cts_io_num);
esp_err_t uart_set_rts(uart_port_t uart_num, int level);
esp_err_t uart_set_dtr(uart_port_t uart_num, int level);
esp_err_t uart_set_line_inverse(uart_port_t uart_num, uint32_t inverse_mask);
esp_err_t uart_set_tx_idle_num(uart_port_t uart_num, uint16_t idle_num);

/* Interrupt control (stubs) */
esp_err_t uart_clear_intr_status(uart_port_t uart_num, uint32_t clr_mask);
esp_err_t uart_enable_intr_mask(uart_port_t uart_num, uint32_t enable_mask);
esp_err_t uart_disable_intr_mask(uart_port_t uart_num, uint32_t disable_mask);
esp_err_t uart_enable_rx_intr(uart_port_t uart_num);
esp_err_t uart_disable_rx_intr(uart_port_t uart_num);
esp_err_t uart_disable_tx_intr(uart_port_t uart_num);
esp_err_t uart_enable_tx_intr(uart_port_t uart_num, int enable, int thresh);

/* Pattern detection (stubs) */
esp_err_t uart_enable_pattern_det_baud_intr(uart_port_t uart_num, char pattern_chr, uint8_t chr_num,
                                            int chr_tout, int post_idle, int pre_idle);
esp_err_t uart_disable_pattern_det_intr(uart_port_t uart_num);
int       uart_pattern_pop_pos(uart_port_t uart_num);
int       uart_pattern_get_pos(uart_port_t uart_num);
esp_err_t uart_pattern_queue_reset(uart_port_t uart_num, int queue_length);

/* Mode / thresholds / RS485 */
esp_err_t uart_set_mode(uart_port_t uart_num, uart_mode_t mode);
esp_err_t uart_set_rx_full_threshold(uart_port_t uart_num, int threshold);
esp_err_t uart_set_tx_empty_threshold(uart_port_t uart_num, int threshold);
esp_err_t uart_set_rx_timeout(uart_port_t uart_num, const uint8_t tout_thresh);
esp_err_t uart_get_collision_flag(uart_port_t uart_num, bool *collision_flag);

/* Wakeup / loopback */
esp_err_t uart_set_wakeup_threshold(uart_port_t uart_num, int wakeup_threshold);
esp_err_t uart_get_wakeup_threshold(uart_port_t uart_num, int *out_wakeup_threshold);
esp_err_t uart_wait_tx_idle_polling(uart_port_t uart_num);
esp_err_t uart_set_loop_back(uart_port_t uart_num, bool loop_back_en);
void      uart_set_always_rx_timeout(uart_port_t uart_num, bool always_rx_timeout_en);

/* Bitrate detection (not supported on Linux) */
esp_err_t uart_detect_bitrate_start(uart_port_t uart_num, const uart_bitrate_detect_config_t *config);
esp_err_t uart_detect_bitrate_stop(uart_port_t uart_num, bool deinit, uart_bitrate_res_t *ret_res);

#ifdef __cplusplus
}
#endif
