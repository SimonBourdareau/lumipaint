#!/bin/bash
#
#  The three dependencies, pinned, cloned into the project.
#
#  CMakeLists looks for them at clap-src/, rtmidi/ and imgui/ inside the project
#  directory - not beside it - so this is the only layout the build accepts. Running it
#  twice is harmless; anything already there is left alone.
#
#  ImGui is pinned hard rather than loosely. The macOS build patches its Cocoa backend
#  in cmake/imgui-osx-context.patch, and the patch refuses to apply to anything else -
#  by design, because a patch that applies with fuzz to an unfamiliar version is worse
#  than one that stops.
#
set -euo pipefail
cd "$(dirname "$0")"

CLAP_TAG=1.2.9
RTMIDI_TAG=6.0.0
IMGUI_TAG=v1.91.5

clone() {
    if [ -d "$2" ]; then
        echo "have $2"
        return
    fi

    echo "cloning $2 at $3"
    git clone --quiet --depth 1 --branch "$3" "$1" "$2"
}

clone https://github.com/free-audio/clap.git   clap-src "$CLAP_TAG"
clone https://github.com/thestk/rtmidi.git     rtmidi   "$RTMIDI_TAG"
clone https://github.com/ocornut/imgui.git     imgui    "$IMGUI_TAG"

echo
echo "dependencies ready:"
echo "  clap-src $CLAP_TAG"
echo "  rtmidi   $RTMIDI_TAG"
echo "  imgui    $IMGUI_TAG"
