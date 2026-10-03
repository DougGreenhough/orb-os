#!/usr/bin/env bash
# Build the simulator for the browser (Emscripten + SDL2 + Asyncify).
#
#   tools/build_web.sh [outdir]          # default build/web/
#
# Writes orb.js and orb.wasm into outdir, and orb.data when there is an SD card to preload.
# The page loads orb.js with a global `Module` whose `canvas` is a 466x466 <canvas
# id="canvas">, and serves api.php next to itself (see ponderer.cpp's demo mode).
#
# Same sources and the same -D/-I flags as `pio run -e native`: both are read out of
# platformio.ini's [env:native] here rather than copied, so the two builds cannot drift.
# Libraries come from .pio/libdeps/native (fetched by `pio pkg install -e native` if absent).
#
# Environment:
#   ORB_WEB_SDCARD=dir   the SD card to preload as /sim/sdcard (default: this checkout's
#                        sim/sdcard, if there is one); ORB_WEB_SDCARD=none preloads nothing.
#   ORB_WEB_DEBUG=1      -O1 -g with assertions, for chasing a crash.
#   JOBS=n               parallel compiles (default: CPU count).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/build/web}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
OBJ="$ROOT/build/web-obj"; [ "${ORB_WEB_DEBUG:-0}" = 1 ] && OBJ="$OBJ-debug"
mkdir -p "$OBJ"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"

command -v emcc >/dev/null || { echo "emcc not found: brew install emscripten" >&2; exit 1; }

LIBDEPS="$ROOT/.pio/libdeps/native"
if [ ! -d "$LIBDEPS/lvgl" ]; then
    echo "[web] fetching libraries (pio pkg install -e native)"
    (cd "$ROOT" && pio pkg install -e native >/dev/null)
fi

# ---- read [env:native] out of platformio.ini --------------------------------------------
read_native() {   # $1 = key; prints its value (continuation lines joined, comments dropped)
    python3 - "$ROOT/platformio.ini" "$1" <<'PY'
import sys, re
path, key = sys.argv[1], sys.argv[2]
sec, cur, out = None, None, []
for raw in open(path):
    line = raw.rstrip('\n')
    m = re.match(r'^\[(.+)\]\s*$', line)
    if m: sec, cur = m.group(1), None; continue
    if sec != 'env:native': continue
    stripped = re.sub(r'\s;.*$', '', line)          # inline comment
    if re.match(r'^\s*;', line) or not stripped.strip():
        continue
    m = re.match(r'^([A-Za-z_]+)\s*=\s*(.*)$', stripped)
    if m: cur = m.group(1); v = m.group(2)
    elif line[:1].isspace() and cur: v = stripped.strip()
    else: continue
    if cur == key and v: out.append(v)
print('\n'.join(out))
PY
}
SOURCES=$(read_native build_src_filter | grep -o '+<[^>]*>' | sed 's/^+<//; s/>$//')
# The -D/-I/-std flags only: the rest of that list is libcurl and SDL for the desktop.
FLAGS=$(read_native build_flags | grep -E '^-(D|I|std)' | sed "s|\${PROJECT_DIR}|$ROOT|g; s|\"||g")
CXXSTD=$(echo "$FLAGS" | grep '^-std' || true)
DEFS=$(echo "$FLAGS" | grep -v '^-std' || true)
[ -n "$SOURCES" ] || { echo "no build_src_filter found in [env:native]" >&2; exit 1; }

if [ "${ORB_WEB_DEBUG:-0}" = 1 ]; then OPT="-O1 -g"; LINKDBG="-sASSERTIONS=2 -g"
else OPT="-O3"; LINKDBG=""; fi

COMMON=(
    $OPT -sUSE_SDL=2
    $DEFS
    -I"$ROOT/src" -I"$ROOT/include"
    -I"$LIBDEPS/lvgl" -I"$LIBDEPS/ArduinoJson/src" -I"$LIBDEPS/PNGdec/src"
    -I"$ROOT/lib/monocypher/src"
    -D__LINUX__        # PNGdec's own switch for "not Arduino" (it knows __MACH__ and __LINUX__)
    -Wno-unused-function -Wno-unused-variable -Wno-deprecated-declarations
)

