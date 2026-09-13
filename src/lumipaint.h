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

#pragma once

#include <clap/clap.h>

#include "device_claim.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lumipaint {

const uint8_t kControlChannel = 15;
const uint8_t kCcBrightness   = 106;
const uint8_t kCcUnlitLevel   = 107;
const uint8_t kCcCommand      = 108;
const uint8_t kCcDisplayOffset = 109;
const uint8_t kCcFoldMode      = 110;
const uint8_t kCcHighlightRed  = 113;
const uint8_t kCcHighlightGreen= 114;
const uint8_t kCcHighlightBlue = 115;
const uint8_t kCcHighlightOn   = 116;
const uint8_t kCcPressRed      = 117;
const uint8_t kCcPressGreen    = 118;
const uint8_t kCcPressBlue     = 119;
const uint8_t kCcPressOn       = 85;
const uint8_t kCcOctave        = 87;
const uint8_t kCcPressureGradRed   = 20;
const uint8_t kCcPressureGradGreen = 21;
const uint8_t kCcPressureGradBlue  = 22;
const uint8_t kCcBendGradRed       = 23;
const uint8_t kCcBendGradGreen     = 24;
const uint8_t kCcBendGradBlue      = 25;
const uint8_t kCcPressureGradOn    = 26;
const uint8_t kCcBendGradOn        = 27;
const uint8_t kCcConfigWriteItem   = 28;
const uint8_t kCcConfigWriteValue  = 29;
const uint8_t kCcConfigItem        = 30;
const uint8_t kCcConfigValue       = 31;
const uint8_t kCcClusterWidth      = 45;
const uint8_t kCcLinkOctaves       = 47;
const uint8_t kCcBlockPos          = 48;
const uint8_t kCcBlockLow          = 49;

/*
    Reports arrive as polyphonic aftertouch on the control channel.

    Not as control changes: one of those is indistinguishable from a real one, so
    filtering them out of the stream meant guessing, and letting them through meant a
    synth downstream being modulated by the keyboard describing its own layout. Poly
    aftertouch cannot be mistaken for a CC by anything, and it carries a note and a
    value, which is the shape of a report - so what took a pair of messages takes one.

    SysEx would be cleaner still and Littlefoot cannot send it: sendMIDI takes three
    bytes at most and a SysEx frame needs more.
*/
const uint8_t kReportStatus        = 0xa0;
const uint8_t kSlotWidth           = 100;
const uint8_t kSlotBase            = 101;
const uint8_t kSlotBlock           = 110;

/*
    Whether a slot number is one the device actually reports.

    Poly aftertouch can legitimately arrive without a note-on - nothing in MIDI forbids
    it - so "the note is not sounding" is not enough on its own to call a message a
    report. A stray poly aftertouch from another controller on channel 16 would have
    been swallowed.

    Requiring the slot to be one the device really uses closes that: the thirteen config
    items it watches, the two chain slots, and one per block. Everything else passes
    through, whether a note is held or not. Between the two tests, a message has to be
    about a note that is not sounding AND carry a slot this device would have sent, and
    that is as close to certain as MIDI allows.
*/
inline bool isReportSlot (uint8_t slot)
{
    /* Exactly the device's watched list, and no wider. A whitelist broader than what
       the device sends reopens the hole it exists to close. */
    static const uint8_t watched[] = { 0, 1, 2, 3, 4, 5, 9, 10, 13, 14, 15, 30, 32 };

    for (uint8_t item : watched)
        if (slot == item)
            return true;

    return slot == kSlotWidth || slot == kSlotBase
        || (slot >= kSlotBlock && slot < kSlotBlock + 5);
}
const uint8_t kCcWindowBase        = 46;
const uint8_t kCcBendFullScale     = 32;
const uint8_t kCcEnablePitchBend   = 43;
const uint8_t kCcEnablePressure    = 44;

const int kConfigUnknown = -1000;

const int kConfigMidiStartChannel = 0;
const int kConfigMidiUseMPE       = 2;
const int kConfigPitchBendRange   = 3;
const int kConfigOctave           = 4;
const int kConfigOctaveTopology   = 8;
const int kConfigTranspose        = 5;

inline bool isSignedConfig (int item)
{
    return item == kConfigOctave || item == kConfigTranspose;
}

const uint8_t kCmdClearColours = 0;
const uint8_t kCmdAllKeysOff   = 1;
const uint8_t kCmdResetColours = 2;

