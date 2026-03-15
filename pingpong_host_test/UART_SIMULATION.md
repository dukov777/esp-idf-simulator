# UART Simulation on ESP-IDF Linux Target

This project includes a UART driver shim that maps the ESP-IDF UART API onto POSIX PTY devices. Firmware that calls `uart_driver_install`, `uart_read_bytes`, `uart_write_bytes`, etc. works unmodified — data flows through a virtual serial link instead of real hardware.

## Prerequisites

- ESP-IDF with the `linux` target (requires `--preview` flag)
- `socat` — creates the PTY pair
- Python 3 with `pyserial` — test script for send/receive
- `minicom` (optional) — interactive terminal for the external side

```bash
sudo apt install socat minicom
pip3 install pyserial
```

## Architecture

```
 ┌──────────────────────┐         socat PTY pair         ┌──────────────────────┐
 │   ESP firmware        │                                │   External tool       │
 │   (Linux ELF)         │                                │   (minicom / script)  │
 │                       │                                │                       │
 │  uart_write_bytes() ──┼──► /tmp/ttyVESP0 ←──socat──→ /tmp/ttyEXT0 ◄──────── │
 │  uart_read_bytes()  ◄─┤                                │                       │
 └──────────────────────┘                                └──────────────────────┘
```

The shim component at `components/esp_driver_uart/` shadows the IDF's built-in (empty) UART component. It:

1. Opens the PTY device at `uart_driver_install` time
2. Runs a FreeRTOS RX task that reads incoming bytes into a ring buffer
3. Exposes the full `driver/uart.h` API — real I/O for core functions, no-op stubs for interrupt/pattern/wakeup functions

## Quick Start

### 1. Start the PTY pair

In a dedicated terminal:

```bash
cd pingpong_host_test
./scripts/socat_pty.sh
```

You should see output like:

```
Creating PTY pair:  /tmp/ttyVESP0  <-->  /tmp/ttyEXT0
2026/03/15 17:38:20 socat[...] N PTY is /dev/pts/18
2026/03/15 17:38:20 socat[...] N PTY is /dev/pts/19
2026/03/15 17:38:20 socat[...] N starting data transfer loop with FDs [5,5] and [7,7]
```

Keep this terminal open — socat must be running for the link to work.

### 2. Build and run the firmware

In a second terminal:

```bash
cd pingpong_host_test
source $IDF_PATH/export.sh
idf.py --preview set-target linux
idf.py build
./build/pingpong_host_test.elf
```

Expected output:

```
I (...) uart_echo: UART echo demo starting...
I (...) uart_linux: UART0 installed on /tmp/ttyVESP0 (fd=3)
I (...) uart_linux: UART0 configured: baud=115200, data=3, parity=0, stop=1
I (...) uart_echo: UART echo ready — send data to /tmp/ttyEXT0
```

### 3. Send data from the external side

**Option A — Python test script (recommended):**

```bash
# Send a message and verify the echo response
python3 scripts/uart_test.py --send "hello"
```

Expected output:

```
>> 'hello'
<< 'hello'
```

**Option B — interactive Python session:**

```bash
python3 scripts/uart_test.py
```

```
Connected to /tmp/ttyEXT0 @ 115200
Type a line and press Enter to send. Ctrl-C to quit.

>> hello
<< 'hello'
>> world
<< 'world'
```

**Option C — listen-only mode:**

```bash
python3 scripts/uart_test.py --listen
```

Prints everything the firmware sends, without transmitting.

**Option D — interactive terminal with minicom:**

```bash
minicom -D /tmp/ttyEXT0 -b 115200
```

Everything you type appears in the firmware log and is echoed back to minicom.

**Option E — quick shell test:**

```bash
echo "hello" > /tmp/ttyEXT0
```

The firmware logs the received bytes. Note: echoed data is discarded unless a reader is open on `/tmp/ttyEXT0`.

## Python Test Script

