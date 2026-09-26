#!/bin/sh
# Runs a UI test command, under xvfb-run when there is no display. Exits 77
# (ctest: skipped) when there is neither a display nor xvfb-run.
if [ -z "$DISPLAY" ] && [ -z "$WAYLAND_DISPLAY" ]; then
    if command -v xvfb-run >/dev/null 2>&1; then
        exec xvfb-run -a -s "-screen 0 1920x1080x24" "$@"
    fi
    echo "no display and no xvfb-run: UI test skipped" >&2
    exit 77
fi
exec "$@"
