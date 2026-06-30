#!/usr/bin/env bash
# In-engine A/B validation for the modern-GL migration (LuaGLCompareMode).
#
# Runs the GL engine with LuaGLCompareMode=1 over a BAR startscript (or replay)
# and FAILS if any wired Lua gl.* callin (gl.Rect/gl.TexRect/gl.BeginEnd)
# diverges from the legacy fixed-function path by more than 1 byte, or if the
# compare FBOs could not be created, or if no callin was compared at all.
#
# This is the contract-C6 live check that the Catch2 unit tests can't give: it
# exercises REAL BAR widgets over real frames. It is NOT a ctest unit test --
# it needs a display, a GPU/llvmpipe, the GL `spring` binary, and a local BAR
# data dir with content.
#
# Usage:
#   glcompare_integration.sh <spring-bin> <data-dir> <startscript-or-replay>
# Env (optional): DISPLAY (def :0), SDL_VIDEODRIVER (def x11),
#   GLCOMPARE_TIMEOUT (def 360), LIBGL_ALWAYS_SOFTWARE (set 1 to force llvmpipe).
#
# Example:
#   test/engine/Rendering/glcompare_integration.sh \
#       build/spring /path/to/bar-data \
#       test/engine/Rendering/glcompare_startscript.txt

set -uo pipefail

SPRING="${1:?usage: glcompare_integration.sh <spring-bin> <data-dir> <startscript-or-replay>}"
DATADIR="${2:?missing data dir}"
SCRIPT="${3:?missing startscript or replay}"

TMP="$(mktemp -d)"
CFG="$TMP/glcompare.cfg"
LOG="$TMP/run.log"
trap 'rm -rf "$TMP"' EXIT

# exclusive config = the local data-dir config + the compare flag, so we don't
# mutate the user's springsettings.cfg.
cp "$DATADIR/springsettings.cfg" "$CFG" 2>/dev/null || : > "$CFG"
cat >> "$CFG" <<EOF
LuaGLCompareMode = 1
LuaModernGLBackend = 0
Fullscreen = 0
XResolutionWindowed = 1280
YResolutionWindowed = 720
HangTimeout = 60
EOF

: "${DISPLAY:=:0}"
timeout "${GLCOMPARE_TIMEOUT:-360}" \
  env DISPLAY="$DISPLAY" \
      SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}" \
      SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null \
      ${LIBGL_ALWAYS_SOFTWARE:+LIBGL_ALWAYS_SOFTWARE="$LIBGL_ALWAYS_SOFTWARE"} \
      "$SPRING" --isolation --write-dir "$DATADIR" --config "$CFG" "$SCRIPT" > "$LOG" 2>&1
ec=$?

compares=$(grep -c "LuaGLCompare: gl\." "$LOG")
fbo=$(grep -c "compare FBO unavailable" "$LOG")
maxd=$(grep -oE "max byte delta = [0-9]+" "$LOG" | awk '{print $5}' | sort -n | tail -1)
diverged=$(grep "max byte delta" "$LOG" | awk -F'= ' '{ if (($2 + 0) > 1) print }')

echo "spring exit=$ec  callins_compared=$compares  fbo_unavailable=$fbo  max_byte_delta=${maxd:-none}"

if [ "$compares" -eq 0 ]; then
	echo "FAIL: no gl.* callins were compared -- did the run reach a frame?"
	echo "--- log tail ---"; tail -25 "$LOG"
	exit 1
fi
if [ "$fbo" -ne 0 ]; then
	echo "FAIL: compare FBOs could not be created ($fbo occurrences)"
	exit 1
fi
if [ -n "$diverged" ]; then
	echo "FAIL: modern path diverged from legacy (> 1 byte):"
	echo "$diverged"
	exit 1
fi

echo "PASS: all $compares wired gl.* callins within 1 LSB of legacy"
exit 0
