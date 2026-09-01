#!/bin/sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG_FILE="${1:-/tmp/waylab-debug.log}"

echo "Launching Waylab on NVIDIA GeForce RTX 2050 (GA107M)"
echo "Logging debug output to $LOG_FILE"

# NVIDIA Wayland / wlroots environment variables
export WLR_DRM_DEVICES="/dev/dri/by-path/pci-0000:01:00.0-card:/dev/dri/by-path/pci-0000:00:02.0-card"
export GBM_BACKEND="nvidia-drm"
export __GLX_VENDOR_LIBRARY_NAME="nvidia"
export LIBVA_DRIVER_NAME="nvidia"
export WLR_NO_HARDWARE_CURSORS=1
export WLR_RENDERER=gles2

exec "$SCRIPT_DIR/build/labwc" -d -C "$SCRIPT_DIR/env" > "$LOG_FILE" 2>&1
