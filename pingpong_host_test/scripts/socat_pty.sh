#!/usr/bin/env bash
# Create a virtual PTY pair for UART simulation.
#
#   /tmp/ttyVESP0  ←→  /tmp/ttyEXT0
#
# The ESP firmware opens ttyVESP0; external tools (minicom, echo, etc.)
# connect to ttyEXT0.
#
# Usage:  ./scripts/socat_pty.sh
# Stop:   Ctrl-C

set -euo pipefail

VESP="${1:-/tmp/ttyVESP0}"
EXT="${2:-/tmp/ttyEXT0}"

echo "Creating PTY pair:  $VESP  <-->  $EXT"
exec socat -d -d \
    "PTY,raw,echo=0,link=${VESP}" \
    "PTY,raw,echo=0,link=${EXT}"
