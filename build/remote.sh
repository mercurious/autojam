#!/usr/bin/env bash
# Builds on a remote arm64 host with docker (etk-cloud) instead of this machine, and brings dist/ back.
#   build/remote.sh [host]          default: etk-cloud.  REMOTE_DIR (default ~/autojam-build)
#   LLAMATALK_DIST on the remote: default ~/llamatalk-builds/llamatalk/dist/llamatalk (the kit built there)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST=${1:-etk-cloud}; RDIR=${REMOTE_DIR:-'~/autojam-build'}
echo "syncing to $HOST:$RDIR"
ssh "$HOST" "mkdir -p $RDIR"
rsync -az --delete --exclude .git --exclude build/work --exclude dist "$ROOT/" "$HOST:$RDIR/"
ssh "$HOST" "cd $RDIR && ENGINE=docker JOBS=\${JOBS:-4} LLAMATALK_DIST=\${LLAMATALK_DIST:-\$HOME/llamatalk-builds/llamatalk/dist/llamatalk} build/build.sh"
mkdir -p "$ROOT/dist"
rsync -az "$HOST:$RDIR/dist/" "$ROOT/dist/"
echo "dist/ is back from $HOST"; ls -la "$ROOT/dist"
