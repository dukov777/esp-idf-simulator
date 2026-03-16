# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

```bash
cd pingpong_host_test
source $IDF_PATH/export.sh
idf.py --preview set-target linux
idf.py build
```

The `--preview` flag is required — the `linux` target is a preview feature.

The built binary is `./build/pingpong_host_test.elf` — a native Linux executable, not a flashable image.

## Testing with Virtual UART

Three terminals are needed:

```bash
# Terminal 1: create PTY pair (must stay running)
./scripts/socat_pty.sh

# Terminal 2: run firmware
./build/pingpong_host_test.elf

# Terminal 3: send/receive data
python3 scripts/uart_test.py --send "hello"
python3 scripts/uart_test.py --listen
```

`socat_pty.sh` accepts optional args: `./scripts/socat_pty.sh /tmp/ttyVESP1 /tmp/ttyEXT1`

## Architecture

The project provides a **UART driver shim** that shadows ESP-IDF's empty `esp_driver_uart` component on the `linux` target. Firmware code uses the standard `driver/uart.h` API unchanged — the shim routes data through POSIX PTY devices instead of hardware.

**Data path:** Firmware → `uart_write_bytes()` → POSIX `write(fd)` → socat PTY pair → external tool (and reverse for RX).

### Key design constraint: Linux FreeRTOS cooperative scheduling

The Linux FreeRTOS port only switches tasks at FreeRTOS API calls. **Never use blocking POSIX calls** (`poll()`, `select()`, `sleep()`, blocking `read()`) inside FreeRTOS tasks — they hold the scheduler and starve all other tasks. Use `vTaskDelay()` for timing and non-blocking I/O with retries.

The RX task uses a `dup()`'d non-blocking fd + `vTaskDelay(pdMS_TO_TICKS(10))` for this reason.

### Self-contained uart.h

`components/esp_driver_uart/include/driver/uart.h` redefines all UART types locally (`uart_port_t`, `uart_config_t`, `uart_sclk_t`, etc.) instead of including `hal/uart_types.h`. This avoids the missing `soc_periph_uart_clk_src_legacy_t` type on the `linux` target. Do not add `#include "hal/uart_types.h"` or any SOC/HAL headers.

### Component dependencies

```
main (uart_echo.c)
  REQUIRES: freertos, esp_driver_uart
  PRIV_REQUIRES: esp_system

esp_driver_uart
  PRIV_REQUIRES: esp_ringbuf, log, freertos
  (no SOC/HAL dependencies)
```

The top-level `CMakeLists.txt` uses `set(COMPONENTS main)` to pull in only what's needed.

## Switching Demo Apps

Only one file with `app_main` can be active. Edit `main/CMakeLists.txt`:

- `SRCS "uart_echo.c"` — UART echo demo (needs `esp_driver_uart` in REQUIRES)
- `SRCS "pingpong.c"` — FreeRTOS queue demo (remove `esp_driver_uart` from REQUIRES)

## Stub vs Real Implementations

Real I/O: `uart_driver_install/delete`, `uart_param_config`, `uart_write_bytes`, `uart_read_bytes`, `uart_flush`, `uart_wait_tx_done`, baud/parity/stop/word-length get/set.

No-op stubs: all interrupt, pin, pattern detection, flow control, wakeup, RS485, loopback, and threshold functions.

Returns `ESP_ERR_NOT_SUPPORTED`: `uart_detect_bitrate_start/stop`.

## Git

Do not mention Claude in commit messages, PR descriptions, or any git artifacts.