const uint32_t kStateMagic   = 0x4c554d31;
const uint32_t kStateVersion = 25;

enum ParamId
{
    kParamBrightness = 0,
    kParamUnlitLevel = 1,
    kParamDisplayOffset = 2,
    kParamFoldOctaves = 3,
    kParamHighlight = 4,
    kParamPressColour = 5,
    kParamOctave = 6,
    kParamPressureGradient = 7,
    kParamBendGradient = 8,
    kParamCount = 9
};

uint32_t defaultColourForNote (int note);

class LumiLink;

class ColourInputDecoder
{
public:
    ColourInputDecoder();

    void feed (LumiLink &link, uint8_t status, uint8_t data1, uint8_t data2);
    void reset();

private:
    int pendingNote;
    int red;
    int green;
    int blue;
    int mirrorItem;
    int pendingBlockPos;
};

class LumiLink
{
public:
    LumiLink();
    ~LumiLink();

    void start();
    void stop();

    void setColour (int note, uint32_t rgb);
    uint32_t getColour (int note) const;

    /* What is actually on the hardware: the painted colour with every running effect
       composited over it. getColour returns the painted table, which is what editing
       and saving work against. */
    uint32_t getDisplayColour (int note) const;
    void publishLitBits (uint64_t low, uint64_t high);
    void setExternalLit (int note, bool isLit);
    void clearExternalLit();
    void setBrightness (double normalised);
    void setUnlitLevel (double normalised);
    void setDisplayOffset (double semitones);
    void setFoldOctaves (bool shouldFold);
    void setHighlightEnabled (bool enabled);
    void setHighlightColour (uint32_t rgb);
    uint32_t getHighlightColour() const;
    void setPressEnabled (bool enabled);
    void setPressColour (uint32_t rgb);
    uint32_t getPressColour() const;
    void setOctave (int octave);
    void applyMirroredConfig (int item, int value);
    void setClusterWidth (int blocks);
    void setWindowBase (int note);
    int getWindowLow() const;
    int getWindowHigh() const;
    int getBlockCount() const;

    /* How many control changes have arrived from the device on the control channel,
       and which one came last. Shown in the editor because "nothing is happening" and
       "the wrong thing is happening" look identical otherwise, and telling them apart
       has taken several rounds of guessing. */
    int getMessagesIn() const { return messagesIn.load (std::memory_order_relaxed); }
    int getLastCcIn() const { return lastCcIn.load (std::memory_order_relaxed); }
    void countMessageIn (int cc);


    /* Whether a note is currently sounding, from this plugin's own view of what is
       held. Used to tell a report apart from real key pressure. */
    bool isNoteSounding (int note) const;

    /* Counted separately from the host's, because the two arrive by completely
       different routes and only one of them has ever worked. */
    int getDirectIn() const { return directIn.load (std::memory_order_relaxed); }
    int getLastDirect() const { return lastDirect.load (std::memory_order_relaxed); }

    /* Each block reports what it is actually showing, so the editor can draw the real
       arrangement instead of assuming blocks must be laid end to end. */
    void setBlockRange (int position, int lowNote);
    int getBlockLow (int position) const;

    /*
        Whether any connected block can show this note.

        Asked block by block rather than as one span. Two blocks are not necessarily
        adjacent - unlinked they sit wherever their own octaves put them - so treating
        the keyboard as one contiguous range dropped everything in the gap between them
        and everything above the second one. Effects appeared to stop partway up, at a
        point that moved when the blocks did, which is exactly what a wrong span looks
        like.
    */
    bool isVisible (int note) const;

    /*
        The span actually sent, recomputed from where the blocks are.

        The rule this enforces: a note under any block is sent, always. Nothing about
        the chain's shape may cause a block to go dark - a block showing nothing is a
        broken keyboard, and no saving in message traffic is worth that.

        Recomputed rather than widened. An earlier attempt widened the span on each
        report, which never narrowed again once a block had been somewhere; and an
        attempt to test each block separately hid any block that had not reported.
        Recomputing from the full set, with unreported blocks assumed to sit where a
        chain would put them, does neither.
    */
    void recomputeSendRange();

    int getDeviceConfig (int item) const;
    void writeDeviceConfig (int item, int value);
    void setPressureGradEnabled (bool enabled);
    void setPressureGradColour (uint32_t rgb);
    uint32_t getPressureGradColour() const;
    void setBendGradEnabled (bool enabled);
    void setBendGradColour (uint32_t rgb);
    uint32_t getBendGradColour() const;
    void setBendFullScale (int steps);
    int getBendFullScale() const;

