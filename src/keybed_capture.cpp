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

#include "keybed_capture.h"
#include "lumipaint.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined (_WIN32)
 #include <windows.h>
#elif defined (__linux__)
 #include <X11/Xlib.h>
 #include <X11/Xutil.h>
#endif

namespace lumipaint {
namespace {

const int kWhitePc[7] = { 0, 2, 4, 5, 7, 9, 11 };

/* Which semitone the black key after white step n is; steps 2 and 6 have none. */
int blackAfterWhiteStep (int step)
{
    switch (step)
    {
        case 0: return 1;
        case 1: return 3;
        case 3: return 6;
        case 4: return 8;
        case 5: return 10;
        default: return -1;
    }
}

struct Run
{
    int first;
    int last;
};

std::vector<Run> findRuns (const uint8_t *row, int from, int to, bool wanted,
                           const std::vector<bool> &mask, int minLength)
{
    (void) row;
    (void) wanted;
    std::vector<Run> out;
    int start = -1;

    for (int i = from; i <= to; ++i)
    {
        /* The mask is indexed absolutely, matching the row it was built from. */
        const bool v = mask[(size_t) i];

        if (v && start < 0)
        {
            start = i;
        }
        else if (! v && start >= 0)
        {
            if (i - start >= minLength)
                out.push_back ({ start, i - 1 });

            start = -1;
        }
    }

    if (start >= 0 && to + 1 - start >= minLength)
        out.push_back ({ start, to });

    return out;
}

/*
    The dividing lines between keys.

    A local minimum, with its depth measured against the pixels outside the whole dark
    run rather than its immediate neighbours.

    That last part matters more than it sounds. A divider drawn four pixels wide reads
    as 38, 41, 40, 38 between two key faces of 238: the darkest pixel's neighbour is
    also dark, so a neighbour comparison sees a depth of three and rejects it. Only
    one-pixel dividers were ever being found, which quietly limited detection to
    plugins that happen to draw them that way. Widening the run first, then comparing
    to what sits outside it, finds a divider of any width without lowering the bar for
    what counts as one.
*/
std::vector<float> separators (const uint8_t *row, int from, int to, int depth)
{
    std::vector<float> out;
    int i = from + 1;

    while (i < to)
    {
        if (row[i] < row[i - 1] && row[i] <= row[i + 1])
        {
            /* Everything within ten levels of the darkest pixel belongs to the same
               divider, however wide it is drawn.

               Ten rather than six: a divider reading 35, 33, 40, 38 has its darkest
               pixel in the middle, and a tighter tolerance stops before the 40 - so
               the depth gets measured against 35, comes out as two, and the divider is
               missed. That lost five dividers on one keyboard, leaving gaps of two and
               three keys in an otherwise perfect grid. */
            int j = i;

            while (j + 1 < to && std::abs ((int) row[j + 1] - (int) row[i]) <= 10)
                ++j;

            /* Growing the run leftward as well would close the last few gaps on a
               wide keybed, and it costs a narrow one entirely: on 12px keys the
               tolerance reaches across a whole key and the dividers merge. Left as a
               one-sided expansion deliberately. */
            const int left = row[i - 1];
            const int right = row[j + 1];
            const int shallower = left < right ? left : right;

            if (shallower - (int) row[i] >= depth)
                out.push_back ((float) (i + j) * 0.5f);

            i = j + 1;
        }
        else
        {
            ++i;
        }
    }

    return out;
}

float medianOf (std::vector<float> v)
{
    if (v.empty())
        return 0.0f;

    std::sort (v.begin(), v.end());
    return v[v.size() / 2];
}

struct Slot
{
    float centre;
    float width;
};

/*
    Split a row of key slots into separate keyboards, then put back together the
    pieces that were only separated for the wrong reason.

    A window can hold more than one keybed and the strips either side of them are made
    of other things entirely; without this a whole row reads as one instrument and
    reports a seventeen-octave keyboard. The merge matters just as much: highlighted or
    selected keys are often drawn as a solid block with no dividers, so a run of them is
    unmeasurable - but the keyboard plainly continues across it and the missing
    positions are predictable.
*/
std::vector<std::vector<Slot>> segmentSlots (const std::vector<Slot> &slots)
{
    std::vector<std::vector<Slot>> out;

    if (slots.size() < 6)
        return out;

    std::vector<float> gaps;
    std::vector<float> widths;

    for (size_t i = 1; i < slots.size(); ++i)
        gaps.push_back (slots[i].centre - slots[i - 1].centre);

    for (const Slot &s : slots)
        widths.push_back (s.width);

    const float medianGap = medianOf (gaps);
    const float medianWidth = medianOf (widths);

    size_t start = 0;

    for (size_t i = 0; i + 1 < slots.size(); ++i)
    {
        const float g = slots[i + 1].centre - slots[i].centre;
        const float w = slots[i + 1].width;

        if (std::fabs (g - medianGap) > std::max (2.0f, medianGap * 0.30f)
            || std::fabs (w - medianWidth) > std::max (2.0f, medianWidth * 0.45f))
        {
            if (i + 1 - start >= 6)
                out.push_back (std::vector<Slot> (slots.begin() + start, slots.begin() + i + 1));

            start = i + 1;
        }
    }

    if (slots.size() - start >= 6)
        out.push_back (std::vector<Slot> (slots.begin() + start, slots.end()));

    std::vector<std::vector<Slot>> merged;

    for (const std::vector<Slot> &piece : out)
    {
        if (! merged.empty())
        {
            std::vector<Slot> &prev = merged.back();
            std::vector<float> pw, cw, steps;

            for (const Slot &s : prev)
                pw.push_back (s.width);

            for (const Slot &s : piece)
                cw.push_back (s.width);

            for (size_t i = 1; i < prev.size(); ++i)
                steps.push_back (prev[i].centre - prev[i - 1].centre);

            const float pwm = medianOf (pw);
            const float cwm = medianOf (cw);
            const float step = steps.empty() ? pwm : medianOf (steps);
            const float gap = piece.front().centre - prev.back().centre;
            const int span = step > 0.0f ? (int) (gap / step + 0.5f) : 0;

            if (std::fabs (pwm - cwm) <= std::max (1.5f, pwm * 0.15f)
                && span >= 1 && span <= 12
                && std::fabs (gap - (float) span * step) <= std::max (2.0f, step * 0.4f))
            {
                for (int k = 1; k < span; ++k)
                {
                    Slot bridge;
                    bridge.centre = prev.back().centre + step * (float) k;
                    bridge.width = pwm;
                    prev.push_back (bridge);
                }

                prev.insert (prev.end(), piece.begin(), piece.end());
                continue;
            }
        }

        merged.push_back (piece);
    }

    return merged;
}

}

bool KeybedCapture::loadImage (const uint32_t *bgra, int w, int h)
{
    width = w;
    height = h;
    pixels.assign (bgra, bgra + (size_t) w * h);
    luma.assign (pixels.size(), 0);

    for (size_t i = 0; i < pixels.size(); ++i)
    {
        const uint32_t p = pixels[i];
        luma[i] = (uint8_t) ((((int) ((p >> 16) & 0xff)) * 30
                            + ((int) ((p >> 8) & 0xff)) * 59
                            + ((int) (p & 0xff)) * 11) / 100);
    }

    message = "loaded image";
    return true;
}

KeybedCapture::KeybedCapture()
    : width (0), height (0), lastWindow (nullptr), measuredWhiteWidth (0.0f),
      detectedOctaves (0), message ("no capture yet")
{
}

#if defined (_WIN32)

namespace {

std::vector<CaptureWindow> *g_collecting = nullptr;

BOOL CALLBACK collectWindow (HWND hwnd, LPARAM)
{
    if (! IsWindowVisible (hwnd))
        return TRUE;

    RECT r;

    if (! GetWindowRect (hwnd, &r))
        return TRUE;

    const int w = r.right - r.left;
    const int h = r.bottom - r.top;

    if (w < 200 || h < 120)
        return TRUE;

    char title[220];
    GetWindowTextA (hwnd, title, sizeof (title));

    if (title[0] == 0)
        return TRUE;

    CaptureWindow cw;
    cw.handle = (void *) hwnd;
    cw.title = title;
    cw.width = w;
    cw.height = h;
    g_collecting->push_back (cw);
    return TRUE;
}

}

void KeybedCapture::refreshWindows()
{
    windowList.clear();
    g_collecting = &windowList;
    EnumWindows (collectWindow, 0);
    g_collecting = nullptr;
}

/*
    PW_RENDERFULLCONTENT reads a window even when it is behind others, which is what
    makes sampling usable while you work. It returns a black bitmap for windows drawn
    with OpenGL or Direct3D; detect() will then simply find nothing.
*/
bool KeybedCapture::grab (size_t index)
{
    if (index >= windowList.size())
    {
        message = "no such window";
        return false;
    }

    const CaptureWindow &cw = windowList[index];
    HWND hwnd = (HWND) cw.handle;

    RECT r;

    if (! GetWindowRect (hwnd, &r))
    {
        message = "window has gone";
        return false;
    }

    width = r.right - r.left;
    height = r.bottom - r.top;

    if (width < 8 || height < 8)
    {
        message = "window too small";
        return false;
    }

    HDC screenDc = GetDC (nullptr);
    HDC memDc = CreateCompatibleDC (screenDc);
    HBITMAP bmp = CreateCompatibleBitmap (screenDc, width, height);
    HGDIOBJ previous = SelectObject (memDc, bmp);

    PrintWindow (hwnd, memDc, 2);

    pixels.assign ((size_t) width * height, 0);

    BITMAPINFO bi;
    ZeroMemory (&bi, sizeof (bi));
    bi.bmiHeader.biSize = sizeof (bi.bmiHeader);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    GetDIBits (memDc, bmp, 0, height, pixels.data(), &bi, DIB_RGB_COLORS);

    SelectObject (memDc, previous);
    DeleteObject (bmp);
    DeleteDC (memDc);
    ReleaseDC (nullptr, screenDc);

    luma.assign (pixels.size(), 0);
    size_t nonBlack = 0;

    for (size_t i = 0; i < pixels.size(); ++i)
    {
        const uint32_t p = pixels[i];
        const int b = (int) (p & 0xff);
        const int g = (int) ((p >> 8) & 0xff);
        const int rr = (int) ((p >> 16) & 0xff);
        luma[i] = (uint8_t) ((rr * 30 + g * 59 + b * 11) / 100);

        if (luma[i] > 8)
            ++nonBlack;
    }

    if (nonBlack * 200 < pixels.size())
    {
        message = "window came back black - drawn on the GPU, cannot be sampled";
        discardPixels();
        return false;
    }

    lastWindow = cw.handle;
    message = "captured " + std::to_string (width) + "x" + std::to_string (height);
    return true;
}

#elif defined (__APPLE__)

/* Implemented in keybed_capture_macos.mm: only finding windows and getting their
   pixels differ by platform, and everything after that is shared. */
struct MacWindow
{
    uint32_t windowId;
    std::string title;
    int width;
    int height;
};

std::vector<MacWindow> macListWindows();
bool macCaptureWindow (uint32_t windowId, std::vector<uint32_t> &pixels,
                       int &width, int &height);

void KeybedCapture::refreshWindows()
{
    windowList.clear();

    for (const MacWindow &w : macListWindows())
    {
        CaptureWindow cw;
        cw.handle = (void *) (uintptr_t) w.windowId;
        cw.title = w.title;
        cw.width = w.width;
        cw.height = w.height;
        windowList.push_back (cw);
    }
}

bool KeybedCapture::grab (size_t index)
{
    if (index >= windowList.size())
    {
        message = "no such window";
        return false;
    }

    const CaptureWindow &cw = windowList[index];

    if (! macCaptureWindow ((uint32_t) (uintptr_t) cw.handle, pixels, width, height))
    {
        /* Almost always the permission on a first run, and worth saying so: there is
           no prompt from inside a plugin and no way to grant it from here. */
        message = "could not read that window - grant Screen Recording in "
                  "System Settings > Privacy & Security, then restart the host";
        return false;
    }

    luma.assign (pixels.size(), 0);

    for (size_t i = 0; i < pixels.size(); ++i)
    {
        const uint32_t p = pixels[i];
        luma[i] = (uint8_t) ((((int) ((p >> 16) & 0xff)) * 30
                            + ((int) ((p >> 8) & 0xff)) * 59
                            + ((int) (p & 0xff)) * 11) / 100);
    }

    lastWindow = cw.handle;
    message = "captured " + std::to_string (width) + "x" + std::to_string (height);
    return true;
}

#elif defined (__linux__)

/*
    X11 window listing and capture.

    Written but never run. XGetImage on a window the compositor owns returns what is
    currently on screen for it, which is the same contract the Windows and macOS paths
    have: the window must be mapped, but it does not have to be in front.

    Wayland has no equivalent by design - a client cannot read another client's
    surface, which is the point of the design rather than an oversight. Under XWayland
    this works because XWayland windows are X11 windows; under a native Wayland session
    it will not, and no amount of code here changes that.
*/
namespace {

void collectWindows (Display *display, Window from, std::vector<CaptureWindow> &out)
{
    Window root = 0;
    Window parent = 0;
    Window *children = nullptr;
    unsigned int count = 0;

    if (! XQueryTree (display, from, &root, &parent, &children, &count))
        return;

    for (unsigned int i = 0; i < count; ++i)
    {
        XWindowAttributes attr;

        if (XGetWindowAttributes (display, children[i], &attr) == 0)
            continue;

        /* Mapped, and big enough to hold a keyboard. Anything smaller is a tooltip,
           a menu or some other furniture. */
        if (attr.map_state == IsViewable && attr.width >= 200 && attr.height >= 120)
        {
            char *name = nullptr;

            if (XFetchName (display, children[i], &name) != 0 && name != nullptr)
            {
                CaptureWindow cw;
                cw.handle = (void *) (uintptr_t) children[i];
                cw.title = name;
                cw.width = attr.width;
                cw.height = attr.height;
                out.push_back (cw);
                XFree (name);
            }
        }

        collectWindows (display, children[i], out);
    }

    if (children != nullptr)
        XFree (children);
}

}

void KeybedCapture::refreshWindows()
{
    windowList.clear();

    Display *display = XOpenDisplay (nullptr);

    if (display == nullptr)
        return;

    collectWindows (display, DefaultRootWindow (display), windowList);
    XCloseDisplay (display);
}

bool KeybedCapture::grab (size_t index)
{
    if (index >= windowList.size())
    {
        message = "no such window";
        return false;
    }

    Display *display = XOpenDisplay (nullptr);

    if (display == nullptr)
    {
        message = "no X display - a Wayland session cannot read another window";
        return false;
    }

    const CaptureWindow &cw = windowList[index];
    const Window target = (Window) (uintptr_t) cw.handle;

    XWindowAttributes attr;

    if (XGetWindowAttributes (display, target, &attr) == 0)
    {
        XCloseDisplay (display);
        message = "the window has gone";
        return false;
    }

    XImage *image = XGetImage (display, target, 0, 0,
                               (unsigned int) attr.width, (unsigned int) attr.height,
                               AllPlanes, ZPixmap);

    if (image == nullptr)
    {
        XCloseDisplay (display);
        message = "could not read that window";
        return false;
    }

    width = attr.width;
    height = attr.height;
    pixels.assign ((size_t) width * height, 0);
    luma.assign (pixels.size(), 0);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const unsigned long p = XGetPixel (image, x, y);
            const uint32_t rgb = (uint32_t) (p & 0x00ffffffu);
            const size_t at = (size_t) y * width + x;
            pixels[at] = rgb;
            luma[at] = (uint8_t) ((((int) ((rgb >> 16) & 0xff)) * 30
                                 + ((int) ((rgb >> 8) & 0xff)) * 59
                                 + ((int) (rgb & 0xff)) * 11) / 100);
        }
    }

    XDestroyImage (image);
    XCloseDisplay (display);

    lastWindow = cw.handle;
    message = "captured " + std::to_string (width) + "x" + std::to_string (height);
    return true;
}

