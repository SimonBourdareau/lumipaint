<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="art/lumipaint_wordmark.svg">
    <img src="art/lumipaint_wordmark_light.svg" alt="LumiPaint" width="440">
  </picture>
</p>

<p align="center">
  Give every MIDI note its own colour on a ROLI LUMI keyboard — directly from your DAW.
</p>

LumiPaint replaces the keyboard's two-colour factory lighting program with a 128-note
colour map controlled by a CLAP or VST3 plugin. Paint notes, generate scales and gradients,
animate the keyboard, or copy colours from another plugin's on-screen keyboard.

## At a glance

- **Device program:** `device/lumi_paint.littlefoot` runs on the keyboard and controls its LEDs.
- **DAW plugin:** `src/` builds the editor and colour engine.
- **Tested hardware:** ROLI LUMI Keys and Piano M, including two chained blocks.
- **Firmware:** ROLI firmware **1.3.0 or later** is required.
- **Platforms:** Windows tested; macOS builds and selected host paths tested; Linux remains untested.

> [!WARNING]
> **Loading the device program replaces ROLI's factory program.** To restore it, use
> the factory reset in ROLI Dashboard. Install the plugin first if you want to try it
> without changing the keyboard; lighting features work only after the device program
> has been loaded.

## Install a release

Download the latest package from the [Releases page](../../releases/latest).

### Windows

1. Unzip the entire package into a folder.
2. Run `install-windows.bat`.
3. In ROLI Dashboard, drag `lumi_paint.littlefoot` onto your keyboard. It appears there as
   *LumiPaint - per-note colour for LUMI Keys*; on its own it shows nothing, because the
   colours come from the plugin.
4. Rescan plugins in your DAW and load LumiPaint on a MIDI track.

To remove the plugins, run `uninstall-windows.bat`.

### macOS

1. Unzip the package and open **Install LumiPaint.app**.
2. Follow the setup prompts.
3. When ROLI Dashboard opens, drag the revealed `lumi_paint.littlefoot` file onto your keyboard.
4. Rescan plugins in your DAW and load LumiPaint on a MIDI track.

The installer puts the device program at
`~/Library/Application Support/LumiPaint/lumi_paint.littlefoot`. It does **not**
upload the program automatically. If macOS blocks the app, see
[Apple's Open Anyway instructions](https://support.apple.com/102445).

### Finish setup in your DAW

1. Put LumiPaint before the instrument on a MIDI track.
2. Choose your keyboard from the editor's port menu. Select it explicitly if you have
   more than one ROLI device.
3. Raise **Unlit level** if the keys are too dark to read.

A dim rainbow on the keyboard after flashing is the expected default display.

## What you can do

- **Paint and select notes:** colour individual notes, all octaves of a pitch class, or
  any multi-selection.
- **Generate maps:** use Wheel, Fifths, Piano, Blackout, scales and multi-stop gradients.
- **Animate:** add ripples, afterglow, beat pulse, chord halo, sustain, pitch-bend effects
  and screensaver patterns.
- **Read another plugin's keyboard:** capture and mirror colour-coded keybeds such as
  Kontakt, Falcon, DecentSampler, SINE Player and Chromaphone.
- **Share a keyboard across tracks:** assign non-overlapping note zones to multiple
  LumiPaint instances.
- **Use chained blocks:** link octave controls or set each block independently.

See the [User guide](docs/user-guide.md) for the editor controls, MIDI routing, shared
zones, and chained-block behaviour.

## Build from source

### Windows (tested path)

Install [MSYS2](https://www.msys2.org), open **MSYS2 MinGW 64-bit**, then install the
toolchain:

```sh
pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-ninja mingw-w64-x86_64-make git
```

From the project directory:

```sh
git clone --depth 1 https://github.com/free-audio/clap.git clap-src
git clone --depth 1 -b v1.91.5 https://github.com/ocornut/imgui.git imgui
git clone --depth 1 -b 6.0.0 https://github.com/thestk/rtmidi.git rtmidi
sh build_gui.sh
```

This builds CLAP and then the VST3 wrapper. To skip VST3 while iterating on CLAP:

```sh
LUMIPAINT_NO_VST3=1 sh build_gui.sh
```

Install the results into your per-user plugin folders:

```sh
cp LumiPaint.clap "$LOCALAPPDATA/Programs/Common/CLAP/"
cp -r build-vst3/Release/LumiPaint.vst3 "$LOCALAPPDATA/Programs/Common/VST3/"
```

Keep the project path free of spaces for the wrapper build.

### macOS

Build the CLAP plugin:

```sh
cmake -B build
cmake --build build
mkdir -p ~/Library/Audio/Plug-Ins/CLAP
cp -r build/LumiPaint.clap ~/Library/Audio/Plug-Ins/CLAP/
```

For native Apple-silicon VST3 support in Ableton Live, see the full commands and
verification notes in the [Developer guide](docs/development.md).

To package the macOS installer after building:

```sh
./packaging/macos/package-macos.sh
```

### Linux

Use the helper script to fetch dependencies, build and install:

```sh
./build-linux.sh --install
```

Add `--vst3` to build the VST3 wrapper, or omit `--install` to leave the result in
`build/`. Linux currently uses X11/GLX; it works under XWayland but not in a pure
Wayland session.

More build options and platform-specific notes are in the
[Developer guide](docs/development.md).

## Important limitations

- Keyboard capture needs roughly **8 pixels per white key**. Smaller captures may be
  misdetected.
- OpenGL- or Direct3D-rendered plugin windows cannot be sampled and may capture as black.
- Firmware older than 1.3.0 cannot load the device program.
- Hardware lighting, complete MIDI routing and screen capture remain partially unverified on macOS;
  Linux is not yet tested.
- On Windows, direct MIDI-port sharing depends on Windows MIDI Services. On older setups,
  LumiPaint listens through the host, which requires the track to be armed.

See [Known limitations and hardware testing](docs/development.md#known-limitations-and-hardware-testing)
for the details.

## Licence and credits

LumiPaint is free software under the **GNU GPL v3 or later**. See the licence text in
the repository for the full terms.

The device protocol builds on the reverse engineering in [benob/LUMI-lights](https://github.com/benob/LUMI-lights) and ROLI's Littlefoot API.
