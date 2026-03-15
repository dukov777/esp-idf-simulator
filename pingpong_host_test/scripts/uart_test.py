#!/usr/bin/env python3
"""
Send and receive data over the virtual UART link.

Usage:
    # Send a string and print the response
    python3 scripts/uart_test.py --send "hello"

    # Listen for incoming data
    python3 scripts/uart_test.py --listen

    # Custom port / baud
    python3 scripts/uart_test.py --port /tmp/ttyEXT0 --baud 115200
"""

import argparse
import termios
import tty

import serial


def open_port(port: str, baud: int) -> serial.Serial:
    """Open the serial port, force raw mode, and flush stale data."""
    ser = serial.Serial(port, baudrate=baud, timeout=2)
    fd = ser.fileno()

    # Force raw mode — pyserial's termios can interfere with PTY
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[4] = attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)

    # Flush stale data from previous sessions
    termios.tcflush(fd, termios.TCIFLUSH)
    ser.reset_input_buffer()

    return ser


def send(ser: serial.Serial, message: str) -> None:
    """Send a message and wait for a response."""
    ser.reset_input_buffer()

    print(f">> {message!r}")
    ser.write(message.encode("utf-8"))
    ser.flush()

    data = ser.read(len(message) + 64)
    if data:
        print(f"<< {data.decode('utf-8', errors='replace')!r}")
    else:
        print("<< (no response)")


def listen(ser: serial.Serial) -> None:
    """Block and print everything received. Ctrl-C to stop."""
    print(f"Listening on {ser.port} @ {ser.baudrate}  (Ctrl-C to stop)\n")
    try:
        while True:
            data = ser.read(256)
            if data:
                print(f"<< {data.decode('utf-8', errors='replace')!r}")
    except KeyboardInterrupt:
        print("\nExiting.")


def main() -> None:
    parser = argparse.ArgumentParser(description="UART PTY test tool")
    parser.add_argument("--port", default="/tmp/ttyEXT0",
                        help="Serial port path (default: /tmp/ttyEXT0)")
    parser.add_argument("--baud", type=int, default=115200,
                        help="Baud rate (default: 115200)")
    parser.add_argument("--send", type=str,
                        help="Send a single message and print the response")
    parser.add_argument("--listen", action="store_true",
                        help="Listen-only mode")
    args = parser.parse_args()

    ser = open_port(args.port, args.baud)
    try:
        if args.send:
            send(ser, args.send)
        elif args.listen:
            listen(ser)
        else:
            parser.print_help()
    finally:
        ser.close()


if __name__ == "__main__":
    main()
