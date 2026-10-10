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

#include "device_claim.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstring>

#if defined (_WIN32)
 #include <windows.h>
#else
 #include <fcntl.h>
 #include <sys/mman.h>
 #include <unistd.h>
#endif

namespace lumipaint {

struct DeviceClaim::Shared
{
    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> owner;
    std::atomic<uint64_t> stamp;
    std::atomic<uint32_t> holder;

    /* The hold's own lease. Owner staleness was fixed and this was not, so a host that
       crashed while holding left a holder nobody could displace - permanently, since
       the shared block outlives the process. A hold is a stronger claim than ownership
       and so needs its expiry more, not less. */
    std::atomic<uint64_t> holderStamp;

    /* Top bit marks an entry as written, so an untouched table is not mistaken for a
       device showing black. */
    std::atomic<uint32_t> deviceColour[128];

    /*
        One clipboard, shared by every instance on the machine.

        Copying a look from one track to another had no route that did not go through a
        file: save a map, find it again, load it. The text here is exactly what a
        .lumimap holds, so it is written and read by the same two functions - a format
        that gains a field gains it here at the same moment, with nothing to keep in
        step.

        The sequence number is what makes the paste button know there is something to
        paste, and the length is stored rather than relying on a terminator, because a
        reader can arrive while a writer is partway through.
    */
    std::atomic<uint32_t> clipSeq;
    std::atomic<uint32_t> clipLength;
    char clipText[kClipBytes];

    /*
        Who owns which notes.

        Zones are in note numbers, not in blocks or key offsets, so they do not move
        when the octave buttons do. A chain showing two octaves at a time can be scrolled
        across a map that covers all 128, which is the point: four instances can divide
        an arrangement between them and the buttons walk the hardware along it.

        Overlap is refused rather than resolved. Two instances lighting one key is not a
        thing to arbitrate at render time - it is a mistake, and the moment to say so is
        when the second one asks. Checking is a scan of fifteen other entries, which
        happens when a person drags a field, not per frame.

        Each carries its own lease for the same reason ownership does: an instance that
        crashes holding keys 24 to 47 must not lock that stretch away from everyone for
        as long as the machine stays up.
    */
    std::atomic<uint32_t> zoneOwner[kMaxZones];
    std::atomic<uint64_t> zoneStamp[kMaxZones];
    std::atomic<int32_t> zoneLow[kMaxZones];
    std::atomic<int32_t> zoneHigh[kMaxZones];

    /*
        When each member last saw a note.

        A plugin cannot arm its own track, so a chain only works if the user lets MIDI
        reach every track in it - and when they have not, the symptom is a range of keys
        that simply stays dark. That is indistinguishable from a zone set wrongly, or
        from the plugin being broken, unless somebody notices that the other members are
        receiving and this one is not. Which is exactly what these let it notice.
    */
    std::atomic<uint64_t> zoneNoteStamp[kMaxZones];

    /*
        What each zone currently looks like, before anything moves.

        Members write only the notes inside their own range, so the table assembles
        itself: no instance has to know the shape of the chain, and the sender reads all
        128 without caring who filled which part. Top bit marks an entry as written, the
        same convention the device-state table uses, so an untouched key is not mistaken
        for one somebody painted black.
    */
    std::atomic<uint32_t> zoneColour[128];

