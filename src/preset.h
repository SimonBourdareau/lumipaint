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

namespace lumipaint {

class LumiLink;

/*
    Colour maps as files.

    Text rather than binary, and readable: a map is a list of notes and colours, which
    is worth being able to open in an editor, diff, paste into a message, or write by
    hand. A binary blob would be smaller and worth nothing.

    Only what makes a map look the way it does travels: the 128 painted colours, the
    colours the effects use, and the two output levels. Not the port name, the octave,
    the window, the send rate or anything else about this particular keyboard on this
    particular day - a map should open the same on someone else's chain.

    Saved anywhere you like. The dialog starts in Documents/LumiPaint because a map has
    to be somewhere by default, but nothing depends on it being there.
*/
struct PresetIO
{
    static bool save (const std::string &path, const LumiLink &link,
                      double brightness, double unlitLevel);

    static bool load (const std::string &path, LumiLink &link,
                      double &brightness, double &unlitLevel);

    /* Native dialogs. Return an empty string if the user cancelled. */
    static std::string askForSavePath();
    static std::string askForOpenPath();

    /* Documents/LumiPaint, created if it is not there. Empty if it cannot be. */
    static std::string defaultDirectory();

    static const char *lastError();
};

}
