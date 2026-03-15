#!/usr/bin/env bash
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ -n "${WATCOM_ENV_SCRIPT:-}" ]; then
  # shellcheck source=/dev/null
  . "${WATCOM_ENV_SCRIPT}"
fi

if [ -n "${WATCOM:-}" ]; then
  export EDPATH="${EDPATH:-$WATCOM/eddat}"
  export INCLUDE="${INCLUDE:-$WATCOM/h/win:$WATCOM/h:$WATCOM/lh}"
  export PATH="$WATCOM/binl64:$WATCOM/binl:${PATH}"
fi

if ! command -v wmake >/dev/null 2>&1; then
  echo "wmake not found. Source Open Watcom first." >&2
  exit 1
fi

exec wmake "$@"
