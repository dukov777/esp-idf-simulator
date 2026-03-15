#!/usr/bin/env python3
"""
Send and receive data over the virtual UART link.

Usage:
    # Interactive mode (default) — type lines, see echoed responses
    python3 scripts/uart_test.py

    # Send a single string
    python3 scripts/uart_test.py --send "hello"

    # Listen only
    python3 scripts/uart_test.py --listen

    # Custom port / baud
    python3 scripts/uart_test.py --port /tmp/ttyEXT0 --baud 115200
"""

import argparse
import os
import select
import termios
import threading
import time
import tty

import serial


def _open_port(port: str, baud: int) -> serial.Serial:
    """Open the serial port, force raw mode, and flush stale data."""
    ser = serial.Serial(port, baudrate=baud, timeout=0.5)
    fd = ser.fileno()

    # Force raw mode — pyserial's termios can interfere with PTY
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[4] = attrs[5] = termios.B115200  # ispeed / ospeed
    termios.tcsetattr(fd, termios.TCSANOW, attrs)

    # Flush any stale data buffered in the PTY from previous sessions
    termios.tcflush(fd, termios.TCIFLUSH)
    while select.select([fd], [], [], 0.05)[0]:
        os.read(fd, 4096)

    return ser


def rx_thread(ser: serial.Serial, stop_event: threading.Event) -> None:
    """Background thread that prints incoming bytes."""
    fd = ser.fileno()
    while not stop_event.is_set():
        try:
            r, _, _ = select.select([fd], [], [], 0.1)
        except (OSError, ValueError):
            break
        if r:
            try:
                data = os.read(fd, 256)
            except OSError:
                break
            if not data:
                break
            text = data.decode("utf-8", errors="replace")
            print(f"<< {text!r}")


def interactive(ser: serial.Serial) -> None:
    """Read lines from stdin, send them, print responses."""
    stop = threading.Event()
    reader = threading.Thread(target=rx_thread, args=(ser, stop), daemon=True)
    reader.start()

    print(f"Connected to {ser.port} @ {ser.baudrate}")
    print("Type a line and press Enter to send. Ctrl-C to quit.\n")

    try:
        while True:
            line = input(">> ")
            if not line:
                continue
            try:
                ser.write(line.encode("utf-8"))
            except OSError as e:
                print(f"Write error: {e}")
                break
            time.sleep(0.1)
    except (KeyboardInterrupt, EOFError):
        print("\nExiting.")
    finally:
        stop.set()
        reader.join(timeout=1)


def send_once(ser: serial.Serial, message: str) -> None:
    """Send a message and wait for a response."""
    fd = ser.fileno()

    # Flush anything pending before our write
    termios.tcflush(fd, termios.TCIFLUSH)
    while select.select([fd], [], [], 0.02)[0]:
        os.read(fd, 4096)

    print(f">> {message!r}")
    os.write(fd, message.encode("utf-8"))

    # Poll for response
    collected = b""
    deadline = time.time() + 2.0
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            chunk = os.read(fd, 256)
            if not chunk:
                break
            collected += chunk
            if len(collected) >= len(message):
                break

    if collected:
        print(f"<< {collected.decode('utf-8', errors='replace')!r}")
    else:
        print("<< (no response)")


def listen(ser: serial.Serial) -> None:
    """Listen and print everything received."""
    fd = ser.fileno()
    print(f"Listening on {ser.port} @ {ser.baudrate}  (Ctrl-C to stop)\n")
    try:
        while True:
            r, _, _ = select.select([fd], [], [], 0.5)
            if r:
                try:
                    data = os.read(fd, 256)
                except OSError:
                    print("Port closed.")
                    break
                if not data:
                    print("Peer disconnected.")
                    break
                print(f"<< {data.decode('utf-8', errors='replace')!r}")
    except KeyboardInterrupt:
        print("\nExiting.")


def main() -> None:
    parser = argparse.ArgumentParser(description="UART PTY test tool")
    parser.add_argument(
        "--port", default="/tmp/ttyEXT0",
        help="Serial port path (default: /tmp/ttyEXT0)",
    )
    parser.add_argument(
        "--baud", type=int, default=115200,
        help="Baud rate (default: 115200)",
    )
    parser.add_argument(
        "--send", type=str,
        help="Send a single message and print the response",
    )
    parser.add_argument(
        "--listen", action="store_true",
        help="Listen-only mode",
    )
    args = parser.parse_args()

    ser = _open_port(args.port, args.baud)

    try:
        if args.send:
            send_once(ser, args.send)
        elif args.listen:
            listen(ser)
        else:
            interactive(ser)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
