#!/bin/sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
: ${BUILD_DIR="${SCRIPT_DIR}/build"}
LOG_FILE="log.txt"
CONFIG_DIR="$SCRIPT_DIR/env"
OUTPUT_MODE="file"
USE_GDB=0
SKIP_BUILD=0
EXTRA_ARGS=""

print_help() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS] [-- <extra labwc args>]

Build and run Waylab with AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan).

Options:
  -w, --wayland-debug [VAL]   Enable Wayland protocol debug (default: server)
  --damage [MODE]             Enable visual damage tracking (highlight/rerender)
  --no-build                  Skip meson configure and ninja compile
  --outputs <N>               Emulate N virtual outputs (WLR_WL_OUTPUTS=N)
  --pixman                    Force Pixman software renderer
  --egl-debug                 Enable verbose EGL/Mesa loader debug
  --gdb                       Run inside GDB with ASan
  --tee                       Log to file and stream live output to terminal
  --stdout                    Stream output directly to terminal (no log file)
  -l, --log <file>            Log file (default: log.txt)
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
        -l|--log)
            LOG_FILE="$2"
            shift 2
            ;;
        --log=*)
            LOG_FILE="${1#*=}"
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
                LOG_FILE="$1"
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

echo "=== Running Waylab with ASan/UBSan ==="
echo "Binary        : $BIN"
echo "Config dir    : $CONFIG_DIR"
[ -n "$WAYLAND_DEBUG" ] && echo "WAYLAND_DEBUG : $WAYLAND_DEBUG"
[ -n "$WLR_SCENE_DEBUG_DAMAGE" ] && echo "Damage Debug  : $WLR_SCENE_DEBUG_DAMAGE"
[ -n "$WLR_WL_OUTPUTS" ] && echo "Virtual Outputs: $WLR_WL_OUTPUTS"
[ -n "$WLR_RENDERER" ] && echo "Renderer      : $WLR_RENDERER"
[ -n "$EGL_LOG_LEVEL" ] && echo "EGL Log Level : $EGL_LOG_LEVEL"
echo "LSAN Suppr.   : $LSAN_OPTIONS"

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
        echo "Logging to $LOG_FILE and streaming to terminal"
        exec "$BIN" -d -C "$CONFIG_DIR" $EXTRA_ARGS 2>&1 | tee "$LOG_FILE"
        ;;
    file|*)
        echo "Logging output to $LOG_FILE"
        exec "$BIN" -d -C "$CONFIG_DIR" $EXTRA_ARGS 2> "$LOG_FILE"
        ;;
esac
