#!/bin/bash
# Drive the real navit binary with the demo vehicle along a real route and dump
# the position trace.
#
# The stall needs a real map (sachsen-anhalt.bin), so this deliberately does not
# use the test fixture. Set DEMO_STALL_REAL_CONFIG to run the user's own config
# instead of the bundled minimal one; that is the reproduction that matters. The
# bundled demo_stall_navit.xml also oscillates but far more slowly, because it
# tracks differently from a config that has been through the GUI.
#
# Progress is read from the vehicle_demo "ci=0x..,0x.." log lines: the northing
# coordinate must increase monotonically. A stalling vehicle reverses and the
# trace goes backwards.
#
# Usage: run_demo_stall.sh <out.log> [secs] [graphics] [dlng dlat] [slng slat]
#   env: ORIENTATION (default -1), FOLLOW (default 1),
#        DEMO_STALL_REAL_CONFIG, DEMO_STALL_ACTIVE_VEHICLE
set -u

OUT="${1:?usage: run_demo_stall.sh <out.log> [seconds] [graphics] [dlng dlat] [slng slat]}"
SECS="${2:-60}"
GFX="${3:-gtkglarea}"
DLNG="${4:-13.595}"
DLAT="${5:-52.136}"
DLNG_START="${6:-11.614722}"
DLAT_START="${7:-52.107222}"
ORIENTATION="${ORIENTATION:--1}"
FOLLOW="${FOLLOW:-1}"

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BUILD="${NAVIT_BUILD:-$ROOT/build}"
CFG=/tmp/demo_stall_cfg.xml

cleanup() {
    [ -n "${NAVIT_PID:-}" ] && kill "$NAVIT_PID" 2>/dev/null
    [ -n "${XVFB_PID:-}" ] && kill "$XVFB_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

BIN="$BUILD/navit/navit"
[ -x "$BIN" ] || BIN="$BUILD/navit"
[ -x "$BIN" ] || { echo "navit binary not found; build first" >&2; exit 1; }

# The build tree nests plugins as <category>/<name>/lib*.so while the install
# layout (and the $NAVIT_LIBDIR globs) wants <libdir>/<category>/lib*.so.
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
# Coordinates are parsed with sscanf("%lf"), so a comma-decimal locale makes
# "11.614722 52.107222" parse as a single field. navit/main.c calls
# setlocale(LC_NUMERIC, "C") itself, but keep the harness locale-safe too.
export LC_ALL=C
export LC_NUMERIC=C

if [ -n "${DEMO_STALL_REAL_CONFIG:-}" ]; then
    # Use the user's own config, only swapping the graphics type so it runs headless.
    # DEMO_STALL_ACTIVE_VEHICLE is needed because a real config often lists several
    # vehicles, and whichever one is active wins over the demo one.
    # Debug output is scoped to the demo vehicle: "global" debug on a real config
    # produces tens of millions of lines and makes the run unusable.
    # Debug elements must follow <plugins> per navit.dtd, and the dbg module name
    # for libnavit_core is "navit", not "route" (see debug.h dbg_module/MODULE).
    sed -e "s/GRAPHICS_TYPE/$GFX/" \
        -e "s|</plugins>|</plugins><debug name=\"vehicle_demo\" dbg_level=\"debug\"/><debug name=\"navit:route_set_position_flags\" dbg_level=\"debug\"/>|" \
        -e "s|<vehicle name=\"${DEMO_STALL_ACTIVE_VEHICLE:-nmea-replay}\" profilename=\"[a-z]*\" active=\"[a-z0-9]*\" enabled=\"yes\"|<vehicle name=\"${DEMO_STALL_ACTIVE_VEHICLE:-nmea-replay}\" profilename=\"car\" active=\"no\" enabled=\"no\"|" \
        "$DEMO_STALL_REAL_CONFIG" > "$CFG"
else
    sed -e "s/GRAPHICS_TYPE/$GFX/" -e "s/ORIENTATION/$ORIENTATION/" -e "s/FOLLOW/$FOLLOW/" \
        -e "s/DLNG/$DLNG/g" -e "s/DLAT/$DLAT/g" \
        -e "s/SLNG/$DLNG_START/g" -e "s/SLAT/$DLAT_START/g" \
        "$HERE/demo_stall_navit.xml" > "$CFG"
fi

DISP=":$((RANDOM % 400 + 100))"
Xvfb "$DISP" -screen 0 800x600x24 >/dev/null 2>&1 &
XVFB_PID=$!
export DISPLAY="$DISP"
for _ in $(seq 1 50); do
    xdpyinfo >/dev/null 2>&1 && break
    sleep 0.1
done

# -e evaluates the command against the config object, which has no callback_list of
# its own, so navit commands have to be reached through "navit.". Coordinates are
# "lng lat" strings, as in <navit center=...>.
"$BIN" -c "$CFG" \
    -e "navit.set_position(\"$DLNG_START $DLAT_START\"); navit.set_destination(\"$DLNG $DLAT\", \"Lerchenwuhne 20\");" \
    >"$OUT" 2>&1 &
NAVIT_PID=$!
sleep "$SECS"
if kill -0 "$NAVIT_PID" 2>/dev/null; then
    kill "$NAVIT_PID" 2>/dev/null
else
    echo "navit exited early" >&2
fi
NAVIT_PID=
echo "log: $OUT"