#!/bin/bash
# Capture a Navit speed-warner frame under Xvfb for a given graphics backend.
# Usage: run_speedwarner.sh <graphics_type> <output.png> [seconds]
set -u

GFX="${1:?usage: run_speedwarner.sh <graphics_type> <out.png> [seconds]}"
OUT="${2:?missing output png}"
SECS="${3:-8}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BUILD="${NAVIT_BUILD:-$ROOT/build}"
CFG="$(mktemp /tmp/speedwarner_cfg_XXXX.xml)"
WORK="$(mktemp -d /tmp/speedwarner_work_XXXXXX)"

# navit needs a mapset or it refuses to start. Compile the small core-test fixture on demand
# rather than depending on the user's multi-hundred-MB .bin maps.
MAPFILE="$WORK/fixture.bin"
MAPTOOL="$BUILD/navit/maptool/maptool"
if [ ! -x "$MAPTOOL" ]; then
    echo "maptool not found at $MAPTOOL; build the project first" >&2
    exit 1
fi
"$MAPTOOL" -i "$ROOT/navit/tests/core/fixture.osm" "$MAPFILE" >/dev/null 2>&1 \
    || { echo "maptool failed to build $MAPFILE" >&2; exit 1; }

sed -e "s/GRAPHICS_TYPE/$GFX/" -e "s|MAPFILE|$MAPFILE|" \
    "$HERE/speedwarner_navit.xml" > "$CFG"

BIN="$BUILD/navit/navit"
[ -x "$BIN" ] || BIN="$BUILD/navit"

cleanup() {
    [ -n "${NAVIT_PID:-}" ] && kill "$NAVIT_PID" 2>/dev/null
    [ -n "${XVFB_PID:-}" ] && kill "$XVFB_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$CFG" "$STAGE" "$WORK"
}
trap cleanup EXIT

# The build tree nests plugins as <category>/<name>/lib*.so, but the install layout (and
# therefore the $NAVIT_LIBDIR globs in the config) is <libdir>/<category>/lib*.so.
# Stage a symlink farm so the standard globs resolve.
STAGE="$(mktemp -d /tmp/navitlib_XXXXXX)"
for so in "$BUILD"/navit/*/*/*.so; do
    [ -e "$so" ] || continue
    cat="$(basename "$(dirname "$(dirname "$so")")")"
    mkdir -p "$STAGE/$cat"
    ln -sf "$so" "$STAGE/$cat/$(basename "$so")"
done
mkdir -p "$STAGE/autoload"

export NAVIT_LIBDIR="$STAGE"
export NAVIT_SHAREDIR="$BUILD/navit"

# Start Xvfb ourselves so we can screenshot the real window contents.
DISP=":$((RANDOM % 400 + 100))"
Xvfb "$DISP" -screen 0 400x400x24 >/dev/null 2>&1 &
XVFB_PID=$!
export DISPLAY="$DISP"
for _ in $(seq 1 50); do
    xdpyinfo >/dev/null 2>&1 && break
    sleep 0.1
done

"$BIN" -c "$CFG" >/tmp/speedwarner_run.log 2>&1 &
NAVIT_PID=$!

# Let the map, vehicle and OSD settle.
sleep "$SECS"

if ! kill -0 "$NAVIT_PID" 2>/dev/null; then
    echo "navit exited early; log tail:" >&2
    tail -20 /tmp/speedwarner_run.log >&2
    exit 1
fi

# Capture the root window. The Navit toplevel sits at +0+0 at the configured w/h, and the
# speed-warner OSD lives at the window's top-left corner. xwd cannot read the GtkGLArea's
# X window directly (it is a GL surface, not a plain drawable), so root capture is the
# reliable path for both backends.
echo "navit window: $(xdotool search --name '^Navit$' 2>/dev/null | while read -r w; do
        xdotool getwindowgeometry --shell "$w" 2>/dev/null | tr '\n' ' '; echo; done)" >&2
import -window root "$OUT" 2>/dev/null || xwd -root -silent | convert xwd:- "$OUT"
echo "captured $OUT"