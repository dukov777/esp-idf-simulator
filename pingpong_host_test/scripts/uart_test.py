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


def _set_raw(ser: serial.Serial) -> None:
    """Force the underlying fd into raw mode.

    pyserial applies its own termios which can re-enable echo or
    canonical mode on a PTY.  We override that after open.
    """
    fd = ser.fileno()
    tty.setraw(fd)
    # Restore the baud rate pyserial configured
    attrs = termios.tcgetattr(fd)
    attrs[4] = attrs[5] = termios.B115200  # ispeed / ospeed
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def rx_thread(ser: serial.Serial, stop_event: threading.Event) -> None:
    """Background thread that prints incoming bytes."""
    fd = ser.fileno()
    while not stop_event.is_set():
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            data = os.read(fd, 256)
            if data:
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
            ser.write(line.encode("utf-8"))
            time.sleep(0.1)
    except (KeyboardInterrupt, EOFError):
        print("\nExiting.")
    finally:
        stop.set()
        reader.join(timeout=1)


def send_once(ser: serial.Serial, message: str) -> None:
    """Send a message and wait briefly for a response."""
    fd = ser.fileno()
    print(f">> {message!r}")
    os.write(fd, message.encode("utf-8"))

    # Poll for echo response
    collected = b""
    deadline = time.time() + 2.0
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            collected += os.read(fd, 256)
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
                data = os.read(fd, 256)
                if data:
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

    ser = serial.Serial(args.port, baudrate=args.baud, timeout=0.5)
    _set_raw(ser)

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