    /* Ripple and splash are animated here, not on the device. The plugin already owns
       the colour table, so a wave is just that table being recomposited each tick -
       the existing diff then sends only the notes that actually changed. */
    void setRippleEnabled (bool on);
    bool getRippleEnabled() const;
    void setRippleColour (uint32_t rgb);
    uint32_t getRippleColour() const;
    void setRippleSpeed (int speed);
    int getRippleSpeed() const;
    /*
        Where a ripple takes its colour from.

        Fixed is the swatch beside the control. The others derive it from the note that
        started the wave, so playing different notes throws differently coloured waves
        and the keyboard says something about what you played rather than just that you
        played. The map option is the most useful of them: the wave carries the colour
        of the key it came from, whatever that key happens to be - a drum map, an
        imported keyswitch layout, a scale.
    */
    enum RippleColourSource
    {
        kRippleFixed = 0,
        kRippleWheel,
        kRippleFifths,
        kRippleDegree,
        kRippleMap
    };

    void setRippleSource (int source);
    int getRippleSource() const;

    void setRippleTrail (int keys);
    int getRippleTrail() const;
    void setSplashEnabled (bool on);
    bool getSplashEnabled() const;
    void setSplashColour (uint32_t rgb);
    uint32_t getSplashColour() const;
    void setSplashCC (int cc);
    int getSplashCC() const;
    void setSplashSpeed (int speed);
    int getSplashSpeed() const;
    void setSplashTrail (int keys);
    int getSplashTrail() const;
    void setAfterglowEnabled (bool on);
    bool getAfterglowEnabled() const;
    void setAfterglowColour (uint32_t rgb);
    uint32_t getAfterglowColour() const;
    void setAfterglowDecay (int tenths);
    int getAfterglowDecay() const;

    void setPulseEnabled (bool on);
    bool getPulseEnabled() const;
    void setPulseColour (uint32_t rgb);
    uint32_t getPulseColour() const;

    /*
        Degrees.

        The last note played becomes the root, and every key is then coloured by its
        interval from it, snapped to a scale shape. Play a different root and the whole
        keyboard recolours - a modulation you can see rather than work out. The root is
        declared by playing, not guessed from a histogram, so it is right immediately
        and changes exactly when you mean it to.

        It sits above the painted or captured colours and below the transient effects,
        so it reads as a map while ripples and afterglow still show over it.
    */
    void setDegreeEnabled (bool on);
    bool getDegreeEnabled() const;
    void setDegreeAlpha (int alpha);
    int getDegreeAlpha() const;
    void setDegreeScale (uint32_t mask);
    uint32_t getDegreeScale() const;

    /*
        The key the scale is in, which is not the note you just played.

        These are two different things and conflating them was the bug. The scale mask
        has to be measured from the key - C major is the same seven notes whichever of
        them you press - while the degree and tension colours are measured from the note
        being played. Using the played note for both re-formed the scale shape from
        whatever you pressed, so pressing E in C major lit an E major scale.
    */
    void setScaleRoot (int pitchClass);
    int getScaleRoot() const;
    void setDegreeColour (int index, uint32_t rgb);
    uint32_t getDegreeColour (int index) const;
    int getDegreeRoot() const;
    void setDegreeRoot (int pitchClass);

    /*
        Tension.

        Colours every key by its distance from the root around the circle of fifths
        rather than by scale degree: home is warm, the tritone - six steps away in
        either direction, the furthest any note can be - is cool. It shows where the
        energy sits in a key, which is a real fact about the geometry rather than a
        decoration, and it is not visible any other way.
    */
    void setTensionEnabled (bool on);
    bool getTensionEnabled() const;
    void setTensionAlpha (int alpha);
    int getTensionAlpha() const;
    void setTensionHome (uint32_t rgb);
    uint32_t getTensionHome() const;
    void setTensionFar (uint32_t rgb);
    uint32_t getTensionFar() const;

    /* Velocity colouring: a hard note reads brighter and more saturated than a soft
       one, so the keyboard shows how you played rather than only what. */
    void setVelocityEnabled (bool on);
    bool getVelocityEnabled() const;
    void noteVelocity (int note, int velocity);