# ---- compile (in parallel, only what changed) ---------------------------------------------
LIST="$OBJ/sources.txt"
: > "$LIST"
for f in $SOURCES; do echo "$ROOT/src/$f" >> "$LIST"; done
find "$LIBDEPS/lvgl/src" -name '*.c' | sort >> "$LIST"
ls "$LIBDEPS/PNGdec/src/"*.c "$LIBDEPS/PNGdec/src/PNGdec.cpp" >> "$LIST"
ls "$ROOT/lib/monocypher/src/"*.c >> "$LIST"

FLAGFILE="$OBJ/flags.txt"
printf '%s\n' "${COMMON[@]}" "$CXXSTD" > "$FLAGFILE.new"
if ! cmp -s "$FLAGFILE.new" "$FLAGFILE"; then rm -f "$OBJ"/*.o; mv "$FLAGFILE.new" "$FLAGFILE"; else rm "$FLAGFILE.new"; fi

export OBJ ROOT CXXSTD
export COMMON_FLAGS="${COMMON[*]}"
compile_one() {
    src="$1"
    o="$OBJ/$(echo "${src#$ROOT/}" | tr '/' '_').o"
    # Headers are not tracked: anything newer in src/ or include/ rebuilds the project sources.
    if [ -f "$o" ] && [ "$o" -nt "$src" ]; then
        case "$src" in
            "$ROOT"/src/*) [ -z "$(find "$ROOT/src" "$ROOT/include" -name '*.h' -newer "$o" -print -quit)" ] && exit 0 ;;
            *) exit 0 ;;
        esac
    fi
    case "$src" in
        *.c)   emcc $COMMON_FLAGS -c "$src" -o "$o" ;;
        *)     em++ $COMMON_FLAGS $CXXSTD -c "$src" -o "$o" ;;
    esac || { echo "FAILED: $src" >&2; exit 255; }
}
export -f compile_one
echo "[web] compiling $(wc -l < "$LIST" | tr -d ' ') files with $JOBS jobs"
tr '\n' '\0' < "$LIST" | xargs -0 -n1 -P "$JOBS" bash -c 'compile_one "$0"'

OBJS=()
while IFS= read -r src; do OBJS+=("$OBJ/$(echo "${src#$ROOT/}" | tr '/' '_').o"); done < "$LIST"

# ---- the SD card -------------------------------------------------------------------------
SD="${ORB_WEB_SDCARD:-$ROOT/sim/sdcard}"
PRELOAD=()
rm -f "$OUT/orb.data"
if [ -n "$SD" ] && [ "$SD" != none ] && [ -d "$SD" ]; then
    PRELOAD=(--preload-file "$SD@/sim/sdcard")
    echo "[web] preloading $SD as /sim/sdcard"
else
    echo "[web] no SD card preloaded (Photos' SD source will say there is no card)"
fi

# Asyncify only has to instrument the network loop's call chains, not every function an
# LVGL callback could reach, so indirect calls are assumed not to suspend. The one indirect
# hop on the way to a fetch is ponderer::net_tick() calling each module's netStep.
echo '["ponderer::net_tick()"]' > "$OBJ/asyncify_add.json"

# ---- link ---------------------------------------------------------------------------------
echo "[web] linking"
em++ "${OBJS[@]}" -o "$OUT/orb.js" \
    $OPT $LINKDBG -sUSE_SDL=2 \
    -sASYNCIFY -sASYNCIFY_STACK_SIZE=131072 \
    -sASYNCIFY_IGNORE_INDIRECT -sASYNCIFY_ADD=@"$OBJ/asyncify_add.json" \
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sSTACK_SIZE=1MB \
    -sENVIRONMENT=web \
    -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_orb_knob,_orb_rock,_orb_app_count,_orb_app_name,_orb_app_hidden,_orb_app_current,_orb_select_app,_orb_redraw,_orb_perf,_orb_frame,_orb_net_loop \
    -sEXPORTED_RUNTIME_METHODS=UTF8ToString,ccall,cwrap,FS,addRunDependency,removeRunDependency \
    -sFORCE_FILESYSTEM=1 \
    ${PRELOAD[@]+"${PRELOAD[@]}"}

ls -l "$OUT"/orb.* | awk '{printf "[web] %-10s %8.1f KB\n", $NF, $5/1024}' | sed "s|$OUT/||"
