#!/bin/bash
# Installs local plugins and prepares the supported Dashboard device-upload step.
set -euo pipefail

show_dialog() {
    /usr/bin/osascript - "$1" "$2" <<'APPLESCRIPT'
on run argv
    activate
    display dialog (item 1 of argv) with title "LumiPaint Setup" buttons {"Cancel", item 2 of argv} default button 2
end run
APPLESCRIPT
}

show_notice() {
    /usr/bin/osascript - "$1" <<'APPLESCRIPT'
on run argv
    activate
    display dialog (item 1 of argv) with title "LumiPaint Setup" buttons {"OK"} default button 1
end run
APPLESCRIPT
}

open_dashboard() { /usr/bin/open -b com.roli.rolidashboard; }
reveal_program() { /usr/bin/open -R "$1"; }

dashboard_handoff() {
    local installed_program=$1
    show_dialog "Plugins installed. Optional step: set up your keyboard.

If your LUMI already has the LumiPaint program, choose Cancel to finish. You can also test plugin loading without setting up a keyboard.

For a new setup, connect LUMI by USB. ROLI Dashboard must show firmware 1.3.0 or later. Drag the selected lumi_paint.littlefoot file onto your keyboard's picture in Dashboard. A dim rainbow is the expected result.

This replaces the keyboard's current program. The repository documents Dashboard's factory reset as the way to restore ROLI's program.

The file has only been copied to your Mac so far; setup cannot verify an upload." "Open Dashboard" >/dev/null || return 0

    if ! open_dashboard; then
        show_notice "Your LumiPaint plugins are installed.

ROLI Dashboard could not be opened. If it is not installed on this Mac, install ROLI Connect, then use it to install ROLI Dashboard.

You can finish now and test LumiPaint in your DAW. If your keyboard already has the LumiPaint program, you do not need to upload it again.

For a new keyboard setup, open Dashboard after installing it and drag in lumi_paint.littlefoot. Finder will show the saved file next." >/dev/null
    fi
    if ! reveal_program "$installed_program"; then
        show_notice "Your LumiPaint plugins are installed, but Finder could not show the keyboard program.

The file is saved here:
$installed_program

Open that location when you are ready to load the program in ROLI Dashboard." >/dev/null
    fi
}

# Allow the handoff regression test to exercise the UI decisions with substitutes
# for the dialogs and app-opening commands, without installing or opening apps.
if [[ ${BASH_SOURCE[0]} != "$0" ]]; then return 0; fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
library_dir="$HOME/Library"
check_only=false
install_only=false
for_arg_usage='Usage: install-macos.command [--check] [--install-only] [--library PATH]'
while (($#)); do
    case "$1" in
        --check) check_only=true; shift ;;
        --install-only) install_only=true; shift ;;
        --library)
            [[ $# -ge 2 && "$2" == /* ]] || { echo "$for_arg_usage" >&2; exit 1; }
            library_dir=$2; shift 2 ;;
        *) echo "$for_arg_usage" >&2; exit 1 ;;
    esac
done

fail() { echo "LumiPaint setup: $*" >&2; exit 1; }
[[ $(uname -s) == Darwin ]] || fail "This installer requires macOS."

if [[ -d "$script_dir/LumiPaint.vst3" ]]; then
    payload_dir=$script_dir
    program_file="$script_dir/lumi_paint.littlefoot"
else
    payload_dir="$script_dir/build-macos"
    program_file="$script_dir/device/lumi_paint.littlefoot"
fi

for format in vst3 clap; do
    bundle="$payload_dir/LumiPaint.$format"
    [[ -f "$bundle/Contents/MacOS/LumiPaint" ]] || fail "Missing $bundle. Unzip the whole Mac download, or build the Mac plugins first."
    /usr/bin/plutil -lint "$bundle/Contents/Info.plist" >/dev/null
    /usr/bin/codesign --verify --deep --strict "$bundle"
    architectures=$(/usr/bin/lipo -archs "$bundle/Contents/MacOS/LumiPaint")
    [[ " $architectures " == *" $(uname -m) "* ]] || fail "This build ($architectures) does not match this Mac ($(uname -m))."
done
[[ -s "$program_file" ]] || fail "The Littlefoot program is missing: $program_file"
if $check_only; then
    echo "PASS: Mac plugin bundles, signatures, architecture and Littlefoot file."
    echo "No files installed and no device program uploaded."
    exit 0
fi

if ! $install_only; then
    show_dialog "Step 1 of 2: Install the Mac plugins.

Save your work and quit Ableton Live and other DAWs first.

Setup will install VST3 and CLAP for your user account, keep a copy of the Littlefoot program, and back up any previous installation. No administrator password is needed.

After installation, you can open ROLI Dashboard to set up the keyboard or finish without that step." "Install" >/dev/null || exit 0
fi

support_dir="$library_dir/Application Support/LumiPaint"
destinations=("$library_dir/Audio/Plug-Ins/VST3/LumiPaint.vst3"
              "$library_dir/Audio/Plug-Ins/CLAP/LumiPaint.clap"
              "$support_dir/lumi_paint.littlefoot")
names=(LumiPaint.vst3 LumiPaint.clap lumi_paint.littlefoot)
for destination in "${destinations[@]}"; do
    [[ ! -L "$destination" ]] || fail "Refusing to replace a symbolic link: $destination"
done
mkdir -p "$support_dir/Backups"
stage_dir=$(mktemp -d "$support_dir/.setup.XXXXXX")
backup_dir=$(mktemp -d "$support_dir/Backups/install.XXXXXX")
changed=0
had_previous=(false false false)
rollback() {
    result=$?
    trap - EXIT
    if ((result != 0)); then
        echo "Installation failed; restoring the previous files." >&2
        for ((i=changed-1; i>=0; i--)); do
            rm -rf -- "${destinations[i]}"
            if ${had_previous[i]}; then
                mv -- "$backup_dir/${names[i]}" "${destinations[i]}"
            fi
        done
    fi
    rm -rf -- "$stage_dir"
    exit "$result"
}
trap rollback EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Stage every payload before changing installed files. Replacing whole bundles
# avoids leaving old nested CLAP binaries or stale signing resources behind.
/usr/bin/ditto "$payload_dir/LumiPaint.vst3" "$stage_dir/LumiPaint.vst3"
/usr/bin/ditto "$payload_dir/LumiPaint.clap" "$stage_dir/LumiPaint.clap"
cp "$program_file" "$stage_dir/lumi_paint.littlefoot"
for ((i=0; i<${#destinations[@]}; i++)); do
    destination=${destinations[i]}
    mkdir -p "$(dirname -- "$destination")"
    if [[ -e "$destination" ]]; then
        mv -- "$destination" "$backup_dir/${names[i]}"
        had_previous[i]=true
    fi
    changed=$((i+1))
    mv -- "$stage_dir/${names[i]}" "$destination"
done
/usr/bin/codesign --verify --deep --strict "${destinations[0]}"
/usr/bin/codesign --verify --deep --strict "${destinations[1]}"
trap - EXIT INT TERM
rm -rf -- "$stage_dir"
echo "Installed VST3 and CLAP. Previous files: $backup_dir"
echo "Littlefoot file: ${destinations[2]}"
echo "Keyboard upload is still pending."

if $install_only; then exit 0; fi

dashboard_handoff "${destinations[2]}"
echo "After the Dashboard upload, reopen Live, rescan VST3 plugins and select the LUMI USB port in LumiPaint."
