#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Install the build dependencies for audiaki: a C compiler, make, pkg-config
# and the ALSA development headers.
#
# Also installs ffmpeg, which `audiaki --visualize` runs to encode a video.
# It is not needed to build audiaki or to record with it; pass --no-ffmpeg to
# leave it out.
#
# Usage: ./scripts/install-deps.sh [--dry-run] [--no-ffmpeg]

set -eu

DRY_RUN=0
WANT_FFMPEG=1
for arg in "$@"; do
  case "$arg" in
    --dry-run) DRY_RUN=1 ;;
    --no-ffmpeg) WANT_FFMPEG=0 ;;
    *)
      echo "usage: $0 [--dry-run] [--no-ffmpeg]" >&2
      exit 2
      ;;
  esac
done

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

# ffmpeg is the same package name everywhere audiaki supports.
FFMPEG=""
if [ "$WANT_FFMPEG" -eq 1 ]; then
  FFMPEG="ffmpeg"
fi

if command -v apt-get >/dev/null 2>&1; then
  run apt-get update
  # shellcheck disable=SC2086
  run apt-get install -y build-essential pkg-config libasound2-dev $FFMPEG
elif command -v dnf >/dev/null 2>&1; then
  # shellcheck disable=SC2086
  run dnf install -y gcc make pkgconf-pkg-config alsa-lib-devel $FFMPEG
elif command -v pacman >/dev/null 2>&1; then
  # shellcheck disable=SC2086
  run pacman -S --needed --noconfirm base-devel pkgconf alsa-lib $FFMPEG
elif command -v zypper >/dev/null 2>&1; then
  # shellcheck disable=SC2086
  run zypper install -y gcc make pkg-config alsa-devel $FFMPEG
elif command -v apk >/dev/null 2>&1; then
  # shellcheck disable=SC2086
  run apk add build-base pkgconf alsa-lib-dev $FFMPEG
else
  cat >&2 <<'EOF'
error: no supported package manager found.

Install these yourself and re-run `make`:
  - a C11 compiler (gcc or clang)
  - make
  - pkg-config
  - the ALSA development headers (libasound2-dev / alsa-lib-devel / alsa-lib)
  - ffmpeg, if you want `audiaki --visualize` to render videos
EOF
  exit 1
fi

echo
echo "Dependencies installed. Build with: make"
