#!/bin/bash
set -euo pipefail
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
"$repo_dir/install-macos.command" --check
release_root="$repo_dir/release"
mkdir -p "$release_root"
package_dir=$(mktemp -d "$release_root/LumiPaint-macOS.XXXXXX")
app="$package_dir/Install LumiPaint.app"
resources="$app/Contents/Resources"
mkdir -p "$resources" "$app/Contents/MacOS"
for format in vst3 clap; do
    /usr/bin/ditto --norsrc --noextattr --noacl \
        "$repo_dir/build-macos/LumiPaint.$format" "$resources/LumiPaint.$format"
done
cp "$repo_dir/device/lumi_paint.littlefoot" "$resources/"
cp "$repo_dir/install-macos.command" "$resources/"
cp "$repo_dir/LICENSE" "$package_dir/"
cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>com.lumipaint.installer</string>
<key>CFBundleName</key><string>Install LumiPaint</string>
<key>CFBundleExecutable</key><string>Install LumiPaint</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>1.0.0</string>
<key>CFBundleVersion</key><string>2</string>
<key>LSMinimumSystemVersion</key><string>11.0</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
architectures=$(/usr/bin/lipo -archs "$resources/LumiPaint.vst3/Contents/MacOS/LumiPaint")
launcher_parts=()
for architecture in $architectures; do
    part="$package_dir/launcher-$architecture"
    /usr/bin/clang -fobjc-arc -framework Cocoa -arch "$architecture" \
        -mmacosx-version-min=11.0 "$repo_dir/src/macos_installer_main.m" -o "$part"
    launcher_parts+=("$part")
done
/usr/bin/lipo -create "${launcher_parts[@]}" -output "$app/Contents/MacOS/Install LumiPaint"
rm -- "${launcher_parts[@]}"
/usr/bin/codesign --force --sign - --timestamp=none "$app"
/usr/bin/codesign --verify --deep --strict "$app"
cat > "$package_dir/READ ME.txt" <<'README'
LumiPaint for macOS

1. Save your work and quit Ableton Live or any other DAW.
2. Double-click Install LumiPaint.app and choose Install.
3. Choose Open Dashboard. Connect LUMI by USB and drag the selected
   lumi_paint.littlefoot file onto your keyboard's picture in Dashboard.
   Firmware 1.3.0 or later is required. Expect a dim rainbow.
4. Reopen Live, enable VST3 system folders in Settings > Plug-Ins and rescan.
5. Load LumiPaint on a MIDI track and select your LUMI USB port.

The installer copies VST3 and CLAP into your user's Library/Audio/Plug-Ins
folders. It keeps the Littlefoot file and backups in
Library/Application Support/LumiPaint.

The installer opens directly without Terminal. Keep the app intact; its plugin
payloads are bundled inside it. If macOS blocks this unnotarized download,
attempt to open it, then check System Settings > Privacy & Security for
Open Anyway. See https://support.apple.com/102445 for Apple's instructions.
The plugin itself may also need approval before rescanning it in Live.
You can cancel the Dashboard step after installation to test plugin loading.

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
"$app/Contents/MacOS/Install LumiPaint" --check
/usr/bin/ditto -c -k --norsrc --noextattr --noacl --keepParent "$package_dir" "$package_dir.zip"
echo "Ready: $package_dir"
echo "Archive: $package_dir.zip"
