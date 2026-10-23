# LumiPaint user guide

This guide covers the editor and common workflows. Installation and build instructions
are in the [main README](../README.md).

## The editor

### Top bar

| Control | Purpose |
|---|---|
| Port | Select the keyboard's MIDI output. `Auto` chooses the first port whose name resembles a ROLI device; choose explicitly when using multiple devices. |
| Hold | Keep the keyboard assigned to this instance. See [Several instances](#several-instances-of-lumipaint-in-one-daw-project). |
| Share | Each instance owns a non-overlapping note range. See [Sharing instances](#(https://github.com/SimonBourdareau/lumipaint/blob/main/docs/user-guide.md#sharing-a-lumi--piano-m-keyboard-across-tracks-in-one-daw-project)). |
| Key width | Changes the width of the on-screen keys only. |
| Range | Changes the displayed note range. The green bracket marks the range shown by the hardware. |

### Piano M preview 

The keyboard preview shows the composited output, including animations. Keys outside the
hardware's range are dimmed.

### Paint and select notes

The colour picker applies to the selected notes. Use **Single note** to select one key,
or **All octaves** to select every octave of its pitch class.

| Control | Purpose |
|---|---|
| Note field / Set this note | Set a note by MIDI note number. |
| Apply to N pressed notes | Paint the notes currently held down and select them. |
| Apply to sel. | Paint the current selection. |
| Pick from sel. | Load the first selected note's colour into the picker. |
| Fill unsel. | Paint every note outside the selection. |
| Invert sel. / Select all / Select none | Change the selection. |
| Save / Load | Save or load the map, effect colours, and related settings. |
| Copy / Paste | Copy the map and settings between LumiPaint instances on the same machine. |

Keyboard selection controls:

- **Click:** select a key; with **All octaves**, select its pitch class across octaves.
- **Shift-click:** extend the selection from the last clicked key.
- **Ctrl-click** (Cmd-click on macOS): add or remove a key from the selection.
- **Ctrl-A** (Cmd-A on macOS): select all 128 notes.
- **Alt-drag:** paint continuously with the current picker colour.

### Gradients and map generators

A **gradient** has two to eight colour stops. Select notes and press **Paint n** to distribute
the gradient evenly across the selection order, not across the MIDI note-number span. This
also works for scattered selections, such as every C. **Reverse** flips the order.

**Map generators** overwrite the entire 128-note table:

| Mode | Result |
|---|---|
| Wheel | One hue per pitch class. |
| Fifths | Colours notes by position in the circle of fifths. |
| Piano | White keys white; black keys off. |
| Blackout | Turns all notes off as a starting point for manual painting. |
| Scale | Display a given scale with the root. |

The scale controls colour the tonic, in-scale notes and out-of-scale notes. **Select
in-scale** selects those notes without painting them. The scale also informs Degrees and
Tension effects.

### Lighting and sounding notes

| Control / effect | Purpose |
|---|---|
| Brightness | Overall LED brightness. |
| Unlit level | Brightness when no note or effect is active. Raise it to read a static map. |
| Send rate | Adjusts how quickly updates are sent; automatic mode accounts for chained blocks. |
| Incoming | Colours notes arriving from the DAW with a chosen highlight colour. |
| Pressed | Colours physically held keys; overrides Incoming. |
| Pressure | Blends a key toward a colour as pressure increases. |
| Bend | Blends toward a colour as pitch bends. |
| Bend full-scale | Sets the bend amount treated as maximum. |
| Degrees | Colours notes by interval from the last played root, relative to the selected scale. |
| Tension | Colours notes by distance around the circle of fifths from the root. |
| Ripple | Sends a wave along the keyboard when a note is struck. |
| Afterglow | Leaves a fading colour after a note is struck. |
| Beat pulse | Flashes the root on every beat, following the host transport. The first beat of the bar has its own colour - the second swatch - so the downbeat reads while you are playing rather than only while you are watching for it. |
| Chord halo | Lights matching pitch classes in other octaves while multiple notes are held. |
| Splash | Uses a selected MIDI CC to trigger a wave. |
| Sustain | Shows notes held by the sustain pedal. Off by default. |
| Bend path | Lights the notes between a bent note and its target pitch. |
| Bend gradient | Colours each sounding note by its own bend, including per-note MPE bend. |
| Velocity brightness | Makes held-note brightness follow velocity. Requires Incoming and Pressed to be off. |

Ripple colour sources are **Fixed**, **Wheel**, **Fifths**, **Degree**, **Map** and
**Gradient**. Gradient uses the gradient stops rather than the painted map, so it can
produce coloured waves over Blackout.

### Undo/Redo

The arrows in the top-right corner provide up to 30 undo/redo steps for colour-map changes:
painting, generators, gradients, captures, map loading and pasting. Effect settings and
output levels are not included.

### Screensaver

The screensaver starts after the configured idle delay and stops as soon as activity is
detected. Held notes, sustain-held notes and notes observed through the listen port count as
activity.

On a member of a shared chain the whole Screensaver section is greyed out and reads
*not active as a member instance*.

In a shared chain, idleness belongs to the keyboard rather than to one track: a note held in
any zone keeps the whole chain awake.

| Pattern | Purpose |
|---|---|
| Waves | Slow, overlapping brightness swells. |
| Aurora | A moving band of green, teal, blue and violet hues. |
| Breathing | Pulses the brightness of the existing map without replacing it. |
| Ember | Like Breathing, but each note has a different phase. |
| Gradient drift | Scrolls the configured gradient across the keys. Takes its colours from the gradient. |
| Rainfall | Coloured drops spread and fade across the keyboard. Takes its colours from the gradient. |
| Diffusion | Paint dropped onto a wet canvas. Takes its colours from the gradient. |

Waves, Aurora, Gradient drift and Rainfall temporarily replace the colour table until a note
is played. Breathing and Ember preserve the map and animate its brightness.

### Reading another plugin's keyboard

The capture panel can read a colour-coded keyboard from plugins such as Kontakt, Falcon,
DecentSampler, SINE Player and Chromaphone.

1. Choose **Find windows**.
2. Select a window and press **Read keyboard**.
3. Use **Find anchor** to identify the leftmost C. LumiPaint plays MIDI note 60 and watches
   which key changes in plugin window; the target plugin must display played notes.
4. Set **Lowest C is** if necessary.
5. Press **Import colours**.
6. Enable **Live** to re-read the keybed about five times per second.

Capture is useful only when the other plugin communicates information through colour. 

NOTE: On macOS, screen capture requires Screen Recording permission. Restart the DAW after
granting it. OpenGL- and Direct3D-rendered windows cannot be sampled.

### Keybed and key mapping

The Keybed panel mirrors settings from the hardware:

- **Send pitch bend** and **Send pressure** control outgoing performance messages.
- **Link octaves across the chain** moves all connected blocks together.
- **MPE**, **Pitch bend range**, **Transpose** and **MIDI channel** configure the device.

The Key mapping panel includes:

- **Octave:** shifts notes and lights together; follows the hardware buttons.
- **Display offset:** shifts the displayed lights without transposing the notes.
- **Fold octaves:** lights every key of a pitch class when that note is active.

Note numbering is fixed: MIDI note 0 is C-2, note 60 is C3 and note 127 is G8.

## Several instances of LumiPaint in one DAW project

By default, the keyboard follows activity. The instance of LumiPaint receiving notes or being opened
claim the device. The previous instance releases its port. All instances share the
currently displayed map, so switching tracks updates only notes whose colours differ.

Use **Hold** to pin the device to one instance. Hold and Share cannot be enabled together.

## Sharing a LUMI / Piano M keyboard across tracks in one DAW project

Enable **Share** in each instance to assign it a note range. Automatically, one instance of LumiPaint becomes the "master".
Other instances become "members". Each instance owns a non-overlapping note range.
Notes are filtered within each track by zone allowing to play multiple tracks (that need to be all armed) together.

- Drag along the zone bar with **Alt** to set the range. Conflicts are shown in red and
  rejected. Each instance reports the ranges declared by other instances.
- The semitone offset separates the physical keys under your hands from the MIDI notes
  sent to the track. For example, keys at C4–B4 can trigger notes C2–B2.
- The octave control is locked while sharing, because changing it would shift the mapping
  for every zone.
- Zone ranges persist through silence and editor closure, and are released when the
  instance exits or after two minutes with no running instances.
- Each instance resolves its own `Incoming`, `Pressed`, `Pressure` and `Bend` colours, so
  every zone keeps its own; they are part of the colours it publishes - the highlight on a held key, the bend tint and the bend
  path are all computed where the notes arrived.
- The lowest live zone owner sends the assembled keyboard display.
- Ripples can cross zone boundaries; most other effects remain within their owning zone.
- Beat pulse is chain-wide.
- Enable input monitoring on every track that should receive MIDI. A plugin cannot arm a
  DAW track itself.

## Multiple LUMI / Piano Ms can be linked or having independent octaves

Physically connected LUMI / Piano Ms form a cluster and report their positions to Lumipaint.
At the same octave setting, two 24-key blocks sit end to end.

- **Linked:** pressing an octave button on either block moves all blocks together.
- **Independent:** each block can move separately, including into different registers or
  overlapping note ranges.

The editor draws a bracket for each block's current note range. LumiPaint sends colours
for all 128 MIDI notes; the device lights the notes its block can display.
