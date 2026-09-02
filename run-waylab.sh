#!/bin/sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG_FILE="/tmp/waylab-debug.log"
CONFIG_DIR="$SCRIPT_DIR/env"
OUTPUT_MODE="file"
USE_GDB=0
BUILD_ASAN=0
EXTRA_ARGS=""

print_help() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS] [-- <extra labwc args>]

Launcher for Waylab with comprehensive debugging options.

Debugging Options:
  -w, --wayland-debug [VAL]   Enable Wayland protocol debug.
                              Values: 'server' (default: logs compositor protocol),
                                      '1' (logs client protocol)
  --damage [MODE]             Enable visual damage tracking via wlroots scene.
                              Values: 'highlight' (default), 'rerender'
  --asan                      Run with AddressSanitizer & LeakSanitizer suppressions
  --build-asan                Reconfigure & compile build directory with ASan/UBSan
                              (meson configure -Db_sanitize=address,undefined)
  --outputs <N>               Emulate N virtual outputs (sets WLR_WL_OUTPUTS=N)
  --egl-debug                 Enable verbose Mesa/EGL loader logs (EGL_LOG_LEVEL=debug)
  --pixman                    Force Pixman software renderer (rules out GPU/driver bugs)
  --gdb                       Run inside GDB for backtraces on crashes
  --tee                       Log to file AND stream live output to terminal
  --stdout                    Output directly to terminal (no log file)
  -l, --log <file>            Specify custom log file (default: /tmp/waylab-debug.log)
  -C, --config-dir <dir>      Specify config directory (default: ./env)
  -h, --help                  Show this help message

Examples:
  ./run-waylab.sh --tee
  ./run-waylab.sh -w server --damage
  ./run-waylab.sh --build-asan --asan --gdb
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
        --asan)
            export LSAN_OPTIONS="suppressions=$SCRIPT_DIR/scripts/asan_leak_suppressions"
            shift
            ;;
        --build-asan)
            BUILD_ASAN=1
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

if [ "$BUILD_ASAN" -eq 1 ]; then
    echo "Configuring and compiling Waylab with ASan/UBSan..."
    meson configure -Db_sanitize=address,undefined "$SCRIPT_DIR/build"
    ninja -C "$SCRIPT_DIR/build"
    export LSAN_OPTIONS="suppressions=$SCRIPT_DIR/scripts/asan_leak_suppressions"
fi

BIN="$SCRIPT_DIR/build/labwc"
if [ ! -x "$BIN" ]; then
    echo "Error: Binary not found at $BIN. Compile first with 'meson compile -C build'." >&2
    exit 1
fi

echo "=== Launching Waylab ==="
echo "Config dir    : $CONFIG_DIR"
[ -n "$WAYLAND_DEBUG" ] && echo "WAYLAND_DEBUG : $WAYLAND_DEBUG"
[ -n "$WLR_SCENE_DEBUG_DAMAGE" ] && echo "Damage Debug  : $WLR_SCENE_DEBUG_DAMAGE"
[ -n "$WLR_WL_OUTPUTS" ] && echo "Virtual Outputs: $WLR_WL_OUTPUTS"
[ -n "$WLR_RENDERER" ] && echo "Renderer      : $WLR_RENDERER"
[ -n "$EGL_LOG_LEVEL" ] && echo "EGL Log Level : $EGL_LOG_LEVEL"
[ -n "$LSAN_OPTIONS" ] && echo "ASan Suppr.   : $LSAN_OPTIONS"

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
        echo "Logging debug output to $LOG_FILE"
        exec "$BIN" -d -C "$CONFIG_DIR" $EXTRA_ARGS > "$LOG_FILE" 2>&1
        ;;
esac
