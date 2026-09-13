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

namespace lumipaint {

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
    void publishColour (int note, uint32_t rgb);
    bool adoptDeviceState (uint32_t *dest) const;
    void forgetDeviceState();

private:
    struct Shared;

    void *mapping;
    Shared *state;
    uint32_t myId;
};

}