#else

void KeybedCapture::refreshWindows()
{
    windowList.clear();
}

bool KeybedCapture::grab (size_t)
{
    message = "window capture is not implemented on this platform";
    return false;
}

#endif

bool KeybedCapture::detect()
{
    keys.clear();
    measuredWhiteWidth = 0.0f;
    detectedOctaves = 0;

    if (luma.empty())
    {
        message = "nothing captured";
        return false;
    }

    int bestScore = 0;

    for (int yBlack = 4; yBlack < height - 6; ++yBlack)
    {
        const uint8_t *blackRow = &luma[(size_t) yBlack * width];

        {
            std::vector<float> rowValues;
            rowValues.reserve ((size_t) width);

            for (int x = 0; x < width; ++x)
                rowValues.push_back ((float) blackRow[x]);

            if (medianOf (rowValues) < 12.0f)
                continue;
        }

        for (int depth = 4; depth <= 12; depth += 8)
        {
            for (int drop = 6; drop <= 70; drop += 4)
            {
                const int yWhite = yBlack + drop;

                if (yWhite >= height - 2)
                    break;

                const uint8_t *whiteRow = &luma[(size_t) yWhite * width];
                std::vector<float> seps = separators (whiteRow, 0, width - 1, depth);

                /*
                    A boundary can be a change of colour rather than a dark line.

                    Where two adjacent keys are painted different colours some plugins
                    draw no divider between them at all - the red key simply becomes the
                    tan key. Looking only for dark dips misses those, and the keys on
                    either side vanish from the grid, which is exactly where an imported
                    map is most worth having.

                    Only added where no dip was found nearby, so this supplements the
                    dividers rather than competing with them.
                */
                {
                    std::vector<float> edges;

                    for (int x = 1; x < width - 1; ++x)
                    {
                        const uint32_t a = pixels[(size_t) yWhite * width + x - 1];
                        const uint32_t b = pixels[(size_t) yWhite * width + x + 1];
                        const int dr = std::abs ((int) ((a >> 16) & 0xff) - (int) ((b >> 16) & 0xff));
                        const int dg = std::abs ((int) ((a >> 8) & 0xff) - (int) ((b >> 8) & 0xff));
                        const int db = std::abs ((int) (a & 0xff) - (int) (b & 0xff));

                        if (dr + dg + db < 90)
                            continue;

                        bool nearDip = false;

                        for (float sp : seps)
                            if (std::fabs (sp - (float) x) < 4.0f)
                                nearDip = true;

                        if (! nearDip)
                            edges.push_back ((float) x);
                    }

                    /* One position per edge, not one per pixel of the ramp. */
                    for (size_t i = 0; i < edges.size(); ++i)
                    {
                        if (i > 0 && edges[i] - edges[i - 1] < 4.0f)
                            continue;

                        seps.push_back (edges[i]);
                    }

                    std::sort (seps.begin(), seps.end());
                }

                if (seps.size() < 8)
                    continue;

                /* Slots first, then split into separate keyboards. A window can hold
                   more than one, and the strips either side of them are other things
                   entirely. */
                std::vector<Slot> slots;

                for (size_t i = 1; i < seps.size(); ++i)
                {
                    const float g = seps[i] - seps[i - 1];

                    if (g >= 2.0f)
                        slots.push_back ({ (seps[i] + seps[i - 1]) * 0.5f, g });
                }

                for (const std::vector<Slot> &piece : segmentSlots (slots))
                {
                    std::vector<float> whites;

                    for (const Slot &sl : piece)
                        whites.push_back (sl.centre);

                    /* Eight, not twelve. One octave is enough to be worth reading, and
                       a small keybed gets cut into pieces by the colour groups drawn
                       over it. The pattern fit and the black-to-white ratio are what
                       reject things that are not keyboards; a length test did that job
                       badly. */
                    if (whites.size() < 8)
                        continue;

                    std::vector<float> pieceGaps;

                    for (size_t i = 1; i < whites.size(); ++i)
                        pieceGaps.push_back (whites[i] - whites[i - 1]);

                    const float medianGap = medianOf (pieceGaps);

                    if (medianGap < 4.0f || medianGap > 60.0f)
                        continue;

                    /*
                        Black keys are found by comparing the two rows, not by looking
                        for dark pixels. A white key's face is the same colour at both
                        rows; a black key's is not, because at the lower row the white
                        key beneath it is showing. That holds whatever colour the black
                        key is drawn in - Falcon tints them blue and green for its drum
                        map, and a darkness test drops those.

                        Only within this keyboard's own span. Scanning the whole row
                        counted every dark region in the GUI as a black key: a two
                        octave keybed came out as 48 black keys against 12 white ones,
                        and the five-per-seven test then threw the real keyboard away.
                    */
                    const int pieceLo = std::max (0, (int) whites.front() - (int) medianGap);
                    const int pieceHi = std::min (width - 1, (int) whites.back() + (int) medianGap);

                    std::vector<bool> differs ((size_t) width, false);

                    for (int x = pieceLo; x <= pieceHi; ++x)
                    {
                        const uint32_t a = pixels[(size_t) yBlack * width + x];
                        const uint32_t b = pixels[(size_t) yWhite * width + x];
                        const int dr = std::abs ((int) ((a >> 16) & 0xff) - (int) ((b >> 16) & 0xff));
                        const int dg = std::abs ((int) ((a >> 8) & 0xff) - (int) ((b >> 8) & 0xff));
                        const int db = std::abs ((int) (a & 0xff) - (int) (b & 0xff));
                        differs[(size_t) x] = (dr + dg + db) > 60;
                    }

                    std::vector<Run> blackRuns = findRuns (blackRow, pieceLo, pieceHi,
                                                           true, differs, 3);

                    /* Stitch a black key back together where a white-key divider runs
                       underneath it. Both rows are dark at the divider, so the
                       difference vanishes mid-key and one black key arrives as two
                       runs - which doubled the count and made the five-per-seven test
                       reject a perfectly good keyboard. */
                    {
                        std::vector<Run> joined;

                        for (const Run &r : blackRuns)
                        {
                            /* Scaled to the key width: a divider is a small fraction
                               of a key, so six pixels is right for a 44px keybed and
                               wrong for a 12px one, where it would weld neighbouring
                               black keys into a single run. */
                            const int stitch = std::max (2, (int) (medianGap / 6.0f));

                            if (! joined.empty() && r.first - joined.back().last <= stitch)
                                joined.back().last = r.last;
                            else
                                joined.push_back (r);
                        }

                        blackRuns.swap (joined);
                    }

                    if (blackRuns.size() < 4)
                        continue;

                    /* Keep only runs the width of a black key. Panel edges, labels and
                       highlight overlays differ between the rows too, and are not the
                       width of a key. */
                    {
                        std::vector<float> widths;

                        for (const Run &r : blackRuns)
                            widths.push_back ((float) (r.last - r.first + 1));

                        const float medianWidth = medianOf (widths);
                        std::vector<Run> kept;

                        for (const Run &r : blackRuns)
                            if (std::fabs ((float) (r.last - r.first + 1) - medianWidth)
                                <= medianWidth * 0.4f)
                                kept.push_back (r);

                        blackRuns.swap (kept);
                    }

                    if (blackRuns.size() < 4)
                        continue;

                    std::vector<float> blacks;

                    for (const Run &r : blackRuns)
                        blacks.push_back ((float) (r.first + r.last) * 0.5f);

                    /* How consistent the widths are. A row through the middle of the
                       black keys gives one width repeated; a row at their top or bottom
                       edge catches the anti-aliased ends and shatters each key into
                       fragments. Both give a readable grid, so prefer the good row
                       rather than take the first that works. */
                    float uniformity = 0.0f;

                    {
                        std::vector<float> keptWidths;

                        for (const Run &r : blackRuns)
                            keptWidths.push_back ((float) (r.last - r.first + 1));

                        const float mw = medianOf (keptWidths);
                        int close = 0;

                        for (float k : keptWidths)
                            if (std::fabs (k - mw) <= 1.0f)
                                ++close;

                        uniformity = (float) close / (float) keptWidths.size();
                    }

                    if (uniformity < 0.7f)
                        continue;

                    /* The lower row has to be below the black keys, not a few pixels
                       under the upper one. If it still cuts through them, the columns
                       where a black key sits are dark there too. */
                    {
                        const uint8_t *lowRow = &luma[(size_t) yWhite * width];
                        std::vector<float> lowValues;

                        for (int x = pieceLo; x <= pieceHi; ++x)
                            lowValues.push_back ((float) lowRow[x]);

                        const float lowMedian = medianOf (lowValues);
                        int stillDark = 0;

                        for (float b : blacks)
                        {
                            const int xi = (int) b;

                            if (xi >= 0 && xi < width
                                && (float) lowRow[xi] < lowMedian * 0.6f)
                                ++stillDark;
                        }

                        if ((float) stillDark > (float) blacks.size() * 0.2f)
                            continue;
                    }

                    /* Five black keys per seven white. Nothing else in a GUI does that,
                       and without it a panel of evenly spaced bars outranks the real
                       keyboard by being one bar wider. */
                    {
                        const float ratio = (float) blacks.size() / (float) whites.size();

                        if (std::fabs (ratio - 5.0f / 7.0f) > 0.25f)
                            continue;
                    }

                    std::vector<bool> hasBlack;

                    for (size_t i = 0; i + 1 < whites.size(); ++i)
                    {
                        bool found = false;

                        for (float b : blacks)
                            if (b > whites[i] && b < whites[i + 1])
                                found = true;

                        hasBlack.push_back (found);
                    }

                    if (hasBlack.size() < 6)
                        continue;

                    /* Where C sits is fitted, not matched exactly. A few stray black
                       keys would otherwise reject a reading that is 95% clean while a
                       far worse one passes on a couple of lucky matches. */
                    const bool expected[7] = { true, true, false, true, true, true, false };
                    int bestOffset = 0;
                    int bestHits = -1;

                    for (int off = 0; off < 7; ++off)
                    {
                        int hits = 0;

                        for (size_t i = 0; i < hasBlack.size(); ++i)
                            if (hasBlack[i] == expected[((int) i - off + 700) % 7])
                                ++hits;

                        if (hits > bestHits)
                        {
                            bestHits = hits;
                            bestOffset = off;
                        }
                    }

                    if ((float) bestHits < (float) hasBlack.size() * 0.80f)
                        continue;

                    /* How evenly the white keys are spaced on this row.

                       Without it nothing preferred a clean row over a messy one, and a
                       row running through the octave labels printed on the keys - the
                       C1, C2, C3 some plugins draw - scored just as well as a row above
                       them, while the letters added dark marks that read as dividers.
                       A real keybed has one spacing repeated; that is worth ranking on. */
                    float regularity = 0.0f;

                    {
                        std::vector<float> gaps;

                        for (size_t i = 1; i < whites.size(); ++i)
                            gaps.push_back (whites[i] - whites[i - 1]);

                        const float mg = medianOf (gaps);
                        int even = 0;

                        for (float g : gaps)
                            if (std::fabs (g - mg) <= std::max (1.5f, mg * 0.08f))
                                ++even;

                        regularity = gaps.empty() ? 0.0f : (float) even / (float) gaps.size();
                    }

                    if (regularity < 0.75f)
                        continue;

                    /* Size by white keys, which barely change with the row chosen; then
                       how even the spacing is and how consistent the black keys look,
                       to choose between rows of the same keyboard. */
                    /* Regularity multiplies rather than adds. Added, it never changed
                       anything: white-key count dominated, so a row carrying a few
                       spurious dividers still beat a clean row with one key fewer.
                       Multiplied, a row has to be both complete and even to win. */
                    const int score = (int) (regularity * uniformity
                                             * (float) whites.size() * 1000.0f);

                    if (score <= bestScore)
                        continue;

                    int firstC = bestOffset;

                    while (firstC - 7 >= 0)
                        firstC -= 7;

                    /* Extend where the grid cannot be measured. A plugin that paints a
                       solid colour over a group of keys erases the dividers underneath
                       and detection stops dead there. Positions are predictable once
                       the spacing is known. */
                    {
                        std::vector<float> steps;

                        for (size_t i = 1; i < whites.size(); ++i)
                            steps.push_back (whites[i] - whites[i - 1]);

                        const float step = medianOf (steps);

                        if (step > 2.0f)
                        {
                            int below = std::min (yWhite + 8, height - 1);

                            {
                                const int probe = (int) whites[whites.size() / 2];
                                const int at = luma[(size_t) yWhite * width + probe];

                                for (int y = yWhite + 1; y < std::min (yWhite + 60, height); ++y)
                                    if (std::abs ((int) luma[(size_t) y * width + probe] - at) > 25)
                                    {
                                        below = std::min (y + 2, height - 1);
                                        break;
                                    }
                            }

                            const int bandTop = std::max (0, yBlack - 4);
                            std::vector<float> keyLumas;

                            for (float wx : whites)
                                keyLumas.push_back ((float) luma[(size_t) yWhite * width + (int) wx]);

                            const float keyLuma = medianOf (keyLumas);

                            auto inKeybed = [&] (float px) -> bool
                            {
                                const int xi = (int) (px + 0.5f);

                                if (xi < 3 || xi >= width - 3)
                                    return false;

                                int lowMax = 0;
                                int bandMax = 0;

                                for (int dx = -2; dx <= 2; ++dx)
                                {
                                    const int v = luma[(size_t) yWhite * width + xi + dx];

                                    if (v > lowMax)
                                        lowMax = v;

                                    for (int y = bandTop; y <= yWhite; ++y)
                                    {
                                        const int q = luma[(size_t) y * width + xi + dx];

                                        if (q > bandMax)
                                            bandMax = q;
                                    }
                                }

                                if (std::abs (lowMax - (int) luma[(size_t) below * width + xi]) <= 20)
                                    return false;

                                return (float) bandMax > keyLuma * 0.35f;
                            };

                            int added = 0;

                            for (float x = whites.front() - step; added < 16; x -= step, ++added)
                            {
                                if (! inKeybed (x))
                                    break;

                                whites.insert (whites.begin(), x);
                                ++firstC;
                            }

                            added = 0;

                            for (float x = whites.back() + step; added < 16; x += step, ++added)
                            {
                                if (! inKeybed (x))
                                    break;

                                whites.push_back (x);
                            }
                        }
                    }

                    std::vector<Key> candidate;

                    for (size_t i = 0; i < whites.size(); ++i)
                    {
                        int rel = (int) i - firstC;
                        int oct = rel >= 0 ? rel / 7 : -(((-rel) + 6) / 7);
                        int step = rel - oct * 7;
                        Key k;
                        k.semitone = oct * 12 + kWhitePc[step];
                        k.x = whites[i];
                        k.y = yWhite;
                        k.rgb = pixels[(size_t) yWhite * width + (int) whites[i]] & 0x00ffffffu;
                        candidate.push_back (k);
                    }

                    /* Black keys are placed, not detected. Detecting them was only ever
                       needed to work out where C is; once the grid and the phase are
                       known, every black key sits on the boundary between two whites.
                       Placing them means the ones a plugin paints over are read like any
                       other rather than going missing. */
                    for (size_t i = 0; i + 1 < whites.size(); ++i)
                    {
                        int rel = (int) i - firstC;
                        int oct = rel >= 0 ? rel / 7 : -(((-rel) + 6) / 7);
                        int stepIndex = rel - oct * 7;
                        const int pc = blackAfterWhiteStep (stepIndex);

                        if (pc < 0)
                            continue;

                        const float bx = (whites[i] + whites[i + 1]) * 0.5f;
                        Key k;
                        k.semitone = oct * 12 + pc;
                        k.x = bx;
                        k.y = yBlack;
                        k.rgb = pixels[(size_t) yBlack * width + (int) bx] & 0x00ffffffu;
                        candidate.push_back (k);
                    }

                    bestScore = score;
                    keys = candidate;
                    measuredWhiteWidth = medianGap;
                    detectedOctaves = (int) (whites.size() / 7);
                }
            }
        }
    }

    discardPixels();

    if (keys.empty())
    {
        message = "no keyboard found in that window";
        return false;
    }

    message = std::to_string (keys.size()) + " keys, "
            + std::to_string (detectedOctaves) + " octaves, "
            + std::to_string ((int) measuredWhiteWidth) + "px per white key";

    if (measuredWhiteWidth < 8.0f)
        message += "  (too small to trust)";

    return true;
}

