#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${1:-"$repo_dir/build-macos"}
mkdir -p "$build_dir/tests"

c++ -std=c++17 -I "$repo_dir/src" -I "$repo_dir/clap-src/include" \
    "$repo_dir/tests/silent_audio_test.cpp" -o "$build_dir/tests/silent-audio-test"
"$build_dir/tests/silent-audio-test"

c++ -std=c++17 -I "$build_dir/cpm/vst3sdk" -framework CoreFoundation \
    "$repo_dir/tests/macos_vst3_smoke.cpp" -o "$build_dir/tests/vst3-smoke"
"$build_dir/tests/vst3-smoke" "$build_dir/LumiPaint.vst3"
codesign --verify --deep --strict "$build_dir/LumiPaint.vst3"

# Exercise the same Cocoa app-deactivation notification as the Live crash, with
# multiple editors, no current context and destroyed editors. No MIDI activation.
c++ -std=c++17 -DNDEBUG -Wno-nullability-completeness \
    -I "$build_dir/imgui-backend" -I "$repo_dir/imgui" -I "$repo_dir/imgui/backends" \
    "$repo_dir/tests/macos_focus_test.mm" \
    "$repo_dir/imgui/imgui.cpp" "$repo_dir/imgui/imgui_draw.cpp" \
    "$repo_dir/imgui/imgui_tables.cpp" "$repo_dir/imgui/imgui_widgets.cpp" \
    -framework Cocoa -framework GameController -o "$build_dir/tests/focus-test"
"$build_dir/tests/focus-test"
"$build_dir/tests/focus-test" --null-context
