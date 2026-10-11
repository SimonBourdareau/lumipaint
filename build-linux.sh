#!/bin/bash
#
#  The Linux build, start to finish.
#
#  Usage:
#      ./build-linux.sh            build the CLAP
#      ./build-linux.sh --vst3     build the CLAP and the VST3 wrapper
#      ./build-linux.sh --install  build, then copy into ~/.clap and ~/.vst3
#
#  X11 and GLX rather than Wayland, which is what CLAP's linux API hands over and what
#  the host layer is written against. Under XWayland this works; under a pure Wayland
#  session it will not.
#
set -euo pipefail
cd "$(dirname "$0")"

vst3=OFF
install=0

for arg in "$@"; do
    case "$arg" in
        --vst3)    vst3=ON ;;
        --install) install=1 ;;
        *) echo "unknown option: $arg"; exit 1 ;;
    esac
done

missing=""

for h in /usr/include/X11/Xlib.h /usr/include/GL/gl.h /usr/include/GL/glx.h \
         /usr/include/alsa/asoundlib.h; do
    [ -f "$h" ] || missing="$missing $h"
done

if [ -n "$missing" ]; then
    echo "missing development headers:$missing"
    echo
    echo "  Debian/Ubuntu: sudo apt install build-essential cmake git \\"
    echo "                   libx11-dev libgl1-mesa-dev libasound2-dev"
    echo "  Fedora:        sudo dnf install gcc-c++ cmake git \\"
    echo "                   libX11-devel mesa-libGL-devel alsa-lib-devel"
    echo "  Arch:          sudo pacman -S base-devel cmake git libx11 mesa alsa-lib"
    exit 1
fi

./fetch-deps.sh

cmake -B build -DCMAKE_BUILD_TYPE=Release -DLUMIPAINT_BUILD_VST3=$vst3
cmake --build build --parallel "$(nproc)"

echo
echo "built: $(ls -d build/LumiPaint.clap)"
[ "$vst3" = ON ] && [ -e build/LumiPaint.vst3 ] && echo "built: build/LumiPaint.vst3"

if [ "$install" = 1 ]; then
    mkdir -p ~/.clap
    cp -r build/LumiPaint.clap ~/.clap/
    echo "installed: ~/.clap/LumiPaint.clap"

    if [ -e build/LumiPaint.vst3 ]; then
        mkdir -p ~/.vst3
        cp -r build/LumiPaint.vst3 ~/.vst3/
        echo "installed: ~/.vst3/LumiPaint.vst3"
    fi

    echo
    echo "Rescan plugins in your DAW. The keyboard still has to be flashed with"
    echo "device/lumi_paint.littlefoot through ROLI Dashboard before anything lights."
fi
