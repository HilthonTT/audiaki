#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Install the build dependencies for audiaki: a C compiler, make, pkg-config
# and the ALSA development headers.
#
# Usage: ./scripts/install-deps.sh [--dry-run]

set -eu

DRY_RUN=0
if [ "${1:-}" = "--dry-run" ]; then
  DRY_RUN=1
fi

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
  if command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
  else
    echo "error: run as root or install sudo" >&2
    exit 1
  fi
fi

run() {
  echo "+ $SUDO $*"
  if [ "$DRY_RUN" -eq 0 ]; then
    # shellcheck disable=SC2086
    $SUDO "$@"
  fi
}

if command -v apt-get >/dev/null 2>&1; then
  run apt-get update
  run apt-get install -y build-essential pkg-config libasound2-dev
elif command -v dnf >/dev/null 2>&1; then
  run dnf install -y gcc make pkgconf-pkg-config alsa-lib-devel
elif command -v pacman >/dev/null 2>&1; then
  run pacman -S --needed --noconfirm base-devel pkgconf alsa-lib
elif command -v zypper >/dev/null 2>&1; then
  run zypper install -y gcc make pkg-config alsa-devel
elif command -v apk >/dev/null 2>&1; then
  run apk add build-base pkgconf alsa-lib-dev
else
  cat >&2 <<'EOF'
error: no supported package manager found.

Install these yourself and re-run `make`:
  - a C11 compiler (gcc or clang)
  - make
  - pkg-config
  - the ALSA development headers (libasound2-dev / alsa-lib-devel / alsa-lib)
EOF
  exit 1
fi

echo
echo "Dependencies installed. Build with: make"
