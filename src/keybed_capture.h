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
#include <vector>

namespace lumipaint {

class LumiLink;

struct CaptureWindow
{
    void *handle;
    std::string title;
    int width;
    int height;
    void *lastWindow;
};

/*
    Reads another plugin's on-screen keyboard and turns it into a colour map.

    Detection does not look for regularly spaced vertical edges - waveforms, sequencer
    lanes and meter bridges all have those and score as well as a keyboard does. It
    looks for the arrangement nothing else in a GUI produces: an upper strip divided
    into twelve per octave sitting on a lower strip divided into seven, with the black
    keys falling in the 2-then-3 grouping that fixes where C is.

    Keys are found from the dividing lines between them rather than by thresholding the
    keys themselves. One plugin draws near-black gaps, another mid-grey, a third draws
    every white key at the same value with a one-pixel divider; no brightness rule
    covers all three, but they all have dividers.

    Known limit: below roughly 8 pixels per white key the black keys are 2-3 pixels and
    cannot be resolved consistently, the grouping test matches on noise, and the result
    is confidently wrong rather than absent. detect() reports the measured key width so
    that case can be refused rather than trusted.
*/
class KeybedCapture
{
public:
    KeybedCapture();
    ~KeybedCapture() { discardPixels(); }

    void refreshWindows();
    const std::vector<CaptureWindow> &windows() const { return windowList; }

    bool grab (size_t index);

    /* Re-read the colours of an already-detected keyboard and push them straight out.
       Detection is not repeated: the geometry is known, so this is a capture and a
       few hundred pixel reads. Cheap enough to run several times a second, which is
       what makes a live view possible without re-analysing the window each time. */
    bool refresh (LumiLink &link, int anchorNote);

    /* Re-read the colours without sending anything. Used by the anchor probe, which
       needs a before and an after. */
    bool sample();

    /* Walk the detected keys, for comparing two samples. */
    bool keyInfo (int index, int &semitone, uint32_t &rgb) const;

    /* Feed pixels directly, so detection can be run against a stored capture off a
       real machine. The plugin never calls this; it exists so the algorithm can be
       tested rather than asserted to work. */
    bool loadImage (const uint32_t *bgra, int w, int h);
    bool detect();

    bool hasResult() const { return ! keys.empty(); }
    int keyCount() const { return (int) keys.size(); }
    float whiteKeyWidth() const { return measuredWhiteWidth; }
    int octaveCount() const { return detectedOctaves; }
    const std::string &status() const { return message; }

    /* The leftmost detected C becomes anchorNote; everything else follows from it.
       Nothing in the pixels says which C is middle C, so this has to be told. */
    void applyTo (LumiLink &link, int anchorNote) const;

    /* Zero and release the captured window. Called automatically once detection has
       run; nothing is ever written to disk. */
    void discardPixels();

    uint32_t colourAt (int semitoneFromFirstC) const;

    /* Where a key was sampled from, for showing the result over the capture. */
    bool samplePoint (int semitoneFromFirstC, int &x, int &y) const;

private:
    bool refreshInternal (LumiLink *link, int anchorNote);

    struct Key
    {
        int semitone;
        float x;
        int y;
        uint32_t rgb;
    };

    std::vector<CaptureWindow> windowList;
    std::vector<uint32_t> pixels;
    std::vector<uint8_t> luma;
    int width;
    int height;
    void *lastWindow;
    std::vector<Key> keys;
    float measuredWhiteWidth;
    int detectedOctaves;
    std::string message;
};

}
