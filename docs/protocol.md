# LumiPaint MIDI protocol

All messages use MIDI **channel 16** unless otherwise noted. The device program is
`device/lumi_paint.littlefoot`; the protocol overview is in `art/protocol.svg`.

## Plugin to keyboard

| Message | Meaning |
|---|---|
| Poly aftertouch, channels 14/15/16 | Red / green / blue component for the note named by the message |
| Note on/off | Light / unlight a key |
| CC 106/107 | Brightness / unlit level |
| CC 108 values 0/1/2 | Clear colours / clear lit keys / restore default wheel |
| CC 109 | Display offset in semitones, centred at 64 |
| CC 110 | Octave folding |
| CC 113–116 | Incoming highlight colour and enable |
| CC 117–119, CC 85 | Pressed colour and enable |
| CC 20–22, CC 26 | Pressure gradient colour and enable |
| CC 23–25, CC 27, CC 32 | Bend gradient colour, enable and full-scale |
| CC 28/29 | Write device configuration item: ID then value |
| CC 43/44 | Send pitch bend / send pressure |
| CC 47 | Link octaves across the chain |
| CC 51 | Device-side bend trail, unused by the plugin |
| CC 87 | Octave, centred at 64 |

Colour updates use poly aftertouch because each message identifies its note. The older
four-message select/R/G/B transaction could be interrupted on a chained relay and apply a
colour to the wrong key. Components are 7-bit values; the device expands each value with
`(v << 1) | (v >> 6)` so full white survives the round trip.

## Keyboard to plugin

Reports return as polyphonic aftertouch on channel 16. Here, the note number is a report
slot rather than a pitch.

| Slot | Meaning |
|---|---|
| 0–63 | Configuration item changed: slot is the item ID, value is the new setting; signed values are offset by 64 |
| 100 | Number of blocks in the chain |
| 101 | Lowest note shown by the leftmost block |
| 110 + n | Lowest note shown by block *n* |

Reports use aftertouch rather than CC because CC messages do not identify their sender and
could be confused with a real controller. Physical keys can also send poly aftertouch on
channel 16 under MPE, so the plugin checks both whether the note is held and whether the
slot is recognized before treating a message as a report.

Some reports are repeated to recover from messages lost in the relay. One watched setting
is re-sent each pass; chain reports repeat every eight seconds, allowing missed information
to return within roughly fifteen seconds. Each block reports its own slot, and a block
without a host route relays the same values to its neighbours.
