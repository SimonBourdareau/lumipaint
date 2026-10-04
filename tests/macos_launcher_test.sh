#!/bin/bash
set -euo pipefail
[[ $# == 1 && -d "$1/Contents" ]] || { echo 'Usage: bash tests/macos_launcher_test.sh APP_PATH' >&2; exit 1; }
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/lumipaint-launcher.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT
app="$test_dir/Relocated user's folder/Install LumiPaint.app"
/usr/bin/ditto --norsrc --noextattr --noacl "$1" "$app"
/usr/bin/codesign --verify --deep --strict "$app"
launcher="$app/Contents/MacOS/Install LumiPaint"
library="$test_dir/User's Library"
# Opening the app must not source shell hooks that can consume input or alter
# execution. The launcher invokes Bash directly with these hooks removed.
poison="$test_dir/startup-hook"
echo 'exit 97' > "$poison"
BASH_ENV="$poison" ENV="$poison" "$launcher" --check --library "$library"
[[ ! -e "$library" ]]
BASH_ENV="$poison" ENV="$poison" "$launcher" --install-only --library "$library"
for format in vst3 clap; do
    case "$format" in vst3) directory=VST3 ;; clap) directory=CLAP ;; esac
    /usr/bin/codesign --verify --deep --strict "$library/Audio/Plug-Ins/$directory/LumiPaint.$format"
done
cmp "$app/Contents/Resources/lumi_paint.littlefoot" "$library/Application Support/LumiPaint/lumi_paint.littlefoot"
mv "$app/Contents/Resources/install-macos.command" "$app/Contents/Resources/hidden.command"
if "$launcher" --check; then
    echo 'FAIL: incomplete app accepted' >&2; exit 1
fi
echo 'PASS: relocated launcher, quoted paths, shell-hook isolation, installation and missing-script diagnostics.'
