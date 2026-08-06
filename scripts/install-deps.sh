#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Install the build dependencies for audiaki: a C compiler, make, pkg-config
# and the ALSA development headers.
#
# Also installs, by default:
#
#   the PipeWire development headers, which build the backend that talks to the
#   sound server rather than to the card. Without them audiaki still builds and
#   records, through ALSA alone; pass --no-pipewire to skip them.
#
#   ffmpeg, which `audiaki --visualize` runs to encode a video. It is not
#   needed to build audiaki or to record with it; pass --no-ffmpeg to skip it.
#
#   the OpenGL and X11 development headers, which the `audiaki-gui` desktop
#   app needs to build its vendored copy of raylib. Not needed for the command
#   line recorder, and useless on a headless machine; pass --no-gui to skip
#   them.
#
# Usage: ./scripts/install-deps.sh [--dry-run] [--no-ffmpeg] [--no-gui]
#                                  [--no-pipewire]

set -eu

DRY_RUN=0
WANT_FFMPEG=1
WANT_GUI=1
WANT_PIPEWIRE=1
for arg in "$@"; do
  case "$arg" in
    --dry-run) DRY_RUN=1 ;;
    --no-ffmpeg) WANT_FFMPEG=0 ;;
    --no-gui) WANT_GUI=0 ;;
    --no-pipewire) WANT_PIPEWIRE=0 ;;
    *)
      echo "usage: $0 [--dry-run] [--no-ffmpeg] [--no-gui] [--no-pipewire]" >&2
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

# PipeWire's development package brings the SPA headers with it as a dependency
# everywhere audiaki supports, so only the one name is needed per distribution.
# Arch has no split -dev packages, so the headers come with the library itself.
pipewire_packages() {
  if [ "$WANT_PIPEWIRE" -eq 0 ]; then
    return
  fi
  case "$1" in
    apt) echo "libpipewire-0.3-dev" ;;
    dnf) echo "pipewire-devel" ;;
    pacman) echo "libpipewire" ;;
    zypper) echo "pipewire-devel" ;;
    apk) echo "pipewire-dev" ;;
    *) ;;
  esac
}

# The GUI package names are not: every distribution splits and capitalises the
# X11 libraries differently, so each branch names its own set. raylib is built
# against the X11 backend, which works under Wayland through XWayland.
gui_packages() {
  if [ "$WANT_GUI" -eq 0 ]; then
    return
  fi
  case "$1" in
    apt)
      echo "libgl1-mesa-dev libx11-dev libxrandr-dev libxi-dev" \
           "libxcursor-dev libxinerama-dev libxkbcommon-dev"
      ;;
    dnf)
      echo "mesa-libGL-devel libX11-devel libXrandr-devel libXi-devel" \
           "libXcursor-devel libXinerama-devel libxkbcommon-devel"
      ;;
    pacman)
      # Arch ships headers in the main packages rather than -dev split ones
      echo "mesa libx11 libxrandr libxi libxcursor libxinerama libxkbcommon"
      ;;
    zypper)
      echo "Mesa-libGL-devel libX11-devel libXrandr-devel libXi-devel" \
           "libXcursor-devel libXinerama-devel libxkbcommon-devel"
      ;;
    apk)
      echo "mesa-dev libx11-dev libxrandr-dev libxi-dev libxcursor-dev" \
           "libxinerama-dev libxkbcommon-dev"
      ;;
    *) ;;
  esac
}

if command -v apt-get >/dev/null 2>&1; then
  GUI=$(gui_packages apt)
  PW=$(pipewire_packages apt)
  run apt-get update
  # shellcheck disable=SC2086
  run apt-get install -y build-essential pkg-config libasound2-dev $PW $FFMPEG $GUI
elif command -v dnf >/dev/null 2>&1; then
  GUI=$(gui_packages dnf)
  PW=$(pipewire_packages dnf)
  # shellcheck disable=SC2086
  run dnf install -y gcc make pkgconf-pkg-config alsa-lib-devel $PW $FFMPEG $GUI
elif command -v pacman >/dev/null 2>&1; then
  GUI=$(gui_packages pacman)
  PW=$(pipewire_packages pacman)
  # shellcheck disable=SC2086
  run pacman -S --needed --noconfirm base-devel pkgconf alsa-lib $PW $FFMPEG $GUI
elif command -v zypper >/dev/null 2>&1; then
  GUI=$(gui_packages zypper)
  PW=$(pipewire_packages zypper)
  # shellcheck disable=SC2086
  run zypper install -y gcc make pkg-config alsa-devel $PW $FFMPEG $GUI
elif command -v apk >/dev/null 2>&1; then
  GUI=$(gui_packages apk)
  PW=$(pipewire_packages apk)
  # shellcheck disable=SC2086
  run apk add build-base pkgconf alsa-lib-dev $PW $FFMPEG $GUI
else
  cat >&2 <<'EOF'
error: no supported package manager found.

Install these yourself and re-run `make`:
  - a C11 compiler (gcc or clang)
  - make
  - pkg-config
  - the ALSA development headers (libasound2-dev / alsa-lib-devel / alsa-lib)
  - the PipeWire development headers, if you want the PipeWire backend
    (libpipewire-0.3-dev / pipewire-devel / libpipewire / pipewire-dev)
  - ffmpeg, if you want `audiaki --visualize` to render videos
  - the OpenGL and X11 development headers, if you want the audiaki-gui
    desktop app: GL, X11, Xrandr, Xi, Xcursor, Xinerama and xkbcommon
EOF
  exit 1
fi

echo
if [ "$WANT_GUI" -eq 1 ]; then
  # The headers alone are not enough: raylib itself lives in a submodule.
  echo "Dependencies installed. For the desktop app, fetch raylib too:"
  echo "    git submodule update --init --depth 1"
  echo
  echo "Then build both binaries with: make"
else
  echo "Dependencies installed. Build with: make"
fi
