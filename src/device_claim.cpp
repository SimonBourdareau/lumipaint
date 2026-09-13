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
};

namespace {

const uint32_t kMagic = 0x4c554d43;

/* How long an owner may go without renewing before another instance may take the
   keyboard. The worker renews on every tick, so this is orders of magnitude longer
   than a healthy instance needs - it only expires a dead one. */
const uint64_t kLeaseMillis = 4000;

uint64_t nowMillis()
{
    using namespace std::chrono;
    return (uint64_t) duration_cast<milliseconds> (
               steady_clock::now().time_since_epoch()).count();
}

}

#if defined (_WIN32)

DeviceClaim::DeviceClaim()
    : mapping (nullptr), state (nullptr), myId (0)
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

        state->magic.store (kMagic, std::memory_order_release);
    }
}

DeviceClaim::~DeviceClaim()
{
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
    : mapping (nullptr), state (nullptr), myId (0)
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

        state->magic.store (kMagic, std::memory_order_release);
    }
}

DeviceClaim::~DeviceClaim()
{
    unhold();
    release();

    if (state != nullptr)
        munmap (state, sizeof (Shared));
}

#endif

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
    if (state->owner.load (std::memory_order_relaxed) == myId)
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
    uint32_t current = state->owner.load (std::memory_order_acquire);

    while (! state->owner.compare_exchange_weak (current, myId,
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

    /* Nobody has claimed it, or we did. Somebody else owning it means exactly that:
       this instance is not the owner and must keep off the port until it is played,
       at which point claim() takes the device outright. */
    return owner == 0 || owner == myId;
}

void DeviceClaim::release()
{
    if (state == nullptr)
        return;

    uint32_t expected = myId;
    state->owner.compare_exchange_strong (expected, 0, std::memory_order_release,
                                          std::memory_order_relaxed);
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

}
