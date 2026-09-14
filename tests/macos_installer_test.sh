#!/bin/bash
set -euo pipefail
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/lumipaint-installer.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT
fixture="$test_dir/Release With Spaces"
library="$test_dir/User Library"
mkdir -p "$fixture"
cp "$repo_dir/install-macos.command" "$fixture/"
cp "$repo_dir/device/lumi_paint.littlefoot" "$fixture/"
for format in vst3 clap; do
    /usr/bin/ditto "$repo_dir/build-macos/LumiPaint.$format" "$fixture/LumiPaint.$format"
done

"$fixture/install-macos.command" --check --library "$library"
[[ ! -e "$library" ]]
"$fixture/install-macos.command" --install-only --library "$library"
cmp "$fixture/lumi_paint.littlefoot" "$library/Application Support/LumiPaint/lumi_paint.littlefoot"
diff -qr "$fixture/LumiPaint.vst3" "$library/Audio/Plug-Ins/VST3/LumiPaint.vst3"
diff -qr "$fixture/LumiPaint.clap" "$library/Audio/Plug-Ins/CLAP/LumiPaint.clap"

# An upgrade must replace, not merge, the bundle and preserve the old version.
touch "$library/Audio/Plug-Ins/VST3/LumiPaint.vst3/old-file"
"$fixture/install-macos.command" --install-only --library "$library"
[[ ! -e "$library/Audio/Plug-Ins/VST3/LumiPaint.vst3/old-file" ]]
backups=("$library/Application Support/LumiPaint/Backups"/install.*/LumiPaint.vst3/old-file)
[[ -f "${backups[0]}" ]]

# Missing hardware program must fail before any destination is created.
mv "$fixture/lumi_paint.littlefoot" "$fixture/hidden.littlefoot"
if "$fixture/install-macos.command" --install-only --library "$test_dir/Missing Payload"; then
    echo "FAIL: missing payload accepted" >&2; exit 1
fi
[[ ! -e "$test_dir/Missing Payload" ]]
mv "$fixture/hidden.littlefoot" "$fixture/lumi_paint.littlefoot"

# Fail while installing the second bundle; restore the first bundle exactly.
rollback_lib="$test_dir/Rollback Library"
mkdir -p "$rollback_lib/Audio/Plug-Ins/VST3/LumiPaint.vst3"
touch "$rollback_lib/Audio/Plug-Ins/VST3/LumiPaint.vst3/original"
touch "$rollback_lib/Audio/Plug-Ins/CLAP"
if "$fixture/install-macos.command" --install-only --library "$rollback_lib"; then
    echo "FAIL: invalid destination accepted" >&2; exit 1
fi
[[ -f "$rollback_lib/Audio/Plug-Ins/VST3/LumiPaint.vst3/original" ]]
[[ ! -e "$rollback_lib/Audio/Plug-Ins/VST3/LumiPaint.vst3/Contents" ]]
[[ ! -e "$rollback_lib/Application Support/LumiPaint/lumi_paint.littlefoot" ]]
echo "PASS: preflight, paths with spaces, installation, upgrade backup and rollback."
