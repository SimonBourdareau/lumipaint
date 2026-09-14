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
    Objective-C is. Compiles and links, but has never been run - there is no Mac here
    to test on.

    What this used to do instead: it declared a defaultDir() it never defined, and
    called a PresetIO_defaultDirectory() that exists in no translation unit. Both were
    invented names for PresetIO::defaultDirectory(), which is right there in preset.h
    and is what the Windows and Linux dialogs already call.
*/

#import <Cocoa/Cocoa.h>

#include "preset.h"

#include <string>

namespace lumipaint {

/*
    Run modally, on the main thread.

    A panel run from the audio thread or from a worker does not merely misbehave - AppKit
    requires the main thread for this, and the failure is a hang rather than a message.
    The editor calls it from its own draw, which is the main thread, so this is a
    precondition rather than something to work around; the check is here so a future
    caller gets an empty string rather than a locked host.
*/
std::string macDialog (bool saving)
{
    if (! [NSThread isMainThread])
        return std::string();

    @autoreleasepool
    {
        const std::string dir = PresetIO::defaultDirectory();
        NSURL *start = nil;

        if (! dir.empty())
        {
            NSString *path = [NSString stringWithUTF8String: dir.c_str()];

            if (path.length > 0)
                start = [NSURL fileURLWithPath: path isDirectory: YES];
        }

        if (saving)
        {
            NSSavePanel *panel = [NSSavePanel savePanel];
            [panel setNameFieldStringValue: @"map.lumimap"];
            [panel setAllowedFileTypes: @[@"lumimap"]];

            if (start != nil)
                [panel setDirectoryURL: start];

            if ([panel runModal] != NSModalResponseOK || [panel URL] == nil)
                return std::string();

            const char *chosen = [[[panel URL] path] UTF8String];
            return chosen != nullptr ? std::string (chosen) : std::string();
        }

        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setAllowedFileTypes: @[@"lumimap"]];
        [panel setAllowsMultipleSelection: NO];
        [panel setCanChooseDirectories: NO];
        [panel setCanChooseFiles: YES];

        if (start != nil)
            [panel setDirectoryURL: start];

        if ([panel runModal] != NSModalResponseOK)
            return std::string();

        NSURL *picked = [[panel URLs] firstObject];

        if (picked == nil)
            return std::string();

        const char *chosen = [[picked path] UTF8String];
        return chosen != nullptr ? std::string (chosen) : std::string();
    }
}

}
