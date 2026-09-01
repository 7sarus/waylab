#!/bin/sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG_FILE="${1:-/tmp/waylab-debug.log}"
echo "Launching labwc with config from $SCRIPT_DIR/env"
echo "Logging debug output to $LOG_FILE"
exec "$SCRIPT_DIR/build/labwc" -d -C "$SCRIPT_DIR/env" > "$LOG_FILE" 2>&1
