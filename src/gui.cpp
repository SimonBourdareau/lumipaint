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

#include "lumipaint.h"
#include "keybed_capture.h"
#include "preset.h"
#include "imgui_host.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace lumipaint {
namespace {

const uint32_t kDefaultWidth = 1340;
const uint32_t kDefaultHeight = 900;

const bool kIsBlackKey[12] = { false, true, false, true, false, false, true, false, true, false, true, false };
const int kWhiteIndexInOctave[12] = { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6 };

const char *kNoteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

struct ScaleDefinition
{
    const char *name;
    uint32_t mask;
};

const ScaleDefinition kScales[] = {
    { "-- Modes of the major scale --", 0 },
    { "Major (Ionian)", 0xab5 },   // 7 notes
    { "Dorian", 0x6ad },   // 7 notes
    { "Phrygian", 0x5ab },   // 7 notes
    { "Lydian", 0xad5 },   // 7 notes
    { "Mixolydian", 0x6b5 },   // 7 notes
    { "Natural minor (Aeolian)", 0x5ad },   // 7 notes
    { "Locrian", 0x56b },   // 7 notes
    { "-- Melodic minor and its modes --", 0 },
    { "Melodic minor", 0xaad },   // 7 notes
    { "Dorian b2", 0x6ab },   // 7 notes
    { "Lydian augmented", 0xb55 },   // 7 notes
    { "Lydian dominant", 0x6d5 },   // 7 notes
    { "Mixolydian b6", 0x5b5 },   // 7 notes
    { "Locrian #2", 0x56d },   // 7 notes
    { "Altered (super Locrian)", 0x55b },   // 7 notes
    { "-- Harmonic minor and its modes --", 0 },
    { "Harmonic minor", 0x9ad },   // 7 notes
    { "Locrian #6", 0x66b },   // 7 notes
    { "Ionian #5", 0xb35 },   // 7 notes
    { "Ukrainian Dorian", 0x6cd },   // 7 notes
    { "Phrygian dominant", 0x5b3 },   // 7 notes
    { "Lydian #2", 0xad9 },   // 7 notes
    { "Altered diminished", 0x35b },   // 7 notes
    { "-- Harmonic major and its modes --", 0 },
    { "Harmonic major", 0x9b5 },   // 7 notes
    { "Dorian b5", 0x66d },   // 7 notes
    { "Phrygian b4", 0x59b },   // 7 notes
    { "Lydian b3", 0xacd },   // 7 notes
    { "Mixolydian b2", 0x6b3 },   // 7 notes
    { "Lydian augmented #2", 0xb59 },   // 7 notes
    { "-- Pentatonic and hexatonic --", 0 },
    { "Major pentatonic", 0x295 },   // 5 notes
    { "Minor pentatonic", 0x4a9 },   // 5 notes
    { "Suspended pentatonic", 0x4a5 },   // 5 notes
    { "Man Gong", 0x529 },   // 5 notes
    { "Ritusen (Yo)", 0x2a5 },   // 5 notes
    { "Blues minor", 0x4e9 },   // 6 notes
    { "Blues major", 0x29d },   // 6 notes
    { "Prometheus", 0x655 },   // 6 notes
    { "Tritone", 0x4d3 },   // 6 notes
    { "Augmented", 0x999 },   // 6 notes
    { "Whole tone", 0x555 },   // 6 notes
    { "-- Japanese and Chinese --", 0 },
    { "Hirajoshi", 0x18d },   // 5 notes
    { "In (Sakura)", 0x1a3 },   // 5 notes
    { "Insen", 0x4a3 },   // 5 notes
    { "Iwato", 0x463 },   // 5 notes
    { "Kumoi", 0x28d },   // 5 notes
    { "Chinese", 0x8d1 },   // 5 notes
    { "Balinese pelog", 0x18b },   // 5 notes
    { "-- Eastern and folk --", 0 },
    { "Double harmonic", 0x9b3 },   // 7 notes
    { "Hungarian minor", 0x9cd },   // 7 notes
    { "Hungarian major", 0x6d9 },   // 7 notes
    { "Neapolitan major", 0xaab },   // 7 notes
    { "Neapolitan minor", 0x9ab },   // 7 notes
    { "Persian", 0x973 },   // 7 notes
    { "Enigmatic", 0xd53 },   // 7 notes
    { "Todi", 0x9cb },   // 7 notes
    { "Marva", 0xad3 },   // 7 notes
    { "Purvi", 0x9d3 },   // 7 notes
    { "-- Bebop and symmetric --", 0 },
    { "Bebop dominant", 0xeb5 },   // 8 notes
    { "Bebop major", 0xbb5 },   // 8 notes
    { "Bebop Dorian", 0x6bd },   // 8 notes
    { "Bebop melodic minor", 0xbad },   // 8 notes
    { "Diminished (whole-half)", 0xb6d },   // 8 notes
    { "Diminished (half-whole)", 0x6db },   // 8 notes
    { "Spanish 8-tone", 0x57b },   // 8 notes
    { "Chromatic", 0xfff },   // 12 notes
};

const int kNumScales = (int) (sizeof (kScales) / sizeof (kScales[0]));

uint32_t hsvToRgb (float h, float s, float v)
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    ImGui::ColorConvertHSVtoRGB (h, s, v, r, g, b);
    return (((uint32_t) (r * 255.0f + 0.5f)) << 16)
         | (((uint32_t) (g * 255.0f + 0.5f)) << 8)
         |  ((uint32_t) (b * 255.0f + 0.5f));
}