/* Overwrite before releasing, so a capture of someone else's plugin is not left
   sitting in freed memory. Nothing here ever writes it to disk. */
void KeybedCapture::discardPixels()
{
    if (! pixels.empty())
        std::memset (pixels.data(), 0, pixels.size() * sizeof (uint32_t));

    if (! luma.empty())
        std::memset (luma.data(), 0, luma.size());

    pixels.clear();
    pixels.shrink_to_fit();
    luma.clear();
    luma.shrink_to_fit();
}

uint32_t KeybedCapture::colourAt (int semitoneFromFirstC) const
{
    for (const Key &k : keys)
        if (k.semitone == semitoneFromFirstC)
            return k.rgb;

    return 0;
}

bool KeybedCapture::samplePoint (int semitoneFromFirstC, int &x, int &y) const
{
    for (const Key &k : keys)
        if (k.semitone == semitoneFromFirstC)
        {
            x = (int) k.x;
            y = k.y;
            return true;
        }

    return false;
}

bool KeybedCapture::keyInfo (int index, int &semitone, uint32_t &rgb) const
{
    if (index < 0 || index >= (int) keys.size())
        return false;

    semitone = keys[index].semitone;
    rgb = keys[index].rgb;
    return true;
}

bool KeybedCapture::sample()
{
    return refreshInternal (nullptr, 0);
}

