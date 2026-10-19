#!/bin/sh
# autojam on the Retroid Pocket Flip 2 (ROCKNIX). Installed to /storage/apps/autojam by flip2/install.sh.
# Starts Pocket TTS (tts-server) and Gemma 4 E4B (llama-server) from the bundled llamatalk kit on the big cores
# at low priority, then the app, whose audio thread takes the prime core with a real-time slice; stops them all
# on exit. Leave with Guide (Home) or Select+Start.
#   AJ_MODEL=e2b          the small Gemma (faster plans)         AJ_NO_AI=1   no servers: the band alone
#   AJ_EARS=0|1           send Gemma 8 s of audio with each full plan (default: 1 when the E4B audio projector is
#                         in the models directory and the device has 10 GB or more)
#   AJ_LLM_PORT=8103 AJ_TTS_PORT=8104 AJ_VOICE=name AJ_AUTO=120 (seconds between automatic plans, 0 = only on request)
#   AJ_SOURCE=<pulse source> AJ_MIDI=auto|none|/dev/snd/midiC1D0
#   AJ_LINK=47800|off     follow Underground Rap Machine on the LAN (its jam-link beacon: exact tempo, key, chords)
[ -f /etc/profile ] && . /etc/profile >/dev/null 2>&1
APP=${AJ_APP:-/storage/apps/autojam}
LOG=$APP/logs/launch.log
mkdir -p "$APP/logs"
echo "===== $(date -u +%FT%TZ) launch" >> "$LOG"
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-wayland}
export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-pulseaudio}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-1}
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/var/run/0-runtime-dir}
export SDL_GAMECONTROLLERCONFIG_FILE=${SDL_GAMECONTROLLERCONFIG_FILE:-/storage/.config/SDL-GameControllerDB/gamecontrollerdb.txt}
export AUTOJAM_DATA=$APP/data AUTOJAM_ASSETS=$APP/assets
export AUTOJAM_LLM="http://127.0.0.1:${AJ_LLM_PORT:-8103}" AUTOJAM_TTS="http://127.0.0.1:${AJ_TTS_PORT:-8104}"
[ -n "${AJ_SOURCE:-}" ] && export AUTOJAM_SOURCE=$AJ_SOURCE
[ -n "${AJ_MIDI:-}" ] && export AUTOJAM_MIDI=$AJ_MIDI
[ -n "${AJ_VOICE:-}" ] && export AUTOJAM_VOICE=$AJ_VOICE
[ -n "${AJ_AUTO:-}" ] && export AUTOJAM_AUTO=$AJ_AUTO
[ -n "${AJ_LINK:-}" ] && export AUTOJAM_LINK=$AJ_LINK
ulimit -c 0     # a crashed server must not leave a multi-GB core file behind

PIDS=""
cleanup() {
  trap - EXIT INT TERM HUP
  for p in $PIDS; do kill "$p" 2>/dev/null; done
  for p in $PIDS; do while kill -0 "$p" 2>/dev/null; do sleep 0.2; done; done
  echo "$(date -u +%FT%TZ) exit" >> "$LOG"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

ARGS=""
if [ -n "${AJ_NO_AI:-}" ]; then
  ARGS="--no-llm"
else
  LT=$APP/llamatalk
  M=$("$LT/models.sh" path 2>/dev/null) || M=""
  if [ "${AJ_MODEL:-e4b}" = e2b ]; then LLM=gemma-4-E2B_q4_0-it; MMPROJ=gemma-4-E2B-it-mmproj.gguf; MTP=mtp-gemma-4-E2B-it-Q4_0.gguf
  else LLM=gemma-4-E4B_q4_0-it; MMPROJ=gemma-4-E4B-it-mmproj.gguf; MTP=mtp-gemma-4-E4B-it-Q4_0.gguf; fi
  missing=""
  for f in "$LT/bin/llama-server" "$LT/bin/tts-server" "$M/$LLM.gguf" "$M/pocket-tts-english_2026-09-Q8_0.gguf" "$M/pocket-tts-english_2026-09-voices.gguf"; do
    [ -e "$f" ] || missing="$missing $f"
  done
  if [ -n "$missing" ]; then
    echo "autojam: the model kit is incomplete, running without Gemma; missing:$missing" | tee -a "$LOG" >&2
    ARGS="--no-llm"
  else
    MEM_MB=$(awk '/MemTotal/ {print int($2/1024)}' /proc/meminfo)
    EARS=${AJ_EARS:-}
    if [ -z "$EARS" ]; then if [ -f "$M/$MMPROJ" ] && [ "$MEM_MB" -ge 10000 ]; then EARS=1; else EARS=0; fi; fi
    [ "$EARS" = 1 ] && [ -f "$M/$MMPROJ" ] && { PROJ="--mmproj $M/$MMPROJ"; export AUTOJAM_EARS=1; } || { PROJ=""; export AUTOJAM_EARS=0; }
    DRAFT=""; [ -f "$M/$MTP" ] && [ -z "$PROJ" ] && DRAFT="-md $M/$MTP --spec-type draft-mtp"
    # SM8250: 0-3 little, 4-6 big, 7 prime. The servers get 4-6 at low priority (the model as a batch task); the
    # app's audio thread pins itself to 7 with a real-time slice, so the band never waits for Gemma.
    env GGML_BACKEND=CPU PT_THREADS=2 nice -n 5 taskset -c 4-6 "$LT/bin/tts-server" \
      --model "$M/pocket-tts-english_2026-09-Q8_0.gguf" --voices "$M/pocket-tts-english_2026-09-voices.gguf" \
      --voice "${AJ_VOICE:-alba}" --host 127.0.0.1 --port "${AJ_TTS_PORT:-8104}" >> "$APP/logs/tts.log" 2>&1 &
    PIDS="$PIDS $!"
    env OMP_PROC_BIND=close OMP_PLACES="{4},{5},{6}" nice -n 10 chrt -b 0 taskset -c 4-6 "$LT/bin/llama-server" -m "$M/$LLM.gguf" $PROJ $DRAFT \
      -dev none -t 2 -tb 3 -fa on -c 8192 -np 1 --jinja -rea off --cache-ram 0 --host 127.0.0.1 --port "${AJ_LLM_PORT:-8103}" >> "$APP/logs/llm.log" 2>&1 &
    PIDS="$PIDS $!"
    echo "servers: $LLM on :${AJ_LLM_PORT:-8103} (ears=$AUTOJAM_EARS), Pocket TTS on :${AJ_TTS_PORT:-8104}" >> "$LOG"
  fi
fi

cd "$APP"
"$APP/autojam" $ARGS "$@" >> "$LOG" 2>&1
