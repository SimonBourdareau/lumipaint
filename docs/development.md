# LumiPaint developer guide

See the [main README](../README.md) for the quick install and build paths.

## Platform build details

### Windows

The tested path uses MSYS2. Install the MinGW 64-bit toolchain, CMake, Ninja, Make and Git
as listed in the README. Fetch CLAP, Dear ImGui 1.91.5 and RtMidi 6.0.0, then run
`sh build_gui.sh`. The script builds CLAP and the VST3 wrapper from the same implementation.
The first VST3 build downloads the VST3 SDK and requires a network connection.

The VST3 is a directory bundle. Copy the entire `LumiPaint.vst3` directory, not just its
outer directory shell. Keep the project path free of spaces for this wrapper build.

### macOS

A CLAP build:

```sh
cmake -B build
cmake --build build
cp -r build/LumiPaint.clap ~/Library/Audio/Plug-Ins/CLAP/
```

For native Apple-silicon VST3 in Ableton Live, fetch dependencies if absent:

```sh
git clone --depth 1 https://github.com/free-audio/clap.git clap-src
git clone --depth 1 -b v1.91.5 https://github.com/ocornut/imgui.git imgui
git clone --depth 1 -b 6.0.0 https://github.com/thestk/rtmidi.git rtmidi
cmake -S . -B build-macos -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DLUMIPAINT_BUILD_VST3=ON -DLUMIPAINT_BUILD_AU=OFF
cmake --build build-macos --parallel 6
mkdir -p ~/Library/Audio/Plug-Ins/VST3
cp -R build-macos/LumiPaint.vst3 ~/Library/Audio/Plug-Ins/VST3/
```

The first configuration downloads clap-wrapper and the VST3 SDK. The VST3 embeds its CLAP
bundle, so install only the VST3 in Live. Use `x86_64` for Intel or `arm64;x86_64` for a
universal build; those architectures have not been verified.

To package the installer app, run `./packaging/macos/package-macos.sh`. The result is placed
under `release/`. Builds are locally signed development builds and are not notarized.

For Ableton Live, enable VST3 system folders and rescan. To route notes through LumiPaint,
put it on a MIDI track and the instrument on another; select the LumiPaint track under
**MIDI From**, choose LumiPaint in the lower chooser, and set **Monitor: In**. Full MIDI
routing remains unverified.

Run the macOS tests with:

```sh
bash tests/macos_installer_test.sh
bash tests/macos_launcher_test.sh "/path/to/Install LumiPaint.app"
sh tests/run-macos-tests.sh
```

The tests cover installer isolation, relocated app paths, plugin initialization, bus
negotiation, MIDI ports, output buffers and signing. They do not verify lighting hardware
or activate the MIDI worker. The macOS regression suite also tests editor focus changes
and notifications after editor destruction. A build-local ImGui Cocoa backend patch is
applied by CMake to address a reproduced null-context crash during host backgrounding.

### Linux

The helper script fetches pinned dependencies, checks required headers, builds and can
install the plugin:

```sh
./build-linux.sh --install
```

Use `--vst3` to build the wrapper; omit `--install` to leave the build under `build/`.
Manual alternative:

```sh
./fetch-deps.sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cp -r build/LumiPaint.clap ~/.clap/
```

Linux needs X11/GLX, `libx11-dev`, `libgl1-mesa-dev` and `libasound2-dev`. XWayland works;
a pure Wayland session does not support the required editor embedding and screen capture.
File dialogs use `zenity` or `kdialog` when installed.

A minimal editor smoke test can be run under Xvfb:

```sh
cc -Iclap-src/include tests/linux_gui_smoke.c -o smoke -ldl -lX11
xvfb-run -s "-screen 0 1400x960x24" ./smoke build/LumiPaint.clap
```

## Release process

Tag a version to trigger the GitHub Actions release workflow:

```sh
git tag v1.0.1
git push origin v1.0.1
```

CI builds all three platforms, runs the Linux editor smoke test and macOS bundle checks,
then attaches artifacts to a **draft** release for review. Nothing is signed or notarized.
Run the workflow manually from Actions to test a build without creating a release.

## Architecture and porting

The implementation is shared across platforms except for three concerns: window creation,
screen capture and file dialogs.