bool KeybedCapture::refresh (LumiLink &link, int anchorNote)
{
    return refreshInternal (&link, anchorNote);
}

/*
    Find the followed window again after it has been closed and reopened.

    Matched by title rather than by handle, and only the geometry is kept - the keys are
    where they were, because the plugin reopens at the size it was left. If it does not,
    the size check in the refresh below notices and stops rather than sampling nonsense.
*/
bool KeybedCapture::refindWindow()
{
    if (followTitle.empty())
        return false;

    refreshWindows();

    for (const CaptureWindow &w : windowList)
    {
        if (w.title == followTitle && w.width == width && w.height == height)
        {
            lastWindow = w.handle;
            return true;
        }
    }

    return false;
}

bool KeybedCapture::refreshInternal (LumiLink *link, int anchorNote)
{
    if (keys.empty() || lastWindow == nullptr)
        return false;

#if defined (_WIN32)
    HWND hwnd = (HWND) lastWindow;

    if (! IsWindow (hwnd))
    {
        message = "the window has closed";
        return false;
    }

    RECT r;

    if (! GetWindowRect (hwnd, &r))
        return false;

    /* A resized window invalidates every key position, so stop rather than sample
       whatever now happens to sit at those coordinates. */
    if (r.right - r.left != width || r.bottom - r.top != height)
    {
        message = "window resized - read the keyboard again";
        keys.clear();
        return false;
    }

    HDC screenDc = GetDC (nullptr);
    HDC memDc = CreateCompatibleDC (screenDc);
    HBITMAP bmp = CreateCompatibleBitmap (screenDc, width, height);
    HGDIOBJ previous = SelectObject (memDc, bmp);

    PrintWindow (hwnd, memDc, 2);

    pixels.assign ((size_t) width * height, 0);

    BITMAPINFO bi;
    ZeroMemory (&bi, sizeof (bi));
    bi.bmiHeader.biSize = sizeof (bi.bmiHeader);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    GetDIBits (memDc, bmp, 0, height, pixels.data(), &bi, DIB_RGB_COLORS);

    SelectObject (memDc, previous);
    DeleteObject (bmp);
    DeleteDC (memDc);
    ReleaseDC (nullptr, screenDc);

    for (Key &k : keys)
        k.rgb = pixels[(size_t) k.y * width + (int) k.x] & 0x00ffffffu;

    discardPixels();

    if (link != nullptr)
        applyTo (*link, anchorNote);

    return true;
#elif defined (__APPLE__)
    if (lastWindow == nullptr)
        return false;

    int w = 0;
    int h = 0;

    if (! macCaptureWindow ((uint32_t) (uintptr_t) lastWindow, pixels, w, h))
    {
        message = "the window has gone";
        return false;
    }

    /* A resized window invalidates every key position, so stop rather than sample
       whatever now happens to sit at those coordinates. */
    if (w != width || h != height)
    {
        message = "window resized - read the keyboard again";
        keys.clear();
        return false;
    }

    for (Key &k : keys)
        k.rgb = pixels[(size_t) k.y * width + (int) k.x] & 0x00ffffffu;

    discardPixels();

    if (link != nullptr)
        applyTo (*link, anchorNote);

    return true;
#elif defined (__linux__)
    if (lastWindow == nullptr)
        return false;

    Display *display = XOpenDisplay (nullptr);

    if (display == nullptr)
        return false;

    const Window target = (Window) (uintptr_t) lastWindow;
    XWindowAttributes attr;

    if (XGetWindowAttributes (display, target, &attr) == 0)
    {
        XCloseDisplay (display);
        message = "the window has gone";
        return false;
    }

    /* A resized window invalidates every key position, so stop rather than sample
       whatever now sits at those coordinates. */
    if (attr.width != width || attr.height != height)
    {
        XCloseDisplay (display);
        message = "window resized - read the keyboard again";
        keys.clear();
        return false;
    }

    XImage *image = XGetImage (display, target, 0, 0,
                               (unsigned int) width, (unsigned int) height,
                               AllPlanes, ZPixmap);

    if (image == nullptr)
    {
        XCloseDisplay (display);
        return false;
    }

    for (Key &k : keys)
        k.rgb = (uint32_t) (XGetPixel (image, (int) k.x, k.y) & 0x00ffffffu);

    XDestroyImage (image);
    XCloseDisplay (display);

    if (link != nullptr)
        applyTo (*link, anchorNote);

    return true;
#else
    (void) link;
    (void) anchorNote;
    return false;
#endif
}

void KeybedCapture::applyTo (LumiLink &link, int anchorNote) const
{
    for (const Key &k : keys)
    {
        const int note = anchorNote + k.semitone;

        if (note >= 0 && note <= 127)
            link.setColour (note, k.rgb);
    }
}

}
