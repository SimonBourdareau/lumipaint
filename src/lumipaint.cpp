/*
    LumiPaint - per-note colour control for ROLI LUMI Keys
    Copyright (C) 2026 Simon Bourdareau

    This program is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version.

    This program is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
    PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along with
    this program. If not, see <https://www.gnu.org/licenses/>.
*/

#include "lumipaint.h"
#include "keybed_capture.h"
#include "silent_audio.h"

#if defined (_WIN32)
 /* far, near and min/max are macros in the Windows headers and collide with ordinary
    identifiers; these keep them out. */
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <mmsystem.h>
 #undef far
 #undef near
#endif

#include <RtMidi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>

namespace lumipaint {
namespace {

const uint32_t kDefaultWheel[12] = {
    0xff2b2b, 0xff7a1f, 0xffc400, 0xd6ff00, 0x5cff00, 0x00ff5c,
    0x00ffd6, 0x00c4ff, 0x007aff, 0x2b2bff, 0x9c00ff, 0xff00b8
};

uint8_t clamp7 (int v)
{
    if (v < 0)
        return 0;

    if (v > 127)
        return 127;

    return (uint8_t) v;
}

int lowestSetBit (uint64_t v)
{
    int index = 0;

    while ((v & 1ull) == 0ull)
    {
        v >>= 1;
        ++index;
    }

    return index;
}

}

/* Straight linear mix, alpha 0-255. */
uint32_t mixColour (uint32_t base, uint32_t tint, int alpha)
{
    if (alpha <= 0)
        return base;

    if (alpha > 255)
        alpha = 255;

    uint32_t out = 0;

    for (int shift = 0; shift <= 16; shift += 8)
    {
        const int b = (int) ((base >> shift) & 0xff);
        const int t = (int) ((tint >> shift) & 0xff);
        out |= ((uint32_t) (b + (((t - b) * alpha) / 255))) << shift;
    }

    return out;
}

uint32_t defaultColourForNote (int note)
{
    return kDefaultWheel[((note % 12) + 12) % 12];
}

struct LumiLink::Backend
{
    std::unique_ptr<RtMidiOut> out;
    std::unique_ptr<RtMidiIn> in;
};

ColourInputDecoder::ColourInputDecoder()
    : pendingNote (-1), red (0), green (0), blue (0), mirrorItem (-1), pendingBlockPos (-1)
{
}

void ColourInputDecoder::reset()
{
    pendingNote = -1;
    red = 0;
    green = 0;
    blue = 0;
    mirrorItem = -1;
    pendingBlockPos = -1;
}

void ColourInputDecoder::feed (LumiLink &link, uint8_t status, uint8_t data1, uint8_t data2)
{
    /*
        Notes arriving on the listen port light up, whatever channel they are on.

        This is how an arpeggiator gets shown. An arp has to sit after LumiPaint in the
        chain - in front of it, every instance's notes would be arpeggiated together
        instead of each track's own - and nothing downstream ever passes back through
        the plugin. So the way to see those notes is to send them to a MIDI port and
        have LumiPaint listen to it: a virtual cable, with the arp's output at one end
        and the Listen selector at the other.

        Checked before the control-channel filter below, because an arpeggiator knows
        nothing about which channel this plugin reserves for the device.

        Zone-filtered like any other note, so a shared chain shows each arp only on the
        keys that track owns.
    */
    const uint8_t kind = status & 0xf0;

    if (kind == 0x90 && data2 > 0)
    {
        link.externalNote (data1, true);
        return;
    }

    if (kind == 0x80 || (kind == 0x90 && data2 == 0))
    {
        link.externalNote (data1, false);
        return;
    }

    if ((status & 0x0f) != kControlChannel)
        return;

    /*
        Reports: poly aftertouch, one message each, the note acting as a slot number.

        The keyboard's own keys also send poly aftertouch - that is how pressure
        travels - and assignChannel can put a key on channel 16, since MPE's lower zone
        uses member channels two to sixteen. A key playing note 60 with pressure would
        otherwise read as config item 60.

        What separates them is that a report is never about a sounding note. Pressure
        only exists for a key that is held; a report describes a config item or a
        block. So a poly aftertouch for a note this plugin knows is down is pressure,
        and anything else is a report. The slots above 99 cannot collide at all, since
        the plugin would have to be holding note 100 for the question to arise.
    */
    if ((status & 0xf0) == kReportStatus && isReportSlot (data1))
    {
        if (link.isNoteSounding (data1))
            return;

        if (data1 == kSlotWidth)
            link.setClusterWidth (data2);
        else if (data1 == kSlotBase)
            link.setWindowBase (data2);
        else if (data1 >= kSlotBlock && data1 < kSlotBlock + 5)
            link.setBlockRange (data1 - kSlotBlock, data2);
        else if (data1 < 64)
        {
            /* Signed items travel offset by 64, and that has to be undone here.

               The control-change path did this and the move to poly aftertouch lost
               it, so an octave of zero arrived as 64. Linked, the plugin adopted 64
               and broadcast it back to every block, which is why they shot to the top
               and snapped back when pushed down. Unlinked nothing is broadcast, so the
               same wrong value sat there doing no visible harm - which is exactly the
               asymmetry that pointed at this line. */
            const int value = isSignedConfig (data1) ? (int) data2 - 64 : (int) data2;
            link.applyMirroredConfig (data1, value);
        }

        return;
    }

    if ((status & 0xf0) != 0xb0)
        return;

    /* The colour and lit-state branches that used to head this list are gone with the
       four-message write: colours arrive as polyphonic aftertouch and lit state as
       ordinary notes, neither of which comes through here. What is left is the
       device's own reports. */
    if (data1 == kCcBlockPos)
    {
        pendingBlockPos = data2;
    }
    else if (data1 == kCcBlockLow)
    {
        if (pendingBlockPos >= 0)
        {
            link.setBlockRange (pendingBlockPos, data2);
            pendingBlockPos = -1;
        }
    }
    else if (data1 == kCcClusterWidth)
    {
        link.setClusterWidth (data2);
    }
    else if (data1 == kCcWindowBase)
    {
        link.setWindowBase (data2);
    }
    else if (data1 == kCcConfigItem)
    {
        mirrorItem = data2;
    }
    else if (data1 == kCcConfigValue)
    {
        if (mirrorItem >= 0)
        {
            const int value = isSignedConfig (mirrorItem) ? data2 - 64 : data2;
            link.applyMirroredConfig (mirrorItem, value);
            mirrorItem = -1;
        }
    }
    else if (data1 == kCcCommand)
    {
        if (data2 == kCmdAllKeysOff)
            link.clearExternalLit();

        if (data2 == kCmdClearColours)
            for (int n = 0; n < 128; ++n)
                link.setColour (n, 0x000000);
        else if (data2 == kCmdResetColours)
            for (int n = 0; n < 128; ++n)
                link.setColour (n, defaultColourForNote (n));

        reset();
    }
}

LumiLink::LumiLink()
    : backend (new Backend()), selectionChanged (false), refreshCountdown (0),
      running (false), connected (false), externalInput (false)
{
    for (int i = 0; i < 128; ++i)
    {
        baseColour[i].store (defaultColourForNote (i), std::memory_order_relaxed);
        desiredColour[i].store (defaultColourForNote (i), std::memory_order_relaxed);
        sentColour[i] = kUnsentColour;
    }

    litBits[0].store (0, std::memory_order_relaxed);
    litBits[1].store (0, std::memory_order_relaxed);
    externalLitBits[0].store (0, std::memory_order_relaxed);
    externalLitBits[1].store (0, std::memory_order_relaxed);
    sentLitBits[0] = 0;
    sentLitBits[1] = 0;

    brightness.store (127, std::memory_order_relaxed);
    unlitLevel.store (8, std::memory_order_relaxed);
    displayOffset.store (0, std::memory_order_relaxed);
    foldOctaves.store (0, std::memory_order_relaxed);
    highlightEnabled.store (0, std::memory_order_relaxed);
    highlightColour.store (0xffffff, std::memory_order_relaxed);
    pressEnabled.store (0, std::memory_order_relaxed);
    pressColour.store (0xff8000, std::memory_order_relaxed);
    octave.store (0, std::memory_order_relaxed);

    windowLow.store (0, std::memory_order_relaxed);
    windowHigh.store (127, std::memory_order_relaxed);
    blockCount.store (0, std::memory_order_relaxed);
    sendLow.store (0, std::memory_order_relaxed);
    sendHigh.store (127, std::memory_order_relaxed);
    messagesIn.store (0, std::memory_order_relaxed);
    lastCcIn.store (-1, std::memory_order_relaxed);
    directIn.store (0, std::memory_order_relaxed);
    lastDirect.store (-1, std::memory_order_relaxed);
    followSource.store (nullptr, std::memory_order_relaxed);
    followAnchor.store (36, std::memory_order_relaxed);
    followTick = 0;
    for (int i = 0; i < 5; ++i)
        blockLow[i].store (-1, std::memory_order_relaxed);

    for (int i = 0; i < 128; ++i)
    {
        deviceConfig[i].store (kConfigUnknown, std::memory_order_relaxed);
        pendingConfigWrite[i].store (kConfigUnknown, std::memory_order_relaxed);
    }

    pressureGradEnabled.store (0, std::memory_order_relaxed);
    pressureGradColour.store (0xffffff, std::memory_order_relaxed);
    bendGradEnabled.store (0, std::memory_order_relaxed);
    bendGradColour.store (0x00c4ff, std::memory_order_relaxed);
    bendFullScale.store (2, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kTriggerSlots; ++i)
        triggerRing[i].store (0, std::memory_order_relaxed);

    triggerWrite.store (0, std::memory_order_relaxed);
    triggerRead = 0;

    for (int i = 0; i < kMaxRipples; ++i)
    {
        rippleNote[i] = 0;
        rippleAge[i] = -1;
        rippleTint[i] = 0x00ffd6;
        rippleLevel[i] = 255;
        rippleStep[i] = 4;
        rippleTrail16[i] = 64;
    }

    activeRipples = 0;
    rippleEnabled.store (0, std::memory_order_relaxed);
    rippleColour.store (0x00ffd6, std::memory_order_relaxed);
    rippleSpeed.store (4, std::memory_order_relaxed);
    rippleTrail.store (2, std::memory_order_relaxed);
    rippleSource.store (kRippleFixed, std::memory_order_relaxed);

    for (int i = 0; i < 128; ++i)
    {
        glowTrigger[i].store (0, std::memory_order_relaxed);
        glowLevel[i] = 0;
    }

    activeGlow = 0;
    afterglowEnabled.store (0, std::memory_order_relaxed);
    afterglowColour.store (0xffd000, std::memory_order_relaxed);
    afterglowDecay.store (8, std::memory_order_relaxed);
    pendingBeatLevel.store (-1, std::memory_order_relaxed);
    pulseLevel = 0;
    pulseEnabled.store (0, std::memory_order_relaxed);
    pulseColour.store (0x4060ff, std::memory_order_relaxed);
    degreeEnabled.store (0, std::memory_order_relaxed);
    degreeAlpha.store (170, std::memory_order_relaxed);
    degreeScale.store (0xab5, std::memory_order_relaxed);
    scaleRoot.store (0, std::memory_order_relaxed);
    degreeRoot.store (0, std::memory_order_relaxed);

    {
        /* Root strong, third and fifth next - the chord tones read first, the rest of
           the scale sits behind them, and anything chromatic is nearly dark. */
        const uint32_t defaults[8] = { 0xff3b30, 0x8a6a2a, 0xffd60a, 0x2a6a5a,
                                       0x30d158, 0x3a4a8a, 0x9c6aff, 0x141414 };

        for (int i = 0; i < 8; ++i)
            degreeColour[i].store (defaults[i], std::memory_order_relaxed);
    }

    tensionEnabled.store (0, std::memory_order_relaxed);
    tensionAlpha.store (150, std::memory_order_relaxed);
    tensionHome.store (0x2ea85e, std::memory_order_relaxed);
    tensionFar.store (0xd02030, std::memory_order_relaxed);

    velocityEnabled.store (0, std::memory_order_relaxed);

    for (int i = 0; i < 128; ++i)
        noteVelocities[i].store (0, std::memory_order_relaxed);

    wavesEnabled.store (0, std::memory_order_relaxed);
    wavesDelay.store (60, std::memory_order_relaxed);
    wavesMode.store (0, std::memory_order_relaxed);
    zoned.store (0, std::memory_order_relaxed);
    zoneLow.store (0, std::memory_order_relaxed);
    zoneHigh.store (127, std::memory_order_relaxed);
    zoneHeld.store (0, std::memory_order_relaxed);
    zoneOffset.store (0, std::memory_order_relaxed);

    sustainHeld.store (0, std::memory_order_relaxed);
    sustainEnabled.store (0, std::memory_order_relaxed);
    sustainColour.store (0x3cff9a, std::memory_order_relaxed);

    /* A default worth looking at rather than a row of black: deep blue to orange is
       readable across a keybed and makes the feature explain itself the first time
       anyone presses the button. */
    gradientCount.store (4, std::memory_order_relaxed);
    gradientStops[0].store (0x1f3dff, std::memory_order_relaxed);
    gradientStops[1].store (0x00c4ff, std::memory_order_relaxed);
    gradientStops[2].store (0xffd400, std::memory_order_relaxed);
    gradientStops[3].store (0xff3d1f, std::memory_order_relaxed);

    for (int i = 4; i < kGradientStops; ++i)
        gradientStops[i].store (0xffffff, std::memory_order_relaxed);
    idleMs.store (0, std::memory_order_relaxed);
    wavePhase = 0;

    bendPathEnabled.store (0, std::memory_order_relaxed);
    bendPathColour.store (0x00c4ff, std::memory_order_relaxed);

    for (int i = 0; i < 16; ++i)
    {
        channelNote[i].store (-1, std::memory_order_relaxed);
        channelBend[i].store (0, std::memory_order_relaxed);
    }

    for (int i = 0; i < 128; ++i)
    {
        noteChannel[i].store (-1, std::memory_order_relaxed);
        noteBendCents[i].store (0, std::memory_order_relaxed);
    }

    lastBendCents.store (0, std::memory_order_relaxed);
    lastBendNote.store (-1, std::memory_order_relaxed);


    haloEnabled.store (0, std::memory_order_relaxed);
    haloColour.store (0x30406a, std::memory_order_relaxed);
    splashEnabled.store (0, std::memory_order_relaxed);
    splashColour.store (0xff7a1f, std::memory_order_relaxed);
    splashCC.store (1, std::memory_order_relaxed);
    splashSpeed.store (6, std::memory_order_relaxed);
    splashTrail.store (3, std::memory_order_relaxed);
    pendingSplashLevel.store (-1, std::memory_order_relaxed);
    splashCooldown = 0;

    enablePitchBend.store (1, std::memory_order_relaxed);
    enablePressure.store (1, std::memory_order_relaxed);
    linkOctaves.store (1, std::memory_order_relaxed);
    sendRate.store (0, std::memory_order_relaxed);
    activityPending.store (0, std::memory_order_relaxed);
    holdDevice.store (0, std::memory_order_relaxed);
    sentBrightness = -1;
    sentUnlitLevel = -1;
    sentDisplayOffset = -1000;
    sentFoldOctaves = -1;
    sentHighlightEnabled = -1;
    sentHighlightColour = 0xffffffffu;
    sentPressEnabled = -1;
    sentPressColour = 0xffffffffu;

    /*
        Octave is deliberately not invalidated.

        Everything else here is re-sent after a reconnect or a handover because the
        plugin is the authority for it. The octave is the one setting the device owns:
        its buttons set it and it reports the result. Marking it unsent made the plugin
        broadcast its own stored value to every block on the next tick - which forced
        both blocks to the same octave whatever the link toggle said, and is why
        unlinking never freed them.

        Keeping it level with what the device last reported means nothing goes out
        until the user actually moves the control.
    */
    sentOctave = octave.load (std::memory_order_relaxed);

    sentPressureGradEnabled = -1;
    sentPressureGradColour = 0xffffffffu;
    sentBendGradEnabled = -1;
    sentBendGradColour = 0xffffffffu;
    sentBendFullScale = -1;
    sentEnablePitchBend = -1;
    sentEnablePressure = -1;
    sentLinkOctaves = -1;
    deviceSelectedNote = -1;
    keyMessagesThisTick = 0;
    refreshCursor = 0;
    dirtyCursor = 0;

    for (int i = 0; i < 128; ++i)
        skipped[i] = 0;
    colourRefreshCountdown = 0;
    lastTick = std::chrono::steady_clock::now();
    lastTickMs = 4;
    globalRefreshCountdown = 0;
}

LumiLink::~LumiLink()
{
    stop();
}

void LumiLink::start()
{
    if (running.load (std::memory_order_acquire))
        return;

    try
    {
        backend->in.reset (new RtMidiIn (RtMidi::UNSPECIFIED, "LumiPaint"));
        backend->in->ignoreTypes (true, true, true);
        backend->in->setCallback (&LumiLink::incomingCallback, this);
    }
    catch (RtMidiError &)
    {
        backend->in.reset();
        externalInput.store (false, std::memory_order_relaxed);
    }

    running.store (true, std::memory_order_release);
    worker = std::thread (&LumiLink::run, this);
}

void LumiLink::stop()
{
    if (! running.load (std::memory_order_acquire))
        return;

    running.store (false, std::memory_order_release);

    try
    {
        if (worker.joinable())
            worker.join();

#if defined (_WIN32)
        /* Matches the timeBeginPeriod the worker raised. Left unbalanced it holds the
           system timer resolution high for the life of the process. */
        timeEndPeriod (1);
#endif
    }
    catch (const std::system_error &)
    {
    }

    if (backend->in)
    {
        backend->in->cancelCallback();
        backend->in->closePort();
        backend->in.reset();
    }

    externalInput.store (false, std::memory_order_relaxed);
}

void LumiLink::incomingCallback (double timeStamp, std::vector<unsigned char> *message, void *userData)
{
    (void) timeStamp;

    if (message == nullptr || userData == nullptr)
        return;

    ((LumiLink *) userData)->handleIncoming (message->data(), message->size());
}

void LumiLink::handleIncoming (const unsigned char *bytes, size_t length)
{
    if (length < 3)
        return;

    /* Counted before the decoder's channel filter, so a port that is receiving the
       wrong thing looks different from one that is receiving nothing. */
    directIn.fetch_add (1, std::memory_order_relaxed);
    lastDirect.store ((int) bytes[0] << 8 | (int) bytes[1], std::memory_order_relaxed);

    portDecoder.feed (*this, bytes[0], bytes[1], bytes[2]);
}

bool LumiLink::hasExternalInput() const
{
    return externalInput.load (std::memory_order_relaxed);
}

/* setColour writes the painted table; what actually goes to the device is that table
   with any running animation composited over it. Keeping the two apart is what lets a
   ripple pass across the keyboard and leave the user's colours untouched behind it. */
void LumiLink::setColour (int note, uint32_t rgb)
{
    if (note < 0 || note > 127)
        return;

    baseColour[note].store (rgb & 0x00ffffffu, std::memory_order_relaxed);
}

uint32_t LumiLink::getColour (int note) const
{
    if (note < 0 || note > 127)
        return 0;

    return baseColour[note].load (std::memory_order_relaxed);
}

/*
    The breath, 60..255, or 255 when breathing is not running.

    One function because two things need the same number: the controller message that
    makes the hardware do it, and the editor's keyboard, which would otherwise sit
    perfectly still while the device swelled. A preview that disagrees with the device
    is worse than no preview.
*/
void LumiLink::setSustain (bool down)
{
    sustainHeld.store (down ? 1 : 0, std::memory_order_relaxed);
}

bool LumiLink::getSustainDown() const
{
    return sustainHeld.load (std::memory_order_relaxed) != 0;
}

void LumiLink::setSustainEnabled (bool on)
{
    sustainEnabled.store (on ? 1 : 0, std::memory_order_relaxed);
}

bool LumiLink::getSustainEnabled() const
{
    return sustainEnabled.load (std::memory_order_relaxed) != 0;
}

void LumiLink::setSustainColour (uint32_t rgb)
{
    sustainColour.store (rgb & 0x00ffffffu, std::memory_order_relaxed);
}

uint32_t LumiLink::getSustainColour() const
{
    return sustainColour.load (std::memory_order_relaxed);
}

int LumiLink::breathLevel() const
{
    if (! wavesRunning() || wavesMode.load (std::memory_order_relaxed) != 2)
        return 255;

    const int phase = ((wavePhase / 26) % 360 + 360) % 360;
    const int tri = phase < 180 ? phase : 360 - phase;
    return 60 + (tri * 195) / 180;
}

uint32_t LumiLink::getDisplayColour (int note) const
{
    if (note < 0 || note > 127)
        return 0;

    const uint32_t rgb = desiredColour[note].load (std::memory_order_relaxed);
    const int breath = breathLevel();

    if (breath >= 255 || isNoteSounding (note))
        return rgb;

    /* A key being played stays at full, exactly as the firmware leaves it. */
    const uint32_t r = (((rgb >> 16) & 0xffu) * (uint32_t) breath) / 255u;
    const uint32_t g = (((rgb >> 8) & 0xffu) * (uint32_t) breath) / 255u;
    const uint32_t b = ((rgb & 0xffu) * (uint32_t) breath) / 255u;

    return (r << 16) | (g << 8) | b;
}

void LumiLink::setRippleEnabled (bool on)
{
    rippleEnabled.store (on ? 1 : 0, std::memory_order_relaxed);
}

bool LumiLink::getRippleEnabled() const
{
    return rippleEnabled.load (std::memory_order_relaxed) != 0;
}

void LumiLink::setRippleColour (uint32_t rgb)
{
    rippleColour.store (rgb & 0x00ffffffu, std::memory_order_relaxed);
}

uint32_t LumiLink::getRippleColour() const
{
    return rippleColour.load (std::memory_order_relaxed);
}

void LumiLink::setRippleSpeed (int speed)
{
    if (speed < 1) speed = 1;
    if (speed > 16) speed = 16;
    rippleSpeed.store (speed, std::memory_order_relaxed);
}

int LumiLink::getRippleSpeed() const
{
    return rippleSpeed.load (std::memory_order_relaxed);
}

/* How many keys the wave keeps burning behind its front before it is back to the
   key's own colour. */
/* Hue to RGB at full saturation and value, which is all these need. */
uint32_t hueColour (int step, int outOf)
{
    const float h = (float) (((step % outOf) + outOf) % outOf) / (float) outOf * 6.0f;
    const int sector = (int) h;
    const float f = h - (float) sector;
    const int q = (int) ((1.0f - f) * 255.0f);
    const int t = (int) (f * 255.0f);

    switch (sector)
    {
        case 0:  return (255u << 16) | ((uint32_t) t << 8);
        case 1:  return ((uint32_t) q << 16) | (255u << 8);
        case 2:  return (255u << 8) | (uint32_t) t;
        case 3:  return ((uint32_t) q << 8) | 255u;
        case 4:  return ((uint32_t) t << 16) | 255u;
        default: return (255u << 16) | (uint32_t) q;
    }
}

void LumiLink::setRippleSource (int source)
{
    /* The ceiling is the last source, not the last one that existed when this was
       written - adding Gradient to the list without moving it here meant picking it
       snapped straight back to Map. */
    if (source < 0) source = 0;
    if (source > kRippleGradient) source = kRippleGradient;
    rippleSource.store (source, std::memory_order_relaxed);
}

int LumiLink::getRippleSource() const
{
    return rippleSource.load (std::memory_order_relaxed);
}

void LumiLink::setRippleTrail (int keys)
{
    if (keys < 1) keys = 1;
    if (keys > 12) keys = 12;
    rippleTrail.store (keys, std::memory_order_relaxed);
}

int LumiLink::getRippleTrail() const
{
    return rippleTrail.load (std::memory_order_relaxed);
}

void LumiLink::setSplashEnabled (bool on)
{
    splashEnabled.store (on ? 1 : 0, std::memory_order_relaxed);
}

bool LumiLink::getSplashEnabled() const
{
    return splashEnabled.load (std::memory_order_relaxed) != 0;
}

void LumiLink::setSplashColour (uint32_t rgb)
{
    splashColour.store (rgb & 0x00ffffffu, std::memory_order_relaxed);
}

uint32_t LumiLink::getSplashColour() const
{
    return splashColour.load (std::memory_order_relaxed);
}

void LumiLink::setSplashCC (int cc)
{
    if (cc < 0) cc = 0;
    if (cc > 119) cc = 119;
    splashCC.store (cc, std::memory_order_relaxed);
}

int LumiLink::getSplashCC() const
{
    return splashCC.load (std::memory_order_relaxed);
}

void LumiLink::setSplashSpeed (int speed)
{
    if (speed < 1) speed = 1;
    if (speed > 16) speed = 16;
    splashSpeed.store (speed, std::memory_order_relaxed);
}

int LumiLink::getSplashSpeed() const
{
    return splashSpeed.load (std::memory_order_relaxed);
}

void LumiLink::setSplashTrail (int keys)
{
    if (keys < 1) keys = 1;
    if (keys > 12) keys = 12;
    splashTrail.store (keys, std::memory_order_relaxed);
}

int LumiLink::getSplashTrail() const
{
    return splashTrail.load (std::memory_order_relaxed);
}

/* Called from the audio thread. Posts into a ring the worker drains, so simultaneous
   notes each get their own ripple instead of competing for the same slot. */
/*
    The colour a wave takes, worked out once from the note that threw it.

    Lifted out of the ripple start so a zoned instance can resolve its own tint when it
    posts a wave to the chain. The sender then draws that colour rather than recomputing
    it from its own settings - which would give every member's waves the sender's
    palette and quietly undo the point of per-track maps.
*/
uint32_t LumiLink::resolveRippleTint (int note) const
{
    const int pc = ((note % 12) + 12) % 12;

    switch (rippleSource.load (std::memory_order_relaxed))
            {
                case kRippleWheel:
                    return hueColour (pc, 12);

                case kRippleFifths:
                    return hueColour ((pc * 7) % 12, 12);

                case kRippleDegree:
                {
                    const int root = degreeRoot.load (std::memory_order_relaxed);
                    const int interval = (((pc - root) % 12) + 12) % 12;
                    const uint32_t mask = degreeScale.load (std::memory_order_relaxed);
                    int slotIndex = 7;

                    if (((mask >> interval) & 1u) != 0u)
                    {
                        int seen = 0;

                        for (int i = 0; i < interval; ++i)
                            if (((mask >> i) & 1u) != 0u)
                                ++seen;

                        slotIndex = seen < 7 ? seen : 6;
                    }

                    return degreeColour[slotIndex].load (std::memory_order_relaxed);
                }

                case kRippleMap:
                    return baseColour[note & 127].load (std::memory_order_relaxed);

                case kRippleGradient:
                {
                    /*
                        Positioned across the keys the device is actually showing, not
                        across all 128.

                        A Piano M covers two octaves, so spreading the gradient over the
                        full MIDI range would give it a narrow slice of one colour and
                        the ripples would all look the same. Measured against the window
                        instead, the whole gradient is reachable from the keys under
                        your hands.
                    */
                    const int low = windowLow.load (std::memory_order_relaxed);
                    const int high = windowHigh.load (std::memory_order_relaxed);
                    const int span = high > low ? high - low : 127;
                    int offset = note - low;

                    if (offset < 0)
                        offset = 0;

                    if (offset > span)
                        offset = span;

                    return gradientAt ((float) offset / (float) span);
                }

                default:
                    break;
            }

    return rippleColour.load (std::memory_order_relaxed);
}

/*
    A wave posted by another member of the chain, started here.

    Everything about it travelled with it, so none of this instance's ripple settings
    are consulted - the colour, the speed and the trail are the originator's. Finding a
    free slot is the only local decision, and when there is none the oldest wave is the
    one that loses, which is the same rule the local ring uses.
*/
/*
    The ripple pass, over whatever is already on the keyboard.

    The same arithmetic the composite uses, applied to desiredColour in place rather
    than to a base colour, so it can run after every zone has been assembled. Written
    once here and called only by the sender; an un-zoned instance never needs it,
    because nothing has overwritten its own composite.
*/
void LumiLink::overlayRipples()
{
    if (activeRipples <= 0)
        return;

    for (int note = 0; note < 128; ++note)
    {
        uint32_t mixed = desiredColour[note].load (std::memory_order_relaxed);
        int applied = 0;

        for (int i = 0; i < kMaxRipples; ++i)
        {
            if (rippleAge[i] < 0)
                continue;

            int distance = note - rippleNote[i];

            if (distance < 0)
                distance = -distance;

            const int front = rippleAge[i] * rippleStep[i] / 16;
            const int behind = front - distance;

            if (behind < 0 || behind * 16 > rippleTrail16[i])
                continue;

            const int trail = rippleTrail16[i] > 0 ? rippleTrail16[i] : 16;
            int alpha = rippleLevel[i] * (trail - behind * 16) / trail;

            if (alpha <= 0)
                continue;

            if (alpha > 255)
                alpha = 255;

            /* Each successive wave mixes into the result so far, so a crossing carries
               both colours instead of one. */
            mixed = mixColour (mixed, rippleTint[i], alpha);
            ++applied;
        }

        if (applied > 0)
            desiredColour[note].store (mixed, std::memory_order_relaxed);
    }
}

void LumiLink::startChainRipple (int note, int level, uint32_t tint, int speed, int trail)
{
    if (note < 0 || note > 127)
        return;

    int slot = -1;
    int oldest = -1;

    for (int i = 0; i < kMaxRipples; ++i)
    {
        if (rippleAge[i] < 0)
        {
            slot = i;
            break;
        }

        if (oldest < 0 || rippleAge[i] > rippleAge[oldest])
            oldest = i;
    }

    if (slot < 0)
        slot = oldest;

    if (slot < 0)
        return;

    rippleNote[slot] = note;
    rippleTint[slot] = tint;
    rippleStep[slot] = speed < 1 ? 1 : speed;
    rippleTrail16[slot] = 16 * (trail < 1 ? 1 : trail);
    rippleLevel[slot] = level;
    rippleAge[slot] = 0;
}

void LumiLink::triggerRipple (int note, int level)
{
    if (rippleEnabled.load (std::memory_order_relaxed) == 0)
        return;

    if (note < 0 || note > 127)
        return;

    /*
        In a chain, a wave is posted and not drawn here.

        Ripples are the one effect that has to leave the range it started in, so there
        is exactly one place that can draw them: whoever is sending. A member that drew
        its own as well would show the near half twice - once in its published zone and
        once from the event - and the two would drift apart as they aged.

        So a member posts and stops. Un-zoned, nothing changes: the local ring is the
        only path and the wave is drawn where it always was.
    */
    const bool chainDraws = zoned.load (std::memory_order_relaxed) != 0;

    if (! chainDraws)
    {
        const uint32_t packed = 0x80000000u | ((uint32_t) note << 8) | (uint32_t) (level & 0xff);
        const uint32_t slot = triggerWrite.fetch_add (1, std::memory_order_relaxed);
        triggerRing[slot % kTriggerSlots].store (packed, std::memory_order_release);
    }

    /*
        A zoned instance also posts the wave to the chain, with its own settings in it.

        This is what lets a ripple cross a boundary. The instance that received the note
        resolves its speed, trail and tint here, where they are known, and whoever is
        sending draws it across all 128 exactly as this instance would have - without
        needing to know anything about this instance at all.

        Posted even when this instance is the sender, because the sender drains the ring
        and would otherwise draw its own waves twice: once locally and once from the
        chain. Its own local ripple is suppressed below instead, so there is one path
        rather than two.
    */
    if (chainDraws)
        claim.postEffect (packEffect (note, level, resolveRippleTint (note),
                                      rippleSpeed.load (std::memory_order_relaxed),
                                      rippleTrail.load (std::memory_order_relaxed),
                                      kEffectRipple));
}

/*
    An effect as sixty-four bits: note, level, tint, speed, trail.

    Everything a wave needs to be drawn by somebody else. Nothing about who sent it,
    because that does not matter to the drawing - which is the property that keeps the
    sender from needing a copy of every member's settings.
*/
uint64_t LumiLink::packEffect (int note, int level, uint32_t tint, int speed, int trail,
                               int kind)
{
    return ((uint64_t) 1 << 63)
         | ((uint64_t) (note & 0x7f) << 52)
         | ((uint64_t) (level & 0xff) << 44)
         | ((uint64_t) (tint & 0x00ffffffu) << 20)
         | ((uint64_t) (speed & 0x1f) << 15)
         | ((uint64_t) (trail & 0x3f) << 9)
         | ((uint64_t) (kind & 3) << 7);
}

void LumiLink::unpackEffect (uint64_t packed, int &note, int &level, uint32_t &tint,
                             int &speed, int &trail, int &kind)
{
    note = (int) ((packed >> 52) & 0x7f);
    level = (int) ((packed >> 44) & 0xff);
    tint = (uint32_t) ((packed >> 20) & 0x00ffffffu);
    speed = (int) ((packed >> 15) & 0x1f);
    trail = (int) ((packed >> 9) & 0x3f);
    kind = (int) ((packed >> 7) & 3);
}

/* Records the level only. The worker decides whether enough time has passed to start
   another splash, so a continuous CC sweep produces a steady pulse rather than one
   ripple per message. */
void LumiLink::triggerSplash (int level)
{
    if (splashEnabled.load (std::memory_order_relaxed) == 0)
        return;

    if (level < 1)
        level = 1;

    if (level > 255)
        level = 255;

    pendingSplashLevel.store (level, std::memory_order_release);
}

/*
    Where a splash starts.

    It was note 60, which is the middle of the MIDI range and almost never the middle of
    anything the keyboard is showing. On a two-octave Piano M starting at C2 the wave
    began off the top of the keys, so half of it never appeared and what did arrive came
    in from one edge - which looks like a bug rather than a splash.

    Taken from the blocks themselves rather than from the window, because the window is
    computed as blocks * 24 keys from one base and only describes a contiguous cluster.
    Blocks that have been given their own octaves report their own bases, and the point
    halfway between the bottom of the lowest and the top of the highest is the middle of
    the instrument in both cases. When they are not contiguous that point can land in the
    gap between them, which is right: the wave then reaches each block's inner edge at
    the same moment and the pair lights symmetrically.
*/
void LumiLink::setZoned (bool on)
{
    const bool wasHeld = hasZone();
    const int oldLow = zoneLow.load (std::memory_order_relaxed);
    const int oldHigh = zoneHigh.load (std::memory_order_relaxed);

    zoned.store (on ? 1 : 0, std::memory_order_relaxed);
    claim.setZoned (on);

    if (! on)
    {
        /*
            Leaving a chain is not just ticking a box off.

            Three things were left behind. The colours this instance had published were
            still in the shared table, so a member that kept sharing went on drawing a
            range whose owner had walked away. The device was still marked as owned by
            the hive, and an instance that is no longer in the hive reads that as "not
            mine" - so it closed its port and sat dark until the lease ran out or
            somebody played a note. And the send cache still believed the device was
            showing whatever the chain had put there, so the keys that happened to match
            were never resent.

            Taking the device outright is the right answer to un-sharing: the user has
            just said this instance owns the whole keyboard again.
        */
        claim.releaseZone();
        zoneHeld.store (0, std::memory_order_relaxed);

        /*
            Only an instance that actually had a range is leaving one.

            Turning sharing off also happens when a range is refused - loading a project
            or a map asks for its old range and falls back to this when somebody else
            already owns it. Claiming the device there knocked the hive off the keyboard
            every time a project was opened alongside one, which is the opposite of what
            un-sharing is for.

            With a range to give up, taking the device is right: the user has just said
            this instance owns the whole keyboard again. Without one, nothing has
            changed and nothing should be taken.
        */
        if (wasHeld)
        {
            claim.clearZoneColours (oldLow, oldHigh);
            claim.claim();
            invalidateCache();
        }
    }
}

bool LumiLink::getZoned() const
{
    return zoned.load (std::memory_order_relaxed) != 0;
}

bool LumiLink::setZoneRange (int low, int high, ZoneInfo &blocker)
{
    if (! claim.claimZone (low, high, blocker))
    {
        /*
            A refused range leaves this instance owning nothing at all.

            It used to leave the stored range alone, which defaults to the whole
            keyboard - so an instance whose claim was refused stayed zoned holding a
            phantom 0 to 127 and published all 128 keys over everybody else's. Whoever
            published last won, which looks exactly like one instance's colours
            replacing the rest.

            Owning nothing is the honest state for a member that asked and was told no:
            it publishes nothing, takes no notes, and the panel says it needs a range.
        */
        zoneHeld.store (0, std::memory_order_relaxed);
        return false;
    }

    zoneLow.store (low < high ? low : high, std::memory_order_relaxed);
    zoneHigh.store (low < high ? high : low, std::memory_order_relaxed);
    zoneHeld.store (1, std::memory_order_relaxed);
    return true;
}

/* Zoned and actually holding a range. Zoned without one owns no keys. */
void LumiLink::setZoneOffset (int semitones)
{
    if (semitones < -127) semitones = -127;
    if (semitones > 127) semitones = 127;

    zoneOffset.store (semitones, std::memory_order_relaxed);
}

int LumiLink::getZoneOffset() const
{
    return hasZone() ? zoneOffset.load (std::memory_order_relaxed) : 0;
}

/*
    Key to note, and back.

    Out of range in either direction is -1 rather than a clamp: a key whose note would
    be off the end of MIDI plays nothing and shows nothing, which is honest. Clamping
    would pile every such key onto note 0 or 127.
*/
int LumiLink::keyToNote (int key) const
{
    const int note = key + getZoneOffset();
    return (note < 0 || note > 127) ? -1 : note;
}

int LumiLink::noteToKey (int note) const
{
    const int key = note - getZoneOffset();
    return (key < 0 || key > 127) ? -1 : key;
}

void LumiLink::setKeyColour (int key, uint32_t rgb)
{
    const int note = keyToNote (key);

    if (note >= 0)
        setColour (note, rgb);
}

uint32_t LumiLink::getKeyColour (int key) const
{
    const int note = keyToNote (key);
    return note >= 0 ? getColour (note) : 0;
}

bool LumiLink::hasZone() const
{
    return zoned.load (std::memory_order_relaxed) != 0
        && zoneHeld.load (std::memory_order_relaxed) != 0;
}

int LumiLink::getZoneLow() const { return zoneLow.load (std::memory_order_relaxed); }
int LumiLink::getZoneHigh() const { return zoneHigh.load (std::memory_order_relaxed); }

/*
    Whether a note belongs to this instance.

    Un-zoned means the whole keyboard, which is what keeps every existing project
    working: an instance that has never heard of zones behaves exactly as before.
*/
bool LumiLink::noteInZone (int note) const
{
    if (zoned.load (std::memory_order_relaxed) == 0)
        return true;

    if (zoneHeld.load (std::memory_order_relaxed) == 0)
        return false;

    return note >= zoneLow.load (std::memory_order_relaxed)
        && note <= zoneHigh.load (std::memory_order_relaxed);
}

void LumiLink::renewZone()
{
    claim.renewZone();
}

void LumiLink::noteReachedZone()
{
    claim.noteSeen();
}

bool LumiLink::zoneStarved() const
{
    return zoned.load (std::memory_order_relaxed) != 0 && claim.zoneStarved();
}

bool LumiLink::zoneAt (int index, ZoneInfo &info) const
{
    return claim.zoneAt (index, info);
}

bool LumiLink::isZoneSender() const
{
    return claim.isSender();
}

uint32_t LumiLink::selfId() const
{
    return claim.selfId();
}

/* The lowest live zone owner, which is the same rule every member applies - so asking
   any of them gives the same answer. */
uint32_t LumiLink::senderId() const
{
    uint32_t lowest = 0;

    for (int i = 0; i < kMaxZones; ++i)
    {
        ZoneInfo info;

        if (! claim.zoneAt (i, info))
            continue;

        if (lowest == 0 || info.owner < lowest)
            lowest = info.owner;
    }

    return lowest;
}

int LumiLink::splashOrigin() const
{
    const int blocks = blockCount.load (std::memory_order_relaxed);

    int lowest = -1;
    int highest = -1;

    for (int i = 0; i < blocks && i < 5; ++i)
    {
        const int base = blockLow[i].load (std::memory_order_relaxed);

        if (base < 0)
            continue;

        if (lowest < 0 || base < lowest)
            lowest = base;

        if (highest < 0 || base > highest)
            highest = base;
    }

    if (lowest < 0 || highest < 0)
    {
        /* Nothing has reported yet, so fall back to the window - which before any
           device is seen is the whole range, and 60 again. */
        lowest = windowLow.load (std::memory_order_relaxed);
        highest = windowHigh.load (std::memory_order_relaxed) - 23;

        if (highest < lowest)
            highest = lowest;
    }

    int centre = (lowest + (highest + 23)) / 2;

    if (centre < 0)
        centre = 0;

    if (centre > 127)
        centre = 127;

    return centre;
}

void LumiLink::setAfterglowEnabled (bool on)
{
    afterglowEnabled.store (on ? 1 : 0, std::memory_order_relaxed);
}

bool LumiLink::getAfterglowEnabled() const
{
    return afterglowEnabled.load (std::memory_order_relaxed) != 0;
}

void LumiLink::setAfterglowColour (uint32_t rgb)
{
    afterglowColour.store (rgb & 0x00ffffffu, std::memory_order_relaxed);
}

uint32_t LumiLink::getAfterglowColour() const
{
    return afterglowColour.load (std::memory_order_relaxed);
}

/* Tenths of a second for a struck key to return to its own colour. */
void LumiLink::setAfterglowDecay (int tenths)
{
    if (tenths < 1) tenths = 1;
    if (tenths > 50) tenths = 50;
    afterglowDecay.store (tenths, std::memory_order_relaxed);
}

int LumiLink::getAfterglowDecay() const
{
    return afterglowDecay.load (std::memory_order_relaxed);
}

void LumiLink::setPulseEnabled (bool on)
{
    pulseEnabled.store (on ? 1 : 0, std::memory_order_relaxed);
}

bool LumiLink::getPulseEnabled() const
{
    return pulseEnabled.load (std::memory_order_relaxed) != 0;
}

void LumiLink::setPulseColour (uint32_t rgb)
{
    pulseColour.store (rgb & 0x00ffffffu, std::memory_order_relaxed);
}

uint32_t LumiLink::getPulseColour() const
{
    return pulseColour.load (std::memory_order_relaxed);
}

void LumiLink::setDegreeEnabled (bool on)
{
    degreeEnabled.store (on ? 1 : 0, std::memory_order_relaxed);
}

bool LumiLink::getDegreeEnabled() const
{
    return degreeEnabled.load (std::memory_order_relaxed) != 0;
}

void LumiLink::setDegreeAlpha (int alpha)
{
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    degreeAlpha.store (alpha, std::memory_order_relaxed);
}

int LumiLink::getDegreeAlpha() const
{
    return degreeAlpha.load (std::memory_order_relaxed);
}

void LumiLink::setDegreeScale (uint32_t mask)
{
    degreeScale.store (mask & 0xfffu, std::memory_order_relaxed);
}

void LumiLink::setScaleRoot (int pitchClass)
{
    scaleRoot.store (((pitchClass % 12) + 12) % 12, std::memory_order_relaxed);
}

int LumiLink::getScaleRoot() const
{
    return scaleRoot.load (std::memory_order_relaxed);
}

uint32_t LumiLink::getDegreeScale() const
{
    return degreeScale.load (std::memory_order_relaxed);
}

void LumiLink::setDegreeColour (int index, uint32_t rgb)
{
    if (index < 0 || index > 7)
        return;

    degreeColour[index].store (rgb & 0x00ffffffu, std::memory_order_relaxed);
}

uint32_t LumiLink::getDegreeColour (int index) const
{
    if (index < 0 || index > 7)
        return 0;

    return degreeColour[index].load (std::memory_order_relaxed);
}

int LumiLink::getDegreeRoot() const
{
    return degreeRoot.load (std::memory_order_relaxed);
}

/* Called from the audio thread on note-on: the note played becomes the root. */
void LumiLink::setDegreeRoot (int pitchClass)
{
    /* Tension uses this root as much as the degree map does, and gating it on Degrees
       alone meant that with only Tension switched on the root never moved - so the
       colours never changed however much you played, which looks exactly like the
       effect not working. */
    if (degreeEnabled.load (std::memory_order_relaxed) == 0
        && tensionEnabled.load (std::memory_order_relaxed) == 0)
        return;

    degreeRoot.store (((pitchClass % 12) + 12) % 12, std::memory_order_relaxed);
}

void LumiLink::setTensionEnabled (bool on)
{
    tensionEnabled.store (on ? 1 : 0, std::memory_order_relaxed);
}

bool LumiLink::getTensionEnabled() const
{
    return tensionEnabled.load (std::memory_order_relaxed) != 0;
}

void LumiLink::setTensionAlpha (int alpha)
{
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    tensionAlpha.store (alpha, std::memory_order_relaxed);
}

int LumiLink::getTensionAlpha() const
{
    return tensionAlpha.load (std::memory_order_relaxed);
}

void LumiLink::setTensionHome (uint32_t rgb)
{
    tensionHome.store (rgb & 0x00ffffffu, std::memory_order_relaxed);
}

uint32_t LumiLink::getTensionHome() const
{
    return tensionHome.load (std::memory_order_relaxed);
}

void LumiLink::setTensionFar (uint32_t rgb)
{
    tensionFar.store (rgb & 0x00ffffffu, std::memory_order_relaxed);
}

uint32_t LumiLink::getTensionFar() const
{
    return tensionFar.load (std::memory_order_relaxed);
}

void LumiLink::setVelocityEnabled (bool on)
{
    velocityEnabled.store (on ? 1 : 0, std::memory_order_relaxed);
}

bool LumiLink::getVelocityEnabled() const
{
    return velocityEnabled.load (std::memory_order_relaxed) != 0;
}

/* Audio thread. */
void LumiLink::noteVelocity (int note, int velocity)
{
    if (note < 0 || note > 127)
        return;

    noteVelocities[note].store (velocity, std::memory_order_relaxed);
}

void LumiLink::setWavesEnabled (bool on)
{
    wavesEnabled.store (on ? 1 : 0, std::memory_order_relaxed);
}

bool LumiLink::getWavesEnabled() const
{
    return wavesEnabled.load (std::memory_order_relaxed) != 0;
}

void LumiLink::setWavesDelay (int seconds)
{
    if (seconds < 5) seconds = 5;
    if (seconds > 600) seconds = 600;
    wavesDelay.store (seconds, std::memory_order_relaxed);
}

int LumiLink::getWavesDelay() const
{
    return wavesDelay.load (std::memory_order_relaxed);
}

void LumiLink::setWavesMode (int mode)
{
    if (mode < 0 || mode > 5)
        mode = 0;

    wavesMode.store (mode, std::memory_order_relaxed);
}

int LumiLink::getWavesMode() const
{
    return wavesMode.load (std::memory_order_relaxed);
}

bool LumiLink::writeClipboard (const std::string &text)
{
    return claim.writeClipboard (text);
}

bool LumiLink::readClipboard (std::string &text) const
{
    return claim.readClipboard (text);
}

void LumiLink::setGradientStop (int index, uint32_t rgb)
{
    if (index < 0 || index >= kGradientStops)
        return;

    gradientStops[index].store (rgb & 0x00ffffffu, std::memory_order_relaxed);
}

uint32_t LumiLink::getGradientStop (int index) const
{
    if (index < 0 || index >= kGradientStops)
        return 0;

    return gradientStops[index].load (std::memory_order_relaxed);
}

void LumiLink::setGradientCount (int count)
{
    if (count < 2)
        count = 2;

    if (count > kGradientStops)
        count = kGradientStops;

    gradientCount.store (count, std::memory_order_relaxed);
}

int LumiLink::getGradientCount() const
{
    const int count = gradientCount.load (std::memory_order_relaxed);
    return count < 2 ? 2 : (count > kGradientStops ? kGradientStops : count);
}

/*
    The colour at a point along the gradient.

    Interpolated per channel between the two stops either side, which is wrong in the
    way every RGB interpolation is wrong - blue to yellow passes through a muddy grey
    rather than through green - and right in the way that matters here, which is that
    the stops are the colours the user picked and the ramp between them is predictable.
    Anything cleverer would mean the keyboard showing hues that are in no stop.
*/
uint32_t LumiLink::gradientAt (float position) const
{
    const int count = getGradientCount();

    if (position <= 0.0f)
        return getGradientStop (0);

    if (position >= 1.0f)
        return getGradientStop (count - 1);

    const float scaled = position * (float) (count - 1);
    int lower = (int) scaled;

    if (lower > count - 2)
        lower = count - 2;

    const float blend = scaled - (float) lower;
    const uint32_t a = getGradientStop (lower);
    const uint32_t b = getGradientStop (lower + 1);

    uint32_t out = 0;

    for (int shift = 16; shift >= 0; shift -= 8)
    {
        const float channelA = (float) ((a >> shift) & 0xffu);
        const float channelB = (float) ((b >> shift) & 0xffu);
        int value = (int) (channelA + (channelB - channelA) * blend + 0.5f);

        if (value < 0)
            value = 0;

        if (value > 255)
            value = 255;

        out |= (uint32_t) value << shift;
    }

    return out;
}

/* Any key lit at all, by a finger, the pedal, or the listen port. */
bool LumiLink::anyNoteSounding() const
{
    return (litBits[0].load (std::memory_order_relaxed)
             | externalLitBits[0].load (std::memory_order_relaxed)) != 0ull
        || (litBits[1].load (std::memory_order_relaxed)
             | externalLitBits[1].load (std::memory_order_relaxed)) != 0ull;
}

bool LumiLink::wavesRunning() const
{
    if (wavesEnabled.load (std::memory_order_relaxed) == 0)
        return false;

    /*
        In a chain, the screensaver belongs to whoever is sending.

        One keyboard going idle is one picture. Four members each starting their own at
        slightly different moments - their idle clocks reset by whichever notes reached
        their own zone - would have made four, with seams between them and a drifting
        pattern restarting at every boundary. Members publish their maps as usual and
        the sender lays the pattern over the whole assembled chain.
    */
    if (hasZone() && ! claim.isSender())
        return false;

    return idleMs.load (std::memory_order_relaxed)
             > wavesDelay.load (std::memory_order_relaxed) * 1000;
}

void LumiLink::setBendPathEnabled (bool on)
{
    bendPathEnabled.store (on ? 1 : 0, std::memory_order_relaxed);
}

bool LumiLink::getBendPathEnabled() const
{
    return bendPathEnabled.load (std::memory_order_relaxed) != 0;
}

void LumiLink::setBendPathColour (uint32_t rgb)
{
    bendPathColour.store (rgb & 0x00ffffffu, std::memory_order_relaxed);
}

uint32_t LumiLink::getBendPathColour() const
{
    return bendPathColour.load (std::memory_order_relaxed);
}

/* Which note is being held on each MPE channel, so a bend on that channel can be
   traced from the right starting key. */
/*
    Bend for one note, held against the note rather than a channel.

    Cleared when the note ends, so a key that is not sounding cannot carry a stale bend
    into the next thing played on it.
*/
void LumiLink::tuningOnNote (int note, double semitones)
{
    if (note < 0 || note > 127)
        return;

    noteBendCents[note].store ((int) (semitones * 100.0), std::memory_order_relaxed);
}

/*
    What a note is bent by, whichever way the host said it.

    A per-note value wins when there is one, because that is a host being explicit about
    this note. Otherwise the note's channel is used, which is how MPE over raw MIDI and
    an ordinary bend wheel both arrive.
*/
int LumiLink::bendCentsForNote (int note) const
{
    if (note < 0 || note > 127)
        return 0;

    const int own = noteBendCents[note].load (std::memory_order_relaxed);

    if (own != 0)
        return own;

    const int ch = noteChannel[note].load (std::memory_order_relaxed);
    return ch >= 0 ? channelBend[ch].load (std::memory_order_relaxed) : 0;
}

void LumiLink::noteOnChannel (int channel, int note, bool on)
{
    if (channel < 0 || channel > 15)
        return;

    if (! on)
        noteBendCents[note].store (0, std::memory_order_relaxed);

    channelNote[channel].store (on ? note : -1, std::memory_order_relaxed);

    if (note >= 0 && note < 128)
        noteChannel[note].store (on ? channel : -1, std::memory_order_relaxed);

    if (! on)
        channelBend[channel].store (0, std::memory_order_relaxed);
}

void LumiLink::bendOnChannel (int channel, int value)
{
    if (channel < 0 || channel > 15)
        return;

    /* Converted here rather than at the drawing end, so both routes in arrive as the
       same thing. */
    int range = deviceConfig[kConfigPitchBendRange].load (std::memory_order_relaxed);

    if (range == kConfigUnknown || range < 1)
        range = 48;

    const int cents = ((value - 8192) * range * 100) / 8192;
    channelBend[channel].store (cents, std::memory_order_relaxed);
    lastBendCents.store (cents, std::memory_order_relaxed);
    lastBendNote.store (channelNote[channel].load (std::memory_order_relaxed),
                        std::memory_order_relaxed);
}

void LumiLink::tuningOnChannel (int channel, double semitones)
{
    if (channel < 0 || channel > 15)
        return;

    const int cents = (int) (semitones * 100.0);
    channelBend[channel].store (cents, std::memory_order_relaxed);
    lastBendCents.store (cents, std::memory_order_relaxed);
    lastBendNote.store (channelNote[channel].load (std::memory_order_relaxed),
                        std::memory_order_relaxed);
}







void LumiLink::setHaloEnabled (bool on)
{
    haloEnabled.store (on ? 1 : 0, std::memory_order_relaxed);
}

bool LumiLink::getHaloEnabled() const
{
    return haloEnabled.load (std::memory_order_relaxed) != 0;
}

void LumiLink::setHaloColour (uint32_t rgb)
{
    haloColour.store (rgb & 0x00ffffffu, std::memory_order_relaxed);
}

uint32_t LumiLink::getHaloColour() const
{
    return haloColour.load (std::memory_order_relaxed);
}

void LumiLink::triggerAfterglow (int note, int level)
{
    if (afterglowEnabled.load (std::memory_order_relaxed) == 0)
        return;

    if (note < 0 || note > 127)
        return;

    glowTrigger[note].store (level < 1 ? 1 : (level > 255 ? 255 : level),
                             std::memory_order_release);
}

void LumiLink::triggerBeat (int level)
{
    if (pulseEnabled.load (std::memory_order_relaxed) == 0)
        return;

    /*
        The beat is posted to the chain as well.

        Every instance sees the transport, but only the ones with the pulse switched on
        want to act on it - and a pulse is a property of the bar, not of a range, so it
        belongs across the whole keyboard rather than inside whichever zone happens to
        have the checkbox ticked. Posting it means switching it on anywhere lights
        everywhere.
    */
    if (zoned.load (std::memory_order_relaxed) != 0)
    {
        claim.postEffect (packEffect (0, level, pulseColour.load (std::memory_order_relaxed),
                                      0, 0, kEffectPulse));
        return;
    }

    pendingBeatLevel.store (level, std::memory_order_release);
}

void LumiLink::advanceGlow (int elapsedMs)
{
    activeGlow = 0;

    /* A decay of n tenths of a second costs 255 * elapsed / (n * 100) this tick. */
    const int tenths = afterglowDecay.load (std::memory_order_relaxed);
    int step = (255 * elapsedMs) / (tenths * 100);

    if (step < 1)
        step = 1;

    for (int note = 0; note < 128; ++note)
    {
        const int fresh = glowTrigger[note].exchange (0, std::memory_order_acquire);

        if (fresh > 0)
            glowLevel[note] = fresh;

        if (glowLevel[note] <= 0)
            continue;

        glowLevel[note] -= step;

        if (glowLevel[note] < 0)
            glowLevel[note] = 0;
        else
            ++activeGlow;
    }

    const int beat = pendingBeatLevel.exchange (-1, std::memory_order_acquire);

    if (beat > 0)
        pulseLevel = beat;
    else if (pulseLevel > 0)
    {
        pulseLevel -= (6 * elapsedMs) / 4;

        if (pulseLevel < 0)
            pulseLevel = 0;
    }
}

void LumiLink::advanceRipples (int elapsedMs)
{
    /*
        The cooldown counts milliseconds, so it must be tested as "expired", not as
        "exactly zero".

        It used to count ticks and land on zero precisely. Once it counted elapsed time
        it stepped 160, 145, 130 and straight past zero into negative numbers, so the
        equality never held again and splash fired once and never more. A comparison
        that only works when a counter decrements by one is a trap waiting for the day
        the step size changes.
    */
    if (splashCooldown > 0)
    {
        splashCooldown -= elapsedMs;

        if (splashCooldown < 0)
            splashCooldown = 0;
    }

    const int splashLevel = splashCooldown <= 0
                          ? pendingSplashLevel.exchange (-1, std::memory_order_acquire)
                          : -1;

    if (splashLevel > 0)
    {
        splashCooldown = 160;
        const uint32_t packed = 0xc0000000u
                              | ((uint32_t) (splashOrigin() & 0x7f) << 8)
                              | (uint32_t) (splashLevel & 0xff);
        const uint32_t slot = triggerWrite.fetch_add (1, std::memory_order_relaxed);
        triggerRing[slot % kTriggerSlots].store (packed, std::memory_order_release);
    }

    for (uint32_t guard = 0; guard < kTriggerSlots; ++guard)
    {
        const uint32_t packed = triggerRing[triggerRead % kTriggerSlots]
                                    .exchange (0, std::memory_order_acquire);

        if (packed == 0)
            break;

        ++triggerRead;

        /* Free slot if there is one, otherwise the oldest - a fast passage should
           show its most recent notes rather than refuse to start new waves. */
        int slot = -1;

        for (int i = 0; i < kMaxRipples; ++i)
            if (rippleAge[i] < 0)
            {
                slot = i;
                break;
            }

        if (slot < 0)
        {
            slot = 0;

            for (int i = 1; i < kMaxRipples; ++i)
                if (rippleAge[i] > rippleAge[slot])
                    slot = i;
        }

        const bool isSplash = (packed & 0x40000000u) != 0u;

        rippleNote[slot] = (int) ((packed >> 8) & 0x7f);
        rippleLevel[slot] = (int) (packed & 0xff);
        if (isSplash)
        {
            rippleTint[slot] = splashColour.load (std::memory_order_relaxed);
        }
        else
        {
            rippleTint[slot] = resolveRippleTint (rippleNote[slot]);
        }
        rippleStep[slot] = isSplash ? splashSpeed.load (std::memory_order_relaxed)
                                    : rippleSpeed.load (std::memory_order_relaxed);
        rippleTrail16[slot] = 16 * (isSplash ? splashTrail.load (std::memory_order_relaxed)
                                             : rippleTrail.load (std::memory_order_relaxed));
        rippleAge[slot] = 0;
    }

    activeRipples = 0;

    for (int i = 0; i < kMaxRipples; ++i)
    {
        if (rippleAge[i] < 0)
            continue;

        /* Speed is in sixteenths of a key per 4 ms, so it stays the number the slider
           shows however long the tick actually took. */
        rippleAge[i] += (rippleStep[i] * elapsedMs) / 4;

        if (rippleAge[i] > 128 * 16 + rippleTrail16[i])
            rippleAge[i] = -1;
        else
            ++activeRipples;
    }
}

/* One pass over the table per tick. Cheap enough at 128 notes by six ripples, and it
   means the wave and the painted colours never get out of step. */
/* Every note is composited, not just the visible ones.
   
   Restricting this to the window was a mistake: notes outside it kept whatever
   desiredColour happened to hold from before, so the editor - which draws the whole
   range - showed correct colours inside the window and stale ones outside, and an
   import never reached the notes beyond it. The saving belongs in the sender, which
   is where the traffic actually is; compositing 128 notes costs nothing. */
/*
    The idle pattern for one key, over whatever colour is already there.

    Lifted out of the composite so the sender can paint it across a whole chain. Members
    of a hive do not run a screensaver of their own - one keyboard idling is one picture,
    and four instances each deciding to start their own at slightly different moments
    would have made four. So they publish their maps as usual and whoever is sending
    lays the pattern over the lot, which is also the only way a drifting or rainfall
    pattern can cross a zone boundary without a seam.

    Takes what is underneath because two of the six keep it: breathing and ember move
    the brightness of the painted colour rather than replacing it.
*/
uint32_t LumiLink::screensaverColour (int note, uint32_t result) const
{
    const int wavesWhich = wavesMode.load (std::memory_order_relaxed);

    /*
    Two of these replace the map and two keep it.

    Waves and aurora are fields: a colour computed from the note and the
    phase, owing nothing to what was painted. Breathing and ember take the
    painted colour and move only its brightness, so an idle keyboard still
    says where the keyswitches are. That is the whole difference, and it is
    why the painted base is read rather than discarded for the last two.

    Every one of them is written to replace rather than blend, because a
    screensaver over a colour map is neither, and every effect below still
    paints over the result - which is what makes a note interrupt the idle
    pattern visibly before the timer has even noticed.
    */
    if (wavesWhich == 1)
    {
    /*
        Aurora: hue drifting along the keyboard, everything lit, nothing
        blinking.

        Interpolated through a palette rather than built from bands. The
        first version divided the hue into six bands and ramped a fraction
        inside each, but the colour branches changed only twice in those six
        - so within a branch the fraction climbed to full, reset to zero and
        climbed again, and the keyboard showed a sawtooth. Three visible
        steps across the keybed, which is what a continuous drift must not
        have.

        The position is smoothstepped before use. A plain triangle wave has
        a corner at its apex, and a corner in a slow drift reads as a crease
        travelling along the keys; easing it at both ends turns the fold
        into a turn.
    */
    static const uint32_t palette[5] = { 0x1fd45a, 0x00d4b4, 0x00a8ff,
                                         0x2a4aff, 0x8a3dff };

    /*
        A cycle spans about forty keys, not three.

        This is the number that decides whether it reads as a drift or as
        noise: at a third of a cycle per key the palette repeats every
        couple of keys and neighbouring keys land on unrelated colours, so
        smoothing the curve buys nothing. Forty keys to a cycle means a
        two-octave block shows about half the palette at once and adjacent
        keys are always close.
    */
    const float t = (float) wavePhase * 0.001f;
    const float first = (float) note * 0.025f + t * 0.055f;
    const float second = (float) note * 0.011f - t * 0.031f;

    /* Two folds of different period beating against each other, so the
       pattern never settles into a repeat the eye can follow. */
    float a = first - (float) (int) first;
    float b = second - (float) (int) second;

    if (a < 0.0f) a += 1.0f;
    if (b < 0.0f) b += 1.0f;

    a = a < 0.5f ? a * 2.0f : (1.0f - a) * 2.0f;
    b = b < 0.5f ? b * 2.0f : (1.0f - b) * 2.0f;

    a = a * a * (3.0f - 2.0f * a);
    b = b * b * (3.0f - 2.0f * b);

    float position = (a * 2.0f + b) / 3.0f;

    if (position < 0.0f) position = 0.0f;
    if (position > 1.0f) position = 1.0f;

    const float scaled = position * 4.0f;
    int lower = (int) scaled;

    if (lower > 3)
        lower = 3;

    const float blend = scaled - (float) lower;
    const uint32_t from = palette[lower];
    const uint32_t to = palette[lower + 1];

    uint32_t out = 0;

    for (int shift = 16; shift >= 0; shift -= 8)
    {
        const float channelA = (float) ((from >> shift) & 0xffu);
        const float channelB = (float) ((to >> shift) & 0xffu);
        int value = (int) (channelA + (channelB - channelA) * blend + 0.5f);

        if (value < 0) value = 0;
        if (value > 255) value = 255;

        out |= (uint32_t) value << shift;
    }

    result = out;
    }
    else if (wavesWhich == 2)
    {
    /*
        Breathing paints nothing here, deliberately.

        The swell is one controller message that the firmware applies to the
        whole keyboard, so this path leaves the painted map exactly as it
        found it. Without this branch mode 2 fell through to the waves case
        below and the keyboard showed waves with a breath on top - which is
        what happened when the controller route was added and this was not
        written.
    */
    }
    else if (wavesWhich == 4)
    {
    /*
        The paint gradient, scrolling along the keys.

        Every other pattern has its palette decided here - waves is blue,
        aurora green through violet, breathing and ember borrow the map. This
        one is the only screensaver that shows the colours you chose, which
        is most of the reason to have it.

        Folded, not wrapped. Wrapping looks like the obvious choice and is
        wrong: a gradient's two ends are whatever colours you picked and
        have no reason to match, so repeating it puts a hard edge between
        the last stop and the first, and that edge travels along the keys.
        Folding runs the gradient up and back instead, which has no seam
        anywhere - the turn happens at a stop, where the colour is already
        standing still.

        Roughly forty keys to a sweep, the same spacing aurora settled on.
    */
    float position = (float) note * 0.0125f - (float) wavePhase * 0.000045f;
    position -= (float) (int) position;

    if (position < 0.0f)
        position += 1.0f;

    /* 0..1..0 rather than 0..1 then back to 0. */
    position = position < 0.5f ? position * 2.0f : (1.0f - position) * 2.0f;

    result = gradientAt (position);
    }
    else if (wavesWhich == 5)
    {
    /*
        Rainfall: keys lighting one at a time and fading, nothing else lit.

        Stateless on purpose. Each key is given its own interval and its own
        starting offset from a hash of its note number, so the drops are
        scattered and no two keys fall together for long - without a random
        generator to seed, a per-note array to keep, or anything that has to
        be reset when the pattern starts. The phase counter the other
        patterns already use is the only input.

        A drop spreads two keys either side rather than landing on one.

        Which is why this asks its neighbours rather than only itself: a key
        looks at the five drops that could reach it, including its own, and
        takes the brightest. The spread arrives slightly later the further it
        goes and dimmer with it, so a drop reads as a small splash rather
        than three keys switching on together - the ripple idea at a scale of
        two keys.

        Still stateless. The neighbour's drop is recomputed from its own
        hash, which costs four extra hashes a key and keeps the pattern free
        of anything that has to be stored or reset.

        The colour comes from the gradient rather than from the map, so this
        works over Blackout, which is where a sparse pattern looks best. A
        key always falls in the same colour, taken from its own hash, so the
        keyboard keeps a consistent character instead of flickering through
        the whole palette.
    */
    const int fall = 520;

    /*
        Contributions are mixed, not picked between.

        Two drops landing near each other overlap, and the keys they share
        should carry both colours - a blue drop beside an amber one makes
        the keys between them something of each, the way two ripples
        crossing do. Taking only the brightest threw that away and left a
        hard edge where one drop stopped mattering and the next started.

        The hue is the average of what reaches a key, weighted by how
        strongly each arrives; the brightness is the sum, so overlapping
        drops are brighter as well as blended.
    */
    int sumR = 0, sumG = 0, sumB = 0;
    int sumWeight = 0;
    int totalLevel = 0;

    for (int d = -2; d <= 2; ++d)
    {
        const int source = note + d;

        if (source < 0 || source > 127)
            continue;

        uint32_t h = (uint32_t) source * 2654435761u;
        h ^= h >> 15;
        h *= 2246822519u;
        h ^= h >> 13;

        /* Between three and ten seconds a key. A splash covers five keys,
           so drops falling as often as single ones did put most of the
           keyboard in motion at once and sent far more than the device
           needs. */
        const int interval = 2800 + (int) (h % 7200u);
        const int offset = (int) ((h >> 7) % (uint32_t) interval);
        const int reach = d < 0 ? -d : d;

        /* Barely later the further it has travelled. Enough that the
           splash reads as leaving the point it landed on rather than the
           whole group appearing at once, and not so much that the edges
           trail behind as separate events. */
        int phase = (wavePhase + offset) % interval - reach * 12;

        if (phase < 0 || phase >= fall)
            continue;

        int level = 255 - (phase * 255) / fall;
        level = (level * level) / 255;

        /* Each key out costs about a third, so the edge of a splash is
           present without competing with its middle. */
        level = level * (3 - reach) / 3;

        if (level <= 0)
            continue;

        const uint32_t tint =
            gradientAt ((float) ((h >> 19) % 1000u) / 999.0f);

        sumR += (int) ((tint >> 16) & 0xffu) * level;
        sumG += (int) ((tint >> 8) & 0xffu) * level;
        sumB += (int) (tint & 0xffu) * level;
        sumWeight += level;
        totalLevel += level;
    }

    if (sumWeight <= 0)
    {
        result = 0;
    }
    else
    {
        if (totalLevel > 255)
            totalLevel = 255;

        /*
            Stepped, so a fading key is not rewritten on every tick.

            A smooth decay changes colour by a shade or two each tick, which
            the eye cannot see and the device has to be told about anyway -
            that was most of the traffic this pattern generated, more than
            the drops themselves. Sixteen steps is invisible in a fade and
            halves what goes out. The ripple decay is quantised for the same
            reason.
        */
        totalLevel = (totalLevel / 16) * 16;

        /* Falls through rather than skipping the rest of the key's work:
           the zone filter and the sustain tint still have to run, and an
           early exit here would leak a colour onto a key this instance does
           not own. */
        if (totalLevel <= 0)
        {
            result = 0;
        }
        else
        {
            const int r = (sumR / sumWeight) * totalLevel / 255;
            const int g = (sumG / sumWeight) * totalLevel / 255;
            const int b = (sumB / sumWeight) * totalLevel / 255;

            result = ((uint32_t) r << 16) | ((uint32_t) g << 8) | (uint32_t) b;
        }
    }
    }
    else if (wavesWhich == 3)
    {
    /*
        Ember only.

        Breathing used to share this branch and no longer does: swelling the
        whole map by one amount is what the device's unlit level already
        means, so it is sent as a single controller message from
        flushGlobals instead of repainting a hundred and twenty-eight notes
        every tick. That also leaves a key being played at full brightness,
        which the firmware does for free and this path could not.

        Ember cannot take that route - a per-note phase is a hundred and
        twenty-eight different levels, and there is one control. So it stays
        here, and it is the expensive one of the two by design.

        A floor under the dim end, because a map that goes fully dark and
        comes back reads as the plugin dropping out. It never quite leaves.
    */
    const int phase = ((note * 17 + wavePhase / 26) % 360 + 360) % 360;
    const int tri = phase < 180 ? phase : 360 - phase;

    int level = 60 + (tri * 195) / 180;

    if (level < 0)
        level = 0;

    if (level > 255)
        level = 255;

    const uint32_t src = result;
    const int r = (int) ((src >> 16) & 0xffu) * level / 255;
    const int g = (int) ((src >> 8) & 0xffu) * level / 255;
    const int b = (int) (src & 0xffu) * level / 255;

    result = ((uint32_t) r << 16) | ((uint32_t) g << 8) | (uint32_t) b;
    }
    else
    {
    /*
        Two swells of different length and speed, so the pattern never
        settles into an obvious repeat - one alone reads as a metronome.

        The phase is wrapped into range before use. It used to be taken
        modulo 360 after a subtraction, and C++ modulo keeps the sign of the
        left operand: once the phase passed the note's offset the result went
        negative, squaring turned that trough into a crest, and the channels
        ran past their range. That is where the yellow came from, and why it
        only appeared after the thing had been running a while.
    */
    const int a = ((note * 24 + wavePhase / 22) % 360 + 360) % 360;
    const int b = ((note * 13 - wavePhase / 37) % 360 + 360) % 360;

    const int ta = a < 180 ? a : 360 - a;
    const int tb = b < 180 ? b : 360 - b;

    int level = ((ta + tb) / 2) * 255 / 180;

    if (level < 0)
        level = 0;

    if (level > 255)
        level = 255;

    /* Curved toward the troughs, so most of the keyboard is dark sea. */
    level = (level * level) / 255;

    /*
        Blue only, with green joining late for the pale crest.

        Red is never used. It existed to whiten the very top, and the moment
        anything went out of range it combined with green into yellow - which
        is the one colour a sea should not be. Without it the crest reaches a
        bright cyan-white instead, and nothing in the ramp can produce a warm
        colour at all, however the arithmetic behaves.
    */
    const int blue = 30 + (level * 225) / 255;
    const int green = level < 140 ? 0 : ((level - 140) * 235) / 115;

    result = ((uint32_t) green << 8) | (uint32_t) blue;
    }
    
    return result;
}

void LumiLink::compositeColours()
{

    const int degreeOn = degreeEnabled.load (std::memory_order_relaxed);
    const int degreeMix = degreeAlpha.load (std::memory_order_relaxed);
    const uint32_t degreeMask = degreeScale.load (std::memory_order_relaxed);
    /* Two roots, deliberately. The key anchors the scale; the played note anchors the
       colours. */
    const int root = degreeRoot.load (std::memory_order_relaxed);
    const int key = scaleRoot.load (std::memory_order_relaxed);

    /* Which slot each interval from the root uses: its position in the scale, or the
       last slot when it is not in the scale at all. */
    /*
        Which colour each pitch class gets, counted in scale steps from the played note.

        The old table was indexed by the interval from the played note while the mask is
        anchored to the key, so a note genuinely in the scale could land on an interval
        whose mask bit happened to be clear and come out chromatic. Play a different
        degree and a different subset of the key went dark, which is the gap you saw.

        Counting scale steps instead: walk upward from the played note through the notes
        of the key, and each one encountered is the next degree. So the played note is
        always degree one, the next note of the scale above it degree two, and every
        note of the key gets a colour whichever degree you are on.
    */
    int slotForPitch[12];

    if (degreeOn != 0)
    {
        const int rootInKey = (((root - key) % 12) + 12) % 12;

        for (int pc = 0; pc < 12; ++pc)
            slotForPitch[pc] = 7;

        int steps = 0;

        for (int offset = 0; offset < 12; ++offset)
        {
            const int inKey = (rootInKey + offset) % 12;

            if (((degreeMask >> inKey) & 1u) == 0u)
                continue;

            const int pc = ((key + inKey) % 12 + 12) % 12;
            slotForPitch[pc] = steps < 7 ? steps : 6;
            ++steps;
        }
    }

    const int tensionOn = tensionEnabled.load (std::memory_order_relaxed);
    const int tensionMix = tensionAlpha.load (std::memory_order_relaxed);
    const uint32_t homeTint = tensionHome.load (std::memory_order_relaxed);
    const uint32_t farTint = tensionFar.load (std::memory_order_relaxed);
    const int velocityOn = velocityEnabled.load (std::memory_order_relaxed);
    const bool wavesOn = wavesRunning();

    const bool sustainOn = sustainEnabled.load (std::memory_order_relaxed) != 0
                        && sustainHeld.load (std::memory_order_relaxed) != 0;
    const uint32_t sustainTint = sustainColour.load (std::memory_order_relaxed);
    const uint64_t sustainMask[2] = { sustainBits[0].load (std::memory_order_relaxed),
                                      sustainBits[1].load (std::memory_order_relaxed) };
    const int bendPathOn = bendPathEnabled.load (std::memory_order_relaxed);
    const uint32_t bendPathTint = bendPathColour.load (std::memory_order_relaxed);

    /*
        Where each bent note is heading: one path per held note, not one per channel.

        Under MPE each note has a channel to itself and the two amount to the same
        thing. With MPE off every note shares one channel and a bend applies to all of
        them at once - so tracking a single note per channel drew a path from whichever
        note arrived last and ignored the rest of the chord. Walking the held notes
        covers both cases.

        Bend arrives in cents, so the semitone target needs no pitch bend range here.
    */
    /*
        The bend tint, per note, because MPE gives every note its own channel.

        The firmware has one global incoming bend and uses it for any key the host lit,
        falling back to its own keyBend only for keys physically pressed on the LUMI. In
        an MPE track every note is host-lit, so one finger's bend tinted every key on the
        keyboard - which is the opposite of what MPE is for.

        Everything needed to do it properly is already here: which channel each sounding
        note is on, and what that channel's bend is. Computed per note and blended into
        the colour that goes out, so the device is told a finished picture and its own
        global bend is left switched off.
    */
    const bool zoneFilterOn = hasZone();
    const int zoneShift = getZoneOffset();
    const int bendGradOn = bendGradEnabled.load (std::memory_order_relaxed);
    const uint32_t bendGradTint = bendGradColour.load (std::memory_order_relaxed);
    int bendScale = bendFullScale.load (std::memory_order_relaxed);

    if (bendScale < 1)
        bendScale = 1;

    int bendAlphaFor[128];

    for (int n = 0; n < 128; ++n)
    {
        bendAlphaFor[n] = 0;

        if (bendGradOn == 0 || ! isNoteSounding (n))
            continue;

        int cents = bendCentsForNote (n);

        if (cents < 0)
            cents = -cents;

        int alpha = (cents * 255) / (bendScale * 100);

        if (alpha > 255)
            alpha = 255;

        bendAlphaFor[n] = alpha;
    }

    int pathTo[128];

    if (bendPathOn != 0)
    {
        for (int n = 0; n < 128; ++n)
        {
            pathTo[n] = -1;

            if (! isNoteSounding (n))
                continue;

            /* Through the same accessor as the tint, so the path and the colour agree
               about how far a note is bent however the host said it. */
            const int cents = bendCentsForNote (n);
            const int semis = cents >= 0 ? (cents + 50) / 100 : (cents - 50) / 100;

            if (semis == 0)
                continue;

            int target = n + semis;

            if (target < 0)
                target = 0;

            if (target > 127)
                target = 127;

            pathTo[n] = target;
        }
    }


    /* Steps around the circle of fifths from the root, folded so that six - the
       tritone - is as far as anything gets. */
    int fifthsDistance[12];

    for (int i = 0; i < 12; ++i)
    {
        int steps = 0;

        while (((steps * 7) % 12) != i && steps < 12)
            ++steps;

        fifthsDistance[i] = steps > 6 ? 12 - steps : steps;
    }

    const int glowOn = afterglowEnabled.load (std::memory_order_relaxed);
    const uint32_t glowTint = afterglowColour.load (std::memory_order_relaxed);
    const int haloOn = haloEnabled.load (std::memory_order_relaxed);
    const uint32_t haloTint = haloColour.load (std::memory_order_relaxed);
    const uint32_t pulseTint = pulseColour.load (std::memory_order_relaxed);

    /* Quantised so a slow decay does not rewrite every visible key on every tick; the
       diff then suppresses all the ticks that land on the same step. */
    const int pulseAlpha = (pulseLevel / 24) * 24;

    const uint64_t low64 = litBits[0].load (std::memory_order_relaxed)
                         | externalLitBits[0].load (std::memory_order_relaxed);
    const uint64_t high64 = litBits[1].load (std::memory_order_relaxed)
                          | externalLitBits[1].load (std::memory_order_relaxed);

    uint32_t haloMask = 0;

    if (haloOn != 0)
    {
        int held = 0;

        for (int n = 0; n < 128; ++n)
        {
            const uint64_t word = n < 64 ? low64 : high64;

            if (((word >> (n & 63)) & 1ull) != 0ull)
            {
                haloMask |= 1u << (((n % 12) + 12) % 12);
                ++held;
            }
        }

        /* A single note is not a chord, and haloing it would just smear one note over
           the whole keyboard. */
        if (held < 2)
            haloMask = 0;
    }

    for (int note = 0; note < 128; ++note)
    {
        /*
            The map is read through the offset, so a key shows the note it plays.

            `note` here is a key on the hardware. With a zone offset the colour for that
            key lives elsewhere in the map - which is the whole point: the map belongs to
            the sound, so moving the zone carries the colours with it instead of leaving
            them behind on the old keys.

            A key whose note falls off the end of MIDI shows nothing, because it plays
            nothing.
        */
        const int mapIndex = zoneFilterOn ? note + zoneShift : note;

        if (mapIndex < 0 || mapIndex > 127)
        {
            desiredColour[note].store (0, std::memory_order_relaxed);
            continue;
        }

        const uint32_t base = baseColour[mapIndex].load (std::memory_order_relaxed);

        /*
            Degrees and tension replace the painted map rather than sitting over it.

            Blended over a scale generated on the keyboard, two colour schemes fought
            for the same key and neither read clearly. Either of these switched on takes
            the keyboard over; switch both off and the painted map comes back untouched,
            because nothing here has altered it.
        */
        uint32_t result = (degreeOn != 0 || tensionOn != 0) ? 0u : base;

        /*
            Waves, when nothing has been played for a while.

            Two swells of different length running at different speeds, so the pattern
            never repeats in any obvious way - one alone reads as a metronome. White at
            the crest, falling through sea blue, to near dark in the troughs.

            It replaces everything rather than blending, because a screensaver over a
            colour map is neither. Every other effect below still runs and paints over
            it, which is what makes a note interrupt the waves visibly before the idle
            timer has even noticed.
        */
        if (wavesOn)
            result = screensaverColour (note, result);

        /* Tension sits below the degree map: one says which scale note this is, the
           other how far from home it is. */
        if (tensionOn != 0)
        {
            /* Membership is measured from the key, so the lit set is the same seven
               notes whichever of them you press. The colour is measured from the note
               played, which is what changes as you move around inside the key. */
            const int inKey = ((((note - key) % 12) + 12) % 12);
            const int interval = ((((note - root) % 12) + 12) % 12);

            if (((degreeMask >> inKey) & 1u) == 0u)
            {
                result = mixColour (result, 0x101014, tensionMix);
            }
            else
            {
            const int steps = fifthsDistance[interval];
            const uint32_t tint = mixColour (homeTint, farTint, (steps * 255) / 6);
            result = mixColour (result, tint, tensionMix);
            }
        }

        if (degreeOn != 0)
        {
            /* The table already knows: every pitch class in the key has a degree
               counted from the played note, and everything else is the chromatic slot. */
            const int pc = ((note % 12) + 12) % 12;
            result = mixColour (result,
                                degreeColour[slotForPitch[pc]].load (std::memory_order_relaxed),
                                degreeMix);
        }

        if (haloMask != 0 && ((haloMask >> (((note % 12) + 12) % 12)) & 1u) != 0u)
            result = mixColour (result, haloTint, 110);

        /* Velocity drives the brightness of a key while it is held, so the keyboard
           shows how hard you played rather than only what. It works on the key's own
           colour, which means the Incoming and Pressed colours have to be off for it
           to be visible - those are applied on the device and override everything. */
        if (velocityOn != 0)
        {
            const uint64_t word = note < 64 ? low64 : high64;

            if (((word >> (note & 63)) & 1ull) != 0ull)
            {
                const int v = noteVelocities[note].load (std::memory_order_relaxed);
                const int factor = 60 + (v * 195) / 127;
                uint32_t scaled = 0;

                for (int shift = 0; shift <= 16; shift += 8)
                {
                    int comp = (int) ((result >> shift) & 0xff);
                    comp = (comp * factor) / 255;
                    scaled |= ((uint32_t) comp) << shift;
                }

                result = scaled;
            }
        }

        if (glowOn != 0 && glowLevel[note] > 0)
            result = mixColour (result, glowTint, glowLevel[note]);

        /* The keys between a bent note and where it is heading, brightest at the
           target and fading back toward the key actually held - so the eye is drawn to
           where the note is going rather than where it started. */
        if (bendPathOn != 0)
        {
            for (int from = 0; from < 128; ++from)
            {
                const int to = pathTo[from];

                if (to < 0 || from == to)
                    continue;

                const int lowEnd = from < to ? from : to;
                const int highEnd = from < to ? to : from;

                if (note < lowEnd || note > highEnd)
                    continue;

                const int span = highEnd - lowEnd;
                const int reached = to > from ? note - from : from - note;
                const int alpha = span > 0 ? 60 + (reached * 195) / span : 255;

                result = mixColour (result, bendPathTint, alpha);
            }
        }



        /* Only the root pitch class pulses.
        
           Flashing every visible key meant rewriting 48 notes on every beat, four
           times a second at 120bpm - far more than the link carries, so the pulse
           arrived late and smeared into the next one. Pulsing the roots is four keys
           instead of forty-eight, it lands on time, and it reads better anyway: a
           metronome on the tonic rather than the whole board blinking. */
        if (pulseAlpha > 0 && (((note % 12) + 12) % 12) == root)
            result = mixColour (result, pulseTint, pulseAlpha);

        const uint32_t afterEffects = result;

        if (activeRipples > 0)
        {
            int bestAlpha = 0;
            uint32_t bestTint = 0;

            for (int i = 0; i < kMaxRipples; ++i)
            {
                if (rippleAge[i] < 0)
                    continue;

                int distance = note - rippleNote[i];

                if (distance < 0)
                    distance = -distance;

                /* How far the front has passed this key. Negative means it has not
                   reached it yet; beyond the trail length means it has gone by and the
                   key is back to its own colour. Brightest right at the front, fading
                   linearly over the keys behind. */
                const int behind = rippleAge[i] - distance * 16;

                if (behind < 0 || behind > rippleTrail16[i])
                    continue;

                const int shape = 255 - ((behind * 255) / rippleTrail16[i]);
                const int fade = 255 - (rippleAge[i] / 2);

                if (fade <= 0)
                    continue;

                const int alpha = (shape * fade * rippleLevel[i]) / 65025;

                if (alpha > bestAlpha)
                {
                    bestAlpha = alpha;
                    bestTint = rippleTint[i];
                }
            }

            /* Overlapping waves blend rather than the loudest one winning.

               Taking only the strongest meant two ripples crossing showed whichever
               happened to be brighter, and the crossing itself - the thing worth
               seeing - was invisible. Accumulating them makes an interference pattern
               where they meet. */
            if (bestAlpha > 0)
            {
                uint32_t mixed = afterEffects;
                int applied = 0;

                for (int i = 0; i < kMaxRipples; ++i)
                {
                    if (rippleAge[i] < 0)
                        continue;

                    int distance = note - rippleNote[i];

                    if (distance < 0)
                        distance = -distance;

                    const int behind = rippleAge[i] - distance * 16;

                    if (behind < 0 || behind > rippleTrail16[i])
                        continue;

                    const int shape = 255 - ((behind * 255) / rippleTrail16[i]);
                    const int fade = 255 - (rippleAge[i] / 2);

                    if (fade <= 0)
                        continue;

                    const int alpha = (shape * fade * rippleLevel[i]) / 65025;

                    if (alpha <= 0)
                        continue;

                    /* Each successive wave mixes into the result so far, so a crossing
                       carries both colours instead of one. */
                    mixed = mixColour (mixed, rippleTint[i], alpha);
                    ++applied;
                }

                result = applied > 0 ? mixed : mixColour (afterEffects, bestTint, bestAlpha);
            }
        }

        /*
            A note the pedal is holding, marked as such.

            It is lit either way - it is sounding, and the keyboard should say so. But
            whether a finger is on the key is a different fact from whether the note is
            ringing, and under the pedal those come apart. A tint over the sustained
            ones and not the pressed ones is the smallest thing that tells them apart,
            and it goes on last so nothing above overwrites it.
        */
        if (sustainOn && sustainMask[note >> 6] != 0
             && ((sustainMask[note >> 6] >> (note & 63)) & 1ull) != 0ull)
        {
            result = mixColour (result, sustainTint, 150);
        }

        if (bendAlphaFor[note] > 0)
            result = mixColour (result, bendGradTint, bendAlphaFor[note]);

        /*
            A key this instance does not own is never written with its colour, not even
            for an instant.

            The sender used to composite all 128 from its own map and then overwrite the
            keys belonging to others a moment later. Correct by the time anything was
            sent, but the editor reads this table on its own thread sixty times a second
            and kept catching the gap - so keys outside every range flickered with the
            sender's own colours at random.

            Writing black straight away costs nothing and removes the window entirely.
            The assembly still fills in what the other members published; it just no
            longer has to undo something first.
        */
        if (zoneFilterOn && ! noteInZone (note))
            result = 0;

        desiredColour[note].store (result, std::memory_order_relaxed);
    }
}

void LumiLink::publishLitBits (uint64_t low, uint64_t high)
{
    litBits[0].store (low, std::memory_order_release);
    litBits[1].store (high, std::memory_order_release);
}

/* Which of the lit notes are lit because of the pedal rather than a finger. Published
   from the audio thread with the lit bits, since that is where both are known. */
void LumiLink::publishSustainBits (uint64_t low, uint64_t high)
{
    sustainBits[0].store (low, std::memory_order_release);
    sustainBits[1].store (high, std::memory_order_release);
}

/*
    A note from the listen port, filtered the same way one from the host is.

    Without the zone test a shared chain would light every arpeggiated note on every
    instance, which is the thing zones exist to stop - and the notes arriving here have
    not been through handleEvent, so nothing else has filtered them.
*/
void LumiLink::externalNote (int note, bool on)
{
    if (! noteInZone (note))
        return;

    /*
        Lit, and nothing else.

        These notes are watched, not received. They never reach the plugin's output -
        the listen port is a separate input and nothing from it is ever pushed to the
        event stream - and they deliberately do not claim the keyboard, start a ripple,
        trigger afterglow or touch the sustain bookkeeping either.

        An arpeggiator can run for minutes without anybody touching the track, and
        letting that take the device would mean whichever track had an arp going quietly
        won every argument about who owns the keyboard. Showing is not playing.
    */
    setExternalLit (note, on);
}

void LumiLink::setExternalLit (int note, bool isLit)
{
    if (note < 0 || note > 127)
        return;

    const int word = note >> 6;
    const uint64_t mask = 1ull << (note & 63);
    uint64_t current = externalLitBits[word].load (std::memory_order_relaxed);

    for (;;)
    {
        const uint64_t updated = isLit ? (current | mask) : (current & ~mask);

        if (externalLitBits[word].compare_exchange_weak (current, updated,
                                                         std::memory_order_release,
                                                         std::memory_order_relaxed))
            return;
    }
}

void LumiLink::clearExternalLit()
{
    externalLitBits[0].store (0, std::memory_order_release);
    externalLitBits[1].store (0, std::memory_order_release);
}

void LumiLink::setHighlightEnabled (bool enabled)
{
    highlightEnabled.store (enabled ? 1 : 0, std::memory_order_relaxed);
}

void LumiLink::setHighlightColour (uint32_t rgb)
{
    highlightColour.store (rgb & 0x00ffffffu, std::memory_order_relaxed);
}

uint32_t LumiLink::getHighlightColour() const
{
    return highlightColour.load (std::memory_order_relaxed);
}

void LumiLink::setPressEnabled (bool enabled)
{
    pressEnabled.store (enabled ? 1 : 0, std::memory_order_relaxed);
}

void LumiLink::setPressColour (uint32_t rgb)
{
    pressColour.store (rgb & 0x00ffffffu, std::memory_order_relaxed);
}

uint32_t LumiLink::getPressColour() const
{
    return pressColour.load (std::memory_order_relaxed);
}



void LumiLink::setOctave (int value)
{
    if (value < -3)
        value = -3;

    if (value > 3)
        value = 3;

    octave.store (value, std::memory_order_relaxed);
}

void LumiLink::setPressureGradEnabled (bool enabled)
{
    pressureGradEnabled.store (enabled ? 1 : 0, std::memory_order_relaxed);
}

void LumiLink::setPressureGradColour (uint32_t rgb)
{
    pressureGradColour.store (rgb & 0x00ffffffu, std::memory_order_relaxed);
}

uint32_t LumiLink::getPressureGradColour() const
{
    return pressureGradColour.load (std::memory_order_relaxed);
}

void LumiLink::setBendGradEnabled (bool enabled)
{
    bendGradEnabled.store (enabled ? 1 : 0, std::memory_order_relaxed);
}

void LumiLink::setBendGradColour (uint32_t rgb)
{
    bendGradColour.store (rgb & 0x00ffffffu, std::memory_order_relaxed);
}

uint32_t LumiLink::getBendGradColour() const
{
    return bendGradColour.load (std::memory_order_relaxed);
}

/*
    The device mirrors its keybed settings out on CC 30/31 whenever they change, from
    Dashboard or from its own buttons. Octave is the one that would otherwise loop:
    marking it as already sent stops the plugin echoing a value the device just told
    it, which would fight anyone turning the knob.
*/
void LumiLink::applyMirroredConfig (int item, int value)
{
    if (item < 0 || item > 127)
        return;

    deviceConfig[item].store (value, std::memory_order_relaxed);

    if (item == kConfigOctave)
    {
        octave.store (value, std::memory_order_relaxed);

        /* Linked: push the value a block just reported back out to all of them, so
           they follow each other over MIDI rather than relying only on the
           block-to-block config sync. Independent: record it and say nothing, which
           is what lets two blocks sit in different registers. */
        sentOctave = linkOctaves.load (std::memory_order_relaxed) != 0 ? -1000 : value;
    }
}

void LumiLink::setClusterWidth (int blocks)
{
    if (blocks < 1)
        blocks = 1;

    if (blocks > 5)
        blocks = 5;

    blockCount.store (blocks, std::memory_order_relaxed);

    const int low = windowLow.load (std::memory_order_relaxed);
    int high = low + blocks * 24 - 1;

    if (high > 127)
        high = 127;

    windowHigh.store (high, std::memory_order_relaxed);
    recomputeSendRange();
}

void LumiLink::setWindowBase (int note)
{
    if (note < 0 || note > 127)
        return;

    const int span = windowHigh.load (std::memory_order_relaxed)
                   - windowLow.load (std::memory_order_relaxed);

    windowLow.store (note, std::memory_order_relaxed);

    int high = note + span;

    if (high > 127)
        high = 127;

    windowHigh.store (high, std::memory_order_relaxed);
    recomputeSendRange();
}

int LumiLink::getWindowLow() const
{
    return windowLow.load (std::memory_order_relaxed);
}

int LumiLink::getWindowHigh() const
{
    return windowHigh.load (std::memory_order_relaxed);
}

/* Zero until a device reports, which is itself worth showing: it means either nothing
   is connected or the program on it is not the one that reports. */
void LumiLink::setBlockRange (int position, int lowNote)
{
    if (position < 0 || position > 4 || lowNote < 0 || lowNote > 127)
        return;

    blockLow[position].store (lowNote, std::memory_order_relaxed);

    if (position + 1 > blockCount.load (std::memory_order_relaxed))
        blockCount.store (position + 1, std::memory_order_relaxed);

    recomputeSendRange();

    /*
        Deliberately nothing else.

        Two attempts to be cleverer here both broke something that worked. Widening the
        sent window to reach a reported block ratchets it wider and never narrows;
        deciding visibility per block hides any block that has not reported, and in a
        chain not every block has a route to report. The reported position is recorded
        for the editor to draw brackets with, and the sending window is left alone.
    */
}

void LumiLink::recomputeSendRange()
{
    int blocks = blockCount.load (std::memory_order_relaxed);

    if (blocks < 1)
    {
        /* Nothing known yet: send everything rather than nothing. */
        sendLow.store (0, std::memory_order_relaxed);
        sendHigh.store (127, std::memory_order_relaxed);
        return;
    }

    if (blocks > 5)
        blocks = 5;

    const int base = windowLow.load (std::memory_order_relaxed);
    int lo = 127;
    int hi = 0;

    for (int b = 0; b < blocks; ++b)
    {
        int low = blockLow[b].load (std::memory_order_relaxed);

        /* A block that has not reported is assumed to sit where a chain would put it.
           Never assumed absent - that is what made one go dark. */
        if (low < 0)
            low = base + b * 24;

        if (low < lo)
            lo = low;

        if (low + 23 > hi)
            hi = low + 23;
    }

    sendLow.store (lo < 0 ? 0 : lo, std::memory_order_relaxed);
    sendHigh.store (hi > 127 ? 127 : hi, std::memory_order_relaxed);
}

bool LumiLink::isVisible (int note) const
{
    if (note < 0 || note > 127)
        return false;

    const int blocks = blockCount.load (std::memory_order_relaxed);

    if (blocks < 1)
        return true;

    const int base = windowLow.load (std::memory_order_relaxed);

    for (int b = 0; b < blocks && b < 5; ++b)
    {
        /*
            Where this block says it is, or where it would be if it were laid end to
            end with the others.

            The fallback matters more than it looks. In a chain only the master has a
            route to the host - a second block's reports have nowhere to go - so it
            will usually never report at all. Treating that silence as "not visible"
            turned the whole second block dark, which is worse than the contiguous
            guess this replaced: that was wrong about where a block sat, but it never
            hid one.

            So a block that has reported is believed, and one that has not is assumed
            to follow the block before it, which is exactly right whenever the octaves
            are linked.
        */
        const int reported = blockLow[b].load (std::memory_order_relaxed);
        const int low = reported >= 0 ? reported : base + b * 24;

        if (note >= low && note <= low + 23)
            return true;
    }

    return false;
}

int LumiLink::getBlockLow (int position) const
{
    if (position < 0 || position > 4)
        return -1;

    return blockLow[position].load (std::memory_order_relaxed);
}

bool LumiLink::isNoteSounding (int note) const
{
    if (note < 0 || note > 127)
        return false;

    const uint64_t word = litBits[note >> 6].load (std::memory_order_relaxed)
                        | externalLitBits[note >> 6].load (std::memory_order_relaxed);

    return ((word >> (note & 63)) & 1ull) != 0ull;
}

void LumiLink::setFollowSource (KeybedCapture *source, int anchorNote)
{
    followAnchor.store (anchorNote, std::memory_order_relaxed);
    followSource.store (source, std::memory_order_release);
}

bool LumiLink::isFollowing() const
{
    return followSource.load (std::memory_order_relaxed) != nullptr;
}

void LumiLink::stopFollowing()
{
    followSource.store (nullptr, std::memory_order_release);
}

void LumiLink::countMessageIn (int cc)
{
    messagesIn.fetch_add (1, std::memory_order_relaxed);
    lastCcIn.store (cc, std::memory_order_relaxed);
}

int LumiLink::getBlockCount() const
{
    return blockCount.load (std::memory_order_relaxed);
}

int LumiLink::getDeviceConfig (int item) const
{
    if (item < 0 || item > 127)
        return kConfigUnknown;

    return deviceConfig[item].load (std::memory_order_relaxed);
}

void LumiLink::writeDeviceConfig (int item, int value)
{
    if (item < 0 || item > 127)
        return;

    deviceConfig[item].store (value, std::memory_order_relaxed);
    pendingConfigWrite[item].store (value, std::memory_order_release);
}

void LumiLink::flushConfigWrites()
{
    for (int item = 0; item < 128; ++item)
    {
        const int value = pendingConfigWrite[item].exchange (kConfigUnknown,
                                                            std::memory_order_acquire);

        if (value == kConfigUnknown)
            continue;

        int encoded = isSignedConfig (item) ? value + 64 : value;

        if (encoded < 0)
            encoded = 0;

        if (encoded > 127)
            encoded = 127;

        sendCC (kCcConfigWriteItem, (uint8_t) item);
        sendCC (kCcConfigWriteValue, (uint8_t) encoded);
    }
}

/*
    How much bend counts as full colour, in units of 64. A key deflects by Key Pitch
    Bend Amount against Pitch Bend Range - one semitone out of 48 is about 170 of the
    14-bit range - so measuring against the whole range leaves the gradient invisible.
    The default of 2 puts full colour at 128 units, which a one-semitone key bend
    against the factory range of 48 comfortably exceeds - so a full sideways slide
    saturates rather than stopping two thirds of the way.
*/
void LumiLink::setBendFullScale (int steps)
{
    if (steps < 1)
        steps = 1;

    if (steps > 127)
        steps = 127;

    bendFullScale.store (steps, std::memory_order_relaxed);
}

int LumiLink::getBendFullScale() const
{
    return bendFullScale.load (std::memory_order_relaxed);
}

void LumiLink::setEnablePitchBend (bool on)
{
    enablePitchBend.store (on ? 1 : 0, std::memory_order_relaxed);
}

bool LumiLink::getEnablePitchBend() const
{
    return enablePitchBend.load (std::memory_order_relaxed) != 0;
}

void LumiLink::setEnablePressure (bool on)
{
    enablePressure.store (on ? 1 : 0, std::memory_order_relaxed);
}

bool LumiLink::getEnablePressure() const
{
    return enablePressure.load (std::memory_order_relaxed) != 0;
}

void LumiLink::setSendRate (int rate)
{
    if (rate < 0) rate = 0;
    if (rate > 4) rate = 4;
    sendRate.store (rate, std::memory_order_relaxed);
}

int LumiLink::getSendRate() const
{
    return sendRate.load (std::memory_order_relaxed);
}

void LumiLink::setLinkOctaves (bool on)
{
    linkOctaves.store (on ? 1 : 0, std::memory_order_relaxed);
}

bool LumiLink::getLinkOctaves() const
{
    return linkOctaves.load (std::memory_order_relaxed) != 0;
}

void LumiLink::setBrightness (double normalised)
{
    brightness.store (clamp7 ((int) (normalised * 127.0 + 0.5)), std::memory_order_relaxed);
}

void LumiLink::setUnlitLevel (double normalised)
{
    unlitLevel.store (clamp7 ((int) (normalised * 127.0 + 0.5)), std::memory_order_relaxed);
}

void LumiLink::setDisplayOffset (double semitones)
{
    int value = (int) (semitones >= 0.0 ? semitones + 0.5 : semitones - 0.5);

    if (value < -64)
        value = -64;

    if (value > 63)
        value = 63;

    displayOffset.store (value, std::memory_order_relaxed);
}

void LumiLink::setFoldOctaves (bool shouldFold)
{
    foldOctaves.store (shouldFold ? 1 : 0, std::memory_order_relaxed);
}

/* Called from the audio thread when notes arrive, so it must not do the claim itself -
   it only raises a flag the worker acts on. */
void LumiLink::noteActivity()
{
    /* Any note at all ends the screensaver, before the key even lights. */
    idleMs.store (0, std::memory_order_relaxed);

    activityPending.store (1, std::memory_order_release);
}

void LumiLink::claimDevice()
{
    claim.claim();
    activityPending.store (0, std::memory_order_relaxed);
}

void LumiLink::setHoldDevice (bool hold)
{
    holdDevice.store (hold ? 1 : 0, std::memory_order_relaxed);

    if (hold)
        claim.hold();
    else
        claim.unhold();
}

bool LumiLink::getHoldDevice() const
{
    return holdDevice.load (std::memory_order_relaxed) != 0;
}

bool LumiLink::hasDevice() const
{
    return claim.isOwner();
}

bool LumiLink::heldElsewhere() const
{
    return claim.heldBySomeoneElse();
}

bool LumiLink::isConnected() const
{
    return connected.load (std::memory_order_relaxed);
}

bool LumiLink::isNoteLit (int note) const
{
    if (note < 0 || note > 127)
        return false;

    const int word = note >> 6;
    const uint64_t bits = litBits[word].load (std::memory_order_relaxed)
                        | externalLitBits[word].load (std::memory_order_relaxed);

    return ((bits >> (note & 63)) & 1ull) != 0ull;
}

/* Sleeps in slices so a stop is noticed within a tick rather than at the end of a
   long wait. */
void LumiLink::interruptibleSleep (int totalMs)
{
    while (totalMs > 0 && running.load (std::memory_order_acquire))
    {
        const int slice = totalMs > 20 ? 20 : totalMs;
        std::this_thread::sleep_for (std::chrono::milliseconds (slice));
        totalMs -= slice;
    }
}

void LumiLink::run()
{
#if defined (_WIN32)
    /*
        Ask Windows for a millisecond timer while this thread runs.

        Without it sleep_for(4ms) returns after about 15.6, because that is the default
        scheduler tick, and every animation advanced by a fixed amount per tick - so
        they all ran at roughly a quarter speed. The editor raises the resolution for
        its own repaint, so this behaved differently with the window open than closed,
        which is a confusing way to meet a bug. Animation is scaled by elapsed time now
        and correct either way, but a finer tick is still smoother and sends less in
        each burst.
    */
    timeBeginPeriod (1);
#endif

    while (running.load (std::memory_order_acquire))
    {
        /* Ownership first. An instance that has lost the claim closes its port and
           sends nothing - on Windows it could not open the port anyway, and elsewhere
           two senders on one port produce flicker. */
        if (activityPending.exchange (0, std::memory_order_acquire) != 0
            || holdDevice.load (std::memory_order_relaxed) != 0)
            claim.claim();

        /* Renew the lease every tick. An owner that stops saying so is treated as gone
           after a few seconds, which is what lets a crashed host be recovered from -
           and it means a live owner has to keep speaking up. */
        if (claim.isOwner())
            claim.claim();

        /* And renew the hold, if this instance is the one holding. Both leases are
           renewed from the same tick, so an instance that stops running loses both. */
        if (holdDevice.load (std::memory_order_relaxed) != 0)
            claim.renewHold();

        if (! claim.isOwner())
        {
            /*
                Hand over quietly.

                This used to clear the keyboard and wipe the shared record of what was
                on it, so the instance taking over had to re-upload all 128 notes -
                which is the exact cost that sharing the device state existed to avoid,
                paid at the one moment it was supposed to save it. Switching tracks, or
                reopening an editor, meant watching the map redraw from nothing.

                The port is released and nothing else. Whatever is on the keys stays
                there, the shared record still describes it, and the next owner sends
                only what differs - usually a handful of keys.
            */
            if (backend->out && backend->out->isPortOpen())
            {
                backend->out->closePort();
                invalidateCache();
            }

            wasOwner = false;
            connected.store (false, std::memory_order_relaxed);
            interruptibleSleep (100);
            continue;
        }

        /* Just taken the keyboard over. Start from what the previous owner left on it
           rather than from nothing, so only the keys that actually differ get sent. */
        if (! wasOwner)
        {
            wasOwner = true;

            claim.adoptDeviceState (sentColour);
        }

        if (backend->in && ! backend->in->isPortOpen())
            ensureInputConnection();

        /* How long the last tick really took, clamped so a stall does not make
           everything jump. */
        const auto nowTick = std::chrono::steady_clock::now();
        int elapsedMs = (int) std::chrono::duration_cast<std::chrono::milliseconds> (
                            nowTick - lastTick).count();
        lastTick = nowTick;

        if (elapsedMs < 1)
            elapsedMs = 1;

        if (elapsedMs > 50)
            elapsedMs = 50;

        /*
            A member that is not sending does its work without a port, and before the
            connection gate rather than after it.

            Everything below this point needed an open output, which is right for an
            instance that sends and wrong for one that does not: a non-sender closes its
            port, so it would fail the gate, loop on a reconnect it does not want, and
            never publish its zone at all. The sender would then see nothing but its own
            range - and on a platform where several processes can open the same port,
            every member would be fighting for it as well.

            So the colours are composited and published here, with no device involved,
            and the rest of the loop is left to whoever is actually sending.
        */
        if (hasZone() && ! claim.isSender())
        {
            if (backend->out && backend->out->isPortOpen())
            {
                backend->out->closePort();
                invalidateCache();
            }

            claim.renewZone();
            wavePhase += elapsedMs;
            advanceGlow (elapsedMs);
            advanceRipples (elapsedMs);
            compositeColours();

            const int low = zoneLow.load (std::memory_order_relaxed);
            const int high = zoneHigh.load (std::memory_order_relaxed);

            for (int note = low; note <= high && note < 128; ++note)
                if (note >= 0)
                    claim.publishZoneColour (note,
                        desiredColour[note].load (std::memory_order_relaxed));

            std::this_thread::sleep_for (std::chrono::milliseconds (4));
            continue;
        }

        if (! ensureConnection())
        {
            /* Half a second between attempts, but woken at once by a stop. Sleeping
               the whole interval meant closing a project could wait on a reconnect
               timer that was never going to succeed. */
            interruptibleSleep (500);
            continue;
        }

        /*
            Nothing here assumes a message arrived. Only the master of a chained cluster
            receives MIDI directly - the others get it relayed, over a link that can drop
            or stall and re-establish itself. A design that sends each setting once and
            trusts it leaves a slave holding stale values with no way back.

            So the globals are re-asserted about once a second, and the colour table is
            continuously rewalked in the background. Anything lost heals on the next pass
            instead of persisting until the user happens to change that note again.
        */
        if (--globalRefreshCountdown <= 0)
        {
            globalRefreshCountdown = 250;
            sentBrightness = -1;
            sentUnlitLevel = -1;
            sentDisplayOffset = -1000;
            sentFoldOctaves = -1;
            sentHighlightEnabled = -1;
            sentHighlightColour = 0xffffffffu;
            sentPressEnabled = -1;
            sentPressColour = 0xffffffffu;

            /* Octave is deliberately NOT re-asserted. The device owns it: the buttons
               write config item 4, it goes round the cluster by setRemoteConfig, and
               the change is mirrored back here. Resending our copy once a second means
               that if a button is pressed just before the tick, the stale value goes
               out and the keyboard jumps back to where it was. */
    sentPressureGradEnabled = -1;
    sentPressureGradColour = 0xffffffffu;
    sentBendGradEnabled = -1;
    sentBendGradColour = 0xffffffffu;
    sentBendFullScale = -1;
    sentEnablePitchBend = -1;
    sentEnablePressure = -1;
    sentLinkOctaves = -1;
        }

        lastTickMs = elapsedMs;

        /*
            Idle time, which a held key is not.

            This was reset only when a note arrived, so holding a chord sent one note-on
            and then nothing - and the clock ran on underneath it until the screensaver
            came up over keys that were still down. Idle has to mean nothing is
            happening, not nothing has started recently.

            Anything lit counts, including notes the pedal is holding and notes seen on
            the listen port: an arpeggiator running is the music playing, whoever is
            touching the keyboard.
        */
        if (anyNoteSounding())
            idleMs.store (0, std::memory_order_relaxed);
        else
            idleMs.fetch_add (elapsedMs, std::memory_order_relaxed);

        /*
            Re-read the followed plugin's keyboard, whether or not the editor is open.

            Roughly five times a second: fast enough that a keyswitch change appears at
            once, slow enough that a few hundred pixel reads cost nothing. If the window
            has gone, it is looked for again by title - a plugin that is closed and
            reopened gets a new handle, and following by handle alone would stop for
            good.
        */
        if (++followTick >= 50)
        {
            followTick = 0;
            KeybedCapture *source = followSource.load (std::memory_order_acquire);

            if (source != nullptr)
                if (! source->refresh (*this, followAnchor.load (std::memory_order_relaxed)))
                    source->refindWindow();
        }
        wavePhase += elapsedMs;

        advanceGlow (elapsedMs);
        advanceRipples (elapsedMs);
        compositeColours();

        /*
            A zoned instance publishes its range and, unless it is the one sending,
            stops there.

            Both halves run every tick rather than only when something changed: the
            table is the only description of the chain anyone has, and a member that
            publishes lazily leaves the sender drawing a stale zone the moment it stops
            being played. A hundred and twenty-eight relaxed stores cost nothing next to
            the MIDI they replace.
        */
        if (hasZone())
        {
            claim.renewZone();

            /*
                The sender takes every wave the chain has posted, whoever threw it.

                Taken rather than read, so a handover cannot leave two instances both
                drawing the same wave. Each arrives with the speed, trail and colour its
                originator resolved, which is what lets a ripple leave one member's
                range carrying that member's look rather than the sender's.
            */
            if (claim.isSender())
            {
                for (int slot = 0; slot < kEffectSlots; ++slot)
                {
                    uint64_t packed = 0;

                    if (! claim.takeEffect (slot, packed))
                        continue;

                    int note = 0, level = 0, speed = 0, trail = 0, kind = 0;
                    uint32_t tint = 0;
                    unpackEffect (packed, note, level, tint, speed, trail, kind);

                    if (kind == kEffectPulse)
                    {
                        pulseColour.store (tint, std::memory_order_relaxed);
                        pendingBeatLevel.store (level, std::memory_order_release);
                    }
                    else
                    {
                        startChainRipple (note, level, tint, speed, trail);
                    }
                }
            }

            const int low = zoneLow.load (std::memory_order_relaxed);
            const int high = zoneHigh.load (std::memory_order_relaxed);

            for (int note = low; note <= high && note < 128; ++note)
                if (note >= 0)
                    claim.publishZoneColour (note,
                        desiredColour[note].load (std::memory_order_relaxed));

            /*
                The sender draws the chain, and nothing outside it.

                It starts from black rather than from its own map. Its own composite
                covers all 128 notes - every instance's does, because the map is 128
                long whatever range it owns - so seeding the assembly with it meant the
                sender's own colours showed everywhere no other member had claimed, and
                on keys it had given away. The sender appeared to ignore its own range
                and light the whole keyboard, which is exactly what it looked like.

                Black for a key nobody owns is the honest answer: no instance has said
                what that key should be.
            */
            uint32_t assembled[128];
            bool owned[128];

            for (int note = 0; note < 128; ++note)
            {
                assembled[note] = 0;
                owned[note] = false;
            }

            const int myLow = zoneLow.load (std::memory_order_relaxed);
            const int myHigh = zoneHigh.load (std::memory_order_relaxed);

            for (int note = myLow; note <= myHigh && note < 128; ++note)
                if (note >= 0)
                {
                    assembled[note] = desiredColour[note].load (std::memory_order_relaxed);
                    owned[note] = true;
                }

            for (int i = 0; i < kMaxZones; ++i)
            {
                ZoneInfo info;

                if (! claim.zoneAt (i, info) || info.owner == claim.selfId())
                    continue;

                for (int note = info.low; note <= info.high && note < 128; ++note)
                    if (note >= 0)
                        owned[note] = true;
            }

            uint32_t fromChain[128];

            for (int note = 0; note < 128; ++note)
                fromChain[note] = assembled[note];

            claim.readZoneColours (fromChain);

            for (int note = 0; note < 128; ++note)
                desiredColour[note].store (owned[note] ? fromChain[note] : 0u,
                                           std::memory_order_relaxed);

            /*
                Waves go on last, across the whole chain.

                They cannot be composited with the rest: each member's zone is painted
                by that member and arrives here finished, and a wave that was already in
                it would stop at the boundary. Laid over the assembled keyboard instead,
                a wave crosses from one range into the next without either member
                knowing it happened - and the sender's own waves cross the same way,
                because by this point its range is just another part of the picture.

                Only waves. Everything else is a property of the keys it touches and is
                already in the zone that owns them.
            */
            /*
                The idle pattern goes on before the waves, over the whole chain.

                Members published their maps without it, so this is the only place it
                exists - and applying it here rather than per zone is what lets a drift
                or a rainfall splash cross a boundary with nothing to show where one
                instance ends and the next begins.
            */
            if (wavesRunning())
                for (int note = 0; note < 128; ++note)
                    if (owned[note])
                        desiredColour[note].store (
                            screensaverColour (note,
                                desiredColour[note].load (std::memory_order_relaxed)),
                            std::memory_order_relaxed);

            overlayRipples();
        }

        flushGlobals();
        flushConfigWrites();
        flushKeys();
        flushColours();
        std::this_thread::sleep_for (std::chrono::milliseconds (4));
    }

    shutdownDevice();
}

void LumiLink::getAvailablePorts (std::vector<std::string> &destination) const
{
    std::lock_guard<std::mutex> lock (portsMutex);
    destination = availablePorts;
}

std::string LumiLink::getRequestedPort() const
{
    std::lock_guard<std::mutex> lock (portsMutex);
    return requestedPort;
}

std::string LumiLink::getActivePort() const
{
    std::lock_guard<std::mutex> lock (portsMutex);
    return activePort;
}

void LumiLink::setRequestedPort (const std::string &name)
{
    {
        std::lock_guard<std::mutex> lock (portsMutex);

        if (requestedPort == name)
            return;

        requestedPort = name;
    }

    selectionChanged.store (true, std::memory_order_release);
}

bool LumiLink::matchesRequest (const std::string &portName, const std::string &request) const
{
    /*
        Automatic matching, case insensitively, against every name this hardware goes by.

        It looked only for "LUMI", which misses a Piano M announcing itself as "Piano M"
        or "ROLI Piano" - and that is the more common device now. It was also case
        sensitive and tried two spellings by hand, which is a sign of a rule that wants
        writing down properly.

        Automatic is a convenience and stops being right the moment you own two of these;
        the port list beside it is the answer then, and the choice is remembered by name.
    */
    if (request.empty())
    {
        std::string lower = portName;

        for (char &ch : lower)
            ch = (char) std::tolower ((unsigned char) ch);

        static const char *known[] = { "lumi", "piano m", "roli piano", "roli" };

        for (const char *name : known)
            if (lower.find (name) != std::string::npos)
                return true;

        return false;
    }

    if (portName == request)
        return true;

    return portName.find (request) != std::string::npos;
}

void LumiLink::refreshPorts()
{
    if (! backend->out)
        return;

    std::vector<std::string> names;

    try
    {
        const unsigned int portCount = backend->out->getPortCount();

        for (unsigned int i = 0; i < portCount; ++i)
            names.push_back (backend->out->getPortName (i));
    }
    catch (RtMidiError &)
    {
        return;
    }

    std::lock_guard<std::mutex> lock (portsMutex);
    availablePorts.swap (names);
}

/* Only a genuine reconnect throws away the shared record: the device may have been
   reflashed or power-cycled while nobody was talking to it, and then nothing anyone
   believed about it is true. An ownership change is not that. */
void LumiLink::deviceMayHaveChanged()
{
    claim.forgetDeviceState();
}

/*
    Listens to the keyboard itself.

    Everything the device reports - the octave it has moved to, how many blocks are in
    the chain, what each one is showing - travels back on the same MIDI channel, and
    until now the plugin had no way to hear any of it. Its only input was a virtual
    port, and WinMM has no virtual ports, so on Windows that input simply did not
    exist. The device has been talking to nobody, which is why the editor said no block
    was reporting and why the octave and the chain layout never behaved.

    Opening the keyboard's own input port, matched by the same name as the output,
    fixes that without the host needing to route anything.
*/
std::vector<std::string> LumiLink::inputPortNames() const
{
    std::vector<std::string> names;

    if (! backend->in)
        return names;

    try
    {
        const unsigned int count = backend->in->getPortCount();

        for (unsigned int i = 0; i < count; ++i)
            names.push_back (backend->in->getPortName (i));
    }
    catch (RtMidiError &)
    {
    }

    return names;
}

void LumiLink::setInputPort (const std::string &name)
{
    std::lock_guard<std::mutex> lock (inputMutex);
    requestedInput = name;
    openedInput.clear();

    if (backend->in && backend->in->isPortOpen())
        backend->in->closePort();

    externalInput.store (false, std::memory_order_relaxed);
}

std::string LumiLink::activeInputPort() const
{
    std::lock_guard<std::mutex> lock (inputMutex);
    return openedInput;
}

void LumiLink::ensureInputConnection()
{
    if (! backend->in || backend->in->isPortOpen())
        return;

    std::string request;

    {
        std::lock_guard<std::mutex> lock (inputMutex);
        request = requestedInput;
    }

    std::string fallback;

    {
        std::lock_guard<std::mutex> lock (portsMutex);
        fallback = requestedPort;
    }

    try
    {
        const unsigned int count = backend->in->getPortCount();

        for (unsigned int i = 0; i < count; ++i)
        {
            const std::string name = backend->in->getPortName (i);

            /* An explicit choice wins; otherwise fall back to whatever matches the
               output port's name, which is right often enough to need no thought. */
            const bool wanted = request.empty() ? matchesRequest (name, fallback)
                                                : name == request;

            if (! wanted)
                continue;

            backend->in->openPort (i, "LumiPaint in");
            externalInput.store (true, std::memory_order_relaxed);

            std::lock_guard<std::mutex> lock (inputMutex);
            openedInput = name;
            return;
        }
    }
    catch (RtMidiError &)
    {
        /* Almost always the port already being open elsewhere: on Windows a MIDI input
           belongs to one application at a time, so a host holding the keyboard as a
           controller keeps everyone else out. */
    }
}

bool LumiLink::ensureConnection()
{
    if (selectionChanged.exchange (false, std::memory_order_acquire))
    {
        if (backend->out && backend->out->isPortOpen())
            backend->out->closePort();

        connected.store (false, std::memory_order_relaxed);

        std::lock_guard<std::mutex> lock (portsMutex);
        activePort.clear();
    }

    if (backend->out && backend->out->isPortOpen())
    {
        if (--refreshCountdown <= 0)
        {
            refreshCountdown = 250;
            refreshPorts();
        }

        return true;
    }

    connected.store (false, std::memory_order_relaxed);

    try
    {
        if (! backend->out)
            backend->out.reset (new RtMidiOut (RtMidi::UNSPECIFIED, "LumiPaint"));

        refreshPorts();

        std::vector<std::string> names;
        std::string request;

        {
            std::lock_guard<std::mutex> lock (portsMutex);
            names = availablePorts;
            request = requestedPort;
        }

        for (size_t i = 0; i < names.size(); ++i)
        {
            if (! matchesRequest (names[i], request))
                continue;

            backend->out->openPort ((unsigned int) i, "LumiPaint out");

            /* The return path matters as much as the outgoing one: without it the
               device's reports go nowhere and half the plugin runs blind. */
            ensureInputConnection();

            invalidateCache();

            /* A port that had to be opened rather than handed over means the device
               may have been reflashed or power-cycled since anyone last spoke to it,
               so nobody's record of what is on it can be trusted. */
            deviceMayHaveChanged();
            connected.store (true, std::memory_order_relaxed);
            refreshCountdown = 250;

            std::lock_guard<std::mutex> lock (portsMutex);
            activePort = names[i];
            return true;
        }
    }
    catch (RtMidiError &)
    {
        backend->out.reset();
    }

    return false;
}

void LumiLink::invalidateCache()
{
    for (int i = 0; i < 128; ++i)
        sentColour[i] = kUnsentColour;

    sentLitBits[0] = 0;
    sentLitBits[1] = 0;
    sentBrightness = -1;
    sentUnlitLevel = -1;
    sentDisplayOffset = -1000;
    sentFoldOctaves = -1;
    sentHighlightEnabled = -1;
    sentHighlightColour = 0xffffffffu;
    sentPressEnabled = -1;
    sentPressColour = 0xffffffffu;

    /*
        Octave is deliberately not invalidated.

        Everything else here is re-sent after a reconnect or a handover because the
        plugin is the authority for it. The octave is the one setting the device owns:
        its buttons set it and it reports the result. Marking it unsent made the plugin
        broadcast its own stored value to every block on the next tick - which forced
        both blocks to the same octave whatever the link toggle said, and is why
        unlinking never freed them.

        Keeping it level with what the device last reported means nothing goes out
        until the user actually moves the control.
    */
    sentOctave = octave.load (std::memory_order_relaxed);

    sentPressureGradEnabled = -1;
    sentPressureGradColour = 0xffffffffu;
    sentBendGradEnabled = -1;
    sentBendGradColour = 0xffffffffu;
    sentBendFullScale = -1;
    sentEnablePitchBend = -1;
    sentEnablePressure = -1;
    sentLinkOctaves = -1;
    deviceSelectedNote = -1;
    sendCC (kCcCommand, kCmdAllKeysOff);
}

bool LumiLink::sendRaw (unsigned char *bytes, size_t length)
{
    if (! backend->out || ! backend->out->isPortOpen())
        return false;

    try
    {
        backend->out->sendMessage (bytes, length);
        return true;
    }
    catch (RtMidiError &)
    {
        backend->out->closePort();
        connected.store (false, std::memory_order_relaxed);
    }

    return false;
}

void LumiLink::sendCC (uint8_t controller, uint8_t value)
{
    unsigned char msg[3];
    msg[0] = (unsigned char) (0xb0 | kControlChannel);
    msg[1] = controller;
    msg[2] = value;
    sendRaw (msg, 3);
}

void LumiLink::sendKeyNote (uint8_t note, bool isOn)
{
    unsigned char msg[3];
    msg[0] = (unsigned char) ((isOn ? 0x90 : 0x80) | kControlChannel);
    msg[1] = note;
    msg[2] = (unsigned char) (isOn ? 100 : 0);
    sendRaw (msg, 3);
}

void LumiLink::flushGlobals()
{
    const int wantBrightness = brightness.load (std::memory_order_relaxed);
    int wantUnlit = unlitLevel.load (std::memory_order_relaxed);

    /*
        Breathing is one controller message, not a hundred and twenty-eight notes.

        The firmware already computes alpha as level * globalBrightness / 255, where
        level is the unlit level for a resting key and 255 for one being played. That is
        exactly what breathing wants: swell the painted map and leave a key under a
        finger at full. Driving it from here costs one CC per tick instead of repainting
        every note, and it is the whole reason the device has that control.

        Only breathing. Ember is the same swell with a per-note phase offset, and a
        single global level cannot express a hundred and twenty-eight different phases -
        it stays in the per-note path, as do waves and aurora, which are colour fields
        rather than brightness.

        The stored level is not touched, so the slider does not move and nothing reaches
        the host's automation. When the screensaver stops, wantUnlit goes back to what
        the user set and the next flush sends it.
    */
    const int breath = breathLevel();

    if (breath < 255)
    {
        wantUnlit = wantUnlit * breath / 255;

        if (wantUnlit < 1)
            wantUnlit = 1;
    }

    if (wantBrightness != sentBrightness)
    {
        sendCC (kCcBrightness, (uint8_t) wantBrightness);
        sentBrightness = wantBrightness;
    }

    if (wantUnlit != sentUnlitLevel)
    {
        sendCC (kCcUnlitLevel, (uint8_t) wantUnlit);
        sentUnlitLevel = wantUnlit;
    }

    const int wantOffset = displayOffset.load (std::memory_order_relaxed);
    const int wantFold = foldOctaves.load (std::memory_order_relaxed);

    if (wantOffset != sentDisplayOffset)
    {
        sendCC (kCcDisplayOffset, (uint8_t) (wantOffset + 64));
        sentDisplayOffset = wantOffset;
    }

    if (wantFold != sentFoldOctaves)
    {
        sendCC (kCcFoldMode, (uint8_t) (wantFold != 0 ? 1 : 0));
        sentFoldOctaves = wantFold;
    }

    const uint32_t wantHiColour = highlightColour.load (std::memory_order_relaxed);

    if (wantHiColour != sentHighlightColour)
    {
        sendCC (kCcHighlightRed,   (uint8_t) (((wantHiColour >> 16) & 0xff) >> 1));
        sendCC (kCcHighlightGreen, (uint8_t) (((wantHiColour >> 8)  & 0xff) >> 1));
        sendCC (kCcHighlightBlue,  (uint8_t) ((wantHiColour         & 0xff) >> 1));
        sentHighlightColour = wantHiColour;
    }

    const int wantHighlight = highlightEnabled.load (std::memory_order_relaxed);

    if (wantHighlight != sentHighlightEnabled)
    {
        sendCC (kCcHighlightOn, (uint8_t) (wantHighlight != 0 ? 1 : 0));
        sentHighlightEnabled = wantHighlight;
    }

    const uint32_t wantPrColour = pressColour.load (std::memory_order_relaxed);

    if (wantPrColour != sentPressColour)
    {
        sendCC (kCcPressRed,   (uint8_t) (((wantPrColour >> 16) & 0xff) >> 1));
        sendCC (kCcPressGreen, (uint8_t) (((wantPrColour >> 8)  & 0xff) >> 1));
        sendCC (kCcPressBlue,  (uint8_t) ((wantPrColour         & 0xff) >> 1));
        sentPressColour = wantPrColour;
    }

    const int wantPress = pressEnabled.load (std::memory_order_relaxed);

    if (wantPress != sentPressEnabled)
    {
        sendCC (kCcPressOn, (uint8_t) (wantPress != 0 ? 1 : 0));
        sentPressEnabled = wantPress;
    }

    const int wantOctave = octave.load (std::memory_order_relaxed);

    if (wantOctave != sentOctave)
    {
        sendCC (kCcOctave, (uint8_t) (wantOctave + 64));
        sentOctave = wantOctave;
    }

    const uint32_t wantPg = pressureGradColour.load (std::memory_order_relaxed);

    if (wantPg != sentPressureGradColour)
    {
        sendCC (kCcPressureGradRed,   (uint8_t) (((wantPg >> 16) & 0xff) >> 1));
        sendCC (kCcPressureGradGreen, (uint8_t) (((wantPg >> 8)  & 0xff) >> 1));
        sendCC (kCcPressureGradBlue,  (uint8_t) ((wantPg         & 0xff) >> 1));
        sentPressureGradColour = wantPg;
    }

    const int wantPgOn = pressureGradEnabled.load (std::memory_order_relaxed);

    if (wantPgOn != sentPressureGradEnabled)
    {
        sendCC (kCcPressureGradOn, (uint8_t) (wantPgOn != 0 ? 1 : 0));
        sentPressureGradEnabled = wantPgOn;
    }

    const uint32_t wantBg = bendGradColour.load (std::memory_order_relaxed);

    if (wantBg != sentBendGradColour)
    {
        sendCC (kCcBendGradRed,   (uint8_t) (((wantBg >> 16) & 0xff) >> 1));
        sendCC (kCcBendGradGreen, (uint8_t) (((wantBg >> 8)  & 0xff) >> 1));
        sendCC (kCcBendGradBlue,  (uint8_t) ((wantBg         & 0xff) >> 1));
        sentBendGradColour = wantBg;
    }

    const int wantBgOn = bendGradEnabled.load (std::memory_order_relaxed);

    if (wantBgOn != sentBendGradEnabled)
    {
        /*
            Always off on the device now.

            The tint is computed per note here and is already in the colour being sent,
            so letting the firmware blend its global bend over the top would put one
            finger's bend on every key a second time. The setting still exists - it just
            means "tint by bend" rather than "ask the device to do it".
        */
        sendCC (kCcBendGradOn, 0);
        sentBendGradEnabled = wantBgOn;
    }

    const int wantScale = bendFullScale.load (std::memory_order_relaxed);

    if (wantScale != sentBendFullScale)
    {
        sendCC (kCcBendFullScale, (uint8_t) wantScale);
        sentBendFullScale = wantScale;
    }

    const int wantBendOut = enablePitchBend.load (std::memory_order_relaxed);

    if (wantBendOut != sentEnablePitchBend)
    {
        sendCC (kCcEnablePitchBend, (uint8_t) (wantBendOut != 0 ? 1 : 0));
        sentEnablePitchBend = wantBendOut;
    }

    const int wantPressOut = enablePressure.load (std::memory_order_relaxed);

    if (wantPressOut != sentEnablePressure)
    {
        sendCC (kCcEnablePressure, (uint8_t) (wantPressOut != 0 ? 1 : 0));
        sentEnablePressure = wantPressOut;
    }

    const int wantLink = linkOctaves.load (std::memory_order_relaxed);

    if (wantLink != sentLinkOctaves)
    {
        sendCC (kCcLinkOctaves, (uint8_t) (wantLink != 0 ? 1 : 0));
        sentLinkOctaves = wantLink;
    }

}

void LumiLink::flushKeys()
{
    keyMessagesThisTick = 0;

    /*
        A key still glowing counts as lit.

        The device gives a key full output while it is pressed or lit, and unlit level
        otherwise - twelve percent by default. So the moment a note was released its
        afterglow was scaled to twelve percent and all but vanished. It looked bright
        before only because brightness was broken and every key came out at full
        whatever the setting said; fixing that exposed this.

        Holding the key lit until the glow has faded is what afterglow means: the key
        is still shining, so it should be treated as shining. The colour still fades in
        the composite, so the effect is unchanged - it is simply not crushed on the way
        out.
    */
    uint64_t glowing[2] = { 0ull, 0ull };

    if (afterglowEnabled.load (std::memory_order_relaxed) != 0)
        for (int note = 0; note < 128; ++note)
            if (glowLevel[note] > 0)
                glowing[note >> 6] |= 1ull << (note & 63);

    for (int word = 0; word < 2; ++word)
    {
        const uint64_t want = litBits[word].load (std::memory_order_acquire)
                            | externalLitBits[word].load (std::memory_order_acquire)
                            | glowing[word];
        uint64_t diff = want ^ sentLitBits[word];

        while (diff != 0ull)
        {
            const int bit = lowestSetBit (diff);
            diff &= diff - 1ull;
            sendKeyNote ((uint8_t) (word * 64 + bit), ((want >> bit) & 1ull) != 0ull);
            ++keyMessagesThisTick;
        }

        sentLitBits[word] = want;
    }
}

/*
    One colour, as up to three self-describing messages.

    Polyphonic aftertouch carries a note number and a value in three bytes, so red on
    channel 14, green on 15 and blue on 16 say everything needed without a note-select
    first. Four messages become three, order stops mattering, and a dropped one costs
    one component of one key for one frame rather than putting a whole colour on the
    wrong note.

    Then only what changed goes out. That was impossible while the four formed a
    transaction; now each stands alone, a trail fading mostly in red costs one message
    instead of four. Together with the write threshold in flushColours it is around
    five times less traffic for the same picture.
*/
bool LumiLink::writeColour (int note, uint32_t rgb)
{

    const uint32_t was = sentColour[note];
    const bool fresh = was == kUnsentColour;

    const uint8_t want[3] = { (uint8_t) (((rgb >> 16) & 0xff) >> 1),
                              (uint8_t) (((rgb >> 8) & 0xff) >> 1),
                              (uint8_t) ((rgb & 0xff) >> 1) };

    const uint8_t had[3] = { (uint8_t) (((was >> 16) & 0xff) >> 1),
                             (uint8_t) (((was >> 8) & 0xff) >> 1),
                             (uint8_t) ((was & 0xff) >> 1) };

    /* Channels 14, 15 and 16, zero based. */
    static const uint8_t channel[3] = { 13, 14, 15 };

    for (int i = 0; i < 3; ++i)
    {
        if (! fresh && want[i] == had[i])
            continue;

        unsigned char message[3];
        message[0] = (unsigned char) (0xa0 | channel[i]);
        message[1] = (unsigned char) note;
        message[2] = (unsigned char) want[i];

        if (! sendRaw (message, 3))
            return false;
    }

    /*
        Published after the writes, not before.

        The shared table is meant to say what is physically on the keyboard, so that the
        next instance to take over sends only the difference. Publishing before sending
        meant a write that failed - a port that had just gone - was still recorded as
        having arrived, and the next owner trusted a colour the device never received.

        sendRaw closes the port and clears the connected flag when it throws, so the
        check below is a fair test of whether these three messages actually left.
    */
    claim.publishColour (note, rgb);
    deviceSelectedNote = -1;
    return true;
}

void LumiLink::flushColours()
{
    const int tickMs = lastTickMs;

    /* A moving wave dirties keys on both sides of the note that started it, and there
       are more of them than one tick can send. Scanning from note 0 every time meant
       the low half was always serviced first and the high half only got whatever was
       left - so a ripple looked like it travelled down the keyboard and hardly up, and
       the keys it had already passed held their old colour long enough to look like a
       trail. Starting where the last pass stopped gives every note equal service. */
    /*
        Two notes a tick is 2000 messages a second, about 48 kbit/s. That is already
        above the 31.25 kbit/s a MIDI-rate link carries, and it is as far as this
        should ever go.

        Animation was allowed eight a tick when ripples were added - 192 kbit/s, six
        times the rate - and it floods the relay that feeds a chained slave. Every
        write is idempotent, so a dropped one does not corrupt anything; it just leaves
        that note holding its old colour while its neighbours change, which reads as
        colours landing on the wrong keys. Smoothness is not worth that.
    */
    /*
        What counts as animation, which is a question about cost rather than movement.

        Waves, aurora and ember repaint every note every tick and need the send budget
        raised or they crawl. Breathing does not: it is one controller message that the
        firmware applies to the whole keyboard, so it moves without sending a single
        note and asking for a bigger note allowance on its behalf would only take
        bandwidth from whatever else is running.
    */
    const int idleMode = wavesMode.load (std::memory_order_relaxed);

    /* Breathing sends no notes at all - it is one controller message - and rainfall
       lights a handful of keys rather than repainting the keybed. Neither should take
       the note allowance a full-field pattern needs. */
    const bool breathingOnly = wavesRunning() && (idleMode == 2 || idleMode == 5);

    const bool animating = activeRipples > 0 || activeGlow > 0 || pulseLevel > 0
                        || (wavesRunning() && ! breathingOnly);
    /*
        How many notes go out this tick.

        The ceiling is not the master block - that is on USB and takes far more. It is
        the relayed block in a chain, which drops writes somewhere above 50 kbit/s. So
        the allowance follows how many blocks are connected, which the device reports,
        and a single block is driven harder than a pair.

        Then scaled by how long the tick really took. The worker asks for 4 ms and on
        Windows gets about 15.6 unless the timer resolution has been raised, so a fixed
        per-tick allowance quietly sent a quarter of what was intended.
    */
    const int visible = windowHigh.load (std::memory_order_relaxed)
                      - windowLow.load (std::memory_order_relaxed) + 1;
    const int manual = sendRate.load (std::memory_order_relaxed);
    const int perTick = manual > 0 ? manual * 3 : (visible > 30 ? 3 : 8);

    int budget = animating ? perTick : (keyMessagesThisTick > 0 ? 1 : 2);
    budget = (budget * tickMs) / 4;

    if (budget < 1)
        budget = 1;

    if (budget > 40)
        budget = 40;
    int scanned = 0;

    while (scanned < 128 && budget > 0)
    {
        const int note = (dirtyCursor + scanned) & 127;
        ++scanned;

        /*
            No filtering by range at all. Every note that changed is sent.

            Four versions of a window were tried here and every one of them could black
            out a block, because they all rest on the plugin knowing where each block
            sits - and it cannot. Only the master has a route back to the host; a
            chained block has no way to say where it has been moved to. Filtering on a
            position that is unknowable in principle is guessing, and the penalty for
            guessing wrong is a dead keyboard.

            The traffic argument that justified the window has also gone. A colour is
            now up to three self-describing messages instead of four, only the
            components that changed are sent, and changes too small to see are skipped
            - about five times less traffic than when the window was introduced. The
            whole 128 costs less today than 48 did then.
        */

        const uint32_t want = desiredColour[note].load (std::memory_order_relaxed);
        const uint32_t have = sentColour[note];

        if (want == have)
            continue;

        /*
            Changes too small to see are not worth a message.

            Colours travel as 7 bits per channel, so anything that rounds to the same
            7-bit value is literally identical on the hardware and was pure waste. A
            step beyond that is skipped too: an LED cannot show a single 1/128 change,
            and a trail fading over two hundred steps looks the same at thirty. During
            animation this is most of the traffic.

            Only while animating. A deliberate colour, painted or imported, goes out
            exactly as chosen however small the difference.
        */
        if (animating && have != kUnsentColour && skipped[note] < 4)
        {
            const int dr = std::abs ((int) ((want >> 16) & 0xff) - (int) ((have >> 16) & 0xff));
            const int dg = std::abs ((int) ((want >> 8) & 0xff) - (int) ((have >> 8) & 0xff));
            const int db = std::abs ((int) (want & 0xff) - (int) (have & 0xff));

            if (dr < 6 && dg < 6 && db < 6)
            {
                /*
                    Deferred, not abandoned.

                    Skipping a change too small to see is right while a colour is
                    moving, and wrong once it has stopped. A ripple ending on a key
                    whose base is black leaves the last trail value a few units above
                    it - under the threshold, so it was skipped, and skipped again on
                    every following tick because nothing about it ever changed again.
                    The key sat visibly lit until the background sweep happened past it,
                    which is about half a second.

                    Counting the skips bounds it: after four the note goes out whatever
                    the difference. A moving colour is still cheap, because it changes
                    by more than the threshold every tick and the count never builds;
                    only a settled one accumulates, and settled is exactly the case that
                    must not be left wrong.
                */
                ++skipped[note];
                continue;
            }
        }

        skipped[note] = 0;

        /* Recorded only if it actually went. A failed write leaves the note dirty, so
           the next tick tries again rather than the cache claiming a colour the device
           never received. */
        if (! writeColour (note, want))
            break;

        sentColour[note] = want;
        --budget;
    }

    dirtyCursor = (dirtyCursor + scanned) & 127;

    if (budget <= 0)
        return;

    /*
        Idle: rewalk the table one note at a time so a colour lost on the relay comes
        back on its own, rather than staying wrong until the user happens to edit that
        note again. One note every fifth tick is a full sweep every 2.5 seconds.
    */
    if (--colourRefreshCountdown > 0)
        return;

    colourRefreshCountdown = 5;

    /* Unfiltered, for the same reason the send path is: a key outside a guessed window
       would never be repaired, so anything lost on a chained block's relay would stay
       wrong forever. */
    writeColour (refreshCursor, desiredColour[refreshCursor].load (std::memory_order_relaxed));

    refreshCursor = (refreshCursor + 1) & 127;
}

void LumiLink::shutdownDevice()
{
    /*
        A member of a chain usually has no port, and still has to be able to blank.

        Closing a project tears the instances down one at a time. The one that was
        sending blanks the keyboard and lets go; a survivor then finds itself the sender,
        opens the port and repaints its range - and whether anything blanks it again
        depends entirely on which order the host happens to destroy them in. Sometimes
        one zone stayed lit, sometimes two.

        So whoever is shutting down opens a port if it does not have one and blanks
        regardless. Blanking an already-blank keyboard costs two controller messages and
        is the only version of this that does not depend on teardown order.

        Its published colours go first, so a survivor that does become the sender has
        nothing of this instance's left to draw.
    */
    /*
        Only a member that actually holds a range clears one.

        This used to run for anything with sharing switched on, using the stored range -
        which for an instance whose claim was refused is still the default 0 to 127. So
        closing a project wiped every other member's published colours on the way past.
    */
    if (hasZone())
    {
        claim.clearZoneColours (zoneLow.load (std::memory_order_relaxed),
                                zoneHigh.load (std::memory_order_relaxed));
    }

    if (zoned.load (std::memory_order_relaxed) != 0)
        claim.releaseZone();

    /*
        Blanking waits for the port rather than giving up on it.

        Whoever was sending holds it, and on Windows that is exclusive - so a member
        shutting down first asked once, was refused, and left its keys lit. Quitting a
        DAW closes every instance within a moment of each other, so a few short attempts
        covers the handover without delaying anything a user would notice.

        If every attempt fails, the keys still go dark: the published colours above are
        gone, so whichever instance is still sending finds that range unowned on its
        next tick and blacks it.
    */
    for (int attempt = 0; attempt < 10; ++attempt)
    {
        if (backend->out && backend->out->isPortOpen())
            break;

        if (ensureConnection())
            break;

        std::this_thread::sleep_for (std::chrono::milliseconds (30));
    }

    if (backend->out && backend->out->isPortOpen())
    {
        sendCC (kCcCommand, kCmdAllKeysOff);
        sendCC (kCcUnlitLevel, 0);
        backend->out->closePort();
    }

    backend->out.reset();
    connected.store (false, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock (portsMutex);
    activePort.clear();
}

namespace {

void applyParamValue (LumiPaint *self, uint32_t paramId, double value)
{
    if (paramId == kParamBrightness)
    {
        self->brightness = value;
        self->link.setBrightness (value);
    }
    else if (paramId == kParamUnlitLevel)
    {
        self->unlitLevel = value;
        self->link.setUnlitLevel (value);
    }
    else if (paramId == kParamDisplayOffset)
    {
        self->displayOffset = value;
        self->link.setDisplayOffset (value);
    }
    else if (paramId == kParamFoldOctaves)
    {
        self->foldOctaves = value;
        self->link.setFoldOctaves (value >= 0.5);
    }
    else if (paramId == kParamHighlight)
    {
        self->highlight = value;
        self->link.setHighlightEnabled (value >= 0.5);
    }
    else if (paramId == kParamPressColour)
    {
        self->pressColour = value;
        self->link.setPressEnabled (value >= 0.5);
    }
    else if (paramId == kParamOctave)
    {
        self->octave = value;
        self->link.setOctave ((int) value);
    }
    else if (paramId == kParamPressureGradient)
    {
        self->pressureGradient = value;
        self->link.setPressureGradEnabled (value >= 0.5);
    }
    else if (paramId == kParamBendGradient)
    {
        self->bendGradient = value;
        self->link.setBendGradEnabled (value >= 0.5);
    }
}

void setNoteRef (LumiPaint *self, int note, int delta)
{
    if (note < 0 || note > 127)
        return;

    /* The zone filter is at the event, not here: by the time a note reaches this it has
       already caused a ripple and an afterglow, and turning it away now would stop the
       key lighting while leaving everything else it set in motion. */
    if (delta > 0)
        self->link.noteReachedZone();

    self->refCount[note] += delta;

    if (self->refCount[note] < 0)
        self->refCount[note] = 0;

    const int word = note >> 6;
    const uint64_t mask = 1ull << (note & 63);

    if (self->refCount[note] > 0)
        self->litBits[word] |= mask;
    else
        self->litBits[word] &= ~mask;
}

/*
    Note off, with the pedal taken into account.

    The release is deferred, not suppressed. Suppressing it would lose the count: a note
    struck twice while the pedal is down is held twice, and letting go of both while
    sustaining has to leave it lit until the pedal rises, then put it out once. So each
    pending release is counted and applied in full when the pedal comes up.

    Everything downstream stays as it was. The note is still lit, so afterglow, ripples
    and the chord halo behave exactly as they do for a finger on the key - which is the
    point: the keyboard should show what is sounding, and under the pedal that is not
    the same as what is being pressed.
*/
void releaseNote (LumiPaint *self, int note)
{
    if (note < 0 || note > 127)
        return;

    if (self->sustainDown && self->refCount[note] > 0)
    {
        ++self->pendingRelease[note];
        return;
    }

    setNoteRef (self, note, -1);
}

/*
    CC 64, in the half the MIDI spec actually settles: 64 and above is down.

    On the way up every note the pedal was holding is released at once, which is what
    makes a pedal lift visible - the whole sustained chord goes out together and the
    afterglow trails from all of it, rather than notes dropping away one at a time as
    fingers happened to leave them.
*/
void setSustain (LumiPaint *self, bool down)
{
    if (down == self->sustainDown)
        return;

    self->sustainDown = down;
    self->link.setSustain (down);

    if (down)
        return;

    for (int note = 0; note < 128; ++note)
    {
        while (self->pendingRelease[note] > 0)
        {
            --self->pendingRelease[note];
            setNoteRef (self, note, -1);
        }
    }
}

void clearAllNotes (LumiPaint *self)
{
    for (int i = 0; i < 128; ++i)
    {
        self->refCount[i] = 0;
        self->pendingRelease[i] = 0;
        self->sentOffset[i] = kNoOffset;
    }

    self->litBits[0] = 0;
    self->litBits[1] = 0;
}

/*
    True for a control change the keyboard sent to this plugin.

    Only the numbers the device actually reports on, and only on the control channel.
    Deliberately not "every CC on channel 16": a sequencer or a controller may
    legitimately send CCs there, and swallowing those would be its own quiet bug.
*/
bool isDeviceReport (LumiPaint *self, const clap_event_header_t *header)
{
    if (header->space_id != CLAP_CORE_EVENT_SPACE_ID || header->type != CLAP_EVENT_MIDI)
        return false;

    const clap_event_midi_t *ev = (const clap_event_midi_t *) header;

    if ((ev->data[0] & 0x0f) != kControlChannel)
        return false;

    /*
        Poly aftertouch on the control channel is a report, and nothing else uses it.

        This used to test for six control-change numbers, which could never be certain:
        a CC 30 from a pedal is byte-identical to a CC 30 from the keyboard, so the
        filter either swallowed a real controller or let the keyboard modulate whatever
        was listening. Moving reports to poly aftertouch removes the ambiguity - a real
        controller does not send poly aftertouch on channel 16, and if one did it would
        be per-note pressure for notes that are not sounding.
    */
    if ((ev->data[0] & 0xf0) != kReportStatus || ! isReportSlot (ev->data[1]))
        return false;

    /* Real key pressure is poly aftertouch too, and a key can land on channel 16 under
       MPE. Pressure is only ever about a sounding note, so a poly aftertouch for a note
       that is down is pressure and passes through; anything else is a report and is
       consumed. */
    return ! self->link.isNoteSounding (ev->data[1]);
}

/*
    Whether an event belongs to this instance's zone.

    Only the events that name a key are judged. A CLAP note carries its key directly; a
    raw MIDI note-on or note-off carries it in the second byte. Anything else - a bend,
    an aftertouch, a controller - is not about one key and passes regardless.
*/
/*
    A note on its way to the track, moved to where the zone says it plays.

    Returns true when it has pushed a transposed copy, false when the event should go
    through untouched.

    The offset applied at note-on is remembered and reused at note-off. Nudging a zone
    while a key is held would otherwise send the off to a different note from the on,
    and the track would hold that note for ever - the one way this feature can leave a
    synth droning with nothing on screen to explain it.
*/
bool pushTransposed (LumiPaint *self, const clap_output_events_t *out,
                     const clap_event_header_t *header)
{
    if (! self->link.hasZone())
        return false;

    const int live = self->link.getZoneOffset();

    if (header->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return false;

    if (header->type == CLAP_EVENT_NOTE_ON || header->type == CLAP_EVENT_NOTE_OFF
         || header->type == CLAP_EVENT_NOTE_CHOKE)
    {
        const clap_event_note_t *ev = (const clap_event_note_t *) header;

        if (ev->key < 0)
            return false;

        const bool starting = header->type == CLAP_EVENT_NOTE_ON;
        int shift = live;

        if (starting)
            self->sentOffset[ev->key] = live;
        else if (self->sentOffset[ev->key] != kNoOffset)
            shift = self->sentOffset[ev->key];

        if (! starting)
            self->sentOffset[ev->key] = kNoOffset;

        const int moved = ev->key + shift;

        if (moved < 0 || moved > 127)
            return true;

        clap_event_note_t copy = *ev;
        copy.key = (int16_t) moved;
        out->try_push (out, &copy.header);
        return true;
    }

    if (header->type == CLAP_EVENT_MIDI)
    {
        const clap_event_midi_t *ev = (const clap_event_midi_t *) header;
        const uint8_t status = ev->data[0] & 0xf0;

        if (status != 0x80 && status != 0x90)
            return false;

        const int key = ev->data[1];
        const bool starting = status == 0x90 && ev->data[2] > 0;
        int shift = live;

        if (starting)
            self->sentOffset[key] = live;
        else if (self->sentOffset[key] != kNoOffset)
            shift = self->sentOffset[key];

        if (! starting)
            self->sentOffset[key] = kNoOffset;

        const int moved = key + shift;

        if (moved < 0 || moved > 127)
            return true;

        clap_event_midi_t copy = *ev;
        copy.data[1] = (uint8_t) moved;
        out->try_push (out, &copy.header);
        return true;
    }

    return false;
}

bool passesZone (LumiPaint *self, const clap_event_header_t *header)
{
    if (! self->link.getZoned())
        return true;

    if (header->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return true;

    if (header->type == CLAP_EVENT_NOTE_ON || header->type == CLAP_EVENT_NOTE_OFF
         || header->type == CLAP_EVENT_NOTE_CHOKE)
    {
        const clap_event_note_t *ev = (const clap_event_note_t *) header;

        /* A key of -1 means every key, which a host sends to silence the instrument.
           That has to get through whatever the zone is, or a stuck note stays stuck. */
        return ev->key < 0 || self->link.noteInZone (ev->key);
    }

    if (header->type == CLAP_EVENT_MIDI)
    {
        const clap_event_midi_t *ev = (const clap_event_midi_t *) header;
        const uint8_t status = ev->data[0] & 0xf0;

        if (status == 0x80 || status == 0x90)
            return self->link.noteInZone (ev->data[1]);
    }

    return true;
}

void handleEvent (LumiPaint *self, const clap_event_header_t *header)
{
    if (header->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return;

    if (header->type == CLAP_EVENT_NOTE_EXPRESSION)
    {
        /* Tuning is bend, in semitones, and it is how a CLAP host expresses it. Without
           this the bend path only worked when the host happened to send raw MIDI - and
           a host that sends CLAP notes sends CLAP expressions with them. */
        const clap_event_note_expression_t *ev = (const clap_event_note_expression_t *) header;

        /*
            Routed by key, not by channel.

            A CLAP expression names its note by note_id and key; the channel is often -1
            because there is no channel involved - that is the point of note_id. Sending
            it to channel zero therefore put every note's bend in the same slot, so one
            note bending moved them all. It looked right in non-MPE only because there
            is never more than one bend at a time there.
        */
        if (ev->expression_id == CLAP_NOTE_EXPRESSION_TUNING)
        {
            if (ev->key >= 0 && ev->key < 128)
                self->link.tuningOnNote (ev->key, ev->value);
            else
                self->link.tuningOnChannel (ev->channel < 0 ? 0 : ev->channel, ev->value);
        }
    }
    else if (header->type == CLAP_EVENT_NOTE_ON)
    {
        const clap_event_note_t *ev = (const clap_event_note_t *) header;

        /*
            Outside the zone, the whole event is dropped.

            Filtering inside setNoteRef only stopped the key lighting. Everything else a
            note causes still happened: the ripple, the afterglow, the degree root, the
            velocity - and noteActivity, which claims the keyboard. So an instance
            reacted to notes it did not own, threw waves from them, and took the device
            on their account. The note is not this instance's business at all, so it is
            turned away before any of that.
        */
        if (! self->link.noteInZone (ev->key))
            return;

        setNoteRef (self, ev->key, 1);

        /* Which note is on which channel, so a bend arriving on that channel knows
           where its path starts. */
        self->link.noteOnChannel (ev->channel < 0 ? 0 : ev->channel, ev->key, true);

        self->link.noteActivity();
        self->link.setDegreeRoot (ev->key);
        self->link.noteVelocity (ev->key, (int) (ev->velocity * 127.0));
        self->link.triggerRipple (ev->key, 255);
        self->link.triggerAfterglow (ev->key, 255);
    }
    else if (header->type == CLAP_EVENT_NOTE_OFF || header->type == CLAP_EVENT_NOTE_CHOKE)
    {
        const clap_event_note_t *ev = (const clap_event_note_t *) header;

        if (ev->key < 0)
        {
            clearAllNotes (self);
        }
        else
        {
            if (! self->link.noteInZone (ev->key))
                return;

            releaseNote (self, ev->key);
            self->link.noteOnChannel (ev->channel < 0 ? 0 : ev->channel, ev->key, false);
        }
    }
    else if (header->type == CLAP_EVENT_MIDI)
    {
        const clap_event_midi_t *ev = (const clap_event_midi_t *) header;
        const uint8_t status = ev->data[0] & 0xf0;

        /* Counted before any filtering, so "the host delivers no raw MIDI at all" and
           "it delivers some but not what we expect" can be told apart. Four rounds
           have now gone on not knowing which. */
        self->link.countMessageIn (ev->data[0]);

        if (status == 0x90 && ev->data[2] > 0)
        {
            if (! self->link.noteInZone (ev->data[1]))
                return;

            setNoteRef (self, ev->data[1], 1);
            self->link.noteOnChannel (ev->data[0] & 0x0f, ev->data[1], true);
            self->link.noteActivity();
            self->link.setDegreeRoot (ev->data[1]);
            self->link.noteVelocity (ev->data[1], ev->data[2]);
            self->link.triggerRipple (ev->data[1], 255);
            self->link.triggerAfterglow (ev->data[1], ev->data[2] * 2);
        }
        else if (status == 0x80 || (status == 0x90 && ev->data[2] == 0))
        {
            if (! self->link.noteInZone (ev->data[1]))
                return;

            releaseNote (self, ev->data[1]);
            self->link.noteOnChannel (ev->data[0] & 0x0f, ev->data[1], false);
        }
        else if (status == 0xe0)
        {
            self->link.bendOnChannel (ev->data[0] & 0x0f,
                                      ((int) ev->data[2] << 7) | (int) ev->data[1]);
        }
        else if (status == 0xb0 && ev->data[1] == 64)
            setSustain (self, ev->data[2] >= 64);
        else if (status == 0xb0 && ev->data[1] == 123)
            clearAllNotes (self);
        else if (status == 0xb0 && ev->data[1] == self->link.getSplashCC())
            self->link.triggerSplash (ev->data[2] * 2);
        else if ((status == 0xb0 || status == kReportStatus)
                 && (ev->data[0] & 0x0f) == kControlChannel)
            self->hostDecoder.feed (self->link, ev->data[0], ev->data[1], ev->data[2]);
    }
    else if (header->type == CLAP_EVENT_PARAM_VALUE)
    {
        const clap_event_param_value_t *ev = (const clap_event_param_value_t *) header;

        applyParamValue (self, ev->param_id, ev->value);
    }
}

void emitGuiParamChanges (LumiPaint *self, const clap_output_events_t *out)
{
    for (uint32_t id = 0; id < kParamCount; ++id)
    {
        GuiParamSlot &slot = self->guiParams[id];

        if (slot.beginPending.exchange (false, std::memory_order_acquire))
        {
            clap_event_param_gesture_t ev;
            ev.header.size = sizeof (ev);
            ev.header.time = 0;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.type = CLAP_EVENT_PARAM_GESTURE_BEGIN;
            ev.header.flags = 0;
            ev.param_id = id;
            out->try_push (out, &ev.header);
        }

        if (slot.valuePending.exchange (false, std::memory_order_acquire))
        {
            const double value = slot.value.load (std::memory_order_relaxed);

            clap_event_param_value_t ev;
            ev.header.size = sizeof (ev);
            ev.header.time = 0;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.type = CLAP_EVENT_PARAM_VALUE;
            ev.header.flags = 0;
            ev.param_id = id;
            ev.cookie = nullptr;
            ev.note_id = -1;
            ev.port_index = -1;
            ev.channel = -1;
            ev.key = -1;
            ev.value = value;
            out->try_push (out, &ev.header);
            applyParamValue (self, id, value);
        }

        if (slot.endPending.exchange (false, std::memory_order_acquire))
        {
            clap_event_param_gesture_t ev;
            ev.header.size = sizeof (ev);
            ev.header.time = 0;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.type = CLAP_EVENT_PARAM_GESTURE_END;
            ev.header.flags = 0;
            ev.param_id = id;
            out->try_push (out, &ev.header);
        }
    }
}

bool pluginInit (const clap_plugin_t *plugin)
{
    LumiPaint *self = (LumiPaint *) plugin->plugin_data;
    clearAllNotes (self);
    self->brightness = 1.0;
    self->unlitLevel = 0.5;
    self->displayOffset = 0.0;
    self->foldOctaves = 0.0;
    self->highlight = 0.0;
    self->pressColour = 0.0;
    self->octave = 0.0;
    self->pressureGradient = 0.0;
    self->bendGradient = 0.0;
    self->link.setBrightness (self->brightness);
    self->link.setUnlitLevel (self->unlitLevel);
    self->link.setDisplayOffset (self->displayOffset);
    self->link.setFoldOctaves (false);
    self->hostParams = (const clap_host_params_t *) self->host->get_extension (self->host, CLAP_EXT_PARAMS);
    self->hostState = (const clap_host_state_t *) self->host->get_extension (self->host, CLAP_EXT_STATE);
    self->hostGui = (const clap_host_gui_t *) self->host->get_extension (self->host, CLAP_EXT_GUI);
    return true;
}

void pluginDestroy (const clap_plugin_t *plugin)
{
    LumiPaint *self = (LumiPaint *) plugin->plugin_data;

    /* Stop the worker looking at it before it goes. */
    self->link.stopFollowing();
    delete self->capture;
    self->capture = nullptr;

    /* The worker goes first.

       Tearing the editor down before stopping the thread meant that if anything in the
       window teardown stalled, the thread was still running and the host process could
       not exit - which shows up as a DAW that closes its window and then sits in the
       task list. Nothing in the GUI teardown needs the link alive, so there is no
       reason for it to go first. */
    self->link.stop();
    guiShutdown (plugin);
    delete self;
}

bool pluginActivate (const clap_plugin_t *plugin, double sampleRate, uint32_t minFrames, uint32_t maxFrames)
{
    LumiPaint *self = (LumiPaint *) plugin->plugin_data;
    (void) sampleRate;
    (void) minFrames;
    (void) maxFrames;
    self->link.start();
    return true;
}

void pluginDeactivate (const clap_plugin_t *plugin)
{
    LumiPaint *self = (LumiPaint *) plugin->plugin_data;
    clearAllNotes (self);
    self->link.publishLitBits (0, 0);
    std::this_thread::sleep_for (std::chrono::milliseconds (20));
    self->link.stop();
}

bool pluginStartProcessing (const clap_plugin_t *plugin)
{
    (void) plugin;
    return true;
}

void pluginStopProcessing (const clap_plugin_t *plugin)
{
    (void) plugin;
}

void pluginReset (const clap_plugin_t *plugin)
{
    LumiPaint *self = (LumiPaint *) plugin->plugin_data;
    clearAllNotes (self);
    self->link.publishLitBits (0, 0);
}

clap_process_status pluginProcess (const clap_plugin_t *plugin, const clap_process_t *process)
{
    clearSilentOutput(process);
    LumiPaint *self = (LumiPaint *) plugin->plugin_data;
    const clap_input_events_t *in = process->in_events;
    const clap_output_events_t *out = process->out_events;
    const uint32_t eventCount = in->size (in);

    emitGuiParamChanges (self, out);

    /* The anchor probe. LumiPaint sits before the instrument on its track, so a note
       emitted here reaches the very plugin whose window is being captured - which is
       what makes working out the anchor automatic rather than something to type in. */
    {
        const int on = self->probeNoteOn.exchange (-1, std::memory_order_acquire);
        const int off = self->probeNoteOff.exchange (-1, std::memory_order_acquire);

        for (int pass = 0; pass < 2; ++pass)
        {
            const int key = pass == 0 ? on : off;

            if (key < 0)
                continue;

            clap_event_note_t ev;
            ev.header.size = sizeof (ev);
            ev.header.time = 0;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.type = pass == 0 ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF;
            ev.header.flags = 0;
            ev.note_id = -1;
            ev.port_index = 0;
            ev.channel = 0;
            ev.key = (int16_t) key;
            ev.velocity = pass == 0 ? 0.7 : 0.0;
            out->try_push (out, &ev.header);
        }
    }

    for (uint32_t i = 0; i < eventCount; ++i)
    {
        const clap_event_header_t *header = in->get (in, i);
        handleEvent (self, header);

        /*
            The device's own reports stop here.

            Everything the keyboard sends back - the config mirror, the cluster width,
            each block's position - travels as control changes on channel 16, and
            passing them on would hand a synth downstream a stream of CC 30, 31, 45,
            46, 48 and 49 that means nothing to it. Anything listening omni, or mapped
            on channel 16, would be modulated by the keyboard reporting where its
            blocks are. That is a genuinely confusing fault to chase: a synth drifting
            for no reason, caused by something that is not even a musical message.

            Consumed rather than forwarded, since this plugin is the only thing they
            are addressed to. Notes and everything else on channel 16 pass through
            untouched - a keyboard playing on channel 16 still plays.
        */
        if (isDeviceReport (self, header))
            continue;

        /*
            A note outside the zone is not passed on either.

            Filtering the display alone left every track's instrument still playing the
            whole keyboard - so splitting an arrangement across tracks lit the right
            keys and sounded like four copies of the same part. A zone is a share of the
            instrument as much as of the lights.

            Notes only. Pitch bend, pressure, the sustain pedal and everything else
            carry on through untouched: they are not addressed to a key, and silently
            dropping a pedal or a bend because of a range would be worse than the
            problem this solves.
        */
        if (! passesZone (self, header))
            continue;

        if (pushTransposed (self, out, header))
            continue;

        out->try_push (out, header);
    }

    /* Adopt an octave the device reports, so the parameter, the host's automation lane
       and the editor all follow the hardware buttons and Dashboard. Guarded by the
       comparison, so it fires once per actual change rather than every block. */
    /* The beat can only come from here - the device has no idea the transport exists.
       A bar start pulses at full, every other beat lower, so the downbeat reads. */
    if (process->transport != nullptr
        && (process->transport->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) != 0
        && (process->transport->flags & CLAP_TRANSPORT_IS_PLAYING) != 0)
    {
        const clap_event_transport_t *t = process->transport;
        const int64_t beat = t->song_pos_beats / CLAP_BEATTIME_FACTOR;

        if (beat != self->lastBeat)
        {
            const int64_t barBeat = t->bar_start / CLAP_BEATTIME_FACTOR;
            self->lastBeat = beat;
            self->link.triggerBeat (beat == barBeat ? 255 : 150);
        }
    }
    else
    {
        self->lastBeat = INT64_MIN;
    self->probeNoteOn.store (-1, std::memory_order_relaxed);
    self->probeNoteOff.store (-1, std::memory_order_relaxed);
    }

    /* Only follow the hardware when the blocks are linked and so share one octave.
       Unlinked they each have their own, and a single control cannot show two numbers -
       it would flip between them as each block reported. The per-block brackets under
       the editor keyboard are what shows where each one is in that case. */
    if (self->link.getLinkOctaves())
    {
        const int deviceOctave = self->link.getDeviceConfig (kConfigOctave);

        if (deviceOctave != kConfigUnknown && (int) self->octave != deviceOctave)
            pushGuiParam (self, kParamOctave, (double) deviceOctave, true, true);
    }

    {
        uint64_t sustained[2] = { 0, 0 };

        if (self->sustainDown)
            for (int note = 0; note < 128; ++note)
                if (self->pendingRelease[note] > 0)
                    sustained[note >> 6] |= 1ull << (note & 63);

        self->link.publishSustainBits (sustained[0], sustained[1]);
    }

    self->link.publishLitBits (self->litBits[0], self->litBits[1]);
    return CLAP_PROCESS_CONTINUE;
}

// Live loads the VST3 as an instrument and requires a main audio output even
// though LumiPaint only produces MIDI. Always write silence to that output.
uint32_t audioPortsCount (const clap_plugin_t *, bool isInput)
{
    return isInput ? 0 : 1;
}

bool audioPortsGet (const clap_plugin_t *, uint32_t index, bool isInput,
                    clap_audio_port_info_t *info)
{
    if (isInput || index != 0)
        return false;

    *info = {};
    info->id = 0;
    std::snprintf(info->name, sizeof(info->name), "Silent Output");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN | CLAP_AUDIO_PORT_SUPPORTS_64BITS;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

const clap_plugin_audio_ports_t s_audioPorts = { audioPortsCount, audioPortsGet };

uint32_t notePortsCount (const clap_plugin_t *plugin, bool isInput)
{
    (void) plugin;
    (void) isInput;
    return 1;
}

bool notePortsGet (const clap_plugin_t *plugin, uint32_t index, bool isInput, clap_note_port_info_t *info)
{
    (void) plugin;

    if (index != 0)
        return false;

    info->id = isInput ? 0 : 1;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;

    /*
        The input asks for raw MIDI, the output for CLAP notes.

        This mattered far more than it looks. A CLAP note event carries a note, a
        channel and an expression - it cannot carry a control change. Asking the host
        for CLAP on the input meant every control change the keyboard sent was dropped
        before it reached here, so the device's reports - the octave it moved to, how
        many blocks are connected, what each one is showing - never arrived, and the
        editor said no block was reporting.

        On Windows the keyboard's own input port is usually held by the host, so the
        route through the host is the only one there is. It has to carry MIDI.

        The output stays on CLAP, since what leaves here is notes for the instrument
        downstream and those are better expressed as CLAP events.
    */
    info->preferred_dialect = isInput ? CLAP_NOTE_DIALECT_MIDI : CLAP_NOTE_DIALECT_CLAP;
    std::snprintf (info->name, sizeof (info->name), "%s", isInput ? "Notes In" : "Notes Out");
    return true;
}

const clap_plugin_note_ports_t s_notePorts = { notePortsCount, notePortsGet };

uint32_t paramsCount (const clap_plugin_t *plugin)
{
    (void) plugin;
    return kParamCount;
}

bool paramsGetInfo (const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info)
{
    (void) plugin;

    if (index >= kParamCount)
        return false;

    std::memset (info, 0, sizeof (*info));
    info->id = index;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    info->min_value = 0.0;
    info->max_value = 1.0;

    if (index == kParamBrightness)
    {
        info->default_value = 1.0;
        std::snprintf (info->name, sizeof (info->name), "Brightness");
    }
    else if (index == kParamUnlitLevel)
    {
        /* Half, not six percent.

           The old default was chosen so that a played note would stand out against a
           dark keyboard, back when brightness was broken and everything came out at
           full anyway. With brightness working it made a painted map nearly invisible
           until you played it, which is the wrong way round - the map is the point,
           and the played note has the highlight colour to distinguish it. */
        info->default_value = 0.5;
        std::snprintf (info->name, sizeof (info->name), "Unlit Level");
    }
    else if (index == kParamDisplayOffset)
    {
        info->flags |= CLAP_PARAM_IS_STEPPED;
        info->min_value = -24.0;
        info->max_value = 24.0;
        info->default_value = 0.0;
        std::snprintf (info->name, sizeof (info->name), "Display Offset");
    }
    else if (index == kParamFoldOctaves)
    {
        info->flags |= CLAP_PARAM_IS_STEPPED;
        info->default_value = 0.0;
        std::snprintf (info->name, sizeof (info->name), "Fold Octaves");
    }
    else if (index == kParamHighlight)
    {
        info->flags |= CLAP_PARAM_IS_STEPPED;
        info->default_value = 0.0;
        std::snprintf (info->name, sizeof (info->name), "Highlight Notes");
    }
    else if (index == kParamPressColour)
    {
        info->flags |= CLAP_PARAM_IS_STEPPED;
        info->default_value = 0.0;
        std::snprintf (info->name, sizeof (info->name), "Pressed Colour");
    }
    else if (index == kParamOctave)
    {
        info->flags |= CLAP_PARAM_IS_STEPPED;
        info->min_value = -3.0;
        info->max_value = 3.0;
        info->default_value = 0.0;
        std::snprintf (info->name, sizeof (info->name), "Octave");
    }
    else if (index == kParamPressureGradient)
    {
        info->flags |= CLAP_PARAM_IS_STEPPED;
        info->default_value = 0.0;
        std::snprintf (info->name, sizeof (info->name), "Pressure Gradient");
    }
    else
    {
        info->flags |= CLAP_PARAM_IS_STEPPED;
        info->default_value = 0.0;
        std::snprintf (info->name, sizeof (info->name), "Bend Gradient");
    }

    return true;
}

bool paramsGetValue (const clap_plugin_t *plugin, clap_id paramId, double *value)
{
    LumiPaint *self = (LumiPaint *) plugin->plugin_data;

    if (paramId == kParamBrightness)
        *value = self->brightness;
    else if (paramId == kParamUnlitLevel)
        *value = self->unlitLevel;
    else if (paramId == kParamDisplayOffset)
        *value = self->displayOffset;
    else if (paramId == kParamFoldOctaves)
        *value = self->foldOctaves;
    else if (paramId == kParamHighlight)
        *value = self->highlight;
    else if (paramId == kParamPressColour)
        *value = self->pressColour;
    else if (paramId == kParamOctave)
        *value = self->octave;
    else if (paramId == kParamPressureGradient)
        *value = self->pressureGradient;
    else if (paramId == kParamBendGradient)
        *value = self->bendGradient;
    else
        return false;

    return true;
}

bool paramsValueToText (const clap_plugin_t *plugin, clap_id paramId, double value, char *out, uint32_t size)
{
    (void) plugin;

    if (paramId == kParamDisplayOffset)
        std::snprintf (out, size, "%+d st", (int) value);
    else if (paramId == kParamOctave)
        std::snprintf (out, size, "%+d", (int) value);
    else if (paramId == kParamFoldOctaves || paramId == kParamHighlight
             || paramId == kParamPressColour || paramId == kParamPressureGradient
             || paramId == kParamBendGradient)
        std::snprintf (out, size, "%s", value >= 0.5 ? "On" : "Off");
    else
        std::snprintf (out, size, "%d %%", (int) (value * 100.0 + 0.5));

    return true;
}

/*
    Text back to a value, per parameter.

    This divided everything by a hundred, on the assumption that every parameter is a
    percentage. Two are not: Display Offset reads "+12 st" and Octave reads "+2", and
    both came back a hundred times too small - so typing a value into a host's parameter
    list, or editing automation numerically, set something entirely different from what
    was typed.

    It also returned true unconditionally, which tells the host a nonsense parse
    succeeded. Refusing is the honest answer and hosts handle it.
*/
bool paramsTextToValue (const clap_plugin_t *plugin, clap_id paramId, const char *text, double *value)
{
    (void) plugin;

    if (text == nullptr || value == nullptr)
        return false;

    while (*text == ' ')
        ++text;

    /* Booleans read as on and off as well as as numbers. */
    if (paramId == kParamFoldOctaves || paramId == kParamHighlight
        || paramId == kParamPressColour || paramId == kParamPressureGradient
        || paramId == kParamBendGradient)
    {
        if (strncmp (text, "on", 2) == 0 || strncmp (text, "On", 2) == 0)
        {
            *value = 1.0;
            return true;
        }

        if (strncmp (text, "off", 3) == 0 || strncmp (text, "Off", 3) == 0)
        {
            *value = 0.0;
            return true;
        }
    }

    char *end = nullptr;
    const double parsed = std::strtod (text, &end);

    /* Nothing numeric at all: say so rather than reporting a successful parse of
       zero. */
    if (end == text)
        return false;

    double lo = 0.0;
    double hi = 1.0;
    double scaled = parsed;

    if (paramId == kParamDisplayOffset)
    {
        lo = -24.0;
        hi = 24.0;
    }
    else if (paramId == kParamOctave)
    {
        lo = -3.0;
        hi = 3.0;
    }
    else if (paramId == kParamBrightness || paramId == kParamUnlitLevel)
    {
        /* Shown as a percentage, so accept one. */
        scaled = parsed / 100.0;
    }
    else if (paramId == kParamFoldOctaves || paramId == kParamHighlight
             || paramId == kParamPressColour || paramId == kParamPressureGradient
             || paramId == kParamBendGradient)
    {
        scaled = parsed != 0.0 ? 1.0 : 0.0;
    }
    else
    {
        /* Every parameter is named above. Anything else is one added later and never
           classified here, and guessing it is a boolean would turn 0.2 into 1 - so say
           the text could not be converted instead. */
        return false;
    }

    if (scaled < lo)
        scaled = lo;

    if (scaled > hi)
        scaled = hi;

    *value = scaled;
    return true;
}

void paramsFlush (const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out)
{
    LumiPaint *self = (LumiPaint *) plugin->plugin_data;
    const uint32_t eventCount = in->size (in);

    for (uint32_t i = 0; i < eventCount; ++i)
        handleEvent (self, in->get (in, i));

    emitGuiParamChanges (self, out);
}

const clap_plugin_params_t s_params = {
    paramsCount, paramsGetInfo, paramsGetValue, paramsValueToText, paramsTextToValue, paramsFlush
};

bool stateSave (const clap_plugin_t *plugin, const clap_ostream_t *stream)
{
    LumiPaint *self = (LumiPaint *) plugin->plugin_data;
    uint32_t header[2] = { kStateMagic, kStateVersion };

    if (stream->write (stream, header, sizeof (header)) != (int64_t) sizeof (header))
        return false;

    uint32_t colours[128];

    for (int i = 0; i < 128; ++i)
        colours[i] = self->link.getColour (i);

    if (stream->write (stream, colours, sizeof (colours)) != (int64_t) sizeof (colours))
        return false;

    double values[kParamCount] = { self->brightness, self->unlitLevel, self->displayOffset,
                                   self->foldOctaves, self->highlight, self->pressColour,
                                   self->octave, self->pressureGradient,
                                   self->bendGradient };

    if (stream->write (stream, values, sizeof (values)) != (int64_t) sizeof (values))
        return false;

    const uint32_t highlightRgb = self->link.getHighlightColour();

    if (stream->write (stream, &highlightRgb, sizeof (highlightRgb)) != (int64_t) sizeof (highlightRgb))
        return false;

    const uint32_t pressRgb = self->link.getPressColour();

    if (stream->write (stream, &pressRgb, sizeof (pressRgb)) != (int64_t) sizeof (pressRgb))
        return false;

    /* Reserved. This was the block-span control, removed once blocks placed themselves
       in the chain. The slot stays so the state layout does not shift under projects
       saved before it went. */
    const uint32_t span = 0;

    if (stream->write (stream, &span, sizeof (span)) != (int64_t) sizeof (span))
        return false;

    const uint32_t gradients[48] = { self->link.getPressureGradColour(),
                                    self->link.getBendGradColour(),
                                    (uint32_t) self->link.getBendFullScale(),
                                    (uint32_t) ((self->link.getEnablePitchBend() ? 1 : 0)
                                                + (self->link.getEnablePressure() ? 2 : 0)
                                                + (self->link.getLinkOctaves() ? 4 : 0)),
                                    self->link.getRippleColour(),
                                    (uint32_t) ((self->link.getRippleEnabled() ? 128 : 0)
                                                + self->link.getRippleSpeed()),
                                    (uint32_t) self->link.getRippleTrail(),
                                    self->link.getSplashColour(),
                                    (uint32_t) ((self->link.getSplashEnabled() ? 128 : 0)
                                                + self->link.getSplashCC()),
                                    (uint32_t) self->link.getSplashSpeed(),
                                    (uint32_t) self->link.getSplashTrail(),
                                    self->link.getAfterglowColour(),
                                    (uint32_t) ((self->link.getAfterglowEnabled() ? 128 : 0)
                                                + self->link.getAfterglowDecay()),
                                    self->link.getPulseColour(),
                                    (uint32_t) (self->link.getPulseEnabled() ? 1 : 0),
                                    self->link.getHaloColour(),
                                    (uint32_t) (self->link.getHaloEnabled() ? 1 : 0),
                                    (uint32_t) ((self->link.getDegreeEnabled() ? 0x100 : 0)
                                                + self->link.getDegreeAlpha()),
                                    self->link.getDegreeScale(),
                                    self->link.getDegreeColour (0), self->link.getDegreeColour (1),
                                    self->link.getDegreeColour (2), self->link.getDegreeColour (3),
                                    self->link.getDegreeColour (4), self->link.getDegreeColour (5),
                                    self->link.getDegreeColour (6), self->link.getDegreeColour (7),
                                    (uint32_t) ((self->link.getTensionEnabled() ? 0x100 : 0)
                                                + self->link.getTensionAlpha()),
                                    self->link.getTensionHome(),
                                    self->link.getTensionFar(),
                                    (uint32_t) (self->link.getVelocityEnabled() ? 1 : 0),

                                    /*
                                        Two reserved words, so the indices below land
                                        where the loader reads them.

                                        They had drifted apart. The loader takes the
                                        send rate from word 33 and this list had it at
                                        31, so a reload read the bend-path colour as a
                                        send rate; ripple source and the whole waves
                                        setting were never written at all and came back
                                        zero every time. Nothing announced it, because
                                        every one of those has a plausible-looking
                                        default. Held open here rather than closed up,
                                        so the two lists stay aligned by position.
                                    */
                                    0u,
                                    0u,

                                    (uint32_t) self->link.getSendRate(),
                                    (uint32_t) (self->link.getBendPathEnabled() ? 1 : 0),
                                    self->link.getBendPathColour(),
                                    (uint32_t) self->link.getRippleSource(),
                                    (uint32_t) (((self->link.getWavesMode() & 7) << 11)
                                                + (self->link.getWavesEnabled() ? 0x400 : 0)
                                                + (self->link.getWavesDelay() & 0x3ff)),

                                    /* The paint gradient: how many stops are in use,
                                       then all eight regardless, so the array length
                                       never depends on the count. */
                                    (uint32_t) self->link.getGradientCount(),
                                    self->link.getGradientStop (0), self->link.getGradientStop (1),
                                    self->link.getGradientStop (2), self->link.getGradientStop (3),
                                    self->link.getGradientStop (4), self->link.getGradientStop (5),
                                    self->link.getGradientStop (6), self->link.getGradientStop (7),

                                    /* The zone, in one word: on, and the two notes.
                                       Seven bits each is exactly a MIDI note, so the
                                       range cannot encode something the keyboard could
                                       not address. */
                                    /* The offset rides in the spare bits of the same
                                       word, biased by 128 so a negative one survives.
                                       A state from before it existed reads zero, which
                                       is a zone that plays where it sits. */
                                    (uint32_t) ((self->link.getZoned() ? 0x4000 : 0)
                                                | ((self->link.getZoneLow() & 0x7f) << 7)
                                                | (self->link.getZoneHigh() & 0x7f)
                                                | (((self->link.getZoneOffset() + 128) & 0xff) << 15)) };

    if (stream->write (stream, gradients, sizeof (gradients)) != (int64_t) sizeof (gradients))
        return false;

    const std::string port = self->link.getRequestedPort();
    uint32_t length = (uint32_t) port.size();

    if (stream->write (stream, &length, sizeof (length)) != (int64_t) sizeof (length))
        return false;

    if (length == 0)
        return true;

    return stream->write (stream, port.data(), length) == (int64_t) length;
}

bool stateLoad (const clap_plugin_t *plugin, const clap_istream_t *stream)
{
    LumiPaint *self = (LumiPaint *) plugin->plugin_data;
    uint32_t header[2];

    if (stream->read (stream, header, sizeof (header)) != (int64_t) sizeof (header))
        return false;

    if (header[0] != kStateMagic || header[1] < 1 || header[1] > kStateVersion)
        return false;

    uint32_t colours[128];

    if (stream->read (stream, colours, sizeof (colours)) != (int64_t) sizeof (colours))
        return false;

    uint32_t valueCount = 4u;

    if (header[1] == 1)
        valueCount = 2u;
    else if (header[1] == 4)
        valueCount = 5u;
    else if (header[1] == 5 || header[1] == 6)
        valueCount = 6u;
    else if (header[1] == 7)
        valueCount = 7u;
    else if (header[1] >= 8)
        valueCount = kParamCount;

    double values[kParamCount] = { 1.0, 0.06, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    const int64_t wanted = (int64_t) (valueCount * sizeof (double));

    if (stream->read (stream, values, (uint64_t) wanted) != wanted)
        return false;

    for (int i = 0; i < 128; ++i)
        self->link.setColour (i, colours[i]);

    for (uint32_t id = 0; id < kParamCount; ++id)
        applyParamValue (self, id, values[id]);

    if (header[1] < 3)
        return true;

    if (header[1] >= 4)
    {
        uint32_t highlightRgb = 0xffffff;

        if (stream->read (stream, &highlightRgb, sizeof (highlightRgb)) != (int64_t) sizeof (highlightRgb))
            return false;

        self->link.setHighlightColour (highlightRgb);
    }

    if (header[1] >= 5)
    {
        uint32_t pressRgb = 0xff8000;

        if (stream->read (stream, &pressRgb, sizeof (pressRgb)) != (int64_t) sizeof (pressRgb))
            return false;

        self->link.setPressColour (pressRgb);
    }

    if (header[1] >= 6)
    {
        uint32_t span = 0;

        if (stream->read (stream, &span, sizeof (span)) != (int64_t) sizeof (span))
            return false;

        (void) span;   /* reserved, see above */
    }

    if (header[1] == 8)
    {
        uint32_t gradients[2] = { 0xffffff, 0x00c4ff };

        if (stream->read (stream, gradients, sizeof (gradients)) != (int64_t) sizeof (gradients))
            return false;

        self->link.setPressureGradColour (gradients[0]);
        self->link.setBendGradColour (gradients[1]);
    }
    else if (header[1] == 9)
    {
        uint32_t gradients[3] = { 0xffffff, 0x00c4ff, 2 };

        if (stream->read (stream, gradients, sizeof (gradients)) != (int64_t) sizeof (gradients))
            return false;

        self->link.setPressureGradColour (gradients[0]);
        self->link.setBendGradColour (gradients[1]);
        self->link.setBendFullScale ((int) gradients[2]);
    }
    else if (header[1] >= 25)
    {
        uint32_t extras[48] = { 0xffffff, 0x00c4ff, 2, 3, 0x00ffd6, 4, 2, 0xff7a1f, 1, 6, 3,
                                0xffd000, 8, 0x4060ff, 0, 0x30406a, 0,
                                170, 0xab5,
                                0xff3b30, 0x8a6a2a, 0xffd60a, 0x2a6a5a,
                                0x30d158, 0x3a4a8a, 0x9c6aff, 0x141414,
                                150, 0x2ea85e, 0xd02030, 0, 60, 0x9c6aff, 0, 0, 0x00c4ff, 0, 60 };

        /*
            Read what this state actually contains, not what the current build writes.

            Versions 25 and 26 wrote thirty-eight words here; 27 added nine more for the
            paint gradient. Reading the newer length from an older state succeeds - the
            stream has more bytes after this block - and silently swallows the first
            thirty-six bytes of the port name that follows, so every read after it is
            misaligned and the port length comes out of the middle of a colour. A
            project saved by an earlier build would fail to restore, which in a host
            that treats a failed state load as a failed plugin is indistinguishable
            from a crash.
        */
        const size_t words = header[1] >= 28 ? 48u : (header[1] >= 27 ? 47u : 38u);
        const size_t bytes = words * sizeof (uint32_t);

        if (stream->read (stream, extras, bytes) != (int64_t) bytes)
            return false;

        self->link.setPressureGradColour (extras[0]);
        self->link.setBendGradColour (extras[1]);
        self->link.setBendFullScale ((int) extras[2]);
        self->link.setEnablePitchBend ((extras[3] & 1u) != 0u);
        self->link.setEnablePressure ((extras[3] & 2u) != 0u);
        self->link.setLinkOctaves (header[1] < 18 || (extras[3] & 4u) != 0u);
        self->link.setRippleColour (extras[4]);
        self->link.setRippleEnabled ((extras[5] & 128u) != 0u);
        self->link.setRippleSpeed ((int) (extras[5] & 127u));
        self->link.setRippleTrail ((int) extras[6]);
        self->link.setSplashColour (extras[7]);
        self->link.setSplashEnabled ((extras[8] & 128u) != 0u);
        self->link.setSplashCC ((int) (extras[8] & 127u));
        self->link.setSplashSpeed ((int) extras[9]);
        self->link.setSplashTrail ((int) extras[10]);
        self->link.setAfterglowColour (extras[11]);
        self->link.setAfterglowEnabled ((extras[12] & 128u) != 0u);
        self->link.setAfterglowDecay ((int) (extras[12] & 127u));
        self->link.setPulseColour (extras[13]);
        self->link.setPulseEnabled (extras[14] != 0);
        self->link.setHaloColour (extras[15]);
        self->link.setHaloEnabled (extras[16] != 0);
        self->link.setDegreeEnabled ((extras[17] & 0x100u) != 0u);
        self->link.setDegreeAlpha ((int) (extras[17] & 0xffu));
        self->link.setDegreeScale (extras[18]);

        for (int i = 0; i < 8; ++i)
            self->link.setDegreeColour (i, extras[19 + i]);

        self->link.setTensionEnabled ((extras[27] & 0x100u) != 0u);
        self->link.setTensionAlpha ((int) (extras[27] & 0xffu));
        self->link.setTensionHome (extras[28]);
        self->link.setTensionFar (extras[29]);
        self->link.setVelocityEnabled ((extras[30] & 1u) != 0u);
        /*
            Only from a state that actually wrote them.

            Versions before 26 saved these words at the wrong indices, or not at all, so
            reading them back restores a send rate taken from a colour. An older state
            keeps the defaults for these five rather than inventing values from whatever
            happened to sit at those offsets.
        */
        if (header[1] >= 26)
        {
            self->link.setSendRate ((int) extras[33]);
            self->link.setBendPathEnabled (extras[34] != 0);
            self->link.setBendPathColour (extras[35]);
            self->link.setRippleSource ((int) extras[36]);
            self->link.setWavesEnabled ((extras[37] & 0x400u) != 0u);
            self->link.setWavesDelay ((int) (extras[37] & 0x3ffu));
            /* Three bits, not two. Six patterns no longer fit in two, and widening it is
               safe without a version bump: states written when there were four put zero
               in the third bit, so they read back the same value either way. */
            self->link.setWavesMode ((int) ((extras[37] >> 11) & 7u));
        }

        if (header[1] >= 27)
        {
            self->link.setGradientCount ((int) extras[38]);

            for (int i = 0; i < kGradientStops; ++i)
                self->link.setGradientStop (i, extras[39 + i]);
        }

        /*
            The zone, claimed rather than simply restored.

            Reopening a project means every instance asks for its range again, and the
            table is empty at that point, so they all get what they had. If something
            else has taken the range in the meantime - another project already open on
            the same machine - the claim fails and this instance comes back un-zoned
            rather than silently lighting keys somebody else owns.
        */
        if (header[1] >= 28)
        {
            const uint32_t packed = extras[47];
            const int low = (int) ((packed >> 7) & 0x7f);
            const int high = (int) (packed & 0x7f);

            if ((packed & 0x4000u) != 0u)
            {
                ZoneInfo blocker;
                self->link.setZoned (true);

                if (self->link.setZoneRange (low, high, blocker))
                {
                    const int biased = (int) ((packed >> 15) & 0xff);
                    self->link.setZoneOffset (biased == 0 ? 0 : biased - 128);
                }
                else
                {
                    self->link.setZoned (false);
                }
            }
        }
    }

    uint32_t length = 0;

    if (stream->read (stream, &length, sizeof (length)) != (int64_t) sizeof (length))
        return false;

    if (length > 512)
        return false;

    if (length == 0)
    {
        self->link.setRequestedPort (std::string());
        return true;
    }

    std::string port (length, '\0');

    if (stream->read (stream, &port[0], length) != (int64_t) length)
        return false;

    self->link.setRequestedPort (port);
    return true;
}

const clap_plugin_state_t s_state = { stateSave, stateLoad };

const void *pluginGetExtension (const clap_plugin_t *plugin, const char *id)
{
    (void) plugin;

    if (std::strcmp (id, CLAP_EXT_AUDIO_PORTS) == 0)
        return &s_audioPorts;

    if (std::strcmp (id, CLAP_EXT_NOTE_PORTS) == 0)
        return &s_notePorts;

    if (std::strcmp (id, CLAP_EXT_PARAMS) == 0)
        return &s_params;

    if (std::strcmp (id, CLAP_EXT_STATE) == 0)
        return &s_state;

    if (std::strcmp (id, CLAP_EXT_GUI) == 0)
        return &s_gui;

    return nullptr;
}

void pluginOnMainThread (const clap_plugin_t *plugin)
{
    (void) plugin;
}

const char *s_features[] = { CLAP_PLUGIN_FEATURE_NOTE_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, nullptr };

const clap_plugin_descriptor_t s_descriptor = {
    CLAP_VERSION_INIT,
    "net.lumipaint.lumipaint",
    "LumiPaint",
    "Simon",
    "",
    "",
    "",
    kPluginVersion,
    "Per-note colour display for ROLI LUMI Keys",
    s_features
};

const clap_plugin_t *createPlugin (const clap_plugin_factory_t *factory, const clap_host_t *host, const char *id)
{
    (void) factory;

    if (std::strcmp (id, s_descriptor.id) != 0)
        return nullptr;

    LumiPaint *self = new LumiPaint();
    self->capture = new KeybedCapture();
    std::memset (self->refCount, 0, sizeof (self->refCount));
    self->litBits[0] = 0;
    self->litBits[1] = 0;
    self->brightness = 1.0;
    self->unlitLevel = 0.5;
    self->displayOffset = 0.0;
    self->foldOctaves = 0.0;
    self->highlight = 0.0;
    self->pressColour = 0.0;
    self->octave = 0.0;
    self->pressureGradient = 0.0;
    self->bendGradient = 0.0;
    self->hostParams = nullptr;
    self->hostState = nullptr;
    self->hostGui = nullptr;
    self->editor = nullptr;
    self->lastBeat = INT64_MIN;
    self->probeNoteOn.store (-1, std::memory_order_relaxed);
    self->probeNoteOff.store (-1, std::memory_order_relaxed);
    self->host = host;
    self->plugin.desc = &s_descriptor;
    self->plugin.plugin_data = self;
    self->plugin.init = pluginInit;
    self->plugin.destroy = pluginDestroy;
    self->plugin.activate = pluginActivate;
    self->plugin.deactivate = pluginDeactivate;
    self->plugin.start_processing = pluginStartProcessing;
    self->plugin.stop_processing = pluginStopProcessing;
    self->plugin.reset = pluginReset;
    self->plugin.process = pluginProcess;
    self->plugin.get_extension = pluginGetExtension;
    self->plugin.on_main_thread = pluginOnMainThread;
    return &self->plugin;
}

uint32_t factoryGetPluginCount (const clap_plugin_factory_t *factory)
{
    (void) factory;
    return 1;
}

const clap_plugin_descriptor_t *factoryGetPluginDescriptor (const clap_plugin_factory_t *factory, uint32_t index)
{
    (void) factory;
    return index == 0 ? &s_descriptor : nullptr;
}

const clap_plugin_factory_t s_factory = {
    factoryGetPluginCount, factoryGetPluginDescriptor, createPlugin
};

bool entryInit (const char *path)
{
    (void) path;
    return true;
}

void entryDeinit()
{
}

const void *entryGetFactory (const char *id)
{
    return std::strcmp (id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &s_factory : nullptr;
}

}

void pushGuiParam (LumiPaint *self, uint32_t paramId, double value, bool begin, bool end)
{
    if (paramId >= kParamCount)
        return;

    GuiParamSlot &slot = self->guiParams[paramId];

    if (begin)
        slot.beginPending.store (true, std::memory_order_release);

    slot.value.store (value, std::memory_order_relaxed);
    slot.valuePending.store (true, std::memory_order_release);

    if (end)
        slot.endPending.store (true, std::memory_order_release);

    if (self->hostParams != nullptr)
        self->hostParams->request_flush (self->host);
}

}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    lumipaint::entryInit,
    lumipaint::entryDeinit,
    lumipaint::entryGetFactory
};