    /*
        Waves.

        Slow swells running along the keybed when nothing has been played for a while -
        deep navy through blue to a pale crest. It stops the instant a note arrives.
    */
    void setWavesEnabled (bool on);
    bool getWavesEnabled() const;
    void setWavesDelay (int seconds);
    int getWavesDelay() const;
    bool wavesRunning() const;

    /*
        Bend path.

        A bent note lights the keys between where it started and where it is heading,
        fading along the way. One path per held note, so a bent chord draws all of them.
    */
    void setBendPathEnabled (bool on);
    bool getBendPathEnabled() const;
    void setBendPathColour (uint32_t rgb);
    uint32_t getBendPathColour() const;
    void noteOnChannel (int channel, int note, bool on);
    void bendOnChannel (int channel, int value);
    void tuningOnChannel (int channel, double semitones);

    int getLastBendCents() const { return lastBendCents.load (std::memory_order_relaxed); }
    int getLastBendNote() const { return lastBendNote.load (std::memory_order_relaxed); }

    void setHaloEnabled (bool on);
    bool getHaloEnabled() const;
    void setHaloColour (uint32_t rgb);
    uint32_t getHaloColour() const;

    void triggerAfterglow (int note, int level);
    void triggerBeat (int level);
    void triggerRipple (int note, int level);
    void triggerSplash (int level);

    void setEnablePitchBend (bool on);
    bool getEnablePitchBend() const;
    void setEnablePressure (bool on);
    bool getEnablePressure() const;
    /* How hard to drive the link, 1 to 4. One block on USB takes far more than a
       relayed chain does, so the default follows how many blocks are connected. */
    void setSendRate (int rate);
    int getSendRate() const;

    void setLinkOctaves (bool on);
    bool getLinkOctaves() const;

    bool isConnected() const;
    bool hasDevice() const;
    bool heldElsewhere() const;
    void noteActivity();
    void claimDevice();
    void setHoldDevice (bool hold);
    bool getHoldDevice() const;
    bool isNoteLit (int note) const;
    bool hasExternalInput() const;

    void getAvailablePorts (std::vector<std::string> &destination) const;
    std::string getRequestedPort() const;
    std::string getActivePort() const;
    void setRequestedPort (const std::string &name);

private:
    struct Backend;

    static const uint32_t kUnsentColour = 0xffffffffu;

    void run();
    bool ensureConnection();
    void ensureInputConnection();

public:
    /* The input side, chosen explicitly.

       Matching the output port's name is a guess that fails whenever a keyboard
       presents its input under a different name, or the obvious one is already held by
       the host. Listing them and letting the port be picked turns that from a mystery
       into a choice. */
    std::vector<std::string> inputPortNames() const;
    void setInputPort (const std::string &name);
    std::string activeInputPort() const;

private:
    void invalidateCache();
    /* Both report whether the bytes actually left, so the caller can record what the
       device has rather than what it was asked for. */
    bool sendRaw (unsigned char *bytes, size_t length);
    void sendCC (uint8_t controller, uint8_t value);
    void sendKeyNote (uint8_t note, bool isOn);
    void flushGlobals();
    void flushKeys();
    void flushColours();
    void interruptibleSleep (int totalMs);
    void deviceMayHaveChanged();
    void flushConfigWrites();
    bool writeColour (int note, uint32_t rgb);
    /*
        Animation advances by elapsed time, not by tick.

        The worker asks to sleep 4 ms; on Windows it gets about 15.6 unless something
        has raised the timer resolution, and the editor only does that while it is open.
        Everything here used to advance by a fixed amount per tick, so a tick four times
        longer than intended ran every animation at a quarter speed - which is why a
        ripple crawled on the hardware while the editor, redrawing on its own clock,
        showed it moving properly. Scaling by the milliseconds that actually passed
        makes the speed the same whatever the tick turns out to be.
    */
    void advanceRipples (int elapsedMs);
    void advanceGlow (int elapsedMs);
    void compositeColours();
    void shutdownDevice();

    void refreshPorts();
    bool matchesRequest (const std::string &portName, const std::string &request) const;
    void handleIncoming (const unsigned char *bytes, size_t length);
    static void incomingCallback (double timeStamp, std::vector<unsigned char> *message, void *userData);

    std::unique_ptr<Backend> backend;
    ColourInputDecoder portDecoder;

