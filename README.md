# ESP-IDF Linux Simulator

Run ESP-IDF firmware on your Linux desktop — no hardware required. This project provides shims and demo apps that let you build and run ESP-IDF projects using the `linux` host target, with virtual UART communication over PTY devices.

## What's Inside

```
pingpong_host_test/
├── components/
│   └── esp_driver_uart/          # UART driver shim (POSIX PTY backend)
│       ├── include/driver/
│       │   ├── uart.h            # Self-contained ESP-IDF UART API
│       │   └── uart_linux.h      # Linux-specific extensions
│       └── src/
│           └── uart_linux.c      # Implementation (~500 lines)
├── main/
│   ├── uart_echo.c               # UART echo demo (active)
│   └── pingpong.c                # FreeRTOS queue demo
├── scripts/
│   ├── socat_pty.sh              # Create virtual serial link
│   └── uart_test.py              # pyserial send/receive tool
└── UART_SIMULATION.md            # Detailed usage guide
```

## Quick Start

### Prerequisites

```bash
sudo apt install socat
pip3 install pyserial
```

ESP-IDF must be installed with `linux` target support.

### Build

```bash
cd pingpong_host_test
source $IDF_PATH/export.sh
idf.py --preview set-target linux
idf.py build
```

### Run

Terminal 1 — start the virtual serial link:

```bash
./scripts/socat_pty.sh
```

Terminal 2 — run the firmware:

```bash
./build/pingpong_host_test.elf
```

Terminal 3 — send data:

```bash
python3 scripts/uart_test.py --send "hello"
```

```
>> 'hello'
<< 'hello'
```

## UART Driver Shim

The `esp_driver_uart` component shadows the IDF's built-in (empty) UART driver on the `linux` target. It maps the standard ESP-IDF UART API onto POSIX PTY devices:

- `uart_driver_install` opens a PTY and spawns an RX reader task
- `uart_write_bytes` / `uart_read_bytes` use POSIX `write`/`read` with a FreeRTOS ring buffer
- `uart_param_config` applies baud rate, parity, stop bits via `termios`
- Pin, interrupt, pattern detection, and wakeup functions are no-op stubs

Self-contained `driver/uart.h` — all types defined locally, no dependency on `hal/uart_types.h` or `soc_periph_uart_clk_src_legacy_t`.

See [UART_SIMULATION.md](pingpong_host_test/UART_SIMULATION.md) for configuration, multi-port setup, and troubleshooting.

## Demo Apps

| App | File | Description |
|-----|------|-------------|
| UART Echo | `main/uart_echo.c` | Reads bytes from PTY, logs them, echoes back |
| Ping-Pong | `main/pingpong.c` | Two FreeRTOS tasks passing messages via queues |

Switch between them by editing `main/CMakeLists.txt`.

## License

Apache-2.0
