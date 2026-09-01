#!/bin/sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG_FILE="${1:-/tmp/waylab-debug.log}"

echo "Launching Waylab on NVIDIA GeForce RTX 2050 (GA107M)"
echo "Logging debug output to $LOG_FILE"

# Use card0 (NVIDIA) as primary and card1 (Intel) as secondary KMS device
export WLR_DRM_DEVICES="/dev/dri/card0:/dev/dri/card1"
export GBM_BACKEND="nvidia-drm"
export __GLX_VENDOR_LIBRARY_NAME="nvidia"
export LIBVA_DRIVER_NAME="nvidia"
export WLR_NO_HARDWARE_CURSORS=1
export WLR_RENDERER=gles2

exec "$SCRIPT_DIR/build/labwc" -d -C "$SCRIPT_DIR/env" > "$LOG_FILE" 2>&1
