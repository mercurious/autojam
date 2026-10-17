#!/usr/bin/env bash
# Builds autojam for an aarch64 ROCKNIX handheld inside an Ubuntu 24.04 container (glibc 2.39 < ROCKNIX 2.41) and
# assembles dist/autojam/ with the llamatalk kit (llama-server, tts-server, models.sh) beside it.
#   ENGINE=podman (default) or docker.  JOBS=4.
#   LLAMATALK_DIST=DIR   an unpacked llamatalk dist to bundle (default: fetch the pinned release tarball; the
#                        llamatalk repo's own build/build.sh makes one).  LLAMATALK_TGZ=FILE  a tarball of it.
# Output: dist/autojam/{autojam,assets,launch.sh,llamatalk/,extras/,tools/} and dist/autojam-<version>-aarch64.tar.gz
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE=${ENGINE:-podman}; IMAGE=autojam-build; VER=$(cat "$ROOT/VERSION")
LT_VER=0.1.0; LT_URL=https://github.com/mercurious/llamatalk/releases/download/v$LT_VER/llamatalk-$LT_VER-aarch64.tar.gz
RUNAS=; [ "$ENGINE" = docker ] && RUNAS="--user $(id -u):$(id -g)"
$ENGINE image inspect $IMAGE >/dev/null 2>&1 || $ENGINE build -q -t $IMAGE -f "$ROOT/build/Containerfile" "$ROOT/build"
mkdir -p "$ROOT/build/work"
$ENGINE run --rm $RUNAS -v "$ROOT":/src:Z -w /src -e JOBS="${JOBS:-4}" $IMAGE bash -c '
  set -e
  cmake -S . -B build/work -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build build/work -j$JOBS
  strip build/work/autojam
'
OUT="$ROOT/dist/autojam"
rm -rf "$OUT"; mkdir -p "$OUT"
cp "$ROOT/build/work/autojam" "$OUT/"
cp -r "$ROOT/assets" "$ROOT/extras" "$ROOT/tools" "$OUT/"
cp "$ROOT/flip2/launch.sh" "$ROOT/LICENSE" "$ROOT/README.md" "$ROOT/THIRD_PARTY.md" "$ROOT/VERSION" "$OUT/"
rm -rf "$OUT/tools/__pycache__"
# the llamatalk kit
if [ -n "${LLAMATALK_DIST:-}" ] && [ -x "$LLAMATALK_DIST/bin/llama-server" ]; then
  cp -r "$LLAMATALK_DIST" "$OUT/llamatalk"
else
  TGZ=${LLAMATALK_TGZ:-$ROOT/build/work/llamatalk-$LT_VER-aarch64.tar.gz}
  [ -f "$TGZ" ] || curl -fsSL -o "$TGZ" "$LT_URL" || { echo "no llamatalk kit: set LLAMATALK_DIST or LLAMATALK_TGZ (see README)" >&2; exit 1; }
  tar -C "$OUT" -xzf "$TGZ"
fi
[ -x "$OUT/llamatalk/bin/llama-server" ] && [ -x "$OUT/llamatalk/bin/tts-server" ] || { echo "the llamatalk kit is incomplete" >&2; exit 1; }
{ echo "autojam $VER $(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo untracked)"; cat "$OUT/llamatalk/BUILD.txt" 2>/dev/null || true; } > "$OUT/BUILD.txt"
chmod +x "$OUT/autojam" "$OUT/launch.sh" "$OUT"/tools/*.sh "$OUT"/llamatalk/models.sh "$OUT"/llamatalk/bin/* 2>/dev/null || true
tar -C "$ROOT/dist" -czf "$ROOT/dist/autojam-$VER-aarch64.tar.gz" autojam
echo "built $OUT"; ls -la "$OUT"; du -sh "$ROOT/dist/autojam-$VER-aarch64.tar.gz"
