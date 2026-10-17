#!/usr/bin/env bash
# Installs dist/autojam onto a ROCKNIX handheld over ssh, writes the Ports entry and fetches the models it needs
# into llamatalk's shared models directory (the one Retro Voice Chat / autogamer / rap-machine already use).
#   flip2/install.sh [ssh-host]      default host: flip2-12g   (TARGET=/storage/apps/autojam)
#   --jack-mic    also install the capture S16 pin the 3.5 mm headset mic needs on stock ROCKNIX (reboot after)
#   --no-models   skip the model check/download
# The Ports entry appears in EmulationStation after a Ports gamelist refresh (or a reboot).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST=flip2-12g; JACK=""; MODELS=1
for a in "$@"; do case $a in --jack-mic) JACK=1 ;; --no-models) MODELS="" ;; *) HOST=$a ;; esac; done
TARGET=${TARGET:-/storage/apps/autojam}
[ -x "$ROOT/dist/autojam/autojam" ] || { echo "no build in dist/autojam; run build/build.sh (or build/remote.sh) first" >&2; exit 1; }
echo "installing to $HOST:$TARGET"
ssh "$HOST" "mkdir -p '$TARGET' /storage/roms/ports"
rsync -a --delete --exclude data --exclude logs "$ROOT/dist/autojam/" "$HOST:$TARGET/"
ssh "$HOST" "cat > '/storage/roms/ports/Autojam.sh' <<'SH'
#!/bin/sh
# autojam: Gemma sits in with a human jam. Written by autojam/flip2/install.sh.
exec /storage/apps/autojam/launch.sh \"\$@\"
SH
chmod +x /storage/roms/ports/Autojam.sh '$TARGET/launch.sh' '$TARGET/autojam' '$TARGET'/llamatalk/models.sh '$TARGET'/llamatalk/bin/* '$TARGET'/tools/*.sh"
if [ -n "$JACK" ]; then
  ssh "$HOST" "mkdir -p /storage/.config/wireplumber/wireplumber.conf.d && cp '$TARGET/extras/51-autojam-capture-s16.conf' /storage/.config/wireplumber/wireplumber.conf.d/ && echo 'capture S16 pin installed (takes effect after a reboot)'"
fi
if [ -n "$MODELS" ]; then
  echo "models: checking llamatalk's shared directory on $HOST"
  ssh "$HOST" "M=\$('$TARGET/llamatalk/models.sh' path 2>/dev/null) || M=/storage/games-external/models; echo \"models directory: \$M\"; '$TARGET/llamatalk/models.sh' fetch --dir \"\$M\" e4b tts" || echo "model download did not finish; run '$TARGET/llamatalk/models.sh fetch e4b tts' on the device"
fi
echo "done: Ports > Autojam"
