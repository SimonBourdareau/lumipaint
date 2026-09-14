#!/bin/bash
set -euo pipefail
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
"$repo_dir/install-macos.command" --check
release_root="$repo_dir/release"
mkdir -p "$release_root"
package_dir=$(mktemp -d "$release_root/LumiPaint-macOS.XXXXXX")
/usr/bin/ditto "$repo_dir/build-macos/LumiPaint.vst3" "$package_dir/LumiPaint.vst3"
/usr/bin/ditto "$repo_dir/build-macos/LumiPaint.clap" "$package_dir/LumiPaint.clap"
cp "$repo_dir/device/lumi_paint.littlefoot" "$package_dir/"
cp "$repo_dir/install-macos.command" "$repo_dir/LICENSE" "$package_dir/"
cat > "$package_dir/READ ME.txt" <<'README'
LumiPaint for macOS

1. Save your work and quit Ableton Live or any other DAW.
2. Double-click install-macos.command and choose Install.
3. Choose Open Dashboard. Connect LUMI by USB and drag the selected
   lumi_paint.littlefoot file onto your keyboard's picture in Dashboard.
   Firmware 1.3.0 or later is required. Expect a dim rainbow.
4. Reopen Live, enable VST3 system folders in Settings > Plug-Ins and rescan.
5. Load LumiPaint on a MIDI track and select your LUMI USB port.

The installer copies VST3 and CLAP into your user's Library/Audio/Plug-Ins
folders. It keeps the Littlefoot file and backups in
Library/Application Support/LumiPaint.

Loading Littlefoot in Dashboard is the device-program replacement step.
It requires the manual drag; this installer cannot upload or verify it.
The repository documents Dashboard's factory reset to restore ROLI's program.

The Mac bundles pass initialization, stereo bus negotiation, MIDI port,
silent-output, app-focus callback and signing checks. This build includes
the fix for a crash when Live goes into the background. Editor loading and
desktop/background switching were tested in Live 12.4.5 on Apple silicon,
macOS 26.5.2. Hardware lighting, full MIDI routing and screen capture remain
unverified. This is a locally signed development build, not notarized.

For sound in Live, put an instrument on a second MIDI track. Set MIDI From
to the LumiPaint track, choose LumiPaint in the lower selector, and set
Monitor to In. LumiPaint's audio output itself is silent.
README
"$package_dir/install-macos.command" --check
/usr/bin/ditto -c -k --sequesterRsrc --keepParent "$package_dir" "$package_dir.zip"
echo "Ready: $package_dir"
echo "Archive: $package_dir.zip"
