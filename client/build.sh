#!/usr/bin/env bash
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

prepend_path() {
  if [ -d "$1" ]; then
    PATH="$1:${PATH}"
  fi
}

use_watcom_root() {
  export WATCOM="$1"
  export EDPATH="${EDPATH:-$WATCOM/eddat}"
  export INCLUDE="${INCLUDE:-$WATCOM/h/win:$WATCOM/h:$WATCOM/lh}"
  prepend_path "$WATCOM/binl64"
  prepend_path "$WATCOM/binl"
  export PATH
}

find_system_watcom_root() {
  local candidate
  local pattern

  for candidate in \
    /opt/openwatcom \
    /opt/openwatcom-v2 \
    /opt/open-watcom \
    /usr/local/openwatcom \
    /usr/local/openwatcom-v2 \
    /usr/local/share/openwatcom \
    /usr/share/openwatcom \
    /usr/lib/openwatcom
  do
    if [ -x "$candidate/binl64/wmake" ] || [ -x "$candidate/binl/wmake" ]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done

  for pattern in \
    /opt/openwatcom* \
    /opt/open-watcom* \
    /usr/local/openwatcom* \
    /usr/local/share/openwatcom* \
    /usr/share/openwatcom* \
    /usr/lib/openwatcom*
  do
    if [ -d "$pattern" ] && { [ -x "$pattern/binl64/wmake" ] || [ -x "$pattern/binl/wmake" ]; }; then
      printf '%s\n' "$pattern"
      return 0
    fi
  done

  return 1
}

if ! command -v wmake >/dev/null 2>&1; then
  if [ -n "${WATCOM_ENV_SCRIPT:-}" ]; then
    if [ ! -f "$WATCOM_ENV_SCRIPT" ]; then
      echo "WATCOM_ENV_SCRIPT not found: $WATCOM_ENV_SCRIPT" >&2
      exit 1
    fi
    ENV_SCRIPT_DIR="$(cd "$(dirname "$WATCOM_ENV_SCRIPT")" && pwd)"
    # shellcheck source=/dev/null
    cd "$ENV_SCRIPT_DIR"
    . "$WATCOM_ENV_SCRIPT"
    cd "$SCRIPT_DIR"
  fi
fi

if ! command -v wmake >/dev/null 2>&1 && [ -n "${WATCOM:-}" ]; then
  use_watcom_root "$WATCOM"
fi

if ! command -v wmake >/dev/null 2>&1; then
  if SYSTEM_WATCOM_ROOT="$(find_system_watcom_root)"; then
    use_watcom_root "$SYSTEM_WATCOM_ROOT"
  fi
fi

if ! command -v wmake >/dev/null 2>&1; then
  echo "wmake not found. Install Open Watcom system-wide, set WATCOM, or set WATCOM_ENV_SCRIPT." >&2
  exit 1
fi

exec wmake "$@"
