#!/bin/sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
: ${BUILD_DIR="${SCRIPT_DIR}/build"}
LOGS_BASE="${HOME}/logs"
CONFIG_DIR="$SCRIPT_DIR/env"
OUTPUT_MODE="split"   # split, tee, stdout
USE_GDB=0
SKIP_BUILD=0
EXTRA_ARGS=""

: ${WAYLAND_DEBUG="server"}
: ${EGL_LOG_LEVEL="debug"}
export WAYLAND_DEBUG
export EGL_LOG_LEVEL

print_help() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS] [-- <extra labwc args>]

Build and run Waylab with AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan),
and categorized, timestamped multi-level logging to ~/logs.

Logging Categories (saved to ~/logs by default):
  Full log        : ~/logs/waylab_<TIMESTAMP>_full.log
  Compositor log  : ~/logs/waylab_<TIMESTAMP>_compositor.log (labwc/wlroots/ASan)
  Wayland log     : ~/logs/waylab_<TIMESTAMP>_wayland.log (protocol traces)
  Effects log     : ~/logs/waylab_<TIMESTAMP>_effects.log (blur & rounded shaders)
  Driver log      : ~/logs/waylab_<TIMESTAMP>_driver.log (Mesa/EGL/DRM)
  Session link    : ~/logs/session_<TIMESTAMP>/ and ~/logs/latest/

Options:
  -w, --wayland-debug [VAL]   Enable Wayland protocol debug (default: server)
  --no-wayland-debug          Disable WAYLAND_DEBUG protocol logging
  --damage [MODE]             Enable visual damage tracking (highlight/rerender)
  --no-build                  Skip meson configure and ninja compile
  --outputs <N>               Emulate N virtual outputs (WLR_WL_OUTPUTS=N)
  --pixman                    Force Pixman software renderer
  --egl-debug                 Enable verbose EGL/Mesa loader debug
  --gdb                       Run inside GDB with ASan
  --tee                       Log to files AND stream live output to terminal
  --stdout                    Stream output directly to terminal (no log file)
  --logs-dir <dir>            Logs directory (default: ~/logs)
  -C, --config-dir <dir>      Config directory (default: ./env)
  -h, --help                  Show this help message
EOF
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)
            print_help
            ;;
        -w|--wayland-debug)
            if [ -n "$2" ] && [ "${2#-}" = "$2" ]; then
                export WAYLAND_DEBUG="$2"
                shift 2
            else
                export WAYLAND_DEBUG="server"
                shift
            fi
            ;;
        --wayland-debug=*)
            export WAYLAND_DEBUG="${1#*=}"
            shift
            ;;
        --no-wayland-debug)
            unset WAYLAND_DEBUG
            shift
            ;;
        --damage)
            if [ -n "$2" ] && [ "${2#-}" = "$2" ]; then
                export WLR_SCENE_DEBUG_DAMAGE="$2"
                shift 2
            else
                export WLR_SCENE_DEBUG_DAMAGE="highlight"
                shift
            fi
            ;;
        --damage=*)
            export WLR_SCENE_DEBUG_DAMAGE="${1#*=}"
            shift
            ;;
        --outputs)
            export WLR_WL_OUTPUTS="$2"
            shift 2
            ;;
        --outputs=*)
            export WLR_WL_OUTPUTS="${1#*=}"
            shift
            ;;
        --egl-debug)
            export EGL_LOG_LEVEL="debug"
            shift
            ;;
        --pixman)
            export WLR_RENDERER="pixman"
            shift
            ;;
        --no-build)
            SKIP_BUILD=1
            shift
            ;;
        --gdb)
            USE_GDB=1
            shift
            ;;
        --tee)
            OUTPUT_MODE="tee"
            shift
            ;;
        --stdout)
            OUTPUT_MODE="stdout"
            shift
            ;;
        --logs-dir)
            LOGS_BASE="$2"
            shift 2
            ;;
        --logs-dir=*)
            LOGS_BASE="${1#*=}"
            shift
            ;;
        -C|--config-dir)
            CONFIG_DIR="$2"
            shift 2
            ;;
        --)
            shift
            EXTRA_ARGS="$*"
            break
            ;;
        *)
            if [ "$1" = "${1#-}" ]; then
                LOGS_BASE="$1"
                shift
            else
                echo "Unknown option: $1" >&2
                echo "Run '$0 --help' for usage." >&2
                exit 1
            fi
            ;;
    esac
done

if [ "$SKIP_BUILD" -eq 0 ]; then
    echo "Configuring and compiling with ASan/UBSan..."
    [ -d "${BUILD_DIR}" ] || meson setup "${BUILD_DIR}"
    meson configure -Db_sanitize=address,undefined "${BUILD_DIR}"
    ninja -C "${BUILD_DIR}"
fi

BIN="${BUILD_DIR}/labwc"
if [ ! -x "$BIN" ]; then
    echo "Error: Binary not found at $BIN." >&2
    exit 1
fi

