#!/usr/bin/env bash
# Vision-capable profile that fits the local 22 GB GPU. Extra arguments are
# forwarded to ninfer-serve through the upstream launcher.
set -euo pipefail

NINFER_DIR=${NINFER_DIR:-/home/anonymous/Documents/DEV/ninfer-2080ti-22g}

exec env \
  VISION=1 \
  MAX_CONTEXT=${MAX_CONTEXT:-16384} \
  KV_CAPACITY=${KV_CAPACITY:-16384} \
  DEFAULT_MAX_TOKENS=${DEFAULT_MAX_TOKENS:-4096} \
  "$NINFER_DIR/serve.sh" "$@"