ImU32 rgbToImU32 (uint32_t rgb, float alpha)
{
    return IM_COL32 ((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, (int) (alpha * 255.0f));
}

int pitchClassOf (int note)
{
    return ((note % 12) + 12) % 12;
}

class LumiEditor
{
public:
    explicit LumiEditor (LumiPaint *ownerIn)
        : owner (ownerIn), window (nullptr)
    {
        for (int i = 0; i < 128; ++i)
            selected[i] = false;

        selected[60] = true;
        anchorNote = 60;
        pickerColour[0] = 1.0f;
        pickerColour[1] = 0.3f;
        pickerColour[2] = 0.1f;
        paintScope = 0;
        scaleRoot = 0;
        scaleIndex = 1;   // first real entry, the major scale
        scaleRootColour[0] = 1.0f; scaleRootColour[1] = 1.0f; scaleRootColour[2] = 1.0f;
        scaleInColour[0] = 0.12f;  scaleInColour[1] = 0.56f;  scaleInColour[2] = 1.0f;
        scaleOutColour[0] = 0.0f;  scaleOutColour[1] = 0.0f;  scaleOutColour[2] = 0.0f;
        whiteKeyWidth = 15.0f;
        /* The whole range by default. Starting at a five-octave window meant painting
           a note outside it needed the Range control moved first, which is a step
           nobody should have to discover. */
        lowNote = 0;
        highNote = 127;
        labelBuffer[0] = 0;
    }

    ImGuiHostWindow *hostWindow() const
    {
        return window;
    }

    void setHostWindow (ImGuiHostWindow *w)
    {
        window = w;
    }

    /*
        Each section sits in its own rounded panel with a tint of its own.

        The panel is an auto-sizing child window rather than a rectangle drawn by hand,
        so it grows with whatever is inside it and nothing has to be measured. The
        tints are dark and close together: enough to separate one group from the next
        at a glance, not enough to compete with the colours being edited, which are the
        thing that actually has to read accurately here.
    */
    void beginSection (const char *title, ImU32 tint)
    {
        ImGui::PushStyleColor (ImGuiCol_ChildBg, tint);
        ImGui::PushStyleVar (ImGuiStyleVar_ChildRounding, 7.0f);
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (10.0f, 8.0f));
        ImGui::BeginChild (title, ImVec2 (0.0f, 0.0f),
                           ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Border);
        ImGui::PushStyleColor (ImGuiCol_Text, IM_COL32 (255, 255, 255, 190));
        ImGui::TextUnformatted (title);
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    void endSection()
    {
        ImGui::EndChild();
        ImGui::PopStyleVar (2);
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    void draw()
    {
        ImGui::SetNextWindowPos (ImVec2 (0.0f, 0.0f));
        ImGui::SetNextWindowSize (ImGui::GetIO().DisplaySize);
        ImGui::Begin ("LumiPaint", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                      | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        drawStatusBar();
        ImGui::Separator();
        drawKeyboard();
        ImGui::Separator();

        /*
            Tabs, so the console does not crowd the controls.

            The keyboard stays above both: it is the thing you look at while doing
            anything here, including reading the log.
        */
        drawControlPanel();
    }

    void drawControlPanel()
    {
        /* Three columns rather than two.

           Two put the panel around 1150 pixels tall, which does not fit a 1080p screen
           with a DAW behind it, so the choice was scrolling forever or going wider.
           Wider wins: these sections are narrow by nature, and the sliders had been
           running off the right edge of the two-column layout anyway. */
        ImGui::Columns (3, "lumiColumns", false);

        /*
            Not equal thirds.

            Display effects is the widest thing in the panel - a checkbox, a swatch and
            two sliders on one line - and at a third of the width its sliders ran off
            the edge and had to be scrolled to inside their own box, which is a silly
            thing to ask of anyone. Key mapping and Auto-detection on the right are the
            narrowest sections, so the space comes from there.

            Set every frame rather than once: ImGui remembers column widths per window,
            and a stale remembered value from an earlier build would otherwise stick.
        */
        {
            const float total = ImGui::GetWindowContentRegionMax().x
                              - ImGui::GetWindowContentRegionMin().x;

            ImGui::SetColumnWidth (0, total * 0.30f);
            ImGui::SetColumnWidth (1, total * 0.40f);
            ImGui::SetColumnWidth (2, total * 0.30f);
        }

        beginSection ("Colour", IM_COL32 (34, 30, 44, 255));
        drawPaintControls();
        endSection();

        beginSection ("Modes", IM_COL32 (28, 36, 30, 255));
        drawGenerators();
        endSection();

        /* The empty space under the first column, which is the only place in the panel
           that stays empty at any size. */
        drawWordmark();

        ImGui::NextColumn();

        beginSection ("Display effects", IM_COL32 (26, 40, 42, 255));
        drawMotionControls();
        endSection();

        beginSection ("Degrees", IM_COL32 (38, 30, 40, 255));
        drawDegreesSection();
        endSection();

        beginSection ("Lighting", IM_COL32 (30, 32, 44, 255));
        drawLightingSection();
        endSection();

        ImGui::NextColumn();

        beginSection ("Sounding notes", IM_COL32 (42, 28, 34, 255));
        drawSoundingSection();
        endSection();

        beginSection ("Auto-detection of coloured keys", IM_COL32 (26, 34, 44, 255));
        drawCaptureControls();
        endSection();

        beginSection ("Keybed", IM_COL32 (40, 34, 26, 255));
        drawKeybedControls();
        endSection();

        beginSection ("Key mapping", IM_COL32 (32, 38, 34, 255));
        drawKeyMappingSection();
        endSection();

        ImGui::Columns (1);

        ImGui::End();
    }

private:
    void drawStatusBar()
    {
        drawPortSelector();

        ImGui::SameLine (0.0f, 20.0f);
        ImGui::SetNextItemWidth (140.0f);
        ImGui::SliderFloat ("Key width", &whiteKeyWidth, 9.0f, 28.0f, "%.0f px");

        ImGui::SameLine (0.0f, 20.0f);
        ImGui::SetNextItemWidth (140.0f);

        if (ImGui::DragIntRange2 ("Range", &lowNote, &highNote, 1.0f, 0, 127, "%d", "%d"))
        {
            if (highNote - lowNote < 12)
                highNote = lowNote + 12;

            if (highNote > 127)
            {
                highNote = 127;
                lowNote = 115;
            }
        }
    }

    /*
        The mark, drawn rather than loaded.

        Three white keys with two black ones over the joins, lit from underneath. Two
        blacks between three whites is the smallest arrangement the eye reads as a
        keyboard rather than as bars, and the glow sits below the keys because the
        plugin lights a keyboard, it does not draw on one.

        Drawn with the draw list so there is no image to decode, no file to ship
        alongside the binary, and it stays sharp at whatever scale the host asks for.
    */
    void drawLogo (float height)
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();

        const float h = height;
        const float kw = h * 0.30f;
        const float kh = h * 0.78f;
        const float bw = kw * 0.62f;
        const float bh = kh * 0.60f;
        const float top = p.y + h * 0.08f;

        static const ImU32 spectrum[5] = {
            IM_COL32 (255, 59, 48, 255),  IM_COL32 (255, 214, 10, 255),
            IM_COL32 (48, 209, 88, 255),  IM_COL32 (0, 196, 255, 255),
            IM_COL32 (156, 106, 255, 255)
        };

        /* The light on the floor, a spectrum bar under the keys. */
        const float gy = top + kh + h * 0.06f;
        const float gw = kw * 3.0f;

        for (int i = 0; i < 4; ++i)
            dl->AddRectFilledMultiColor (
                ImVec2 (p.x + gw * (float) i / 4.0f, gy),
                ImVec2 (p.x + gw * (float) (i + 1) / 4.0f, gy + h * 0.09f),
                spectrum[i], spectrum[i + 1], spectrum[i + 1], spectrum[i]);

        for (int i = 0; i < 3; ++i)
        {
            const float x = p.x + kw * (float) i;
            const ImU32 face = i == 1 ? IM_COL32 (0, 196, 255, 255)
                                      : IM_COL32 (242, 242, 244, 255);
            dl->AddRectFilled (ImVec2 (x + 1.0f, top),
                               ImVec2 (x + kw - 1.0f, top + kh), face, h * 0.09f);
        }

        for (int i = 0; i < 2; ++i)
        {
            const float x = p.x + kw * ((float) i + 1.0f) - bw * 0.5f;
            dl->AddRectFilled (ImVec2 (x, top), ImVec2 (x + bw, top + bh),
                               IM_COL32 (34, 34, 42, 255), h * 0.06f);
        }

        ImGui::Dummy (ImVec2 (gw + 8.0f, h));
        ImGui::SameLine();
    }

    /* The horizontal lockup, for the empty space under the last column. */
    void drawWordmark()
    {
        ImGui::Spacing();
        ImGui::Spacing();
        drawLogo (30.0f);

        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float scale = 25.0f / ImGui::GetFontSize();

        dl->AddText (nullptr, 25.0f, ImVec2 (p.x, p.y + 1.0f),
                     IM_COL32 (232, 232, 236, 235), "Lumi");
        dl->AddText (nullptr, 25.0f,
                     ImVec2 (p.x + ImGui::CalcTextSize ("Lumi").x * scale, p.y + 1.0f),
                     IM_COL32 (0, 196, 255, 245), "Paint");

        /* The version under the wordmark, quietly. Someone reporting a problem should
           not have to go looking for which build they are on. */
        dl->AddText (nullptr, 13.0f, ImVec2 (p.x + 2.0f, p.y + 28.0f),
                     IM_COL32 (110, 114, 124, 220), kPluginVersion);

        ImGui::Dummy (ImVec2 (200.0f, 40.0f));
    }

    void drawPortSelector()
    {
        drawLogo (22.0f);

        /* Opening the editor is as good a sign of intent as playing the track. */
        if (! shownOnce)
        {
            shownOnce = true;
            owner->link.claimDevice();
        }

        const bool connected = owner->link.isConnected();
        const std::string active = owner->link.getActivePort();
        const std::string request = owner->link.getRequestedPort();

        const bool mine = owner->link.hasDevice();
        ImGui::PushStyleColor (ImGuiCol_Text, connected ? IM_COL32 (110, 220, 140, 255)
                                                        : IM_COL32 (230, 170, 90, 255));
        ImGui::TextUnformatted (connected ? "*" : "-");
        ImGui::PopStyleColor();
        ImGui::SameLine();

        std::vector<std::string> ports;
        owner->link.getAvailablePorts (ports);

        const char *preview = request.empty() ? "Auto (first ROLI keyboard)" : request.c_str();

        ImGui::SetNextItemWidth (230.0f);

        if (ImGui::BeginCombo ("##port", preview))
        {
            if (ImGui::Selectable ("Auto (first ROLI keyboard)", request.empty()))
                owner->link.setRequestedPort (std::string());

            for (size_t i = 0; i < ports.size(); ++i)
                if (ImGui::Selectable (ports[i].c_str(), ports[i] == request))
                    owner->link.setRequestedPort (ports[i]);

            if (ports.empty())
                ImGui::TextDisabled ("no MIDI outputs found");

            ImGui::EndCombo();
        }

        ImGui::SameLine();

        if (! mine)
            ImGui::TextDisabled (owner->link.heldElsewhere()
                                     ? "another instance is holding the keyboard"
                                     : "another instance has the keyboard");
        else if (! connected)
            ImGui::TextDisabled ("not connected");
        else if (! owner->link.hasExternalInput())
            ImGui::TextDisabled ("listening through the host");
        else if (! active.empty() && active != request)
            ImGui::TextDisabled ("using %s", active.c_str());

        /* The input port, chosen rather than guessed. On Windows a MIDI input belongs
           to one application at a time, so the obvious port is often held by the host
           and a second one - Bluetooth, or a loopback - is the way in. */
        ImGui::SameLine (0.0f, 12.0f);
        ImGui::SetNextItemWidth (170.0f);

        const std::string activeIn = owner->link.activeInputPort();

        if (ImGui::BeginCombo ("##inport", activeIn.empty() ? "listen: auto"
                                                            : activeIn.c_str()))
        {
            if (ImGui::Selectable ("auto (match the output)"))
                owner->link.setInputPort (std::string());

            for (const std::string &name : owner->link.inputPortNames())
                if (ImGui::Selectable (name.c_str(), name == activeIn))
                    owner->link.setInputPort (name);

            ImGui::EndCombo();
        }

        ImGui::SameLine (0.0f, 20.0f);


        ImGui::SameLine (0.0f, 20.0f);
        bool hold = owner->link.getHoldDevice();

        if (ImGui::Checkbox ("Hold", &hold))
            owner->link.setHoldDevice (hold);

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip ("Keep the keyboard on this instance instead of letting\n"
                               "whichever track you play take it");
    }

    void drawKeyboard()
    {
        const int firstWhite = whiteIndexForNote (lowNote);
        const int lastWhite = whiteIndexForNote (highNote);
        const float totalWidth = (float) (lastWhite - firstWhite + 1) * whiteKeyWidth;
        const float whiteHeight = 130.0f;
        const float blackHeight = whiteHeight * 0.62f;
        const float blackWidth = whiteKeyWidth * 0.62f;

        ImGui::BeginChild ("keyboard", ImVec2 (0.0f, whiteHeight + 40.0f), 0,
                           ImGuiWindowFlags_HorizontalScrollbar);

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList *draw = ImGui::GetWindowDrawList();

        ImGui::InvisibleButton ("keybed", ImVec2 (totalWidth, whiteHeight));
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 mouse = ImGui::GetIO().MousePos;

        int hitNote = -1;

        if (hovered)
            hitNote = noteAtPosition (mouse, origin, firstWhite, whiteHeight, blackHeight, blackWidth);

        for (int note = lowNote; note <= highNote; ++note)
        {
            if (kIsBlackKey[pitchClassOf (note)])
                continue;

            const float x = origin.x + (float) (whiteIndexForNote (note) - firstWhite) * whiteKeyWidth;
            drawKey (draw, note, x, origin.y, whiteKeyWidth, whiteHeight, note == hitNote);
        }

        for (int note = lowNote; note <= highNote; ++note)
        {
            if (! kIsBlackKey[pitchClassOf (note)])
                continue;

            const float x = blackKeyX (note, origin.x, firstWhite, blackWidth);
            drawKey (draw, note, x, origin.y, blackWidth, blackHeight, note == hitNote);
        }

        for (int note = lowNote; note <= highNote; ++note)
        {
            if (pitchClassOf (note) != 0)
                continue;

            const float x = origin.x + (float) (whiteIndexForNote (note) - firstWhite) * whiteKeyWidth;
            draw->AddText (ImVec2 (x + 2.0f, origin.y + whiteHeight + 4.0f),
                           IM_COL32 (150, 150, 155, 255), octaveLabel (note));
        }

        /*
            One bracket per block, not one for the whole chain.

            A single bracket said which notes the hardware could show but not which
            block showed them, so a chain that had not spread out looked exactly like
            one that had. Drawing them separately makes the arrangement visible: two
            brackets side by side is a chain covering two ranges, two brackets on top
            of each other is two blocks showing the same notes, which means the
            topology is not being read.
        */
        {
            const int wLow = owner->link.getWindowLow();
            const int blocks = owner->link.getBlockCount();
            const float y0 = origin.y + whiteHeight + 18.0f;

            if (blocks < 1)
            {
                draw->AddText (ImVec2 (origin.x, y0 - 2.0f),
                               IM_COL32 (150, 150, 155, 200), "no block reporting");
            }

            for (int b = 0; b < blocks; ++b)
            {
                /* Where the block says it is, not where the layout implies it should
                   be. Mirrored blocks then draw on top of each other, which is exactly
                   what they are doing. */
                /* Reported if it can report, assumed contiguous if it cannot - a
                   chained block has no route back to the host and stays silent. */
                const int reported = owner->link.getBlockLow (b);
                const int bLow = reported >= 0 ? reported : wLow + b * 24;
                const int bHigh = bLow + 23;

                if (bHigh < lowNote || bLow > highNote)
                    continue;

                const int fromNote = bLow < lowNote ? lowNote : bLow;
                const int toNote = bHigh > highNote ? highNote : bHigh;
                const float x0 = origin.x + (float) (whiteIndexForNote (fromNote) - firstWhite) * whiteKeyWidth;
                const float x1 = origin.x + (float) (whiteIndexForNote (toNote) - firstWhite + 1) * whiteKeyWidth;

                /* A colour each. With the blocks linked they sit end to end and the
                   colours simply label them; unlinked they can be anywhere, including
                   on top of each other, and the colours are the only way to tell which
                   block is where. */
                static const ImU32 blockTints[5] = {
                    IM_COL32 (110, 220, 140, 220), IM_COL32 (120, 170, 255, 220),
                    IM_COL32 (255, 190, 90, 220),  IM_COL32 (220, 130, 220, 220),
                    IM_COL32 (255, 120, 110, 220)
                };

                const ImU32 tint = blockTints[b % 5];

                /* Stepped down a little each, so two blocks on the same notes draw as
                   two brackets rather than one. */
                const float y = y0 + (float) b * 6.0f;

                draw->AddLine (ImVec2 (x0, y), ImVec2 (x1, y), tint, 2.0f);
                draw->AddLine (ImVec2 (x0, y - 4.0f), ImVec2 (x0, y), tint, 2.0f);
                draw->AddLine (ImVec2 (x1, y - 4.0f), ImVec2 (x1, y), tint, 2.0f);

                char label[16];
                std::snprintf (label, sizeof (label), "block %d", b + 1);
                draw->AddText (ImVec2 (x0 + 3.0f, y - 15.0f), tint, label);
            }
        }

        if (hitNote >= 0 && ImGui::IsItemClicked (ImGuiMouseButton_Left))
            applyClick (hitNote);

        if (hitNote >= 0 && ImGui::IsItemActive() && ImGui::GetIO().KeyAlt)
            paintNote (hitNote);

        ImGui::EndChild();

        if (hitNote >= 0)
            ImGui::SetTooltip ("%s%d", kNoteNames[pitchClassOf (hitNote)], octaveOf (hitNote));
    }

    void drawKey (ImDrawList *draw, int note, float x, float y, float width, float height, bool isHovered)
    {
        /* The composited colour, so ripples, afterglow, pulse and halo all show here
           as they do on the keyboard. Redrawn at the editor's own frame rate rather
           than the MIDI send rate, so this is a slightly smoother view of the same
           thing - handy for setting trail and speed. */
        const uint32_t rgb = owner->link.getDisplayColour (note);
        const bool lit = owner->link.isNoteLit (note);
        const bool isSelected = selected[note];
        const bool onHardware = note >= owner->link.getWindowLow()
                             && note <= owner->link.getWindowHigh();
        const float restAlpha = kIsBlackKey[pitchClassOf (note)] ? 0.55f : 0.80f;

        const ImVec2 tl (x + 1.0f, y);
        const ImVec2 br (x + width - 1.0f, y + height);

        draw->AddRectFilled (tl, br, rgbToImU32 (rgb, lit ? 1.0f : restAlpha), 2.0f);

        /* Outside the window the hardware shows nothing, so say so rather than
           implying these keys are lit somewhere. */
        if (! onHardware)
            draw->AddRectFilled (tl, br, IM_COL32 (10, 10, 12, 150), 2.0f);

        if (lit)
            draw->AddRectFilled (ImVec2 (tl.x, br.y - 8.0f), br, IM_COL32 (255, 255, 255, 220), 2.0f);

        draw->AddRect (tl, br,
                       isSelected ? IM_COL32 (255, 255, 255, 255) : IM_COL32 (25, 25, 28, 255),
                       2.0f, 0, isSelected ? 2.0f : 1.0f);

        if (isHovered)
            draw->AddRect (tl, br, IM_COL32 (180, 200, 255, 200), 2.0f, 0, 1.5f);
    }

    /*
        The octave number shown beside a note name.

        One convention, not a setting: note 0 is C-2, so note 60 is C3 and note 127 is
        G8. That is the range a DAW shows, and a keyboard that disagrees with the track
        above it about the name of the key just pressed is worse than one with no
        labels. There used to be a chooser here offering four conventions; it could only
        ever be set wrong, and being wrong looked exactly like the anchor being wrong.
    */
    int octaveOf (int note) const
    {
        return note / 12 - 2;
    }

    int whiteIndexForNote (int note) const
    {
        const int octave = note / 12;
        return octave * 7 + kWhiteIndexInOctave[pitchClassOf (note)];
    }

    float blackKeyX (int note, float originX, int firstWhite, float blackWidth) const
    {
        const int leftWhite = whiteIndexForNote (note - 1);
        return originX + (float) (leftWhite - firstWhite + 1) * whiteKeyWidth - blackWidth * 0.5f;
    }

    int noteAtPosition (const ImVec2 &mouse, const ImVec2 &origin, int firstWhite,
                        float whiteHeight, float blackHeight, float blackWidth) const
    {
        for (int note = lowNote; note <= highNote; ++note)
        {
            if (! kIsBlackKey[pitchClassOf (note)])
                continue;

            const float x = blackKeyX (note, origin.x, firstWhite, blackWidth);

            if (mouse.x >= x && mouse.x < x + blackWidth
                && mouse.y >= origin.y && mouse.y < origin.y + blackHeight)
                return note;
        }

        for (int note = lowNote; note <= highNote; ++note)
        {
            if (kIsBlackKey[pitchClassOf (note)])
                continue;

            const float x = origin.x + (float) (whiteIndexForNote (note) - firstWhite) * whiteKeyWidth;

            if (mouse.x >= x && mouse.x < x + whiteKeyWidth
                && mouse.y >= origin.y && mouse.y < origin.y + whiteHeight)
                return note;
        }

        return -1;
    }

    void applyClick (int note)
    {
        const ImGuiIO &io = ImGui::GetIO();

        if (io.KeyShift && anchorNote >= 0)
        {
            const int from = anchorNote < note ? anchorNote : note;
            const int to = anchorNote < note ? note : anchorNote;

            for (int n = from; n <= to; ++n)
                selected[n] = true;
        }
        else if (io.KeyCtrl)
        {
            selected[note] = ! selected[note];
            anchorNote = note;
        }
        else
        {
            for (int n = 0; n < 128; ++n)
                selected[n] = false;

            if (paintScope == 1)
            {
                const int pc = pitchClassOf (note);

                for (int n = 0; n < 128; ++n)
                    selected[n] = pitchClassOf (n) == pc;
            }
            else
            {
                selected[note] = true;
            }

            anchorNote = note;
        }
    }

    void paintNote (int note)
    {
        const uint32_t rgb = currentPickerRgb();

        if (paintScope == 1)
        {
            const int pc = pitchClassOf (note);

            for (int n = 0; n < 128; ++n)
                if (pitchClassOf (n) == pc)
                    owner->link.setColour (n, rgb);
        }
        else
        {
            owner->link.setColour (note, rgb);
        }

        markDirty();
    }

    int countSelected() const
    {
        int total = 0;

        for (int n = 0; n < 128; ++n)
            if (selected[n])
                ++total;

        return total;
    }

    uint32_t currentPickerRgb() const
    {
        return (((uint32_t) (pickerColour[0] * 255.0f + 0.5f)) << 16)
             | (((uint32_t) (pickerColour[1] * 255.0f + 0.5f)) << 8)
             |  ((uint32_t) (pickerColour[2] * 255.0f + 0.5f));
    }

    void drawPaintControls()
    {
        ImGui::SetNextItemWidth (150.0f);
        ImGui::ColorPicker3 ("##picker", pickerColour,
                             ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview
                             | ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_PickerHueBar);

        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::ColorButton ("##swatch", ImVec4 (pickerColour[0], pickerColour[1], pickerColour[2], 1.0f),
                            ImGuiColorEditFlags_NoTooltip, ImVec2 (60.0f, 40.0f));
        ImGui::Text ("%d of 128 selected", countSelected());
        ImGui::SetNextItemWidth (120.0f);
        ImGui::Combo ("##scope", &paintScope, "Single note\0All octaves\0");
        ImGui::TextDisabled ("alt-drag paints");
        ImGui::EndGroup();

        ImGui::SetNextItemWidth (90.0f);

        if (ImGui::DragInt ("##note", &anchorNote, 0.25f, 0, 127, "%d"))
            selectSingle (anchorNote);

        ImGui::SameLine();
        ImGui::Text ("%s%d", kNoteNames[pitchClassOf (anchorNote)], octaveOf (anchorNote));
        ImGui::SameLine();

        if (ImGui::Button ("Set this note", ImVec2 (120.0f, 0.0f)))
        {
            owner->link.setColour (anchorNote, currentPickerRgb());
            markDirty();
        }

        if (ImGui::Button ("Apply to selection", ImVec2 (150.0f, 0.0f)))
            applyToSelection (currentPickerRgb());

        ImGui::SameLine();

        if (ImGui::Button ("Pick from selection", ImVec2 (150.0f, 0.0f)))
            pickFromSelection();

        if (ImGui::Button ("Fill unselected", ImVec2 (150.0f, 0.0f)))
            applyToUnselected (currentPickerRgb());

        ImGui::SameLine();

        if (ImGui::Button ("Invert selection", ImVec2 (150.0f, 0.0f)))
            for (int n = 0; n < 128; ++n)
                selected[n] = ! selected[n];

        /*
            Maps as files, saved wherever you like.

            The dialog is not opened here. It is modal, and a modal dialog runs its own
            message loop - so opening one in the middle of building a frame lets the
            repaint timer fire and re-enter a frame that is still open, which locks the
            editor up. The button records what was asked for and the frame finishes
            first.
        */
        if (ImGui::Button ("Save map...", ImVec2 (150.0f, 0.0f)))
            pendingDialog = 1;

        ImGui::SameLine();

        if (ImGui::Button ("Load map...", ImVec2 (150.0f, 0.0f)))
            pendingDialog = 2;

        if (! presetMessage.empty())
            ImGui::TextDisabled ("%s", presetMessage.c_str());

        if (ImGui::Button ("Select all", ImVec2 (150.0f, 0.0f)))
            for (int n = 0; n < 128; ++n)
                selected[n] = true;

        ImGui::SameLine();

        if (ImGui::Button ("Select none", ImVec2 (150.0f, 0.0f)))
            for (int n = 0; n < 128; ++n)
                selected[n] = false;
    }

    void drawGenerators()
    {

        if (ImGui::Button ("Chromatic wheel", ImVec2 (160.0f, 0.0f)))
            generateWheel (1.0f);

        ImGui::SameLine();

        if (ImGui::Button ("Circle of fifths", ImVec2 (160.0f, 0.0f)))
            generateFifths();

        if (ImGui::Button ("Piano", ImVec2 (160.0f, 0.0f)))
            generatePiano();

        ImGui::SameLine();

        if (ImGui::Button ("Blackout", ImVec2 (160.0f, 0.0f)))
            applyToAll (0x000000);

        ImGui::Spacing();
        ImGui::SetNextItemWidth (110.0f);

        /* Changing either recolours immediately. Making you blacken the keyboard,
           select the in-scale notes and then apply was three steps to express one
           intention. */
        if (ImGui::Combo ("Root", &scaleRoot, kNoteNames, 12))
            generateScale();

        ImGui::SameLine();
        ImGui::SetNextItemWidth (170.0f);

        if (ImGui::BeginCombo ("Scale", kScales[scaleIndex].name))
        {
            /* Entries with an empty mask are category headings, not scales. */
            for (int i = 0; i < kNumScales; ++i)
            {
                if (kScales[i].mask == 0)
                {
                    ImGui::TextDisabled ("%s", kScales[i].name);
                    continue;
                }

                if (ImGui::Selectable (kScales[i].name, i == scaleIndex))
                {
                    scaleIndex = i;
                    generateScale();
                }
            }

            ImGui::EndCombo();
        }

        ImGui::ColorEdit3 ("Root##scalecol", scaleRootColour, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::ColorEdit3 ("In##scalecol", scaleInColour, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::ColorEdit3 ("Out##scalecol", scaleOutColour, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();

        if (ImGui::Button ("Reapply"))
            generateScale();

        if (ImGui::IsItemDeactivatedAfterEdit())
            generateScale();

        ImGui::SameLine();

        if (ImGui::Button ("Select in-scale", ImVec2 (130.0f, 0.0f)))
            selectScale();

    }

    void drawLightingSection()
    {
        drawParamSlider ("Brightness", kParamBrightness, owner->brightness);
        drawParamSlider ("Unlit level", kParamUnlitLevel, owner->unlitLevel);

        /* Automatic follows how many blocks are connected: one block is on USB and
           takes a hard drive, a chain has to feed its second block over the relay and
           drops writes if pushed. Raise it by hand if your chain copes. */
        int rate = owner->link.getSendRate();
        ImGui::SetNextItemWidth (200.0f);

        if (ImGui::SliderInt ("Send rate", &rate, 0, 4,
                              rate == 0 ? "automatic" : "%d"))
        {
            owner->link.setSendRate (rate);
            markDirty();
        }
    }

    void drawSoundingSection()
    {
        drawHighlightControl();
        drawPressControl();
        drawGradientControl ("Pressure", kParamPressureGradient, owner->pressureGradient,
                             owner->link.getPressureGradColour(), true);
        drawGradientControl ("Bend", kParamBendGradient, owner->bendGradient,
                             owner->link.getBendGradColour(), false);

        /* Full colour at this much deflection, in units of 64. Needed because a key
           bends a semitone against a range of 48, so measuring against the whole
           14-bit range leaves the gradient almost invisible. */
        int scale = owner->link.getBendFullScale();
        ImGui::SetNextItemWidth (200.0f);

        if (ImGui::SliderInt ("Bend full-scale", &scale, 1, 32, "%d x64"))
        {
            owner->link.setBendFullScale (scale);
            markDirty();
        }
    }

    void drawDegreesSection()
    {
        /* The scale these measure against follows the one chosen in Modes.

           It used to be set only by the button below, so changing the scale up there
           left Degrees and Tension measuring against whatever had been copied across
           earlier - the colours simply did not follow the key you were in. Kept in step
           every frame, which costs one comparison. */
        if (owner->link.getDegreeScale() != kScales[scaleIndex].mask)
            owner->link.setDegreeScale (kScales[scaleIndex].mask);

        if (owner->link.getScaleRoot() != scaleRoot)
            owner->link.setScaleRoot (scaleRoot);

        drawDegreeControls();


    }

    void drawKeyMappingSection()
    {
        drawOctaveControl();
        drawOffsetControl();
        drawFoldControl();

        ImGui::TextDisabled ("keys show the note they actually play - note 0 is C-2, "
                             "note 60 is C3, as in the DAW");
    }

    void drawParamSlider (const char *label, uint32_t paramId, double current)
    {
        float value = (float) current;
        ImGui::SetNextItemWidth (240.0f);

        const bool changed = ImGui::SliderFloat (label, &value, 0.0f, 1.0f, "%.2f");

        if (ImGui::IsItemActivated())
            pushGuiParam (owner, paramId, (double) value, true, false);

        if (changed)
            pushGuiParam (owner, paramId, (double) value, false, false);

        if (ImGui::IsItemDeactivatedAfterEdit())
            pushGuiParam (owner, paramId, (double) value, false, true);
    }

    /*
        One way to set a colour everywhere in the panel: click the swatch and edit it.

        There used to be a separate "Set from picker" button beside each swatch, which
        meant loading the big picker first and then remembering which button applied it
        - two steps and a mode. A swatch that opens its own picker is one step and
        needs no explaining.
    */
    bool drawSwatch (const char *id, uint32_t rgb, uint32_t &out)
    {
        float col[3] = { (float) ((rgb >> 16) & 0xff) / 255.0f,
                         (float) ((rgb >> 8) & 0xff) / 255.0f,
                         (float) (rgb & 0xff) / 255.0f };

        ImGui::PushID (id);
        const bool changed = ImGui::ColorEdit3 ("##sw", col,
                                                ImGuiColorEditFlags_NoInputs
                                                | ImGuiColorEditFlags_NoLabel);
        ImGui::PopID();

        if (changed)
            out = rgbOf (col);

        return changed;
    }

    void drawEffectToggle (const char *label, uint32_t paramId, double current)
    {
        bool on = current >= 0.5;

        if (ImGui::Checkbox (label, &on))
        {
            pushGuiParam (owner, paramId, on ? 1.0 : 0.0, true, false);
            pushGuiParam (owner, paramId, on ? 1.0 : 0.0, false, true);
        }
    }

    void drawHighlightControl()
    {
        drawEffectToggle ("Incoming", kParamHighlight, owner->highlight);
        ImGui::SameLine (140.0f);
        uint32_t rgb = 0;

        if (drawSwatch ("hl", owner->link.getHighlightColour(), rgb))
        {
            owner->link.setHighlightColour (rgb);
            markDirty();
        }
    }

    void drawPressControl()
    {
        drawEffectToggle ("Pressed", kParamPressColour, owner->pressColour);
        ImGui::SameLine (140.0f);
        uint32_t rgb = 0;

        if (drawSwatch ("pr", owner->link.getPressColour(), rgb))
        {
            owner->link.setPressColour (rgb);
            markDirty();
        }
    }

    void drawOffsetControl()
    {
        int semitones = (int) owner->displayOffset;
        ImGui::SetNextItemWidth (240.0f);

        const bool changed = ImGui::SliderInt ("Display offset", &semitones, -24, 24, "%+d st");

        if (ImGui::IsItemActivated())
            pushGuiParam (owner, kParamDisplayOffset, (double) semitones, true, false);

        if (changed)
            pushGuiParam (owner, kParamDisplayOffset, (double) semitones, false, false);

        if (ImGui::IsItemDeactivatedAfterEdit())
            pushGuiParam (owner, kParamDisplayOffset, (double) semitones, false, true);
    }

    /* Chained units place themselves from getClusterXpos(); this is the gap between
       them. Adjustable here because the right value depends on the key count of the
       left unit, and 48 or 49 is a musical choice rather than a fixed fact. */
    /* The gradient colours ride on top of whatever the key already shows, so pressure
       and glide read as intensity of that key rather than replacing its colour. */
    void drawGradientControl (const char *label, uint32_t paramId, double current,
                              uint32_t rgb, bool isPressure)
    {
        drawEffectToggle (label, paramId, current);
        ImGui::SameLine (140.0f);
        uint32_t picked = 0;

        if (drawSwatch (label, rgb, picked))
        {
            if (isPressure)
                owner->link.setPressureGradColour (picked);
            else
                owner->link.setBendGradColour (picked);

            markDirty();
        }
    }

    /* These are the device's own config items, mirrored back over CC 30/31 whenever
       they change - so Dashboard, the octave buttons and this panel all show the same
       thing, and editing here writes back to every block in the chain. */
    /* Animated in the plugin, composited over the painted colour table. The device
       program knows nothing about either of these. */
    /* Reads another plugin's on-screen keyboard and copies its colours here. Useful
       where those colours mean something - a sampler's key switches, a drum map -
       rather than for plugins that just draw a plain piano. */
    void drawCaptureControls()
    {
        if (ImGui::Button ("Find windows", ImVec2 (110.0f, 0.0f)))
        {
            owner->capture->refreshWindows();
            captureIndex = 0;
        }

        ImGui::SameLine();
        const auto &wins = owner->capture->windows();
        const char *preview = wins.empty() ? "(none found)"
                                           : wins[(size_t) captureIndex].title.c_str();
        ImGui::SetNextItemWidth (250.0f);

        if (ImGui::BeginCombo ("##capwin", preview))
        {
            for (size_t i = 0; i < wins.size(); ++i)
                if (ImGui::Selectable (wins[i].title.c_str(), (int) i == captureIndex))
                    captureIndex = (int) i;

            ImGui::EndCombo();
        }

        if (ImGui::Button ("Read keyboard", ImVec2 (110.0f, 0.0f)) && ! wins.empty())
        {
            if (owner->capture->grab ((size_t) captureIndex))
                owner->capture->detect();

                if (captureIndex < owner->capture->windows().size())
                    owner->capture->rememberTitle (owner->capture->windows()[captureIndex].title);
        }

        ImGui::SameLine();
        ImGui::TextDisabled ("%s", owner->capture->status().c_str());

        if (! owner->capture->hasResult())
            return;

        /* Re-reads the same key positions a few times a second. Detection is not
           repeated - the geometry is already known - so this costs one window capture
           and a few hundred pixel reads, and the keyboard follows whatever the plugin
           draws as you change patch or articulation. */
        /* Handed to the worker rather than ticked here, so it keeps following after this
           window is closed. */
        if (ImGui::Checkbox ("Live", &captureLive))
        {
            if (captureLive)
                owner->link.setFollowSource (owner->capture, captureAnchor);
            else
                owner->link.stopFollowing();
        }

        if (captureLive)
        {
            ImGui::SameLine();
            ImGui::TextDisabled (owner->capture->hasResult() ? "following, editor can be closed"
                                                     : "window lost - looking for it again");
        }

        /* Nothing in the pixels says which C is middle C. It can be found rather than
           typed: play a note into the plugin and see which of its keys changes. */
        if (ImGui::Button ("Find anchor", ImVec2 (110.0f, 0.0f)) && probeStage == 0)
            probeStage = 1;

        ImGui::SameLine();
        runAnchorProbe();

        ImGui::SetNextItemWidth (110.0f);
        ImGui::DragInt ("Lowest C is", &captureAnchor, 0.25f, 0, 120, "note %d");
        ImGui::SameLine();
        ImGui::Text ("%s%d", kNoteNames[pitchClassOf (captureAnchor)],
                     octaveOf (captureAnchor));
        ImGui::SameLine();

        if (ImGui::Button ("Import colours", ImVec2 (130.0f, 0.0f)))
        {
            owner->capture->applyTo (owner->link, captureAnchor);
            markDirty();
        }

        /* A strip of what was read, so a wrong anchor or a misdetected grid is
           obvious before it overwrites anything. */
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float bw = 9.0f;

        for (int s = 0; s < 61; ++s)
        {
            const uint32_t rgb = owner->capture->colourAt (s);

            if (rgb == 0)
                continue;

            dl->AddRectFilled (ImVec2 (p.x + s * bw, p.y),
                               ImVec2 (p.x + s * bw + bw - 1.0f, p.y + 16.0f),
                               rgbToImU32 (rgb, 1.0f));
        }

        ImGui::Dummy (ImVec2 (61 * bw, 20.0f));
    }

    /* Sits above the painted or captured colours and below the transient effects, so
       it reads as a map while ripples and afterglow still show over it. */
    void drawDegreeControls()
    {
        bool on = owner->link.getDegreeEnabled();

        if (ImGui::Checkbox ("Colour by degree from the note played", &on))
        {
            owner->link.setDegreeEnabled (on);
            markDirty();
        }

        const int root = owner->link.getDegreeRoot();
        ImGui::SameLine();
        ImGui::TextDisabled ("root: %s", kNoteNames[root]);

        int mix = owner->link.getDegreeAlpha();
        ImGui::SetNextItemWidth (200.0f);

        if (ImGui::SliderInt ("Strength", &mix, 0, 255))
        {
            owner->link.setDegreeAlpha (mix);
            markDirty();
        }

        ImGui::SameLine();

        /* Its own scale shape, independent of the generator above: the generator
           paints the base map, this decides what counts as a degree. */
        ImGui::TextDisabled ("follows the scale in Modes");

        static const char *labels[8] = { "1", "2", "3", "4", "5", "6", "7", "chr" };

        drawTensionControls();

        for (int i = 0; i < 8; ++i)
        {
            const uint32_t rgb = owner->link.getDegreeColour (i);
            float col[3] = { (float) ((rgb >> 16) & 0xff) / 255.0f,
                             (float) ((rgb >> 8) & 0xff) / 255.0f,
                             (float) (rgb & 0xff) / 255.0f };

            ImGui::PushID (i);

            if (ImGui::ColorEdit3 (labels[i], col,
                                   ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
            {
                owner->link.setDegreeColour (i, rgbOf (col));
                markDirty();
            }

            ImGui::PopID();
            ImGui::SameLine();
            ImGui::TextDisabled ("%s", labels[i]);

            if (i < 7)
                ImGui::SameLine();
        }
    }

    void drawMotionControls()
    {
        bool ripple = owner->link.getRippleEnabled();

        if (ImGui::Checkbox ("Ripple", &ripple))
        {
            owner->link.setRippleEnabled (ripple);
            markDirty();
        }

        ImGui::SameLine (140.0f);
        {
            uint32_t rgb = 0;

            if (drawSwatch ("rip", owner->link.getRippleColour(), rgb))
            {
                owner->link.setRippleColour (rgb);
                markDirty();
            }
        }

        ImGui::SameLine();

        /* Where the wave takes its colour. Everything but Fixed derives it from the
           note that threw it, so different notes give differently coloured waves. */
        {
            static const char *sources[] = { "Fixed", "Wheel", "Fifths", "Degree", "Map" };
            int source = owner->link.getRippleSource();
            ImGui::SetNextItemWidth (95.0f);

            if (ImGui::Combo ("##ripsrc", &source, sources, 5))
            {
                owner->link.setRippleSource (source);
                markDirty();
            }

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip ("Fixed: the swatch\n"
                                   "Wheel: hue by pitch class\n"
                                   "Fifths: hue by position in the circle of fifths\n"
                                   "Degree: the degree colour of the note played\n"
                                   "Map: the colour of the key it came from");
        }

        ImGui::SameLine();
        int speed = owner->link.getRippleSpeed();
        ImGui::SetNextItemWidth (120.0f);

        if (ImGui::SliderInt ("Speed", &speed, 1, 16))
        {
            owner->link.setRippleSpeed (speed);
            markDirty();
        }

        ImGui::SameLine();
        int trail = owner->link.getRippleTrail();
        ImGui::SetNextItemWidth (120.0f);

        if (ImGui::SliderInt ("Trail", &trail, 1, 12))
        {
            owner->link.setRippleTrail (trail);
            markDirty();
        }

        drawEffectRow ("Afterglow", owner->link.getAfterglowEnabled(),
                       owner->link.getAfterglowColour(), 2);
        ImGui::SameLine();
        int decay = owner->link.getAfterglowDecay();
        ImGui::SetNextItemWidth (120.0f);

        if (ImGui::SliderInt ("Decay", &decay, 1, 30, "%d/10 s"))
        {
            owner->link.setAfterglowDecay (decay);
            markDirty();
        }

        drawEffectRow ("Beat pulse", owner->link.getPulseEnabled(),
                       owner->link.getPulseColour(), 3);
        ImGui::SameLine();
        ImGui::TextDisabled ("follows host transport");

        drawEffectRow ("Chord halo", owner->link.getHaloEnabled(),
                       owner->link.getHaloColour(), 4);
        ImGui::SameLine();
        ImGui::TextDisabled ("2+ notes held");

        bool waves = owner->link.getWavesEnabled();

        if (ImGui::Checkbox ("Waves", &waves))
        {
            owner->link.setWavesEnabled (waves);
            markDirty();
        }

        ImGui::SameLine (140.0f);
        int delay = owner->link.getWavesDelay();
        ImGui::SetNextItemWidth (150.0f);

        if (ImGui::SliderInt ("##wavedelay", &delay, 5, 600, "after %ds"))
        {
            owner->link.setWavesDelay (delay);
            markDirty();
        }

        ImGui::SameLine();
        ImGui::TextDisabled (owner->link.wavesRunning() ? "running" : "idle screensaver");

        bool path = owner->link.getBendPathEnabled();

        if (ImGui::Checkbox ("Bend path", &path))
        {
            owner->link.setBendPathEnabled (path);
            markDirty();
        }

        ImGui::SameLine (140.0f);
        {
            uint32_t rgb = 0;

            if (drawSwatch ("bpath", owner->link.getBendPathColour(), rgb))
            {
                owner->link.setBendPathColour (rgb);
                markDirty();
            }
        }

        ImGui::SameLine();

        const int cents = owner->link.getLastBendCents();
        const int bentNote = owner->link.getLastBendNote();

        if (bentNote >= 0 && cents != 0)
            ImGui::TextDisabled ("%+d cents from %s -> %s", cents,
                                 kNoteNames[bentNote % 12],
                                 kNoteNames[((bentNote + (cents >= 0 ? (cents + 50) / 100
                                                                     : (cents - 50) / 100))
                                             % 12 + 12) % 12]);
        else
            ImGui::TextDisabled ("lights where a bent note is heading");

        bool vel = owner->link.getVelocityEnabled();

        if (ImGui::Checkbox ("Velocity brightness", &vel))
        {
            owner->link.setVelocityEnabled (vel);
            markDirty();
        }

        ImGui::SameLine();
        ImGui::TextDisabled ("turn Incoming and Pressed off to see it");

        bool splash = owner->link.getSplashEnabled();

        if (ImGui::Checkbox ("Splash", &splash))
        {
            owner->link.setSplashEnabled (splash);
            markDirty();
        }

        ImGui::SameLine (140.0f);
        {
            uint32_t rgb = 0;

            if (drawSwatch ("spl", owner->link.getSplashColour(), rgb))
            {
                owner->link.setSplashColour (rgb);
                markDirty();
            }
        }

        ImGui::SameLine();
        int cc = owner->link.getSplashCC();
        ImGui::SetNextItemWidth (120.0f);

        if (ImGui::SliderInt ("on CC", &cc, 0, 119))
        {
            owner->link.setSplashCC (cc);
            markDirty();
        }

        ImGui::Indent (110.0f);
        int splashSpeed = owner->link.getSplashSpeed();
        ImGui::SetNextItemWidth (120.0f);

        if (ImGui::SliderInt ("Speed##splash", &splashSpeed, 1, 16))
        {
            owner->link.setSplashSpeed (splashSpeed);
            markDirty();
        }

        ImGui::SameLine();
        int splashTrail = owner->link.getSplashTrail();
        ImGui::SetNextItemWidth (120.0f);

        if (ImGui::SliderInt ("Trail##splash", &splashTrail, 1, 12))
        {
            owner->link.setSplashTrail (splashTrail);
            markDirty();
        }

        ImGui::Unindent (110.0f);
    }

    /* target: 2 afterglow, 3 pulse, 4 halo. */
    void drawEffectRow (const char *label, bool enabled, uint32_t rgb, int target)
    {
        bool on = enabled;
        ImGui::PushID (label);

        if (ImGui::Checkbox (label, &on))
        {
            if (target == 2) owner->link.setAfterglowEnabled (on);
            else if (target == 3) owner->link.setPulseEnabled (on);
            else owner->link.setHaloEnabled (on);

            markDirty();
        }

        ImGui::SameLine (140.0f);
        uint32_t picked = 0;

        if (drawSwatch ("fx", rgb, picked))
        {
            if (target == 2) owner->link.setAfterglowColour (picked);
            else if (target == 3) owner->link.setPulseColour (picked);
            else owner->link.setHaloColour (picked);

            markDirty();
        }

        ImGui::PopID();
    }

    /*
        Works out the anchor by playing a note and watching the plugin.

        LumiPaint sits before the instrument on its track, so the note reaches the
        plugin being captured; whichever of its keys changes colour is that note. Run
        over several editor frames because the plugin needs time to redraw between the
        two samples.
    */
    void runAnchorProbe()
    {
        const int probeNote = 60;

        switch (probeStage)
        {
            case 0:
                return;

            case 1:
                probeBefore.clear();
                {
                    /* Read the window now rather than trusting whatever the last
                       refresh left behind. With Live off nothing has sampled since the
                       panel was opened, so the "before" could be minutes old and any
                       key the plugin repainted in between reads as the one that
                       changed. */
                    owner->capture->sample();

                    int semi = 0;
                    uint32_t rgb = 0;

                    for (int i = 0; owner->capture->keyInfo (i, semi, rgb); ++i)
                        probeBefore.push_back ({ semi, rgb });
                }

                owner->probeNoteOn.store (probeNote, std::memory_order_release);
                probeWait = 0;
                probeStage = 2;
                break;

            case 2:
                /* Give the plugin time to receive the note and repaint. */
                if (++probeWait < 30)
                    break;

                owner->capture->sample();
                probeStage = 3;
                break;

            case 3:
            {
                owner->probeNoteOff.store (probeNote, std::memory_order_release);

                int bestSemi = 0;
                int bestDelta = 0;
                int semi = 0;
                uint32_t rgb = 0;

                for (int i = 0; owner->capture->keyInfo (i, semi, rgb); ++i)
                {
                    for (const auto &b : probeBefore)
                    {
                        if (b.first != semi)
                            continue;

                        const int d = std::abs ((int) ((rgb >> 16) & 0xff) - (int) ((b.second >> 16) & 0xff))
                                    + std::abs ((int) ((rgb >> 8) & 0xff) - (int) ((b.second >> 8) & 0xff))
                                    + std::abs ((int) (rgb & 0xff) - (int) (b.second & 0xff));

                        if (d > bestDelta)
                        {
                            bestDelta = d;
                            bestSemi = semi;
                        }
                    }
                }

                /* A plugin that does not show played notes on its keybed gives no
                   usable change; say so rather than moving the anchor to noise. */
                if (bestDelta > 40)
                {
                    captureAnchor = probeNote - bestSemi;
                    probeResult = "anchor found";
                    markDirty();
                }
                else
                {
                    probeResult = "no key changed - set the anchor by hand";
                }

                probeStage = 0;
                break;
            }

            default:
                probeStage = 0;
                break;
        }

        if (probeStage != 0)
            ImGui::TextDisabled ("playing a note...");
        else if (! probeResult.empty())
            ImGui::TextDisabled ("%s", probeResult.c_str());
    }

    /* Distance from the root around the circle of fifths, warm at home and cool at the
       tritone. It sits below the degree map: one says which scale note this is, the
       other how far from home. */
    void drawTensionControls()
    {
        bool on = owner->link.getTensionEnabled();

        if (ImGui::Checkbox ("Tension by circle-of-fifths distance", &on))
        {
            owner->link.setTensionEnabled (on);
            markDirty();
        }

        ImGui::SameLine();
        uint32_t rgb = 0;

        if (drawSwatch ("thome", owner->link.getTensionHome(), rgb))
        {
            owner->link.setTensionHome (rgb);
            markDirty();
        }

        ImGui::SameLine();
        ImGui::TextDisabled ("home");
        ImGui::SameLine();

        if (drawSwatch ("tfar", owner->link.getTensionFar(), rgb))
        {
            owner->link.setTensionFar (rgb);
            markDirty();
        }

        ImGui::SameLine();
        ImGui::TextDisabled ("tritone");

        int mix = owner->link.getTensionAlpha();
        ImGui::SetNextItemWidth (200.0f);

        if (ImGui::SliderInt ("Tension strength", &mix, 0, 255))
        {
            owner->link.setTensionAlpha (mix);
            markDirty();
        }
    }

    void drawKeybedControls()
    {
        bool bendOut = owner->link.getEnablePitchBend();

        if (ImGui::Checkbox ("Send pitch bend", &bendOut))
        {
            owner->link.setEnablePitchBend (bendOut);
            markDirty();
        }

        ImGui::SameLine (170.0f);
        bool pressOut = owner->link.getEnablePressure();

        if (ImGui::Checkbox ("Send pressure", &pressOut))
        {
            owner->link.setEnablePressure (pressOut);
            markDirty();
        }

        bool link = owner->link.getLinkOctaves();

        if (ImGui::Checkbox ("Link octaves across the chain", &link))
        {
            owner->link.setLinkOctaves (link);
            markDirty();
        }

        ImGui::SameLine();
        ImGui::TextDisabled (link ? "(both blocks shift together)"
                                  : "(each block shifts on its own)");

        drawConfigToggle ("MPE", kConfigMidiUseMPE);
        drawConfigInt ("Pitch bend range", kConfigPitchBendRange, 1, 96);
        drawConfigInt ("Transpose", kConfigTranspose, -12, 12);
        drawConfigInt ("MIDI channel", kConfigMidiStartChannel, 1, 16);
    }

    void drawConfigToggle (const char *label, int item)
    {
        const int value = owner->link.getDeviceConfig (item);

        if (value == kConfigUnknown)
        {
            ImGui::TextDisabled ("%s: waiting for device", label);
            return;
        }

        bool on = value != 0;

        if (ImGui::Checkbox (label, &on))
            owner->link.writeDeviceConfig (item, on ? 1 : 0);
    }

    void drawConfigInt (const char *label, int item, int low, int high)
    {
        const int value = owner->link.getDeviceConfig (item);

        if (value == kConfigUnknown)
        {
            ImGui::TextDisabled ("%s: waiting for device", label);
            return;
        }

        int edited = value;
        ImGui::SetNextItemWidth (200.0f);

        if (ImGui::SliderInt (label, &edited, low, high))
            owner->link.writeDeviceConfig (item, edited);
    }

    void drawOctaveControl()
    {
        int oct = (int) owner->octave;
        ImGui::SetNextItemWidth (240.0f);

        const bool changed = ImGui::SliderInt ("Octave", &oct, -3, 3, "%+d");

        if (ImGui::IsItemActivated())
            pushGuiParam (owner, kParamOctave, (double) oct, true, false);

        if (changed)
            pushGuiParam (owner, kParamOctave, (double) oct, false, false);

        if (ImGui::IsItemDeactivatedAfterEdit())
            pushGuiParam (owner, kParamOctave, (double) oct, false, true);
    }

    void drawFoldControl()
    {
        bool fold = owner->foldOctaves >= 0.5;

        if (ImGui::Checkbox ("Fold octaves", &fold))
        {
            pushGuiParam (owner, kParamFoldOctaves, fold ? 1.0 : 0.0, true, false);
            pushGuiParam (owner, kParamFoldOctaves, fold ? 1.0 : 0.0, false, true);
        }
    }

    void applyToSelection (uint32_t rgb)
    {
        for (int n = 0; n < 128; ++n)
            if (selected[n])
                owner->link.setColour (n, rgb);

        markDirty();
    }

    void applyToUnselected (uint32_t rgb)
    {
        for (int n = 0; n < 128; ++n)
            if (! selected[n])
                owner->link.setColour (n, rgb);

        markDirty();
    }

    void selectSingle (int note)
    {
        for (int n = 0; n < 128; ++n)
            selected[n] = false;

        if (note >= 0 && note <= 127)
            selected[note] = true;
    }

    void applyToAll (uint32_t rgb)
    {
        for (int n = 0; n < 128; ++n)
            owner->link.setColour (n, rgb);

        markDirty();
    }

    void pickFromSelection()
    {
        for (int n = 0; n < 128; ++n)
        {
            if (! selected[n])
                continue;

            const uint32_t rgb = owner->link.getColour (n);
            pickerColour[0] = (float) ((rgb >> 16) & 0xff) / 255.0f;
            pickerColour[1] = (float) ((rgb >> 8) & 0xff) / 255.0f;
            pickerColour[2] = (float) (rgb & 0xff) / 255.0f;
            return;
        }
    }

    void generateWheel (float saturation)
    {
        for (int n = 0; n < 128; ++n)
            owner->link.setColour (n, hsvToRgb ((float) pitchClassOf (n) / 12.0f, saturation, 1.0f));

        markDirty();
    }

    void generateFifths()
    {
        for (int n = 0; n < 128; ++n)
        {
            const int position = (pitchClassOf (n) * 7) % 12;
            owner->link.setColour (n, hsvToRgb ((float) position / 12.0f, 0.85f, 1.0f));
        }

        markDirty();
    }

    void generatePiano()
    {
        for (int n = 0; n < 128; ++n)
            owner->link.setColour (n, kIsBlackKey[pitchClassOf (n)] ? 0x000000u : 0xf0f0ffu);

        markDirty();
    }

    static uint32_t rgbOf (const float *c)
    {
        return (((uint32_t) (c[0] * 255.0f + 0.5f)) << 16)
             | (((uint32_t) (c[1] * 255.0f + 0.5f)) << 8)
             |  ((uint32_t) (c[2] * 255.0f + 0.5f));
    }

    void generateScale()
    {
        const uint32_t mask = kScales[scaleIndex].mask;

        if (mask == 0)
            return;
        const uint32_t rootRgb = rgbOf (scaleRootColour);
        const uint32_t inRgb = rgbOf (scaleInColour);
        const uint32_t outRgb = rgbOf (scaleOutColour);

        for (int n = 0; n < 128; ++n)
        {
            const int degree = ((pitchClassOf (n) - scaleRoot) + 12) % 12;

            if (((mask >> degree) & 1u) == 0u)
                owner->link.setColour (n, outRgb);
            else if (degree == 0)
                owner->link.setColour (n, rootRgb);
            else
                owner->link.setColour (n, inRgb);
        }

        markDirty();
    }

    void selectScale()
    {
        const uint32_t mask = kScales[scaleIndex].mask;

        if (mask == 0)
            return;

        for (int n = 0; n < 128; ++n)
        {
            const int degree = ((pitchClassOf (n) - scaleRoot) + 12) % 12;
            selected[n] = ((mask >> degree) & 1u) != 0u;
        }
    }

    const char *octaveLabel (int note)
    {
        std::snprintf (labelBuffer, sizeof (labelBuffer), "C%d", octaveOf (note));
        return labelBuffer;
    }

    void markDirty()
    {
        if (owner->hostState != nullptr)
            owner->hostState->mark_dirty (owner->host);
    }

    LumiPaint *owner;
    ImGuiHostWindow *window;
    bool selected[128];
    int anchorNote;
    float pickerColour[3];
    int paintScope;
    int scaleRoot;

    int scaleIndex;
    float scaleRootColour[3];
    float scaleInColour[3];
    float scaleOutColour[3];
    bool shownOnce = false;
    int captureIndex = 0;
    int captureAnchor = 36;
    bool captureLive = false;
    std::string presetMessage;

public:
    /* 0 none, 1 save, 2 load. Acted on after the frame, never during it. */
    int pendingDialog = 0;

    void runPendingDialog()
    {
        const int which = pendingDialog;
        pendingDialog = 0;

        if (which == 1)
        {
            const std::string path = PresetIO::askForSavePath();

            if (! path.empty())
                presetMessage = PresetIO::save (path, owner->link, owner->brightness,
                                                owner->unlitLevel)
                              ? "saved" : PresetIO::lastError();
        }
        else if (which == 2)
        {
            const std::string path = PresetIO::askForOpenPath();

            if (path.empty())
                return;

            double b = owner->brightness;
            double u = owner->unlitLevel;

            if (PresetIO::load (path, owner->link, b, u))
            {
                pushGuiParam (owner, kParamBrightness, b, true, false);
                pushGuiParam (owner, kParamBrightness, b, false, true);
                pushGuiParam (owner, kParamUnlitLevel, u, true, false);
                pushGuiParam (owner, kParamUnlitLevel, u, false, true);
                presetMessage = "loaded";
                markDirty();
            }
            else
            {
                presetMessage = PresetIO::lastError();
            }
        }
    }
    int probeStage = 0;
    int probeWait = 0;
    std::string probeResult;
    std::vector<std::pair<int, uint32_t>> probeBefore;
    int captureTick = 0;
    float whiteKeyWidth;
    int lowNote;
    int highNote;
    char labelBuffer[16];
};

LumiEditor *editorOf (const clap_plugin_t *plugin)
{
    LumiPaint *self = (LumiPaint *) plugin->plugin_data;
    return (LumiEditor *) self->editor;
}

const char *preferredApi()
{
#if defined (_WIN32)
    return CLAP_WINDOW_API_WIN32;
#elif defined (__APPLE__)
    return CLAP_WINDOW_API_COCOA;
#else
    return CLAP_WINDOW_API_X11;
#endif
}

/*
    Embedded only. Floating is refused.

    A floating editor is a top-level window this plugin owns, and owning a top-level
    window from inside a host causes trouble that an embedded child does not: the
    taskbar can end up behind it, the cursor stops being tracked properly when it is not
    the foreground window, it can open behind the host until something forces a redraw,
    and - worst - it has to be destroyed from the thread that created it, which a host
    unloading a plugin will not necessarily honour. A window that fails to be destroyed
    keeps the class registered, which keeps this library loaded, which is why the host
    process was still in the task list after its window had gone.

    The VST3 wrapper always embeds, and that path has none of these problems. Refusing
    floating makes every host take the route that works.
*/
bool guiIsApiSupported (const clap_plugin_t *plugin, const char *api, bool isFloating)
{
    (void) plugin;

    if (isFloating)
        return false;
    return std::strcmp (api, preferredApi()) == 0;
}

/* Embedded, matching what is_api_supported allows. */
bool guiGetPreferredApi (const clap_plugin_t *plugin, const char **api, bool *isFloating)
{
    (void) plugin;
    *api = preferredApi();
    *isFloating = false;
    return true;
}

void renderEditor (void *userData)
{
    LumiPaint *self = (LumiPaint *) userData;

    if (self == nullptr || self->editor == nullptr)
        return;

    LumiEditor *editor = (LumiEditor *) self->editor;
    editor->draw();

    /* Anything modal the frame asked for runs now, once drawing is done. The host
       layer refuses to re-enter a frame in any case, so this is belt and braces - but
       the belt is what keeps the editor responsive rather than merely unfrozen. */
    editor->runPendingDialog();
}

void editorClosed (void *userData)
{
    LumiPaint *self = (LumiPaint *) userData;

    if (self->hostGui != nullptr && self->hostGui->closed != nullptr)
        self->hostGui->closed (self->host, false);
}

bool guiCreate (const clap_plugin_t *plugin, const char *api, bool isFloating)
{
    if (! guiIsApiSupported (plugin, api, isFloating))
        return false;

    LumiPaint *self = (LumiPaint *) plugin->plugin_data;

    if (self->editor != nullptr)
        return true;

    LumiEditor *editor = new LumiEditor (self);
    self->editor = editor;
    editor->setHostWindow (imguiHostCreate (kDefaultWidth, kDefaultHeight, isFloating,
                                            renderEditor, editorClosed, self));

    if (editor->hostWindow() == nullptr)
    {
        self->editor = nullptr;
        delete editor;
        return false;
    }

    return true;
}

void guiDestroy (const clap_plugin_t *plugin)
{
    LumiPaint *self = (LumiPaint *) plugin->plugin_data;

    if (self->editor == nullptr)
        return;

    LumiEditor *editor = (LumiEditor *) self->editor;
    imguiHostDestroy (editor->hostWindow());
    delete editor;
    self->editor = nullptr;
}

bool guiSetScale (const clap_plugin_t *plugin, double scale)
{
    LumiEditor *editor = editorOf (plugin);

    if (editor == nullptr)
        return false;

    imguiHostSetScale (editor->hostWindow(), scale);
    return true;
}

bool guiGetSize (const clap_plugin_t *plugin, uint32_t *width, uint32_t *height)
{
    (void) plugin;
    *width = kDefaultWidth;
    *height = kDefaultHeight;
    return true;
}

bool guiCanResize (const clap_plugin_t *plugin)
{
    (void) plugin;
    return true;
}

bool guiGetResizeHints (const clap_plugin_t *plugin, clap_gui_resize_hints_t *hints)
{
    (void) plugin;
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    hints->preserve_aspect_ratio = false;
    hints->aspect_ratio_width = 0;
    hints->aspect_ratio_height = 0;
    return true;
}

bool guiAdjustSize (const clap_plugin_t *plugin, uint32_t *width, uint32_t *height)
{
    (void) plugin;

    if (*width < 720)
        *width = 720;

    if (*height < 500)
        *height = 500;

    return true;
}

bool guiSetSize (const clap_plugin_t *plugin, uint32_t width, uint32_t height)
{
    LumiEditor *editor = editorOf (plugin);

    if (editor == nullptr)
        return false;

    imguiHostSetSize (editor->hostWindow(), width, height);
    return true;
}

bool guiSetParent (const clap_plugin_t *plugin, const clap_window_t *window)
{
    LumiEditor *editor = editorOf (plugin);

    if (editor == nullptr)
        return false;

    return imguiHostSetParent (editor->hostWindow(), (void *) window->ptr);
}

bool guiSetTransient (const clap_plugin_t *plugin, const clap_window_t *window)
{
    LumiEditor *editor = editorOf (plugin);

    if (editor == nullptr || window == nullptr)
        return false;

    return imguiHostSetTransient (editor->hostWindow(), (void *) window->ptr);
}

void guiSuggestTitle (const clap_plugin_t *plugin, const char *title)
{
    LumiEditor *editor = editorOf (plugin);

    if (editor != nullptr)
        imguiHostSetTitle (editor->hostWindow(), title);
}

bool guiShow (const clap_plugin_t *plugin)
{
    LumiEditor *editor = editorOf (plugin);

    if (editor == nullptr)
        return false;

    imguiHostShow (editor->hostWindow());
    return true;
}

bool guiHide (const clap_plugin_t *plugin)
{
    LumiEditor *editor = editorOf (plugin);

    if (editor == nullptr)
        return false;

    imguiHostHide (editor->hostWindow());
    return true;
}

}

void guiShutdown (const clap_plugin_t *plugin)
{
    LumiPaint *self = (LumiPaint *) plugin->plugin_data;

    if (self != nullptr && self->editor != nullptr)
        guiDestroy (plugin);
}

const clap_plugin_gui_t s_gui = {
    guiIsApiSupported, guiGetPreferredApi, guiCreate, guiDestroy, guiSetScale,
    guiGetSize, guiCanResize, guiGetResizeHints, guiAdjustSize, guiSetSize,
    guiSetParent, guiSetTransient, guiSuggestTitle, guiShow, guiHide
};

}
