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

/*
    Everything the plain C++ half of the plugin asks of the Objective-C half.

    The two sides used to agree by writing the same thing twice. struct MacWindow was
    declared once in keybed_capture.cpp and again in keybed_capture_macos.mm, and the
    program was only well-formed because the two copies happened to match to the token -
    edit one and it is undefined behaviour with no diagnostic. macDialog was worse: it
    sat in an anonymous namespace on the C++ side, where it had internal linkage and
    could never have resolved against the definition in the .mm at all, so the macOS
    build compiled both files and failed at the link with an undefined symbol.

    One header both sides include instead, so the next mismatch is a compile error.
*/

/*
    Include this at file scope. It opens namespace lumipaint itself, so including it
    from inside that namespace nests the two and every name in here ends up one level
    too deep.
*/

#include <cstdint>
#include <string>
#include <vector>

namespace lumipaint {

/* One on-screen window belonging to another application, large enough to hold a
   keyboard. */
struct MacWindow
{
    uint32_t windowId;
    std::string title;
    int width;
    int height;
};

/* Implemented in keybed_capture_macos.mm. */
std::vector<MacWindow> macListWindows();

/*
    Pixels for one window, as 0x00RRGGBB, top row first - the same layout the Windows
    side produces, so the detector needs no knowledge of where they came from.

    False when the image cannot be had, which on a first run usually means Screen
    Recording permission has not been granted yet.
*/
bool macCaptureWindow (uint32_t windowId, std::vector<uint32_t> &pixels,
                       int &width, int &height);

/* Implemented in preset_macos.mm. Empty when the user cancelled. */
std::string macDialog (bool saving);

}
