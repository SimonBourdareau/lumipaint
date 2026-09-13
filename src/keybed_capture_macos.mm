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
    Window listing and capture on macOS, for the keybed reader.

    Only the two platform-specific steps live here - finding windows and getting their
    pixels. Everything after that, the whole detector, is the same code on both
    platforms.

    Written but not run: there is no Mac here to test on.

    Two things are different from Windows in ways that will be felt.

    Permission. Reading another application's window needs Screen Recording permission,
    which the user grants once in System Settings > Privacy & Security. The first
    attempt returns nothing and the system shows a prompt; the host has to be restarted
    afterwards before it takes effect. There is no way around that, and no way to ask
    for it politely from inside a plugin - so the caller reports it rather than failing
    silently.

    Deprecation. CGWindowListCreateImage is deprecated from macOS 14 in favour of
    ScreenCaptureKit, which is asynchronous and much heavier. This still works and is
    far simpler; if it stops, that is the replacement.
*/

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#include <cstdint>
#include <string>
#include <vector>

namespace lumipaint {

struct MacWindow
{
    uint32_t windowId;
    std::string title;
    int width;
    int height;
};

/* Every on-screen window big enough to hold a keyboard, owned by another application. */
std::vector<MacWindow> macListWindows()
{
    std::vector<MacWindow> out;

    @autoreleasepool
    {
        CFArrayRef list = CGWindowListCopyWindowInfo (
            kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
            kCGNullWindowID);

        if (list == nullptr)
            return out;

        const CFIndex count = CFArrayGetCount (list);

        for (CFIndex i = 0; i < count; ++i)
        {
            NSDictionary *info = (__bridge NSDictionary *)
                                     CFArrayGetValueAtIndex (list, i);

            NSNumber *layer = info[(id) kCGWindowLayer];

            /* Layer zero is an ordinary application window. Anything else is a menu,
               a dock tile, a shadow or some other furniture. */
            if (layer == nil || [layer intValue] != 0)
                continue;

            NSDictionary *bounds = info[(id) kCGWindowBounds];

            if (bounds == nil)
                continue;

            CGRect rect;
            CGRectMakeWithDictionaryRepresentation ((__bridge CFDictionaryRef) bounds,
                                                    &rect);

            if (rect.size.width < 200 || rect.size.height < 120)
                continue;

            NSString *name = info[(id) kCGWindowName];
            NSString *owner = info[(id) kCGWindowOwnerName];
            NSString *shown = (name.length > 0) ? name : owner;

            if (shown.length == 0)
                continue;

            MacWindow w;
            w.windowId = (uint32_t) [info[(id) kCGWindowNumber] unsignedIntValue];
            w.title = std::string ([shown UTF8String]);
            w.width = (int) rect.size.width;
            w.height = (int) rect.size.height;
            out.push_back (w);
        }

        CFRelease (list);
    }

    return out;
}

/*
    Pixels for one window, as 0x00RRGGBB, top row first - the same layout the Windows
    side produces, so the detector needs no knowledge of where they came from.

    Returns false when the image cannot be had, which on a first run usually means
    Screen Recording permission has not been granted yet.
*/
bool macCaptureWindow (uint32_t windowId, std::vector<uint32_t> &pixels,
                       int &width, int &height)
{
    @autoreleasepool
    {
        CGImageRef image = CGWindowListCreateImage (
            CGRectNull,
            kCGWindowListOptionIncludingWindow,
            (CGWindowID) windowId,
            kCGWindowImageBoundsIgnoreFraming | kCGWindowImageNominalResolution);

        if (image == nullptr)
            return false;

        width = (int) CGImageGetWidth (image);
        height = (int) CGImageGetHeight (image);

        if (width < 8 || height < 8)
        {
            CGImageRelease (image);
            return false;
        }

        pixels.assign ((size_t) width * height, 0);

        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate (
            pixels.data(), (size_t) width, (size_t) height, 8,
            (size_t) width * 4, space,
            kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);

        if (ctx == nullptr)
        {
            CGColorSpaceRelease (space);
            CGImageRelease (image);
            return false;
        }

        CGContextDrawImage (ctx, CGRectMake (0, 0, width, height), image);
        CGContextRelease (ctx);
        CGColorSpaceRelease (space);
        CGImageRelease (image);

        for (size_t i = 0; i < pixels.size(); ++i)
            pixels[i] &= 0x00ffffffu;

        return true;
    }
}

}