    mutable std::mutex portsMutex;
    std::vector<std::string> availablePorts;
    std::string requestedPort;
    std::string activePort;
    std::atomic<bool> selectionChanged;
    int refreshCountdown;

    std::thread worker;
    std::atomic<bool> running;
    std::atomic<bool> connected;
    std::atomic<bool> externalInput;

    std::atomic<uint32_t> baseColour[128];
    std::atomic<uint32_t> desiredColour[128];

    /* Twelve, so a two-handed chord does not run out of slots. */
    static const int kMaxRipples = 12;

    /* The trigger queue is separate from the ripple slots on purpose. Sharing an index
       between them meant a new note landed on a slot that was still animating and wiped
       it, so chords and arpeggios silently lost ripples. */
    static const uint32_t kTriggerSlots = 32;
    std::atomic<uint32_t> triggerRing[kTriggerSlots];
    std::atomic<uint32_t> triggerWrite;
    uint32_t triggerRead;

    /* Speed and trail are captured per ripple when it starts, so a splash can move and
       fade differently from a note ripple while both are running. */
    int rippleNote[kMaxRipples];
    int rippleAge[kMaxRipples];
    uint32_t rippleTint[kMaxRipples];
    int rippleLevel[kMaxRipples];
    int rippleStep[kMaxRipples];
    int rippleTrail16[kMaxRipples];
    int activeRipples;

    std::atomic<int> rippleEnabled;
    std::atomic<uint32_t> rippleColour;
    std::atomic<int> rippleSpeed;
    std::atomic<int> rippleTrail;
    std::atomic<int> rippleSource;

    /* Afterglow: a struck key holds its hit colour and fades back to its own. Only
       ever touches keys that were played, so it is the cheapest of the three. */
    std::atomic<int> glowTrigger[128];
    int glowLevel[128];
    int activeGlow;
    std::atomic<int> afterglowEnabled;
    std::atomic<uint32_t> afterglowColour;
    std::atomic<int> afterglowDecay;

    /* Beat pulse: tempo comes from the host transport, which the device cannot see.
       The level is quantised as it decays so most ticks produce no change at all -
       otherwise every visible key would be rewritten on every tick after each beat. */
    std::atomic<int> pendingBeatLevel;
    int pulseLevel;
    std::atomic<int> pulseEnabled;
    std::atomic<uint32_t> pulseColour;

    /* Chord halo: with two or more notes held, the same pitch classes light in the
       other octaves. Changes only when the chord does. */
    /* Eight slots: scale degrees one to seven, then anything outside the scale. */
    std::atomic<int> degreeEnabled;
    std::atomic<int> degreeAlpha;
    std::atomic<uint32_t> degreeScale;
    std::atomic<int> scaleRoot;
    std::atomic<int> degreeRoot;
    std::atomic<uint32_t> degreeColour[8];

    std::atomic<int> tensionEnabled;
    std::atomic<int> tensionAlpha;
    std::atomic<uint32_t> tensionHome;
    std::atomic<uint32_t> tensionFar;

    std::atomic<int> velocityEnabled;
    std::atomic<int> noteVelocities[128];

    std::atomic<int> wavesEnabled;
    std::atomic<int> wavesDelay;
    std::atomic<int> idleMs;
    int wavePhase;

    std::atomic<int> bendPathEnabled;
    std::atomic<uint32_t> bendPathColour;
    std::atomic<int> channelNote[16];

    /* Which channel each held note is on, so every held note gets its own path rather
       than only the last one to arrive. */
    std::atomic<int> noteChannel[128];
    std::atomic<int> channelBend[16];
    std::atomic<int> lastBendCents;
    std::atomic<int> lastBendNote;


    std::atomic<int> haloEnabled;
    std::atomic<uint32_t> haloColour;
    std::atomic<int> splashEnabled;
    std::atomic<uint32_t> splashColour;
    std::atomic<int> splashCC;
    std::atomic<int> splashSpeed;
    std::atomic<int> splashTrail;