`scripts/uart_test.py` uses pyserial to communicate with the firmware over the PTY link. It forces the PTY into raw mode after pyserial opens it (pyserial's default termios settings can interfere with raw PTY operation).

### Modes

| Mode | Command | Description |
|------|---------|-------------|
| Send + receive | `python3 scripts/uart_test.py --send "message"` | Sends a string, waits for the echo, prints both |
| Interactive | `python3 scripts/uart_test.py` | Type lines to send, see responses in real time |
| Listen only | `python3 scripts/uart_test.py --listen` | Prints everything received, sends nothing |

### Options

| Flag | Default | Description |
|------|---------|-------------|
| `--port` | `/tmp/ttyEXT0` | Serial port path |
| `--baud` | `115200` | Baud rate |
| `--send` | — | Send a single message and exit |
| `--listen` | — | Listen-only mode |

### Integration with custom firmware

The script works with any firmware that uses the UART shim, not just the echo demo. Point `--port` at the corresponding external PTY endpoint:

```bash
# If firmware opens UART_NUM_1 on /tmp/ttyVESP1
python3 scripts/uart_test.py --port /tmp/ttyEXT1 --send "AT"
```

## Custom PTY Paths

### At runtime (in firmware code)

Call `uart_linux_set_device_path` before `uart_driver_install`:

```c
#include "driver/uart_linux.h"

uart_linux_set_device_path(UART_NUM_0, "/tmp/my_custom_pty");
uart_driver_install(UART_NUM_0, 1024, 0, 16, &queue, 0);
```

### Via Kconfig

Run `idf.py menuconfig` and navigate to **UART Linux PTY Shim**:

| Setting | Default | Description |
|---------|---------|-------------|
| `UART_LINUX_PTY0_PATH` | `/tmp/ttyVESP0` | PTY path for UART_NUM_0 |
| `UART_LINUX_PTY1_PATH` | `/tmp/ttyVESP1` | PTY path for UART_NUM_1 |
| `UART_LINUX_PTY2_PATH` | `/tmp/ttyVESP2` | PTY path for UART_NUM_2 |
| `UART_LINUX_RX_TASK_STACK` | `4096` | RX reader task stack size |
| `UART_LINUX_RX_TASK_PRIO` | `5` | RX reader task priority |

### socat with custom paths

```bash
./scripts/socat_pty.sh /tmp/my_custom_pty /tmp/my_external_pty
```

## Multiple UART Ports

Run one socat instance per port:

```bash
./scripts/socat_pty.sh /tmp/ttyVESP0 /tmp/ttyEXT0 &
./scripts/socat_pty.sh /tmp/ttyVESP1 /tmp/ttyEXT1 &
```

In firmware:

```c
uart_linux_set_device_path(UART_NUM_0, "/tmp/ttyVESP0");
uart_linux_set_device_path(UART_NUM_1, "/tmp/ttyVESP1");

uart_driver_install(UART_NUM_0, 1024, 0, 16, &q0, 0);
uart_driver_install(UART_NUM_1, 1024, 0, 16, &q1, 0);
```

## Switching Between Demo Apps

The project includes two `app_main` implementations. Only one can be active at a time — edit `main/CMakeLists.txt`:

```cmake
# UART echo demo (default)
SRCS "uart_echo.c"

# Original FreeRTOS ping-pong demo (no UART)
# SRCS "pingpong.c"
```

When using `pingpong.c`, remove `esp_driver_uart` from the `REQUIRES` line.

## API Coverage

| Category | Functions | Implementation |
|----------|-----------|----------------|
| Driver lifecycle | `uart_driver_install`, `uart_driver_delete`, `uart_is_driver_installed` | Full PTY open/close/task management |
| Configuration | `uart_param_config`, `uart_set_baudrate`, `uart_set_word_length`, `uart_set_parity`, `uart_set_stop_bits` (+ getters) | Applied via termios |
| Data transfer | `uart_write_bytes`, `uart_read_bytes`, `uart_tx_chars`, `uart_write_bytes_with_break` | Real POSIX read/write |
| Flush/wait | `uart_flush`, `uart_flush_input`, `uart_wait_tx_done` | tcflush/tcdrain |
| Buffered data | `uart_get_buffered_data_len`, `uart_get_tx_buffer_free_size` | Ring buffer tracking |
| Pin/signal control | `uart_set_pin`, `uart_set_rts`, `uart_set_dtr`, `uart_set_line_inverse` | No-op stubs |
| Interrupts | `uart_enable_intr_mask`, `uart_disable_intr_mask`, `uart_intr_config`, etc. | No-op stubs |
| Pattern detection | `uart_enable_pattern_det_baud_intr`, `uart_pattern_pop_pos`, etc. | No-op stubs |
| RS485/wakeup/loopback | `uart_set_mode`, `uart_set_wakeup_threshold`, `uart_set_loop_back`, etc. | No-op stubs |
| Bitrate detection | `uart_detect_bitrate_start`, `uart_detect_bitrate_stop` | Returns `ESP_ERR_NOT_SUPPORTED` |

## Troubleshooting

**"failed to open /tmp/ttyVESP0: No such file or directory"**
Start socat first — the PTY symlinks only exist while socat is running.

**No data received by firmware**
Verify socat is still running (`pgrep socat`). Both PTY endpoints must exist for data to flow.

**Firmware hangs or no output**
The Linux FreeRTOS port uses cooperative scheduling. Avoid raw POSIX blocking calls (`sleep`, `poll`, `select`) in FreeRTOS tasks — use `vTaskDelay` instead.

**Echo data not received on external side**
Make sure you have a reader open on `/tmp/ttyEXT0` (e.g. `cat /tmp/ttyEXT0 &`) before writing. PTY data is discarded if no reader is attached.

**pyserial opens the port but reads return empty**
pyserial applies its own termios settings when opening a port, which can override the raw mode that socat configured. The `uart_test.py` script handles this by calling `tty.setraw()` after open. If writing your own script, do the same:

```python
import tty
import serial

ser = serial.Serial("/tmp/ttyEXT0", 115200, timeout=0.5)
tty.setraw(ser.fileno())  # force raw mode after pyserial's open
```
