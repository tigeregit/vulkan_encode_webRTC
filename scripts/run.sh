#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
if [[ -n ${VIEWER_ENV:-} ]]; then source "$VIEWER_ENV"; fi
exec "$root/build/viewer" --models "${MODEL_LIBRARY:-$root/models}" --web "$root/web" --shaders "$root/build/shaders" "$@"
