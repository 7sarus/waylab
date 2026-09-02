#!/usr/bin/awk -f
# Split live Waylab output stream into categorized log files:
# Variables expected via -v:
#   FULL_LOG       : path for complete unfiltered log
#   COMPOSITOR_LOG : path for labwc/wlroots compositor logs
#   WAYLAND_LOG    : path for Wayland protocol traces
#   EFFECTS_LOG    : path for [gl-effects] blur & shader pipeline
#   DRIVER_LOG     : path for Mesa / EGL / DRM / GBM / NVIDIA logs
#   TEE            : 1 to also print to stdout in real-time, 0 otherwise

{
    line = $0

    if (TEE) {
        print line
        fflush()
    }

    if (FULL_LOG != "") {
        print line >> FULL_LOG
        fflush(FULL_LOG)
    }

    # 1. Wayland Protocol Traces (WAYLAND_DEBUG)
    # Examples:
    #   [12345.678] -> wl_compositor@1.create_surface(...)
    #   [12345.678] wl_surface@3.attach(...)
    #   [12345.678] zxdg_toplevel_v6@7.configure(...)
    if (line ~ /^\[[0-9]+(\.[0-9]+)?\] (-> )?[a-zA-Z0-9_]+@[0-9]+/ || line ~ /@[0-9]+(\.[a-zA-Z0-9_]+)?\(/) {
        if (WAYLAND_LOG != "") {
            print line >> WAYLAND_LOG
            fflush(WAYLAND_LOG)
        }
    }
    # 2. Compositor logs (labwc & wlroots)
    # Examples:
    #   00:09:36.100 [INFO] [../src/config/rcxml.c:623] ...
    #   00:09:36.247 [DEBUG] [xcursor/wlr_xcursor.c:233] ...
    else if (line ~ /^[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3} \[(INFO|DEBUG|ERROR|WARN|TRACE)\]/ || line ~ /^[0-9]{2}:[0-9]{2}:[0-9]{2} \[(INFO|DEBUG|ERROR|WARN|TRACE)\]/) {
        if (COMPOSITOR_LOG != "") {
            print line >> COMPOSITOR_LOG
            fflush(COMPOSITOR_LOG)
        }

        # Sub-category: GL Effects shader & blur pipeline
        if (line ~ /\[gl-effects\]/ || line ~ /gl-effects\.c/) {
            if (EFFECTS_LOG != "") {
                print line >> EFFECTS_LOG
                fflush(EFFECTS_LOG)
            }
        }
    }
    # 3. Graphics Drivers / Mesa / EGL / DRM / GBM / NVIDIA
    # Examples:
    #   MESA-LOADER: loading /usr/lib/dri/...
    #   libEGL debug: ...
    #   [drm] ...
    #   NVIDIA: ...
    else if (line ~ /MESA-LOADER/ || line ~ /libEGL/ || line ~ /EGL/ || line ~ /GBM/ || line ~ /NVIDIA/ || line ~ /nvidia/ || line ~ /\[drm\]/ || line ~ /iris_dri/ || line ~ /radeonsi_dri/) {
        if (DRIVER_LOG != "") {
            print line >> DRIVER_LOG
            fflush(DRIVER_LOG)
        }
    }
    # 4. Fallback / Unclassified logs go to compositor log
    else {
        if (COMPOSITOR_LOG != "") {
            print line >> COMPOSITOR_LOG
            fflush(COMPOSITOR_LOG)
        }
    }
}
