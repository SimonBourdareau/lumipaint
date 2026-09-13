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
        noteChannel[i].store (-1, std::memory_order_relaxed);

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

uint32_t LumiLink::getDisplayColour (int note) const
{
    if (note < 0 || note > 127)
        return 0;

    return desiredColour[note].load (std::memory_order_relaxed);
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
    if (source < 0) source = 0;
    if (source > kRippleMap) source = kRippleMap;
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
void LumiLink::triggerRipple (int note, int level)
{
    if (rippleEnabled.load (std::memory_order_relaxed) == 0)
        return;

    if (note < 0 || note > 127)
        return;

    const uint32_t packed = 0x80000000u | ((uint32_t) note << 8) | (uint32_t) (level & 0xff);
    const uint32_t slot = triggerWrite.fetch_add (1, std::memory_order_relaxed);
    triggerRing[slot % kTriggerSlots].store (packed, std::memory_order_release);
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

bool LumiLink::wavesRunning() const
{
    if (wavesEnabled.load (std::memory_order_relaxed) == 0)
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
void LumiLink::noteOnChannel (int channel, int note, bool on)
{
    if (channel < 0 || channel > 15)
        return;

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
        const uint32_t packed = 0xc0000000u | (60u << 8) | (uint32_t) (splashLevel & 0xff);
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
            /* Worked out once, when the wave starts, from the note that threw it. */
            const int note = rippleNote[slot];
            const int pc = ((note % 12) + 12) % 12;

            switch (rippleSource.load (std::memory_order_relaxed))
            {
                case kRippleWheel:
                    rippleTint[slot] = hueColour (pc, 12);
                    break;

                case kRippleFifths:
                    rippleTint[slot] = hueColour ((pc * 7) % 12, 12);
                    break;

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

                    rippleTint[slot] = degreeColour[slotIndex].load (std::memory_order_relaxed);
                    break;
                }

                case kRippleMap:
                    rippleTint[slot] = baseColour[note & 127].load (std::memory_order_relaxed);
                    break;

                default:
                    rippleTint[slot] = rippleColour.load (std::memory_order_relaxed);
                    break;
            }
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
    int pathTo[128];

    if (bendPathOn != 0)
    {
        for (int n = 0; n < 128; ++n)
        {
            pathTo[n] = -1;

            const int ch = noteChannel[n].load (std::memory_order_relaxed);

            if (ch < 0 || ! isNoteSounding (n))
                continue;

            const int cents = channelBend[ch].load (std::memory_order_relaxed);
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
        const uint32_t base = baseColour[note].load (std::memory_order_relaxed);

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
        {
            /*
                Two swells of different length and speed, so the pattern never settles
                into an obvious repeat - one alone reads as a metronome.

                The phase is wrapped into range before use. It used to be taken modulo
                360 after a subtraction, and C++ modulo keeps the sign of the left
                operand: once the phase passed the note's offset the result went
                negative, squaring turned that trough into a crest, and the channels ran
                past their range. That is where the yellow came from, and why it only
                appeared after the thing had been running a while.
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
                anything went out of range it combined with green into yellow - which is
                the one colour a sea should not be. Without it the crest reaches a bright
                cyan-white instead, and nothing in the ramp can produce a warm colour at
                all, however the arithmetic behaves.
            */
            const int blue = 30 + (level * 225) / 255;
            const int green = level < 140 ? 0 : ((level - 140) * 235) / 115;

            result = ((uint32_t) green << 8) | (uint32_t) blue;
        }

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

        desiredColour[note].store (result, std::memory_order_relaxed);
    }
}

void LumiLink::publishLitBits (uint64_t low, uint64_t high)
{
    litBits[0].store (low, std::memory_order_release);
    litBits[1].store (high, std::memory_order_release);
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

        lastTickMs = elapsedMs;

        /* Idle time, reset by anything played. The waves ride on it. */
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
    const int wantUnlit = unlitLevel.load (std::memory_order_relaxed);

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
        sendCC (kCcBendGradOn, (uint8_t) (wantBgOn != 0 ? 1 : 0));
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
    /* The waves count as animation: they change every frame and the send budget has to
       allow for them or they crawl. */
    const bool animating = activeRipples > 0 || activeGlow > 0 || pulseLevel > 0
                        || wavesRunning();
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

void clearAllNotes (LumiPaint *self)
{
    for (int i = 0; i < 128; ++i)
        self->refCount[i] = 0;

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

        if (ev->expression_id == CLAP_NOTE_EXPRESSION_TUNING)
            self->link.tuningOnChannel (ev->channel < 0 ? 0 : ev->channel, ev->value);
    }
    else if (header->type == CLAP_EVENT_NOTE_ON)
    {
        const clap_event_note_t *ev = (const clap_event_note_t *) header;
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
            setNoteRef (self, ev->key, -1);
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
            setNoteRef (self, ev->data[1], -1);
            self->link.noteOnChannel (ev->data[0] & 0x0f, ev->data[1], false);
        }
        else if (status == 0xe0)
        {
            self->link.bendOnChannel (ev->data[0] & 0x0f,
                                      ((int) ev->data[2] << 7) | (int) ev->data[1]);
        }
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

    self->link.publishLitBits (self->litBits[0], self->litBits[1]);
    return CLAP_PROCESS_CONTINUE;
}

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

    const uint32_t gradients[38] = { self->link.getPressureGradColour(),
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
                                    (uint32_t) ((self->link.getVelocityEnabled() ? 1 : 0)
),

                                    (uint32_t) self->link.getSendRate(),
                                    (uint32_t) ((self->link.getBendPathEnabled() ? 1 : 0)),
                                    self->link.getBendPathColour() };

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
        uint32_t extras[38] = { 0xffffff, 0x00c4ff, 2, 3, 0x00ffd6, 4, 2, 0xff7a1f, 1, 6, 3,
                                0xffd000, 8, 0x4060ff, 0, 0x30406a, 0,
                                170, 0xab5,
                                0xff3b30, 0x8a6a2a, 0xffd60a, 0x2a6a5a,
                                0x30d158, 0x3a4a8a, 0x9c6aff, 0x141414,
                                150, 0x2ea85e, 0xd02030, 0, 60, 0x9c6aff, 0, 0, 0x00c4ff, 0, 60 };

        if (stream->read (stream, extras, sizeof (extras)) != (int64_t) sizeof (extras))
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
        self->link.setSendRate ((int) extras[33]);
        self->link.setBendPathEnabled (extras[34] != 0);
        self->link.setBendPathColour (extras[35]);
        self->link.setRippleSource ((int) extras[36]);
        self->link.setWavesEnabled ((extras[37] & 0x400u) != 0u);
        self->link.setWavesDelay ((int) (extras[37] & 0x3ffu));
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
