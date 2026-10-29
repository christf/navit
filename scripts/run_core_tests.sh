#!/usr/bin/env bash
# #####################################################################################################################
# Build and run the navit core test harness (navit/tests) in a dedicated static build tree.
#
# The harness links against builtin_init(), which is only part of libnavit_core when plugins
# are compiled static (USE_PLUGINS=OFF), so it gets its own build directory and must not be
# mixed into the regular plugin-based development builds.
#
# A static libnavit_core must contain every enabled module, and every module drags in its
# external dependencies at link time. Only the modules the harness exercises are kept, so the
# build is minimal and deterministic on machines that have GL/GTK/Qt/dbus/etc. installed.
#
# --mutate  additionally proves that each scenario can spot a regression: it applies one
#           deliberate bug at a time to the sources, rebuilds, runs the whole suite, and
#           restores each file from a backup copy afterwards.
#
# Extra arguments are passed to the cmake configure step.
# Usage: scripts/run_core_tests.sh [--mutate] [-DCMAKE_BUILD_TYPE=Release ...]
# #####################################################################################################################
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-tests}"

cmake_opts=(
    -DUSE_PLUGINS=OFF
    -DBUILD_NAVIT_TESTS=ON
)

for module in \
    binding/dbus \
    font/freetype \
    graphics/gtk_drawing_area graphics/gtkglarea graphics/opengl graphics/sdl graphics/svg_debug \
    map/csv map/garmin map/mg map/shapefile map/textfile \
    speech/dbus speech/speech_dispatcher \
    support/shapefile \
    traffic/traff_http \
    vehicle/demo vehicle/file vehicle/geoclue vehicle/gpsd vehicle/gpsd_dbus; do
    cmake_opts+=("-D${module}:BOOL=FALSE")
done

MUTATE=0
for arg in "$@"; do
    [[ "$arg" == "--mutate" ]] && MUTATE=1
done

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    cmake -S "$ROOT" -B "$BUILD_DIR" "${cmake_opts[@]}" "${@//--mutate/}"
fi

scenarios=(rect_convention proj_roundtrip route_car_grid route_bike_vs_car route_oneway
    route_turn_restriction search_town_street)

run_suite() {
    local out s
    out="$(cd "$BUILD_DIR" && NAVIT_TEST_MAP="$BUILD_DIR/navit/tests/fixture.bin" \
        timeout 600 ./navit/tests/navit_core_test all 2>&1)"
    for s in "${scenarios[@]}"; do
        printf "%-24s %s\n" "$s" "$(printf '%s\n' "$out" | grep -qE "^${s}[[:space:]]+PASS" \
            && echo PASS || echo FAIL)"
    done
}

# label|file|sed (BRE) replacement|scenario expected to catch the bug
MUTATIONS=(
    "rect_selection|navit/route.c|s/^\\([ \t]*\\)sel->u\\.c_rect\\.rl\\.y -= m;/\\1sel->u.c_rect.rl.y += m;/|rect_convention"
    "mercator_y|navit/transform.c|s@^\\([ \t]*\\)c->y = log(navit_tan(G_PI_4 + g->lat \\* G_PI \\/ 360)) \\* 6371000\\.0;@\\1c->y = navit_tan(G_PI_4 + g->lat * G_PI / 360) * 6371000.0;@|proj_roundtrip"
    "reverse_mask|navit/route.c|s/dir >= 0 ? profile->flags_forward_mask : profile->flags_reverse_mask/profile->flags_forward_mask/|route_oneway"
    "turn_restriction|navit/route.c|s/^\\([ \t]*\\)route_graph_add_turn_restriction(rg, item);$/\\1(void) 0; \\/\\/ route_graph_add_turn_restriction(rg, item);/|route_turn_restriction"
)

if [[ "$MUTATE" -eq 1 ]]; then
    exit_status=0
    cmake --build "$BUILD_DIR" --target navit_core_test -j "$(nproc)" >/dev/null
    printf '%-24s %s\n' "baseline" "$(run_suite | grep -c FAIL >/dev/null && echo FAIL || echo PASS)"
    for mut in "${MUTATIONS[@]}"; do
        IFS='|' read -r label file repl target <<<"$mut"
        printf '%-24s mutating %s (%s)\n' "" "$label" "$file"
        backup="$(mktemp)"
        cp "$ROOT/$file" "$backup"
        sed -i "$repl" "$ROOT/$file"
        trap "cp '$backup' '$ROOT/$file'; rm -f '$backup'" EXIT
        cmake --build "$BUILD_DIR" --target navit_core_test -j "$(nproc)" >/dev/null
        matrix="$(run_suite)"
        printf '%s\n' "$matrix"
        failures="$(printf '%s\n' "$matrix" | grep -c FAIL)"
        target_caught=0
        printf '%s\n' "$matrix" | grep -qE "^${target}[[:space:]]+FAIL" && target_caught=1
        if [[ "$failures" -gt 0 && "$target_caught" -eq 1 ]]; then
            printf '%-24s ok: caught by %s (+%s other fail(s))\n' "$label" "$target" "$((failures - 1))"
        else
            printf '%-24s FAILED to prove scenario sensitivity\n' "$label"
            exit_status=1
        fi
        cp "$backup" "$ROOT/$file"
        rm -f "$backup"
        trap - EXIT
    done
    printf '%-24s %s\n' "mutation matrix" "$([ "$exit_status" -eq 0 ] && echo ok || echo FAILED)"
    exit "$exit_status"
fi

cmake --build "$BUILD_DIR" --target navit_core_test -j "$(nproc)"

ctest --test-dir "$BUILD_DIR" --output-on-failure -R '^core$'