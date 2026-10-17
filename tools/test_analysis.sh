#!/bin/sh
# The listener and the band against a synthetic jam with a known answer: C major at 100 BPM, chords C F G Am.
#   tools/test_analysis.sh [path-to-autojam]      (default: build/work/autojam, or dist/autojam/autojam)
# Stage 1: --analyze on the jam must name the key, a tempo within 3 BPM (or its double/half) and all four chords.
# Stage 2: the band plays along to the jam (--render); its own output, analyzed, must give the same key and chords.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${1:-}; [ -n "$BIN" ] || { [ -x "$ROOT/build/work/autojam" ] && BIN=$ROOT/build/work/autojam || BIN=$ROOT/dist/autojam/autojam; }
[ -x "$BIN" ] || { echo "no autojam binary at $BIN"; exit 2; }
T=${TMPDIR:-/tmp}/autojam-test.$$; mkdir -p "$T"
python3 "$ROOT/tools/make_test_jam.py" "$T/jam.wav" --bpm 100 --key C >/dev/null || exit 2
FAIL=0
check() {   # check <label> <summary line>
  S=$2; echo "$S"
  echo "$S" | grep -q "key C major" && echo "  PASS: $1 key C major" || { echo "  FAIL: $1 key"; FAIL=1; }
  BPM=$(echo "$S" | sed -n 's/.*tempo \([0-9.]*\) BPM.*/\1/p')
  python3 -c "import sys; b=float('$BPM' or 0); ok=any(abs(b-x)<=3 for x in (100,200,50)); print('  PASS: $1 tempo', b) if ok else print('  FAIL: $1 tempo', b); sys.exit(0 if ok else 1)" || FAIL=1
  for c in " C " " F " " G " " Am"; do echo "$S chords:" | grep -q "chords:.*$c" && echo "  PASS: $1 chord$c" || { echo "  FAIL: $1 chord$c"; FAIL=1; }; done
}
echo "== stage 1: the listener"
OUT=$("$BIN" --analyze "$T/jam.wav") || { echo "$OUT"; exit 1; }
check listener "$(echo "$OUT" | tail -1)"
echo "== stage 2: the band plays along, its output analyzed"
R=$("$BIN" --wav "$T/jam.wav" --render "$T/band.wav" --seconds 30) || { echo "$R"; exit 1; }
echo "$R"; echo "$R" | grep -q "peak -[0-9]" && echo "  PASS: band made sound" || { echo "  FAIL: band silent"; FAIL=1; }
OUT=$("$BIN" --analyze "$T/band.wav") || { echo "$OUT"; exit 1; }
check band "$(echo "$OUT" | tail -1)"
rm -rf "$T"; exit $FAIL