export LSAN_OPTIONS="suppressions=${SCRIPT_DIR}/scripts/asan_leak_suppressions"

TIMESTAMP="$(date +'%Y%m%d_%H%M%S')"
mkdir -p "$LOGS_BASE"

FULL_LOG="${LOGS_BASE}/waylab_${TIMESTAMP}_full.log"
COMPOSITOR_LOG="${LOGS_BASE}/waylab_${TIMESTAMP}_compositor.log"
WAYLAND_LOG="${LOGS_BASE}/waylab_${TIMESTAMP}_wayland.log"
EFFECTS_LOG="${LOGS_BASE}/waylab_${TIMESTAMP}_effects.log"
DRIVER_LOG="${LOGS_BASE}/waylab_${TIMESTAMP}_driver.log"

SESSION_DIR="${LOGS_BASE}/session_${TIMESTAMP}"
mkdir -p "$SESSION_DIR"

ln -sf "$FULL_LOG" "${SESSION_DIR}/full.log"
ln -sf "$COMPOSITOR_LOG" "${SESSION_DIR}/compositor.log"
ln -sf "$WAYLAND_LOG" "${SESSION_DIR}/wayland.log"
ln -sf "$EFFECTS_LOG" "${SESSION_DIR}/effects.log"
ln -sf "$DRIVER_LOG" "${SESSION_DIR}/driver.log"

ln -sf "$FULL_LOG" "${LOGS_BASE}/latest_full.log"
ln -sf "$COMPOSITOR_LOG" "${LOGS_BASE}/latest_compositor.log"
ln -sf "$WAYLAND_LOG" "${LOGS_BASE}/latest_wayland.log"
ln -sf "$EFFECTS_LOG" "${LOGS_BASE}/latest_effects.log"
ln -sf "$DRIVER_LOG" "${LOGS_BASE}/latest_driver.log"
ln -sfn "$SESSION_DIR" "${LOGS_BASE}/latest"

echo "=== Running Waylab with ASan/UBSan ==="
echo "Binary         : $BIN"
echo "Config dir     : $CONFIG_DIR"
echo "Logs dir       : $LOGS_BASE"
echo "  - Full       : $FULL_LOG"
echo "  - Compositor : $COMPOSITOR_LOG"
echo "  - Wayland    : $WAYLAND_LOG"
echo "  - GL Effects : $EFFECTS_LOG"
echo "  - Driver/EGL : $DRIVER_LOG"
echo "  - Latest link: ${LOGS_BASE}/latest"
[ -n "$WAYLAND_DEBUG" ] && echo "WAYLAND_DEBUG  : $WAYLAND_DEBUG"
[ -n "$WLR_SCENE_DEBUG_DAMAGE" ] && echo "Damage Debug   : $WLR_SCENE_DEBUG_DAMAGE"
[ -n "$WLR_WL_OUTPUTS" ] && echo "Virtual Outputs: $WLR_WL_OUTPUTS"
[ -n "$WLR_RENDERER" ] && echo "Renderer       : $WLR_RENDERER"
[ -n "$EGL_LOG_LEVEL" ] && echo "EGL Log Level  : $EGL_LOG_LEVEL"
echo "LSAN Suppr.    : $LSAN_OPTIONS"

SPLITTER="$SCRIPT_DIR/scripts/split-logs.awk"

if [ "$USE_GDB" -eq 1 ]; then
    echo "Running inside GDB..."
    exec gdb -ex run --args "$BIN" -d -C "$CONFIG_DIR" $EXTRA_ARGS
fi

case "$OUTPUT_MODE" in
    stdout)
        echo "Streaming logs to stdout/stderr"
        exec "$BIN" -d -C "$CONFIG_DIR" $EXTRA_ARGS
        ;;
    tee)
        echo "Splitting logs to $LOGS_BASE and streaming to terminal"
        exec "$BIN" -d -C "$CONFIG_DIR" $EXTRA_ARGS 2>&1 | awk -f "$SPLITTER" \
            -v FULL_LOG="$FULL_LOG" \
            -v COMPOSITOR_LOG="$COMPOSITOR_LOG" \
            -v WAYLAND_LOG="$WAYLAND_LOG" \
            -v EFFECTS_LOG="$EFFECTS_LOG" \
            -v DRIVER_LOG="$DRIVER_LOG" \
            -v TEE=1
        ;;
    split|*)
        echo "Splitting logs in background to $LOGS_BASE"
        exec "$BIN" -d -C "$CONFIG_DIR" $EXTRA_ARGS 2>&1 | awk -f "$SPLITTER" \
            -v FULL_LOG="$FULL_LOG" \
            -v COMPOSITOR_LOG="$COMPOSITOR_LOG" \
            -v WAYLAND_LOG="$WAYLAND_LOG" \
            -v EFFECTS_LOG="$EFFECTS_LOG" \
            -v DRIVER_LOG="$DRIVER_LOG" \
            -v TEE=0
        ;;
esac
