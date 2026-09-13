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

/*
    The macOS half of the preset dialogs.

    Split out so preset.cpp stays plain C++ and only the part that has to be
    Objective-C is. Written but not run - there is no Mac here to test on.
*/

#import <Cocoa/Cocoa.h>

#include <string>

namespace lumipaint {
namespace {

std::string defaultDir();

}

std::string PresetIO_defaultDirectory();

namespace {

std::string macDialogImpl (bool saving)
{
    @autoreleasepool
    {
        NSString *dir = [NSString stringWithUTF8String:
                            PresetIO_defaultDirectory().c_str()];

        if (saving)
        {
            NSSavePanel *panel = [NSSavePanel savePanel];
            [panel setAllowedFileTypes: @[@"lumimap"]];
            [panel setNameFieldStringValue: @"map.lumimap"];

            if (dir.length > 0)
                [panel setDirectoryURL: [NSURL fileURLWithPath: dir]];

            if ([panel runModal] != NSModalResponseOK)
                return std::string();

            return std::string ([[[panel URL] path] UTF8String]);
        }

        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setAllowedFileTypes: @[@"lumimap"]];
        [panel setAllowsMultipleSelection: NO];
        [panel setCanChooseDirectories: NO];

        if (dir.length > 0)
            [panel setDirectoryURL: [NSURL fileURLWithPath: dir]];

        if ([panel runModal] != NSModalResponseOK)
            return std::string();

        return std::string ([[[[panel URLs] firstObject] path] UTF8String]);
    }
}

}

std::string macDialog (bool saving)
{
    return macDialogImpl (saving);
}

}