    /*
        Effects in flight, as events rather than as pixels.

        A ripple started in one member's range has to carry on into the next, and the
        sender is the only instance in a position to draw it - but it has no business
        knowing anyone's ripple speed or trail length. So the event carries them: the
        instance that received the note resolves its own settings into sixty-four bits
        and posts that, and whoever is sending draws it exactly as its originator would
        have, across the whole chain.

        A ring rather than a queue, because a reader that falls behind should lose the
        oldest effects rather than the newest.
    */
    std::atomic<uint64_t> effectRing[kEffectSlots];
    std::atomic<uint32_t> effectWrite;
};

namespace {

/*
    Bumped when the shared block grew a clipboard.

    The block outlives every process that uses it, so an instance built before the
    clipboard existed and one built after would otherwise map the same memory with two
    different ideas of its shape. A new magic means the first instance to notice
    reinitialises it, which costs one forgotten ownership record and nothing else.
*/
const uint32_t kMagic = 0x4c554d44;

/* How long an owner may go without renewing before another instance may take the
   keyboard. The worker renews on every tick, so this is orders of magnitude longer
   than a healthy instance needs - it only expires a dead one. */
const uint64_t kLeaseMillis = 4000;

/*
    A zone outlives silence, because a zone is configuration rather than a claim.

    Ownership expires in seconds on purpose: it follows activity, and a dead owner must
    not hold the keyboard. A range is the opposite - it is a decision the user made, and
    it was expiring on the same four seconds. Any instance the host deactivated, or
    whose worker simply stopped ticking, lost its range to whoever asked next, which
    reads as one track wiping another's settings.

    Two minutes is far longer than any gap in a session and still short enough that a
    machine which has been rebooted does not come back to a table full of ranges owned
    by nobody.
*/
const uint64_t kZoneLeaseMillis = 120000;

uint64_t nowMillis()
{
    using namespace std::chrono;
    return (uint64_t) duration_cast<milliseconds> (
               steady_clock::now().time_since_epoch()).count();
}

}

#if defined (_WIN32)

DeviceClaim::DeviceClaim()
    : mapping (nullptr), state (nullptr), myId (0), inHive (false)
{
    /* Process id and address together: unique across instances in one host and across
       separate hosts, without needing a registry or a file on disk. */
    myId = (uint32_t) GetCurrentProcessId() ^ (uint32_t) (uintptr_t) this;

    if (myId == 0)
        myId = 1;

    HANDLE h = CreateFileMappingA (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                   0, sizeof (Shared), "Local\\LumiPaintDeviceClaim");

    if (h == nullptr)
        return;

    const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
    void *view = MapViewOfFile (h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (Shared));

    if (view == nullptr)
    {
        CloseHandle (h);
        return;
    }

    mapping = h;
    state = (Shared *) view;

    if (created || state->magic.load (std::memory_order_relaxed) != kMagic)
    {
        state->owner.store (0, std::memory_order_relaxed);
        state->stamp.store (0, std::memory_order_relaxed);
        state->holder.store (0, std::memory_order_relaxed);
        state->holderStamp.store (0, std::memory_order_relaxed);

        for (int i = 0; i < 128; ++i)
            state->deviceColour[i].store (0, std::memory_order_relaxed);

        state->clipSeq.store (0, std::memory_order_relaxed);
        state->clipLength.store (0, std::memory_order_relaxed);

        for (int i = 0; i < 128; ++i)
            state->zoneColour[i].store (0, std::memory_order_relaxed);

        for (int i = 0; i < kEffectSlots; ++i)
            state->effectRing[i].store (0, std::memory_order_relaxed);

        state->effectWrite.store (0, std::memory_order_relaxed);

        for (int i = 0; i < kMaxZones; ++i)
        {
            state->zoneOwner[i].store (0, std::memory_order_relaxed);
            state->zoneStamp[i].store (0, std::memory_order_relaxed);
            state->zoneLow[i].store (0, std::memory_order_relaxed);
            state->zoneHigh[i].store (0, std::memory_order_relaxed);
            state->zoneNoteStamp[i].store (0, std::memory_order_relaxed);
        }

        state->magic.store (kMagic, std::memory_order_release);
    }
}

DeviceClaim::~DeviceClaim()
{
    releaseZone();
    unhold();
    release();

    if (state != nullptr)
        UnmapViewOfFile (state);

    if (mapping != nullptr)
        CloseHandle ((HANDLE) mapping);
}

#else

/*
    The same claim, backed by a file the instances share.

    POSIX shared memory rather than a Win32 mapping, and otherwise identical: one small
    block holding who owns the keyboard and what it is currently showing. Written but
    not run - there is no Mac here to test on.
*/
DeviceClaim::DeviceClaim()
    : mapping (nullptr), state (nullptr), myId (0), inHive (false)
{
    myId = (uint32_t) getpid() ^ (uint32_t) (uintptr_t) this;

    if (myId == 0)
        myId = 1;

    const int fd = shm_open ("/LumiPaintDeviceClaim", O_CREAT | O_RDWR, 0600);

    if (fd < 0)
        return;

    if (ftruncate (fd, sizeof (Shared)) != 0)
    {
        close (fd);
        return;
    }

    void *view = mmap (nullptr, sizeof (Shared), PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
    close (fd);

    if (view == MAP_FAILED)
        return;

    state = (Shared *) view;

    if (state->magic.load (std::memory_order_relaxed) != kMagic)
    {
        state->owner.store (0, std::memory_order_relaxed);
        state->stamp.store (0, std::memory_order_relaxed);
        state->holder.store (0, std::memory_order_relaxed);
        state->holderStamp.store (0, std::memory_order_relaxed);

        for (int i = 0; i < 128; ++i)
            state->deviceColour[i].store (0, std::memory_order_relaxed);

        state->clipSeq.store (0, std::memory_order_relaxed);
        state->clipLength.store (0, std::memory_order_relaxed);

        for (int i = 0; i < 128; ++i)
            state->zoneColour[i].store (0, std::memory_order_relaxed);

        for (int i = 0; i < kEffectSlots; ++i)
            state->effectRing[i].store (0, std::memory_order_relaxed);

        state->effectWrite.store (0, std::memory_order_relaxed);

        for (int i = 0; i < kMaxZones; ++i)
        {
            state->zoneOwner[i].store (0, std::memory_order_relaxed);
            state->zoneStamp[i].store (0, std::memory_order_relaxed);
            state->zoneLow[i].store (0, std::memory_order_relaxed);
            state->zoneHigh[i].store (0, std::memory_order_relaxed);
        }

        state->magic.store (kMagic, std::memory_order_release);
    }
}

DeviceClaim::~DeviceClaim()
{
    releaseZone();
    unhold();
    release();

    if (state != nullptr)
        munmap (state, sizeof (Shared));
}

#endif

void DeviceClaim::setZoned (bool zoned)
{
    inHive = zoned;
}

/*
    Who sends.

    Solo: whoever owns it. Hive: the lowest live zone owner, read from the same table by
    every member, so they agree without electing anything. An instance leaving the hive
    changes the answer for everyone at once and the next lowest takes over on its next
    tick - there is nothing to hand over because nothing was held.
*/
bool DeviceClaim::isSender() const
{
    if (state == nullptr)
        return true;

    const uint32_t owner = state->owner.load (std::memory_order_acquire);

    /*
        A member of a chain follows the chain's rule whether or not anything has been
        played yet.

        This used to fall straight through to the solo answer whenever the device was
        unclaimed - and it is unclaimed until somebody plays a note. So on a freshly
        opened project every zoned instance believed it was the sender, every one took
        the sending path, and on a platform where the port is exclusive all but one of
        them spun on a connection they were never going to get. They never composited,
        so their editors showed stale colours and painting a key appeared to do nothing.

        Zoned, the answer is always the lowest live zone owner. The only thing that can
        override it is an un-zoned instance holding the device outright, which is the
        hive standing down.
    */
    if (inHive)
    {
        if (owner != 0 && owner != kHiveOwner)
            return false;
    }
    else if (owner != kHiveOwner)
    {
        return owner == 0 || owner == myId;
    }
    else
    {
        return false;
    }

    uint32_t lowest = 0;

    for (int i = 0; i < kMaxZones; ++i)
    {
        ZoneInfo info;

        if (! zoneAt (i, info))
            continue;

        if (lowest == 0 || info.owner < lowest)
            lowest = info.owner;
    }

    return lowest == 0 || lowest == myId;
}

bool DeviceClaim::hiveOwnsDevice() const
{
    return state != nullptr
            && state->owner.load (std::memory_order_acquire) == kHiveOwner;
}

void DeviceClaim::claim()
{
    if (state == nullptr)
        return;

    /* Somebody is holding it and it is not us. Activity does not override that - that
       is what holding means - unless the hold has expired, in which case whoever set it
       is gone. */
    const uint32_t holder = state->holder.load (std::memory_order_acquire);

    if (holder != 0 && holder != myId && holdIsLive())
        return;

    /* Renewing our own claim. The stamp is the lease: it says this owner is alive. */
    const uint32_t held = state->owner.load (std::memory_order_relaxed);

    if (held == myId || (inHive && held == kHiveOwner))
    {
        state->stamp.store (nowMillis(), std::memory_order_relaxed);
        return;
    }

    /*
        Taking it, atomically, and only from an owner that is free or has gone quiet.

        A plain store let two instances both read owner as free and both write
        themselves in. Each then believed it had won, and each opened the port and
        started sending - the precise race this class exists to prevent, and one that
        only appears with two tracks or two hosts running, which is what makes it so
        confusing when it does.

        The stale case matters as much. If a host crashes without releasing, the shared
        block survives with a dead owner in it and nobody could ever take over. The
        stamp was being written for exactly this and never read. An owner that has not
        renewed for several seconds is treated as gone - long enough that a busy or
        stalled instance is not robbed, short enough that a crash is not permanent.
    */
    /*
        Taken unconditionally, and atomically.

        Ownership follows activity: an instance receiving notes takes the keyboard, and
        the previous owner notices on its next tick and closes its port. That is the
        whole design, and a lease check here broke it - the owner renews every tick, so
        its lease was always fresh and no second instance could ever take over.

        A stale owner needs no special handling as a result: an instance being played
        simply takes the device, whether the previous owner is alive, busy or gone. The
        compare-exchange is still worth having, since without it two instances could
        both believe they had won and both open the port.

        The lease matters for the hold, which is the claim that does refuse to be
        displaced, and so is the one that must expire if its instance dies.
    */
    const uint64_t now = nowMillis();
    const uint32_t want = inHive ? kHiveOwner : myId;
    uint32_t current = state->owner.load (std::memory_order_acquire);

    while (! state->owner.compare_exchange_weak (current, want,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire))
    {
    }

    state->stamp.store (now, std::memory_order_release);
}

void DeviceClaim::hold()
{
    if (state == nullptr)
        return;

    state->holderStamp.store (nowMillis(), std::memory_order_release);
    state->holder.store (myId, std::memory_order_release);
    claim();
}

/* Renewed from the worker while Hold is ticked, the same way ownership is. A hold that
   stops being renewed is a hold whose instance has gone. */
void DeviceClaim::renewHold()
{
    if (state == nullptr)
        return;

    if (state->holder.load (std::memory_order_acquire) == myId)
        state->holderStamp.store (nowMillis(), std::memory_order_release);
}

/* True when somebody else's hold is still live. A stale one is ignored, and cleared so
   the next reader does not have to work it out again. */
bool DeviceClaim::holdIsLive() const
{
    if (state == nullptr)
        return false;

    uint32_t holder = state->holder.load (std::memory_order_acquire);

    if (holder == 0 || holder == myId)
        return holder == myId;

    const uint64_t now = nowMillis();
    const uint64_t seen = state->holderStamp.load (std::memory_order_acquire);

    if (now >= seen && now - seen >= kLeaseMillis)
    {
        state->holder.compare_exchange_strong (holder, 0, std::memory_order_acq_rel,
                                               std::memory_order_relaxed);
        return false;
    }

    return true;
}

void DeviceClaim::unhold()
{
    if (state == nullptr)
        return;

    uint32_t expected = myId;
    state->holder.compare_exchange_strong (expected, 0, std::memory_order_release,
                                           std::memory_order_relaxed);
}

bool DeviceClaim::heldBySomeoneElse() const
{
    if (state == nullptr)
        return false;

    const uint32_t holder = state->holder.load (std::memory_order_acquire);
    return holder != 0 && holder != myId && holdIsLive();
}

bool DeviceClaim::isOwner() const
{
    if (state == nullptr)
        return true;

    /* A live hold settles it outright, whoever last had activity. An expired one does
       not, or a crash while holding would lock the keyboard away for good. */
    const uint32_t holder = state->holder.load (std::memory_order_acquire);

    if (holder != 0 && holdIsLive())
        return holder == myId;

    const uint32_t owner = state->owner.load (std::memory_order_acquire);

    /* A hive-owned device belongs to every zoned instance, which is the whole point of
       the hive being one claimant: they do not take it from each other. */
    if (owner == kHiveOwner)
        return inHive;

    /* Nobody has claimed it, or we did. Somebody else owning it means exactly that:
       this instance is not the owner and must keep off the port until it is played,
       at which point claim() takes the device outright. */
    return owner == 0 || owner == myId;
}

void DeviceClaim::release()
{
    if (state == nullptr)
        return;

    /* A hive claim is only given up when the last member goes, and the lease handles
       that: one member releasing must not take the keyboard from the others. */
    if (inHive && state->owner.load (std::memory_order_acquire) == kHiveOwner)
        return;

    uint32_t expected = myId;
    state->owner.compare_exchange_strong (expected, 0, std::memory_order_release,
                                          std::memory_order_relaxed);
}

/*
    Whether a zone entry still belongs to somebody.

    Same lease as ownership, and cleared on the way past so the next reader does not
    have to work it out again. A zone whose instance died is a stretch of keys nobody
    can claim until the machine reboots, which would be a worse failure than the overlap
    it exists to prevent.
*/
bool DeviceClaim::zoneIsLive (int index) const
{
    uint32_t owner = state->zoneOwner[index].load (std::memory_order_acquire);

    if (owner == 0)
        return false;

    if (owner == myId)
        return true;

    const uint64_t now = nowMillis();
    const uint64_t seen = state->zoneStamp[index].load (std::memory_order_acquire);

    if (now >= seen && now - seen >= kZoneLeaseMillis)
    {
        state->zoneOwner[index].compare_exchange_strong (owner, 0,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_relaxed);
        return false;
    }

    return true;
}

bool DeviceClaim::claimZone (int low, int high, ZoneInfo &blocker)
{
    blocker.owner = 0;
    blocker.low = 0;
    blocker.high = 0;

    if (state == nullptr)
        return false;

    if (low > high)
    {
        const int swap = low;
        low = high;
        high = swap;
    }

    if (low < 0) low = 0;
    if (high > 127) high = 127;

    /*
        Checked against everyone else first, then written.

        Not one atomic operation, and it does not need to be: the thing being guarded
        against is two people dragging fields at the same moment on two tracks, which is
        rare enough that the cost of losing the race is one refusal the user can see and
        retry. What matters is that the check and the write use the same liveness rule,
        so a dead instance's range never blocks a live one.
    */
    int mine = -1;

    for (int i = 0; i < kMaxZones; ++i)
    {
        if (state->zoneOwner[i].load (std::memory_order_acquire) == myId)
        {
            mine = i;
            continue;
        }

        if (! zoneIsLive (i))
            continue;

        const int otherLow = state->zoneLow[i].load (std::memory_order_acquire);
        const int otherHigh = state->zoneHigh[i].load (std::memory_order_acquire);

        if (low <= otherHigh && otherLow <= high)
        {
            blocker.owner = state->zoneOwner[i].load (std::memory_order_acquire);
            blocker.low = otherLow;
            blocker.high = otherHigh;
            return false;
        }
    }

    if (mine < 0)
    {
        for (int i = 0; i < kMaxZones && mine < 0; ++i)
        {
            uint32_t expected = 0;

            if (! zoneIsLive (i)
                 && state->zoneOwner[i].compare_exchange_strong (expected, myId,
                                                                 std::memory_order_acq_rel,
                                                                 std::memory_order_relaxed))
            {
                mine = i;
            }
        }
    }

    if (mine < 0)
        return false;

    /*
        Claiming counts as activity, for the grace period only.

        Without this a zone that has never received anything is stale from the instant
        it exists, so opening a project and playing one track would warn on every other
        track before anyone had touched them. Starting the clock at the claim means the
        warning can only appear after this instance has had a fair chance to receive
        something.
    */
    if (state->zoneNoteStamp[mine].load (std::memory_order_relaxed) == 0)
        state->zoneNoteStamp[mine].store (nowMillis(), std::memory_order_release);

    state->zoneLow[mine].store (low, std::memory_order_release);
    state->zoneHigh[mine].store (high, std::memory_order_release);
    state->zoneStamp[mine].store (nowMillis(), std::memory_order_release);
    state->zoneOwner[mine].store (myId, std::memory_order_release);
    return true;
}

void DeviceClaim::releaseZone()
{
    if (state == nullptr)
        return;

    for (int i = 0; i < kMaxZones; ++i)
    {
        uint32_t expected = myId;
        state->zoneOwner[i].compare_exchange_strong (expected, 0,
                                                     std::memory_order_release,
                                                     std::memory_order_relaxed);
    }
}

/* Renewed from the worker, the same way ownership and holding are. */
void DeviceClaim::renewZone()
{
    if (state == nullptr)
        return;

    for (int i = 0; i < kMaxZones; ++i)
        if (state->zoneOwner[i].load (std::memory_order_acquire) == myId)
            state->zoneStamp[i].store (nowMillis(), std::memory_order_release);
}

bool DeviceClaim::zoneAt (int index, ZoneInfo &info) const
{
    if (state == nullptr || index < 0 || index >= kMaxZones)
        return false;

    if (! zoneIsLive (index))
        return false;

    info.owner = state->zoneOwner[index].load (std::memory_order_acquire);
    info.low = state->zoneLow[index].load (std::memory_order_acquire);
    info.high = state->zoneHigh[index].load (std::memory_order_acquire);
    return info.owner != 0;
}

bool DeviceClaim::myZone (ZoneInfo &info) const
{
    if (state == nullptr)
        return false;

    for (int i = 0; i < kMaxZones; ++i)
        if (state->zoneOwner[i].load (std::memory_order_acquire) == myId)
            return zoneAt (i, info);

    return false;
}

/*
    One key of this member's zone, as it should look before effects.

    Written every time it changes rather than every tick: the sender reads the whole
    table each frame regardless, so a write here only has to be no older than the last
    change.
*/
/* Stamped from the note path, so it means "a note arrived here", not "this instance is
   running" - the lease already says that. */
void DeviceClaim::noteSeen()
{
    if (state == nullptr)
        return;

    for (int i = 0; i < kMaxZones; ++i)
        if (state->zoneOwner[i].load (std::memory_order_acquire) == myId)
            state->zoneNoteStamp[i].store (nowMillis(), std::memory_order_release);
}

/*
    True when this instance is getting no MIDI and others plainly are.

    Both halves are needed. Silence on its own means nobody is playing, which is not a
    problem; somebody else receiving while this one does not is the track that was never
    given an input. The windows are deliberately far apart - several seconds of silence
    here against a recent note elsewhere - so a chord split across two zones, where one
    hand pauses, never trips it.
*/
bool DeviceClaim::zoneStarved() const
{
    if (state == nullptr)
        return false;

    const uint64_t now = nowMillis();
    uint64_t mine = 0;
    uint64_t others = 0;
    bool haveMine = false;

    for (int i = 0; i < kMaxZones; ++i)
    {
        if (! zoneIsLive (i))
            continue;

        const uint64_t seen = state->zoneNoteStamp[i].load (std::memory_order_acquire);

        if (state->zoneOwner[i].load (std::memory_order_acquire) == myId)
        {
            mine = seen;
            haveMine = true;
        }
        else if (seen > others)
        {
            others = seen;
        }
    }

    if (! haveMine || others == 0)
        return false;

    const bool othersRecent = now >= others && now - others < 6000;
    const bool mineStale = mine != 0 && now >= mine && now - mine > 12000;

    return othersRecent && mineStale;
}

void DeviceClaim::publishZoneColour (int note, uint32_t rgb)
{
    if (state == nullptr || note < 0 || note > 127)
        return;

    state->zoneColour[note].store (0x80000000u | (rgb & 0x00ffffffu),
                                   std::memory_order_relaxed);
}

/* Cleared on the way out, so a member that takes over as sender does not redraw a
   range whose instance has gone. */
void DeviceClaim::clearZoneColours (int low, int high)
{
    if (state == nullptr)
        return;

    for (int note = low; note <= high && note < 128; ++note)
        if (note >= 0)
            state->zoneColour[note].store (0, std::memory_order_relaxed);
}

bool DeviceClaim::readZoneColours (uint32_t *dest) const
{
    if (state == nullptr || dest == nullptr)
        return false;

    bool any = false;

    for (int i = 0; i < 128; ++i)
    {
        const uint32_t v = state->zoneColour[i].load (std::memory_order_relaxed);

        if ((v & 0x80000000u) != 0u)
        {
            dest[i] = v & 0x00ffffffu;
            any = true;
        }
    }

    return any;
}

void DeviceClaim::postEffect (uint64_t packed)
{
    if (state == nullptr || packed == 0)
        return;

    const uint32_t slot = state->effectWrite.fetch_add (1, std::memory_order_relaxed);
    state->effectRing[slot % kEffectSlots].store (packed, std::memory_order_release);
}

/* Taken rather than read, so two senders cannot both draw the same ripple - which
   matters for the moment the sender changes and two instances briefly agree they are
   it. */
bool DeviceClaim::takeEffect (int slot, uint64_t &packed)
{
    if (state == nullptr || slot < 0 || slot >= kEffectSlots)
        return false;

    packed = state->effectRing[slot].exchange (0, std::memory_order_acquire);
    return packed != 0;
}

void DeviceClaim::publishColour (int note, uint32_t rgb)
{
    if (state == nullptr || note < 0 || note > 127)
        return;

    state->deviceColour[note].store (0x80000000u | (rgb & 0x00ffffffu),
                                     std::memory_order_relaxed);
}

bool DeviceClaim::adoptDeviceState (uint32_t *dest) const
{
    if (state == nullptr || dest == nullptr)
        return false;

    bool any = false;

    for (int i = 0; i < 128; ++i)
    {
        const uint32_t v = state->deviceColour[i].load (std::memory_order_relaxed);

        if ((v & 0x80000000u) != 0u)
        {
            dest[i] = v & 0x00ffffffu;
            any = true;
        }
    }

    return any;
}

void DeviceClaim::forgetDeviceState()
{
    if (state == nullptr)
        return;

    for (int i = 0; i < 128; ++i)
        state->deviceColour[i].store (0, std::memory_order_relaxed);
}

uint32_t DeviceClaim::ownerId() const
{
    return state != nullptr ? state->owner.load (std::memory_order_acquire) : myId;
}


/*
    Written length last, so a reader never sees a length that outruns the text.

    The sequence is bumped first and the length set at the end, which means the window
    where a reader could catch a half-written clipboard shows a length of zero rather
    than a plausible-looking number pointing at stale bytes. There is no lock here and
    there does not need to be: pasting a look is a thing a person does once in a while,
    and the cost of losing a race is one empty paste.
*/
bool DeviceClaim::writeClipboard (const std::string &text)
{
    if (state == nullptr)
        return false;

    if (text.size() >= (size_t) kClipBytes)
        return false;

    state->clipLength.store (0, std::memory_order_release);
    state->clipSeq.fetch_add (1, std::memory_order_acq_rel);

    std::memcpy (state->clipText, text.data(), text.size());
    state->clipText[text.size()] = '\0';

    state->clipLength.store ((uint32_t) text.size(), std::memory_order_release);
    return true;
}

bool DeviceClaim::readClipboard (std::string &text) const
{
    if (state == nullptr)
        return false;

    const uint32_t length = state->clipLength.load (std::memory_order_acquire);

    if (length == 0 || length >= (uint32_t) kClipBytes)
        return false;

    text.assign (state->clipText, length);
    return true;
}

uint32_t DeviceClaim::clipboardSeq() const
{
    return state != nullptr ? state->clipSeq.load (std::memory_order_acquire) : 0u;
}

}
