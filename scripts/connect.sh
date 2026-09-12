#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
# Temporary server expires 2026-09-14 14:04 Asia/Shanghai. Override for later servers.
exec ssh -o IdentitiesOnly=yes -o ExitOnForwardFailure=yes \
 -o ServerAliveInterval=30 -o ServerAliveCountMax=3 \
 -i "${SSH_KEY:-$root/../id_ed25519_48h}" \
 -R 7897:127.0.0.1:7897 -L 8080:127.0.0.1:8080 -L 3478:127.0.0.1:3478 \
 -p "${SSH_PORT:-49365}" "${SSH_USER:-root}@${SSH_HOST:-region-41.seetacloud.com}"