- `src/imgui_host.h` defines the window/OpenGL contract.
- `src/platform/win32/`, `src/platform/macos/` and `src/platform/x11/` implement it.
- `LUMIPAINT_HOST_SOURCES` selects the platform implementation.
- `src/platform/macos/macos_bridge.h` is the interface between C++ and Objective-C++.
- `cmake/` contains build configuration, the bundle plist and the ImGui patch.
- `packaging/macos/` is separate from plugin code; the installer entry point is compiled
  directly by `package-macos.sh`.

The host layer provides an after-frame callback for modal UI. Opening a native file dialog
inside the render callback can run a nested event loop while the plugin's GL context remains
current, freezing the DAW or leaving its window black. Deferring until after the frame is
therefore required. Repaint uses a host-layer timer rather than the CLAP timer extension,
so hosts without that extension still redraw.

## Wire protocol

All device messages use MIDI **channel 16**. This makes the keyboard usable from any MIDI
sender, not just the LumiPaint plugin. See `art/protocol.svg` for the overview.

### Plugin to keyboard

| Message | Meaning |
|---|---|
| Poly aftertouch on channels 14/15/16 | Red / green / blue components for the note named by the message |
| Note on/off | Light / unlight a key |
| CC 106/107 | Brightness / unlit level |
| CC 108 values 0/1/2 | Clear colours / clear lit keys / restore default wheel |
| CC 109 | Display offset, centred at 64 |
| CC 110 | Octave folding |
| CC 113–116 | Incoming highlight colour and enable |
| CC 117–119, CC 85 | Pressed colour and enable |
| CC 20–22, CC 26 | Pressure gradient colour and enable |
| CC 23–25, CC 27, CC 32 | Bend gradient colour, enable and full-scale |
| CC 28/29 | Write device configuration item: ID then value |
| CC 43/44 | Send pitch bend / send pressure |
| CC 47 | Link octaves across the chain |
| CC 51 | Device-side bend trail; unused by the plugin |
| CC 87 | Octave, centred at 64 |

Colours use poly aftertouch because each message names its note. The older multi-message
select/R/G/B transaction could leave a partial update on a chained relay. Colour components
are 7-bit values; the device expands them with `(v << 1) | (v >> 6)` so full white survives.

### Keyboard to plugin

Reports return as polyphonic aftertouch on channel 16. The note number is a report slot,
not a pitch:

| Slot | Meaning |
|---|---|
| 0–63 | Configuration item changed; slot is the item ID and value is its new setting. Signed values are offset by 64. |
| 100 | Number of blocks in the chain |
| 101 | Lowest note shown by the leftmost block |
| 110 + n | Lowest note shown by block *n* |

Reports use aftertouch rather than CC so they can be distinguished from real controller
messages. The plugin also checks whether the note is held and whether the slot is known,
because physical key pressure can use channel 16 under MPE. Some reports are repeated to
recover from messages lost in the chain relay.

## Larger ROLI pianos

Only LUMI Keys and Piano M have been tested. To inspect a larger device, flash
`device/probe_ruler.littlefoot`; it lights each key with its LED index. Then flash
`device/lumi_paint.littlefoot` again.

If the device reports as a chain of 24-key blocks, it should work without changes. The
topology code supports up to five blocks. If it is one larger block, update
`device/lumi_paint.littlefoot` to:

1. Read the key count with `getNumKeys()` instead of fixed `numKeys = 24`.
2. Size `pressed`, `keyPressure`, `keyBend`, `keyChannel` and `keyNote` arrays to that count.
3. Derive `startNote` rather than assuming 48.

Do not oversize the arrays: Littlefoot has a limited heap, and oversized arrays can make
Dashboard's settings panel silently disappear. The plugin itself should not need changes.

## Known limitations and hardware testing

- Capture requires around 8 pixels per white key. Below that, black-key detection may be
  wrong rather than simply failing.
- OpenGL- or Direct3D-rendered windows may capture as black.
- A fixed brightness threshold can miss edge keys in very dark plugin interfaces.
- Firmware 1.3.0 or later is required to accept a Littlefoot program.
- Windows has been tested with a host and keyboard. macOS Apple-silicon CLAP/VST3 builds,
  bundle metadata, VST3 initialization and bus negotiation have been checked; editor
  loading and desktop/background switching were tested in Ableton Live 12.4.5 on macOS
  26.5.2. Hardware lighting, full MIDI routing and screen capture remain unverified.
- AU, Intel and universal macOS builds are unverified. Linux remains untested.
- On Windows 11 with Windows MIDI Services, the plugin and host can share MIDI 1.0 ports.
  Older configurations fall back to host listening, which requires the track to be armed.
