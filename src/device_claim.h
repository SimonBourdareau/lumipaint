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

#include <cstdint>
#include <string>
#include <string>

namespace lumipaint {

/* Room for a whole look: 128 note lines plus the effect colours and settings. A map
   that does not fit is refused rather than truncated - half a look pasted silently
   would be worse than a message. */
const int kClipBytes = 8192;

/* Sixteen instances dividing one chain is already more tracks than anyone will point at
   one keyboard; the table is small enough that a generous ceiling costs nothing. */
const int kMaxZones = 16;

/* Effects in flight across the whole chain at once. Ripples expire in under a second,
   so this only has to cover a busy chord, not a performance. */
const int kEffectSlots = 64;

struct ZoneInfo
{
    uint32_t owner;
    int low;
    int high;
};

/*
    The owner id that means "the zoned instances, together".

    A hive is one claimant, not several. Instances that have taken a zone do not compete
    with each other for the keyboard - they compete as a group against any instance that
    has not, and the group either holds the device or does not. That is what keeps the
    half-configured case out of existence: there is no state where one zoned track is
    lit and another is dark because they outbid each other.
*/
const uint32_t kHiveOwner = 0xffffffffu;

/*
    Which instance owns the keyboard.

    There is one keyboard and there can be a LumiPaint on every track. Without
    arbitration each instance opens the MIDI port on its own: on Windows the port opens
    exclusively, so whichever instance loaded first wins and every other one sits at
    "not connected" - which is almost never the track being worked on. Elsewhere they
    all open it and fight, and the colours flicker between whatever each is sending.

    Ownership is claimed by activity rather than assigned. Receiving notes or opening
    the editor claims the device, the previous owner notices it has lost the claim and
    closes its port, and the new owner opens it. Playing a track is what puts that
    track's colours on the keyboard, which is the behaviour that needs no explaining.

    The claim lives in shared memory named for the machine session, so it works across
    separate plugin instances in one host and across separate hosts.
*/
class DeviceClaim
{
public:
    DeviceClaim();
    ~DeviceClaim();

    /* This instance is being used - take the device, unless someone is holding it. */
    /*
        Zoning mode, which changes who the claim is made for.

        A zoned instance claims on behalf of the hive rather than for itself, so
        activity on any zoned track keeps the whole group on the keyboard. An un-zoned
        instance claims for itself as before, and taking the device that way stands the
        hive down - its zones are untouched, it simply stops sending until it wins back.
    */
    void setZoned (bool zoned);

    /* True when this instance should be the one opening the port. For a solo owner that
       is itself; for a hive it is the lowest live zone owner, which every member works
       out from the same table and so agrees on without anyone being told. */
    bool isSender() const;

    bool hiveOwnsDevice() const;

    void claim();

    /*
        Hold, and let go.

        A holder cannot be displaced by activity. Without this, Hold announced itself
        correctly and then lost the device to the next note played anywhere else,
        because claiming happened on activity regardless of who was holding - so the
        other instance would report "another instance has the keyboard" and overwrite
        it a moment later anyway.
    */
    void hold();
    void unhold();

    /* Renewed while Hold stays ticked. A hold that stops being renewed belongs to an
       instance that is gone, and expires like ownership does. */
    void renewHold();
    bool holdIsLive() const;
    bool heldBySomeoneElse() const;

    /* True when this instance currently holds it. Instances that do not hold it must
       close their port: on Windows they could not open it anyway, and elsewhere two
       senders on one port produce flickering nonsense. */
    bool isOwner() const;

    /* Give it up without waiting to be displaced - on editor close or unload. */
    void release();

    uint32_t ownerId() const;
    uint32_t selfId() const { return myId; }

    /*
        What the hardware is currently showing, shared between every instance.

        Handing the keyboard from one track to another used to mean the new owner knew
        nothing about the device and re-sent all 128 notes - half a second of the
        keyboard being wrong on every switch. Publishing each write here means the next
        owner starts from what is actually on the device and sends only the difference,
        so switching tracks changes the handful of keys that differ and nothing else.

        Stale entries are self-correcting: the background sweep rewalks the table every
        couple of seconds regardless.
    */
    /*
        Copy and paste between instances, carrying the same text a .lumimap holds.

        Shared memory rather than the system clipboard: this is only ever read by
        another LumiPaint, the format is ours, and putting several kilobytes of plugin
        state on the user's clipboard would be rude.
    */
    bool writeClipboard (const std::string &text);
    bool readClipboard (std::string &text) const;
    uint32_t clipboardSeq() const;


    /*
        Zones: which instance owns which notes.

        claimZone takes the range or refuses it, and refuses only for overlap with a
        live zone belonging to somebody else. The caller finds out which range it
        collided with so the editor can point at it rather than saying no.
    */
    bool claimZone (int low, int high, ZoneInfo &blocker);
    void releaseZone();
    void renewZone();
    bool zoneAt (int index, ZoneInfo &info) const;
    bool myZone (ZoneInfo &info) const;

    /* Each member's zone as it looks before effects; the sender assembles all of them
       and draws the effects itself. */
    /* A note arrived at this instance, and whether that has stopped happening while it
       is still happening to everyone else. */
    void noteSeen();
    bool zoneStarved() const;

    void publishZoneColour (int note, uint32_t rgb);
    void clearZoneColours (int low, int high);
    bool readZoneColours (uint32_t *dest) const;

    /* Effects as events carrying their own parameters, so whoever draws them does not
       need anybody's settings. */
    void postEffect (uint64_t packed);
    bool takeEffect (int slot, uint64_t &packed);

    void publishColour (int note, uint32_t rgb);
    bool adoptDeviceState (uint32_t *dest) const;
    void forgetDeviceState();

private:
    struct Shared;

    bool zoneIsLive (int index) const;

    void *mapping;
    Shared *state;
    uint32_t myId;
    bool inHive;
};

}