    /* A CC sweep sends a message per step. Left unthrottled that spawns a splash per
       message and starves the note ripples, so the audio thread only records the most
       recent value and the worker starts at most one splash per cooldown. */
    std::atomic<int> pendingSplashLevel;
    int splashCooldown;
    std::atomic<uint64_t> litBits[2];
    std::atomic<uint64_t> externalLitBits[2];
    std::atomic<int> brightness;
    std::atomic<int> unlitLevel;
    std::atomic<int> displayOffset;
    std::atomic<int> foldOctaves;
    std::atomic<int> highlightEnabled;
    std::atomic<uint32_t> highlightColour;
    std::atomic<int> pressEnabled;
    std::atomic<uint32_t> pressColour;
    std::atomic<int> octave;
    /* Which notes the hardware can actually show. Reported by the device, so adding a
       block or changing octave narrows or moves the window with no configuring. Until
       it reports, the whole range is assumed - correct, just wasteful. */
    std::atomic<int> windowLow;
    std::atomic<int> windowHigh;
    std::atomic<int> blockCount;
    std::atomic<int> messagesIn;
    std::atomic<int> lastCcIn;
    std::atomic<int> directIn;
    std::atomic<int> lastDirect;
    std::string requestedInput;
    std::string openedInput;
    mutable std::mutex inputMutex;
    std::atomic<int> blockLow[5];
    std::atomic<int> sendLow;
    std::atomic<int> sendHigh;


    std::atomic<int> deviceConfig[128];
    std::atomic<int> pendingConfigWrite[128];
    std::atomic<int> pressureGradEnabled;
    std::atomic<uint32_t> pressureGradColour;
    std::atomic<int> bendGradEnabled;
    std::atomic<uint32_t> bendGradColour;
    std::atomic<int> bendFullScale;
    std::atomic<int> enablePitchBend;
    std::atomic<int> enablePressure;
    std::atomic<int> linkOctaves;
    std::atomic<int> sendRate;

    /* One keyboard, potentially a LumiPaint on every track. Ownership follows
       activity: the instance being played takes the device and the others let go. */
    DeviceClaim claim;
    std::atomic<int> activityPending;
    std::atomic<int> holdDevice;

    uint32_t sentColour[128];
    uint64_t sentLitBits[2];
    int sentBrightness;
    int sentUnlitLevel;
    int sentDisplayOffset;
    int sentFoldOctaves;
    int sentHighlightEnabled;
    uint32_t sentHighlightColour;
    int sentPressEnabled;
    uint32_t sentPressColour;
    int sentOctave;
    int sentPressureGradEnabled;
    uint32_t sentPressureGradColour;
    int sentBendGradEnabled;
    uint32_t sentBendGradColour;
    int sentBendFullScale;
    int sentEnablePitchBend;
    int sentEnablePressure;
    int sentLinkOctaves;
    int deviceSelectedNote;
    bool wasOwner;
    int refreshCursor;
    int dirtyCursor;

    /* How many ticks each note has been held back by the visible-change threshold.
       Bounded, so a note can be deferred but never abandoned. */
    uint8_t skipped[128];
    std::chrono::steady_clock::time_point lastTick;
    int lastTickMs;
    int colourRefreshCountdown;
    int globalRefreshCountdown;
    int keyMessagesThisTick;
};

struct GuiParamSlot
{
    std::atomic<bool> valuePending;
    std::atomic<bool> beginPending;
    std::atomic<bool> endPending;
    std::atomic<double> value;

    GuiParamSlot()
        : valuePending (false), beginPending (false), endPending (false), value (0.0)
    {
    }
};

struct LumiPaint
{
    clap_plugin_t plugin;
    const clap_host_t *host;
    LumiLink link;

    int refCount[128];
    uint64_t litBits[2];
    double brightness;
    double unlitLevel;
    double displayOffset;
    double foldOctaves;
    double highlight;
    double pressColour;
    double octave;
    double pressureGradient;
    double bendGradient;

    GuiParamSlot guiParams[kParamCount];
    const clap_host_params_t *hostParams;
    const clap_host_state_t *hostState;
    const clap_host_gui_t *hostGui;
    void *editor;
    ColourInputDecoder hostDecoder;

    /* A note the editor asks to be played, so the plugin downstream lights the key it
       corresponds to. Emitted from process() like any other note. */
    std::atomic<int> probeNoteOn;
    std::atomic<int> probeNoteOff;
    int64_t lastBeat;
};

void pushGuiParam (LumiPaint *self, uint32_t paramId, double value, bool begin, bool end);

extern const clap_plugin_gui_t s_gui;

/* Called from pluginDestroy, in case the host never called gui.destroy. The
 * spec says it should and every host I know of does - but a window and a
 * running timer whose procedures live in a library about to be unloaded are
 * not something to leave to good manners. */
void guiShutdown (const clap_plugin_t *plugin);

}
