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
#include <cfloat>

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
        /*
            Select-all, before anything draws.

            Cmd-A on macOS and Ctrl-A elsewhere, through the same modifier the keyboard
            click uses. The repeat flag is off so holding the chord down does not fire
            every frame - harmless here, since selecting everything twice is selecting
            everything, but it would spin the undo stack the moment this grows to cover
            anything destructive.
        */
        if (multiSelectDown() && ImGui::IsKeyPressed (ImGuiKey_A, false))
            for (int n = 0; n < 128; ++n)
                selected[n] = true;

        trimSelectionToZone();
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

        beginSection ("Gradient", IM_COL32 (36, 30, 26, 255));
        drawGradientControls();
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

        beginSection ("Screensaver", IM_COL32 (30, 28, 44, 255));
        drawScreensaverControls();
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
    /*
        Undo, over the painted colours.

        Every destructive thing in this editor replaces some or all of the 128 colours:
        the generators, the gradient, apply-to-selection, importing a captured keybed,
        loading a map. None of them could be taken back, so one misplaced click on a map
        built by hand was the end of it.

        What is kept is the colour table and nothing else. Effect settings and levels are
        left out deliberately - they are one control each and trivially reset by hand,
        while a colour table is a hundred and twenty-eight decisions. Mixing the two
        would mean an undo that moved sliders the user never touched.

        Thirty steps, oldest dropped. Fixed-size entries, so the whole history is under
        sixteen kilobytes and there is nothing to tune.
    */
    struct ColourSnapshot
    {
        uint32_t note[128];
    };

    static const int kUndoDepth = 30;

    std::vector<ColourSnapshot> undoStack;
    std::vector<ColourSnapshot> redoStack;

    ColourSnapshot currentColours() const
    {
        ColourSnapshot shot;

        for (int n = 0; n < 128; ++n)
            shot.note[n] = owner->link.getColour (n);

        return shot;
    }

    void restoreColours (const ColourSnapshot &shot)
    {
        for (int n = 0; n < 128; ++n)
            setColourIfOwned (n, shot.note[n]);

        markDirty();
    }

    /*
        Called before an edit, never after.

        A redo stack only means anything until the next edit: having gone back three
        steps and then painted something new, the branch that was ahead is unreachable
        and keeping it would let the redo arrow walk into a history that no longer
        happened.
    */
    void pushUndo()
    {
        undoStack.push_back (currentColours());

        if ((int) undoStack.size() > kUndoDepth)
            undoStack.erase (undoStack.begin());

        redoStack.clear();
    }

    void undo()
    {
        if (undoStack.empty())
            return;

        redoStack.push_back (currentColours());
        restoreColours (undoStack.back());
        undoStack.pop_back();
    }

    void redo()
    {
        if (redoStack.empty())
            return;

        undoStack.push_back (currentColours());
        restoreColours (redoStack.back());
        redoStack.pop_back();
    }

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

        /*
            Pushed to the right edge rather than placed after the range control, so the
            arrows stay in the corner whatever the window is doing. Measured from the
            window width so they do not drift when the editor is resized.
        */
        const float arrow = 30.0f;
        ImGui::SameLine (ImGui::GetWindowWidth() - (arrow * 2.0f + 28.0f));

        ImGui::BeginDisabled (undoStack.empty());

        if (ImGui::Button ("<##undo", ImVec2 (arrow, 0.0f)))
            undo();

        if (! undoStack.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip ("Undo colours (%d)", (int) undoStack.size());

        ImGui::EndDisabled();
        ImGui::SameLine (0.0f, 4.0f);
        ImGui::BeginDisabled (redoStack.empty());

        if (ImGui::Button (">##redo", ImVec2 (arrow, 0.0f)))
            redo();

        if (! redoStack.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip ("Redo colours (%d)", (int) redoStack.size());

        ImGui::EndDisabled();
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
        /* One blank line, not two. The version line underneath was landing a few
           pixels past the bottom of the window, which is a silly thing to lose it to. */
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

        /*
            Hold and Share answer the same question and give opposite answers.

            Hold says this instance keeps the whole keyboard whatever else happens;
            Share says it takes a slice and leaves the rest to others. Both at once is
            not a state with a meaning, so turning one on turns the other off rather
            than leaving the user to work out which won.
        */
        ImGui::BeginDisabled (owner->link.getZoned());

        if (ImGui::Checkbox ("Hold", &hold))
        {
            owner->link.setHoldDevice (hold);

            if (hold && owner->link.getZoned())
                owner->link.setZoned (false);
        }

        ImGui::EndDisabled();

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip ("Keep the keyboard on this instance instead of letting\n"
                               "whichever track you play take it");

        /*
            Sharing sits beside Hold because they answer the same question - who gets
            the keyboard - and because the range itself is now set on the keyboard,
            which left nothing for a section of its own to hold.
        */
        ImGui::SameLine (0.0f, 20.0f);
        bool sharing = owner->link.getZoned();

        ImGui::BeginDisabled (owner->link.getHoldDevice());

        if (ImGui::Checkbox ("Share", &sharing))
        {
            if (sharing)
                owner->link.setHoldDevice (false);

            owner->link.setZoned (sharing);

            if (sharing)
                takeFreeRange();

            markDirty();
        }

        ImGui::EndDisabled();

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip ("Split the keyboard between instances.\n"
                               "Alt-drag the bar under the keys to set this one's range.\n"
                               "Ticking this takes the largest free stretch to begin with.");

        if (sharing)
        {
            ImGui::SameLine();

            if (! owner->link.hasZone())
                ImGui::TextColored (ImVec4 (1.0f, 0.55f, 0.25f, 1.0f), "no range");
            else if (owner->link.zoneStarved())
                ImGui::TextColored (ImVec4 (1.0f, 0.55f, 0.25f, 1.0f), "no MIDI on this track");
            else
                ImGui::TextDisabled (owner->link.isZoneSender() ? "master" : "member");

        }
    }

    /*
        Which instance owns which keys, drawn under the keys themselves.

        The ranges mean nothing in the abstract - what matters is which keys they cover,
        and the keyboard is right there. A bar under the keys says it without anyone
        having to read two numbers and count octaves, and alt-dragging along it is the
        same gesture as reading it.

        Alt, not a plain drag, because a plain drag on the keybed above already selects
        and this sits directly beneath it. Alt is also what paints, but painting only
        happens on the keys; down here it is unambiguous.
    */
    void drawZoneBars (const ImVec2 &origin, int firstWhite, float whiteHeight,
                       float totalWidth)
    {
        ImDrawList *draw = ImGui::GetWindowDrawList();
        const float top = origin.y + whiteHeight + 30.0f;
        const float height = 14.0f;

        ImGui::SetCursorScreenPos (ImVec2 (origin.x, top));
        ImGui::InvisibleButton ("zonebar", ImVec2 (totalWidth, height));

        const bool sharing = owner->link.getZoned();

        if (sharing && ImGui::IsItemActive() && ImGui::GetIO().KeyAlt)
        {
            const float startX = ImGui::GetIO().MouseClickedPos[0].x;
            const float nowX = ImGui::GetIO().MousePos.x;

            requestZone (noteAtBarX (startX, origin.x, firstWhite),
                         noteAtBarX (nowX, origin.x, firstWhite));
        }

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip (sharing ? "alt-drag to set this instance's range"
                                       : "sharing is off - this instance owns the whole keyboard");

        /*
            Where the zone's keys play, under the bar that says which keys they are.

            A captured keyboard wants the notes it was written for; the keys you want it
            under are wherever your hands reach. The offset is the distance between the
            two, in semitones rather than octaves because captured ranges are not whole
            octaves - C2 to E3 is sixteen.
        */
        if (sharing && owner->link.hasZone())
        {
            ImGui::SetCursorScreenPos (ImVec2 (origin.x, top + height + 4.0f));
            ImGui::SetNextItemWidth (80.0f);

            int shift = owner->link.getZoneOffset();

            if (ImGui::DragInt ("##zoneoffset", &shift, 0.2f, -60, 60, "%+d st"))
            {
                owner->link.setZoneOffset (shift);
                markDirty();
            }

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip ("How far the notes sit from the keys.\n"
                                   "The colour map moves with them.");

            const int lowNoteOut = zoneLow + shift;
            const int highNoteOut = zoneHigh + shift;

            ImGui::SameLine();

            if (lowNoteOut < 0 || highNoteOut > 127)
            {
                ImGui::TextColored (ImVec4 (1.0f, 0.55f, 0.25f, 1.0f),
                                    "keys %s%d-%s%d play off the end of MIDI",
                                    kNoteNames[pitchClassOf (zoneLow)], octaveOf (zoneLow),
                                    kNoteNames[pitchClassOf (zoneHigh)], octaveOf (zoneHigh));
            }
            else
            {
                ImGui::TextDisabled ("keys %s%d-%s%d  play  %s%d-%s%d",
                                     kNoteNames[pitchClassOf (zoneLow)], octaveOf (zoneLow),
                                     kNoteNames[pitchClassOf (zoneHigh)], octaveOf (zoneHigh),
                                     kNoteNames[pitchClassOf (lowNoteOut)], octaveOf (lowNoteOut),
                                     kNoteNames[pitchClassOf (highNoteOut)], octaveOf (highNoteOut));
            }
        }

        draw->AddRectFilled (ImVec2 (origin.x, top), ImVec2 (origin.x + totalWidth, top + height),
                             IM_COL32 (16, 16, 20, 255));

        if (! sharing)
            return;

        const uint32_t me = owner->link.selfId();

        for (int i = 0; i < kMaxZones; ++i)
        {
            ZoneInfo info;

            if (! owner->link.zoneAt (i, info))
                continue;

            const int from = info.low < lowNote ? lowNote : info.low;
            const int to = info.high > highNote ? highNote : info.high;

            if (to < from)
                continue;

            const float x0 = origin.x + (float) (whiteIndexForNote (from) - firstWhite) * whiteKeyWidth;
            const float x1 = origin.x + (float) (whiteIndexForNote (to) - firstWhite + 1) * whiteKeyWidth;

            const bool mine = info.owner == me;
            const bool blocking = zoneBlocker.owner == info.owner && zoneBlocker.owner != 0;

            const ImU32 fill = blocking ? IM_COL32 (210, 60, 50, 230)
                                        : (mine ? IM_COL32 (90, 170, 255, 235)
                                                : IM_COL32 (96, 100, 112, 200));

            draw->AddRectFilled (ImVec2 (x0, top + 2.0f), ImVec2 (x1, top + height - 2.0f),
                                 fill, 2.0f);

            /* The one that sends is marked, because when something is wrong it is the
               first thing worth knowing and nothing else on screen says it. */
            if (info.owner == owner->link.senderId())
                draw->AddText (ImVec2 (x0 + 4.0f, top - 1.0f),
                               IM_COL32 (255, 255, 255, 230), "master");
            else if (mine)
                draw->AddText (ImVec2 (x0 + 4.0f, top - 1.0f),
                               IM_COL32 (180, 210, 255, 220), "this one");
        }
    }

    /* Nearest note to a point along the bar, by white key, which is how the bar is
       drawn and so how it reads. */
    int noteAtBarX (float x, float originX, int firstWhite) const
    {
        int index = firstWhite + (int) ((x - originX) / whiteKeyWidth);
        const int lastWhite = whiteIndexForNote (highNote);

        if (index < firstWhite) index = firstWhite;
        if (index > lastWhite) index = lastWhite;

        for (int note = lowNote; note <= highNote; ++note)
            if (! kIsBlackKey[pitchClassOf (note)] && whiteIndexForNote (note) == index)
                return note;

        return lowNote;
    }

    void drawKeyboard()
    {
        const int firstWhite = whiteIndexForNote (lowNote);
        const int lastWhite = whiteIndexForNote (highNote);
        const float totalWidth = (float) (lastWhite - firstWhite + 1) * whiteKeyWidth;
        const float whiteHeight = 130.0f;
        const float blackHeight = whiteHeight * 0.62f;
        const float blackWidth = whiteKeyWidth * 0.62f;

        ImGui::BeginChild ("keyboard", ImVec2 (0.0f, whiteHeight + 104.0f), 0,
                           ImGuiWindowFlags_HorizontalScrollbar);

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList *draw = ImGui::GetWindowDrawList();

        ImGui::InvisibleButton ("keybed", ImVec2 (totalWidth, whiteHeight));

        /*
            The keybed's own state, read before anything else is submitted.

            IsItemClicked and IsItemActive answer for the last item, and the zone bar
            below is submitted between this and the places that used to ask - so every
            click and drag test on the keys was quietly reading the bar's state instead.
            Taken here, while this is still the last item, and used by value afterwards.
        */
        const bool hovered = ImGui::IsItemHovered();
        const bool keybedClicked = ImGui::IsItemClicked (ImGuiMouseButton_Left);
        const bool keybedActive = ImGui::IsItemActive();
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

                /* Sixteen is one short of what "block %d" can produce for an int,
                   and the compiler says so. A block index is single digit in practice,
                   but a warning that is right about the arithmetic and wrong about the
                   data is still a warning everyone learns to scroll past. */
                char label[24];
                std::snprintf (label, sizeof (label), "block %d", b + 1);
                draw->AddText (ImVec2 (x0 + 3.0f, y - 15.0f), tint, label);
            }
        }

        if (hitNote >= 0 && keybedClicked)
            applyClick (hitNote);

        /*
            One snapshot per stroke, taken on the press rather than per key.

            A drag across two octaves is one thing the user did; undoing it a key at a
            time would need forty presses of the arrow to get back. IsItemClicked fires
            once at the start of the drag, IsItemActive stays true for the rest of it.
        */
        if (hitNote >= 0 && ImGui::GetIO().KeyAlt && keybedClicked)
            pushUndo();

        if (hitNote >= 0 && keybedActive && ImGui::GetIO().KeyAlt)
            paintNote (hitNote);

        /* Submitted last, so nothing above it is asking ImGui about the wrong item. */
        drawZoneBars (origin, firstWhite, whiteHeight, totalWidth);

        ImGui::EndChild();

        /*
            The legend goes on the keyboard's tooltip, not in a panel.

            It is the one place a person is already looking when they want to know what
            a click will do, it costs no height in a column that had none to spare, and
            it cannot drift out of sync with the keys it describes.
        */
        if (hitNote >= 0)
            ImGui::SetTooltip ("%s%d\nclick selects  ·  shift extends  ·  %s-click adds"
                               "\nalt-drag paints with the picker colour",
                               kNoteNames[pitchClassOf (hitNote)], octaveOf (hitNote),
                               multiSelectName());
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

        /*
            A key belonging to another instance is struck through.

            It already draws black, because this instance composites nothing outside its
            range - but black is also a colour somebody might have painted, so on its own
            it says nothing about why. A line across it says the key is not this
            instance's to touch, which is the question anyone clicking it is asking.
        */
        if (owner->link.getZoned() && ! paintable (note))
        {
            draw->AddRectFilled (tl, br, IM_COL32 (18, 18, 22, 210), 2.0f);
            draw->AddLine (ImVec2 (tl.x + 2.0f, (tl.y + br.y) * 0.5f),
                           ImVec2 (br.x - 2.0f, (tl.y + br.y) * 0.5f),
                           IM_COL32 (90, 94, 104, 190), 1.0f);
        }

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

    /*
        The "add to selection" modifier, which is not the same key everywhere.

        Command on macOS, Control on Windows and Linux. ImGui sets ConfigMacOSXBehaviors
        from the Cocoa backend, so this asks the backend what platform it is on rather
        than compiling the answer in - which also means a Mac user gets Cmd-click even
        in a build that was not made on a Mac.
    */
    /* What to call it on screen. Saying Ctrl to a Mac user is the kind of small wrong
       detail that makes a panel feel like it was written for somewhere else. */
    static const char *multiSelectName()
    {
        return ImGui::GetIO().ConfigMacOSXBehaviors ? "Cmd" : "Ctrl";
    }

    static bool multiSelectDown()
    {
        const ImGuiIO &io = ImGui::GetIO();
        return io.ConfigMacOSXBehaviors ? io.KeySuper : io.KeyCtrl;
    }

    void applyClick (int note)
    {
        if (! paintable (note))
            return;

        const ImGuiIO &io = ImGui::GetIO();

        if (io.KeyShift && anchorNote >= 0)
        {
            const int from = anchorNote < note ? anchorNote : note;
            const int to = anchorNote < note ? note : anchorNote;

            for (int n = from; n <= to; ++n)
                selected[n] = true;
        }
        else if (multiSelectDown())
        {
            /* Toggle, so a second click takes a key back out again - which is what
               makes building a scattered selection by hand survive a misclick. */
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

        /* One snapshot per stroke, not per key: a drag across two octaves is one thing
           the user did, and undoing it a key at a time would be useless. The caller
           pushes on mouse-down and this runs for every key the drag touches. */
        if (paintScope == 1)
        {
            const int pc = pitchClassOf (note);

            for (int n = 0; n < 128; ++n)
                if (pitchClassOf (n) == pc)
                    setColourIfOwned (n, rgb);
        }
        else
        {
            setColourIfOwned (note, rgb);
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

    int countPressed() const
    {
        int total = 0;

        for (int n = 0; n < 128; ++n)
            if (owner->link.isNoteSounding (n))
                ++total;

        return total;
    }

    /*
        Paint whatever is being held, and leave it selected.

        Read into a snapshot first. isNoteSounding reads bits the audio thread writes,
        so a key lifted between counting and painting would leave the selection
        describing a different set of notes than the ones that actually changed colour -
        rare, and baffling when it happens.

        The selection is set as well as painted because that is what makes cycling work:
        hold a chord, press this once, then move the picker and use Apply to selection
        for each colour without playing the chord again.
    */
    void applyToPressed (uint32_t rgb)
    {
        bool pressed[128];
        int total = 0;

        for (int n = 0; n < 128; ++n)
        {
            pressed[n] = owner->link.isNoteSounding (n);

            if (pressed[n])
                ++total;
        }

        if (total == 0)
            return;

        pushUndo();

        for (int n = 0; n < 128; ++n)
        {
            selected[n] = pressed[n];

            if (pressed[n])
                setColourIfOwned (n, rgb);
        }

        markDirty();
    }

    /*
        Paint the gradient across whatever is selected.

        Spread by position in the selection rather than by note number, so a selection
        with gaps still runs the whole gradient end to end - select every C and you get
        one stop per octave, not a gradient squeezed into the span and sampled at
        twelve-note intervals.
    */
    void applyGradientToSelection()
    {
        int notes[128];
        int total = 0;

        for (int n = 0; n < 128; ++n)
            if (selected[n])
                notes[total++] = n;

        if (total == 0)
            return;

        pushUndo();

        for (int i = 0; i < total; ++i)
        {
            const float position = total > 1 ? (float) i / (float) (total - 1) : 0.0f;
            setColourIfOwned (notes[i], owner->link.gradientAt (position));
        }

        markDirty();
    }

    void drawGradientStrip()
    {
        ImDrawList *draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = 300.0f;
        const float height = 14.0f;

        /* Drawn a column at a time through the same function that paints the keys, so
           what is on screen cannot disagree with what the button would produce. */
        for (int x = 0; x < (int) width; ++x)
        {
            const uint32_t rgb = owner->link.gradientAt ((float) x / (width - 1.0f));
            const ImU32 col = IM_COL32 ((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, 255);
            draw->AddRectFilled (ImVec2 (origin.x + (float) x, origin.y),
                                 ImVec2 (origin.x + (float) x + 1.0f, origin.y + height), col);
        }

        draw->AddRect (origin, ImVec2 (origin.x + width, origin.y + height),
                       IM_COL32 (90, 90, 100, 255));
        ImGui::Dummy (ImVec2 (width, height));
    }

    void drawGradientControls()
    {
        int count = owner->link.getGradientCount();
        drawGradientStrip();

        for (int i = 0; i < count; ++i)
        {
            uint32_t rgb = owner->link.getGradientStop (i);
            float col[3] = { (float) ((rgb >> 16) & 0xff) / 255.0f,
                             (float) ((rgb >> 8) & 0xff) / 255.0f,
                             (float) (rgb & 0xff) / 255.0f };

            char label[24];
            std::snprintf (label, sizeof (label), "##stop%d", i);

            if (i > 0)
                ImGui::SameLine();

            if (ImGui::ColorEdit3 (label, col, ImGuiColorEditFlags_NoInputs
                                             | ImGuiColorEditFlags_NoLabel))
            {
                owner->link.setGradientStop (i, ((uint32_t) (col[0] * 255.0f + 0.5f) << 16)
                                              | ((uint32_t) (col[1] * 255.0f + 0.5f) << 8)
                                              |  (uint32_t) (col[2] * 255.0f + 0.5f));
                markDirty();
            }
        }

        /* Beside the swatches, in the gap their row already leaves, so saying where the
           colours land costs the section nothing. Painting needs a selection and the
           button alone does not say so. */
        ImGui::SameLine (0.0f, 14.0f);
        ImGui::TextDisabled ("paints the selected keys");

        /* Stops, Reverse and the paint button share one row with the swatches above
           them: three rows became one, which is the whole section's height problem. */
        ImGui::SetNextItemWidth (86.0f);

        if (ImGui::SliderInt ("##stops", &count, 2, kGradientStops, "%d stops"))
        {
            owner->link.setGradientCount (count);
            markDirty();
        }

        ImGui::SameLine();

        if (ImGui::Button ("Reverse", ImVec2 (74.0f, 0.0f)))
        {
            const int n = owner->link.getGradientCount();

            for (int i = 0; i < n / 2; ++i)
            {
                const uint32_t a = owner->link.getGradientStop (i);
                owner->link.setGradientStop (i, owner->link.getGradientStop (n - 1 - i));
                owner->link.setGradientStop (n - 1 - i, a);
            }

            markDirty();
        }

        int picked = 0;

        for (int n = 0; n < 128; ++n)
            if (selected[n])
                ++picked;

        ImGui::SameLine();
        ImGui::BeginDisabled (picked == 0);

        char apply[48];
        std::snprintf (apply, sizeof (apply), "Paint %d", picked);

        if (ImGui::Button (apply, ImVec2 (116.0f, 0.0f)))
            applyGradientToSelection();

        ImGui::EndDisabled();
    }

    /*
        Copy and paste between instances, through the shared block rather than a file.

        The payload is exactly what a .lumimap holds, so there is one format and one
        parser; a setting that saves is a setting that copies, with nothing to keep in
        step. Paste is disabled until something has been copied, so the button is never
        a guess.
    */
    void drawClipboardControls (const ImVec2 &size)
    {
        if (ImGui::Button ("Copy", size))
        {
            const std::string text = PresetIO::toText (owner->link, owner->brightness,
                                                       owner->unlitLevel);
            presetMessage = owner->link.writeClipboard (text) ? "copied"
                                                               : "too large to copy";
        }

        ImGui::SameLine();

        std::string pending;
        const bool have = owner->link.readClipboard (pending);

        ImGui::BeginDisabled (! have);

        if (ImGui::Button ("Paste", size))
        {
            double b = owner->brightness;
            double u = owner->unlitLevel;

            pushUndo();

            if (PresetIO::fromText (pending, owner->link, b, u))
            {
                /* The same two levels the host has to be told about on a map load, by
                   the same route. A paste that changed brightness without telling the
                   host would leave the parameter and the device disagreeing until
                   something else nudged it. */
                pushGuiParam (owner, kParamBrightness, b, true, false);
                pushGuiParam (owner, kParamBrightness, b, false, true);
                pushGuiParam (owner, kParamUnlitLevel, u, true, false);
                pushGuiParam (owner, kParamUnlitLevel, u, false, true);
                presetMessage = "pasted";
                markDirty();
            }
            else
            {
                presetMessage = PresetIO::lastError();
            }
        }

        ImGui::EndDisabled();
    }

    /*
        Zones: this instance's share of the chain, and everyone else's.

        The chain strip is the part that matters. Refusing an overlap is only half an
        answer - without seeing where the other zones are, the only way to find a free
        range is to keep guessing, so every live zone is drawn across the full note
        range with this instance's own highlighted and the one that refused a request
        shown in red until the next attempt.
    */
    /*
        The longest stretch nobody else has.

        Also what ticking the box asks for. Asking for the whole keyboard - which the
        editor's fields default to - is refused the moment anybody else is sharing, and
        a refused instance owns nothing and lights nothing. Joining a chain should put
        you somewhere sensible rather than nowhere.
    */
    void takeFreeRange()
    {
            bool taken[128] = { false };

            for (int i = 0; i < kMaxZones; ++i)
            {
                ZoneInfo info;

                if (! owner->link.zoneAt (i, info))
                    continue;

                if (info.low == zoneLow && info.high == zoneHigh)
                    continue;

                for (int n = info.low; n <= info.high && n < 128; ++n)
                    if (n >= 0)
                        taken[n] = true;
            }

            int bestLow = -1, bestHigh = -1, runStart = -1;

            for (int n = 0; n <= 128; ++n)
            {
                const bool free = n < 128 && ! taken[n];

                if (free && runStart < 0)
                    runStart = n;

                if (! free && runStart >= 0)
                {
                    if (bestLow < 0 || (n - 1 - runStart) > (bestHigh - bestLow))
                    {
                        bestLow = runStart;
                        bestHigh = n - 1;
                    }

                    runStart = -1;
                }
            }

            if (bestLow >= 0)
                requestZone (bestLow, bestHigh);
    }


    /*
        Ask for a range, and remember what refused it.

        One place, because the strip, the number fields and the two buttons all want the
        same thing to happen - including keeping the fields showing what was asked for
        rather than snapping back, so a refused range can be nudged clear rather than
        typed again from scratch.
    */
    void requestZone (int low, int high)
    {
        if (low > high)
        {
            const int swap = low;
            low = high;
            high = swap;
        }

        zoneLow = low < 0 ? 0 : low;
        zoneHigh = high > 127 ? 127 : high;

        ZoneInfo blocker;
        zoneBlocker.owner = 0;

        if (! owner->link.setZoneRange (zoneLow, zoneHigh, blocker))
            zoneBlocker = blocker;

        markDirty();
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
        /* One line, not two: the group beside the picker has width to spare and the
           column below it does not have the height. */
        ImGui::TextDisabled ("alt-drag paints  ·  %s adds", multiSelectName());

        /*
            The note field sits beside the picker rather than under it.

            The picker is 150 tall and the column beside it held four short controls, so
            there was dead space on the right and the whole left column ran off the
            bottom of the window. Nothing here is smaller than it was - it is in the gap
            that was already there.
        */
        ImGui::SetNextItemWidth (60.0f);

        if (ImGui::DragInt ("##note", &anchorNote, 0.25f, 0, 127, "%d"))
            selectSingle (anchorNote);

        ImGui::SameLine();
        ImGui::Text ("%s%d", kNoteNames[pitchClassOf (anchorNote)], octaveOf (anchorNote));

        if (ImGui::Button ("Set this note", ImVec2 (150.0f, 0.0f)))
        {
            pushUndo();
            setColourIfOwned (anchorNote, currentPickerRgb());
            markDirty();
        }

        ImGui::EndGroup();

        {
            const int pressed = countPressed();
            char label[64];

            std::snprintf (label, sizeof (label),
                           pressed == 1 ? "Apply to %d pressed note"
                                        : "Apply to %d pressed notes", pressed);

            ImGui::BeginDisabled (pressed == 0);

            if (ImGui::Button (label, ImVec2 (200.0f, 0.0f)))
                applyToPressed (currentPickerRgb());

            ImGui::EndDisabled();

            /* A held key shows the highlight colour rather than its own while Highlight
               is on, so the paint lands but cannot be seen until the key is released.
               Worth saying, since it looks like the button did nothing. */
            if (owner->highlight)
            {
                ImGui::SameLine();
                ImGui::TextDisabled ("Highlight hides it until you let go");
            }
        }

        /* Three to a row rather than two. The column is wide enough for it and the
           labels still fit; six rows of buttons become four, which is most of what the
           left column needed to stop running off the bottom. */
        const ImVec2 wide (116.0f, 0.0f);

        if (ImGui::Button ("Apply to sel.", wide))
            applyToSelection (currentPickerRgb());

        ImGui::SameLine();

        if (ImGui::Button ("Pick from sel.", wide))
            pickFromSelection();

        ImGui::SameLine();

        if (ImGui::Button ("Fill unsel.", wide))
            applyToUnselected (currentPickerRgb());

        if (ImGui::Button ("Invert sel.", wide))
            for (int n = 0; n < 128; ++n)
                selected[n] = ! selected[n];

        ImGui::SameLine();

        if (ImGui::Button ("Select all", wide))
            for (int n = 0; n < 128; ++n)
                selected[n] = true;

        ImGui::SameLine();

        if (ImGui::Button ("Select none", wide))
            for (int n = 0; n < 128; ++n)
                selected[n] = false;

        /*
            Maps as files, saved wherever you like.

            The dialog is not opened here. It is modal, and a modal dialog runs its own
            message loop - so opening one in the middle of building a frame lets the
            repaint timer fire and re-enter a frame that is still open, which locks the
            editor up. The button records what was asked for and the frame finishes
            first.
        */
        const ImVec2 narrow (84.0f, 0.0f);

        if (ImGui::Button ("Save...", narrow))
            pendingDialog = 1;

        ImGui::SameLine();

        if (ImGui::Button ("Load...", narrow))
            pendingDialog = 2;

        ImGui::SameLine();
        drawClipboardControls (narrow);

        if (! presetMessage.empty())
            ImGui::TextDisabled ("%s", presetMessage.c_str());

    }

    void drawGenerators()
    {

        /* Four generators on one row rather than two. */
        const ImVec2 gen (86.0f, 0.0f);

        if (ImGui::Button ("Wheel", gen))
        {
            pushUndo();
            generateWheel (1.0f);
        }

        ImGui::SameLine();

        if (ImGui::Button ("Fifths", gen))
        {
            pushUndo();
            generateFifths();
        }

        ImGui::SameLine();

        if (ImGui::Button ("Piano", gen))
        {
            pushUndo();
            generatePiano();
        }

        ImGui::SameLine();

        if (ImGui::Button ("Blackout", gen))
        {
            pushUndo();
            applyToAll (0x000000);
        }

        ImGui::SetNextItemWidth (76.0f);

        /* Changing either recolours immediately. Making you blacken the keyboard,
           select the in-scale notes and then apply was three steps to express one
           intention. */
        if (ImGui::Combo ("Root", &scaleRoot, kNoteNames, 12))
            generateScale();

        ImGui::SameLine();
        ImGui::SetNextItemWidth (150.0f);

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
        /*
            Asked for here, done after the frame.

            Enumerating windows and reading one are the same hazard the file dialogs
            were: PrintWindow makes another application draw and can pump its message
            loop, and doing that between NewFrame and Render - with this plugin's GL
            context current - lets the host paint into it. The symptom is the editor
            and the host going black, which is exactly what the dialogs used to do
            before they were moved out of the frame.
        */
        if (ImGui::Button ("Find windows", ImVec2 (110.0f, 0.0f)))
            pendingCapture = 1;

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
            pendingCapture = 2;

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
            {
                owner->link.setFollowSource (owner->capture, captureAnchor);
                appliedAnchor = captureAnchor;
            }
            else
            {
                owner->link.stopFollowing();
            }
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

        {
            int typed = captureAnchor;

            if (ImGui::DragInt ("Lowest C is", &typed, 0.25f, 0, 120, "note %d"))
                setCaptureAnchor (typed);
        }

        ImGui::SameLine();
        ImGui::Text ("%s%d", kNoteNames[pitchClassOf (captureAnchor)],
                     octaveOf (captureAnchor));
        ImGui::SameLine();

        if (ImGui::Button ("Import colours", ImVec2 (130.0f, 0.0f)))
        {
            pushUndo();
            owner->capture->applyTo (owner->link, captureAnchor);
            appliedAnchor = captureAnchor;
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

    /*
        The idle pattern, in its own section.

        It sat among the display effects, which was the wrong company: everything else
        there reacts to playing, and this one only runs when nothing is. Having a mode
        to choose made the mismatch worse, so it moved out.
    */
    void drawScreensaverControls()
    {
        bool waves = owner->link.getWavesEnabled();

        if (ImGui::Checkbox ("Screensaver", &waves))
        {
            owner->link.setWavesEnabled (waves);
            markDirty();
        }

        ImGui::SameLine();
        ImGui::TextDisabled (owner->link.wavesRunning() ? "running" : "idle");

        int mode = owner->link.getWavesMode();
        ImGui::SetNextItemWidth (150.0f);

        if (ImGui::Combo ("Pattern", &mode,
                              "Waves\0Aurora\0Breathing\0Ember\0Gradient drift\0Rainfall\0"))
        {
            owner->link.setWavesMode (mode);
            markDirty();
        }

        int delay = owner->link.getWavesDelay();
        ImGui::SetNextItemWidth (150.0f);

        if (ImGui::SliderInt ("Starts after", &delay, 5, 600, "%ds"))
        {
            owner->link.setWavesDelay (delay);
            markDirty();
        }

        /* Which ones keep the painted map and which replace it, because that is the
           only thing about the choice that is not obvious from watching it. */
        /* Three kinds now, and which one you picked is the thing worth saying. */
        ImGui::TextDisabled (mode == 2 || mode == 3
                                 ? "dims your colours - keyswitches stay readable"
                                 : (mode == 4 || mode == 5
                                        ? "uses your gradient, not the painted map"
                                        : "replaces your colours until a note is played"));
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
            static const char *sources[] = { "Fixed", "Wheel", "Fifths", "Degree", "Map",
                                             "Gradient" };
            int source = owner->link.getRippleSource();
            ImGui::SetNextItemWidth (95.0f);

            if (ImGui::Combo ("##ripsrc", &source, sources, 6))
            {
                owner->link.setRippleSource (source);
                markDirty();
            }

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip ("Fixed: the swatch\n"
                                   "Wheel: hue by pitch class\n"
                                   "Fifths: hue by position in the circle of fifths\n"
                                   "Degree: the degree colour of the note played\n"
                                   "Map: the colour of the key it came from\n"
                                   "Gradient: the paint gradient, by where the note sits\n"
                                   "          on the keyboard - owes the map nothing, so\n"
                                   "          it works over Blackout, Wheel or Piano");
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
                       owner->link.getAfterglowColour(), kFxAfterglow);
        ImGui::SameLine();
        int decay = owner->link.getAfterglowDecay();
        ImGui::SetNextItemWidth (120.0f);

        if (ImGui::SliderInt ("Decay", &decay, 1, 30, "%d/10 s"))
        {
            owner->link.setAfterglowDecay (decay);
            markDirty();
        }

        drawEffectRow ("Beat pulse", owner->link.getPulseEnabled(),
                       owner->link.getPulseColour(), kFxPulse);
        ImGui::SameLine();
        ImGui::TextDisabled ("follows host transport");

        drawEffectRow ("Chord halo", owner->link.getHaloEnabled(),
                       owner->link.getHaloColour(), kFxHalo);
        ImGui::SameLine();
        ImGui::TextDisabled ("2+ notes held");

        drawEffectRow ("Sustain", owner->link.getSustainEnabled(),
                       owner->link.getSustainColour(), kFxSustain);
        ImGui::SameLine();
        ImGui::TextDisabled (owner->link.getSustainDown() ? "pedal down"
                                                          : "tints notes CC 64 is holding");

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
    /*
        Which effect a row drives, named rather than numbered.

        This was a bare integer with a chain ending in else, so the last effect was
        whatever did not match - and adding the pedal, which took the number chord halo
        was already using, silently handed halo's checkbox to the pedal and left halo
        with nothing to write to. Two rows toggled one setting and the other could not
        be toggled at all.

        A switch with no default means the next one of these that collides fails to
        compile instead of quietly stealing someone else's control.
    */
    enum EffectTarget
    {
        kFxAfterglow = 0,
        kFxPulse,
        kFxHalo,
        kFxSustain
    };

    void drawEffectRow (const char *label, bool enabled, uint32_t rgb, EffectTarget target)
    {
        bool on = enabled;
        ImGui::PushID (label);

        if (ImGui::Checkbox (label, &on))
        {
            switch (target)
            {
                case kFxAfterglow: owner->link.setAfterglowEnabled (on); break;
                case kFxPulse:     owner->link.setPulseEnabled (on); break;
                case kFxHalo:      owner->link.setHaloEnabled (on); break;
                case kFxSustain:   owner->link.setSustainEnabled (on); break;
            }

            markDirty();
        }

        ImGui::SameLine (140.0f);
        uint32_t picked = 0;

        if (drawSwatch ("fx", rgb, picked))
        {
            switch (target)
            {
                case kFxAfterglow: owner->link.setAfterglowColour (picked); break;
                case kFxPulse:     owner->link.setPulseColour (picked); break;
                case kFxHalo:      owner->link.setHaloColour (picked); break;
                case kFxSustain:   owner->link.setSustainColour (picked); break;
            }

            markDirty();
        }

        ImGui::PopID();
    }

    /*
        Moving the anchor, which has to reach three places and used to reach one.

        The follow loop lives on the worker now, and the worker reads followAnchor -
        which was written only when the Live box was ticked. So Find anchor could resolve
        the anchor perfectly, print the right note, and change nothing at all: the worker
        carried on placing the captured colours at whatever anchor was current when
        following started. Dragging the field by hand had the same non-effect. Before
        following moved off the editor's draw loop this could not happen, because the
        editor passed the live value on every frame.

        Clearing the old span matters as much as setting the new one. applyTo only writes
        the notes the capture covers, so shifting down an octave leaves the top octave
        lit at its last values - which reads as the shift not having happened rather than
        as leftovers. Only cleared when colours were actually applied at that anchor, so
        a first Find anchor cannot wipe a map painted by hand.
    */
    void setCaptureAnchor (int note)
    {
        const int clamped = note < 0 ? 0 : (note > 120 ? 120 : note);

        if (clamped == captureAnchor)
            return;

        if (appliedAnchor >= 0)
        {
            int semi = 0;
            uint32_t rgb = 0;

            for (int i = 0; owner->capture->keyInfo (i, semi, rgb); ++i)
            {
                const int old = appliedAnchor + semi;

                if (old >= 0 && old < 128)
                    setColourIfOwned (old, 0);
            }
        }

        captureAnchor = clamped;
        owner->capture->applyTo (owner->link, captureAnchor);
        appliedAnchor = captureAnchor;

        if (captureLive)
            owner->link.setFollowSource (owner->capture, captureAnchor);

        markDirty();
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
                    setCaptureAnchor (probeNote - bestSemi);
                    probeResult = "anchor found";
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
        /*
            Pinned while the keyboard is shared.

            This shifts which notes the device reports for a given key. With zones that
            would move every zone's key-to-note mapping underneath it at once - one
            control quietly undoing what each instance had been set to individually.
            The per-zone offset under the keyboard is the one to use instead, and the
            hardware's own octave buttons are inert for the same reason.
        */
        const bool pinned = owner->link.hasZone();

        ImGui::BeginDisabled (pinned);

        int oct = (int) owner->octave;
        ImGui::SetNextItemWidth (240.0f);

        const bool changed = ImGui::SliderInt ("Octave", &oct, -3, 3, "%+d");

        if (ImGui::IsItemActivated())
            pushGuiParam (owner, kParamOctave, (double) oct, true, false);

        if (changed)
            pushGuiParam (owner, kParamOctave, (double) oct, false, false);

        if (ImGui::IsItemDeactivatedAfterEdit())
            pushGuiParam (owner, kParamOctave, (double) oct, false, true);

        ImGui::EndDisabled();

        if (pinned)
            ImGui::TextDisabled ("pinned while sharing - use the zone offset");

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

    /*
        Whether this instance may paint a key at all.

        Sharing the chain means owning a stretch of notes, and a key outside it is drawn
        black whatever the map says - so letting the editor paint one is offering a
        control that does nothing. Worse than nothing: the colour is stored, the key
        stays dark, and the obvious conclusion is that painting is broken.

        One gate, used by everything that writes a colour or changes the selection, so
        there is no path that can quietly bypass it.
    */
    /*
        Everything the editor paints is addressed by key, not by note.

        The keyboard on screen is the hardware's keys, and so is the selection taken
        from it. With a zone offset the map entry those keys read lives elsewhere, so
        the write goes through setKeyColour - which is the one place the two coordinate
        systems meet. Without it, clicking a key would colour whatever note happens to
        share its number and the keyboard would appear to paint somewhere else.
    */
    void setColourIfOwned (int key, uint32_t rgb)
    {
        if (paintable (key))
            owner->link.setKeyColour (key, rgb);
    }

    bool paintable (int note) const
    {
        return note >= 0 && note < 128 && owner->link.noteInZone (note);
    }

    /* Selection can only ever hold keys this instance owns, so every operation that
       works from the selection inherits the limit without repeating it. */
    void trimSelectionToZone()
    {
        if (! owner->link.getZoned())
            return;

        for (int n = 0; n < 128; ++n)
            if (selected[n] && ! paintable (n))
                selected[n] = false;
    }

    void applyToSelection (uint32_t rgb)
    {
        pushUndo();

        for (int n = 0; n < 128; ++n)
            if (selected[n])
                setColourIfOwned (n, rgb);

        markDirty();
    }

    void applyToUnselected (uint32_t rgb)
    {
        for (int n = 0; n < 128; ++n)
            if (! selected[n])
                setColourIfOwned (n, rgb);

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
            setColourIfOwned (n, rgb);

        markDirty();
    }

    void pickFromSelection()
    {
        for (int n = 0; n < 128; ++n)
        {
            if (! selected[n])
                continue;

            const uint32_t rgb = owner->link.getKeyColour (n);
            pickerColour[0] = (float) ((rgb >> 16) & 0xff) / 255.0f;
            pickerColour[1] = (float) ((rgb >> 8) & 0xff) / 255.0f;
            pickerColour[2] = (float) (rgb & 0xff) / 255.0f;
            return;
        }
    }

    void generateWheel (float saturation)
    {
        for (int n = 0; n < 128; ++n)
            setColourIfOwned (n, hsvToRgb ((float) pitchClassOf (n) / 12.0f, saturation, 1.0f));

        markDirty();
    }

    void generateFifths()
    {
        for (int n = 0; n < 128; ++n)
        {
            const int position = (pitchClassOf (n) * 7) % 12;
            setColourIfOwned (n, hsvToRgb ((float) position / 12.0f, 0.85f, 1.0f));
        }

        markDirty();
    }

    void generatePiano()
    {
        for (int n = 0; n < 128; ++n)
            setColourIfOwned (n, kIsBlackKey[pitchClassOf (n)] ? 0x000000u : 0xf0f0ffu);

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
                setColourIfOwned (n, outRgb);
            else if (degree == 0)
                setColourIfOwned (n, rootRgb);
            else
                setColourIfOwned (n, inRgb);
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

    /* The anchor the captured colours are currently sitting at, or -1 when none have
       been applied. Only that span is cleared when the anchor moves. */
    int appliedAnchor = -1;

    int zoneLow = 0;
    int zoneHigh = 127;
    ZoneInfo zoneBlocker = { 0, 0, 0 };
    bool captureLive = false;
    std::string presetMessage;

public:
    /* 0 none, 1 save, 2 load. Acted on after the frame, never during it. */
    int pendingDialog = 0;

    /* 0 none, 1 find windows, 2 read the selected one. Same rule, same reason. */
    int pendingCapture = 0;

    /* Compared with the previous frame's; any difference means redraw. */
    uint32_t lastSignature = 0;
    bool lastHadMouse = true;
    ImVec2 lastMouse = ImVec2 (0.0f, 0.0f);
    int settleFrames = 0;
    double lastColourFrame = 0.0;

    bool needsFrame()
    {
        uint32_t signature = 0;

        for (int n = 0; n < 128; ++n)
            signature = signature * 31u + owner->link.getDisplayColour (n);

        signature = signature * 31u + (uint32_t) (owner->link.hasDevice() ? 1 : 0);
        signature = signature * 31u + (uint32_t) pendingDialog;
        signature = signature * 31u + (uint32_t) pendingCapture;
        signature = signature * 31u + (uint32_t) (owner->link.getZoned() ? 1 : 0);
        signature = signature * 31u + (uint32_t) owner->link.getZoneLow();
        signature = signature * 31u + (uint32_t) owner->link.getZoneHigh();

        /*
            Colours changing are rate-limited; input is not.

            A whole frame is rebuilt and redrawn whenever anything changes - an
            immediate-mode editor has no way to redraw only the keyboard - so while
            notes are playing or a screensaver is running the colours change every tick
            and the saving disappears entirely. Thirty frames a second is as much as
            anyone can see of a keyboard mirror, and it halves that case.

            The device itself is unaffected: it is fed from the worker at its own rate
            and never waits for the editor. This only slows the picture of it.

            Input is deliberately not throttled. A pointer that redraws at thirty frames
            feels worse than one that redraws at sixty, and input frames are rare enough
            that their cost does not matter.
        */
        const double now = ImGui::GetTime();
        const bool changed = signature != lastSignature
                          && (now - lastColourFrame) >= 0.033;

        if (changed)
            lastColourFrame = now;

        lastSignature = changed ? signature : lastSignature;

        /*
            Input is the host's business, not this function's.

            ImGui applies queued input during NewFrame, so an editor that skipped the
            frame would never see the pointer move and could never decide to wake up.
            The host polls or receives input either way and ORs its answer with this
            one; what is left here is everything the host cannot see - the colours, and
            anything mid-interaction that outlives a single input event.
        */
        const bool busy = ImGui::IsAnyItemActive()
                       || ImGui::IsPopupOpen (nullptr, ImGuiPopupFlags_AnyPopupId
                                                     | ImGuiPopupFlags_AnyPopupLevel)
                       || settleFrames > 0;

        if (changed)
            settleFrames = 3;
        else if (settleFrames > 0)
            --settleFrames;

        return changed || busy;
    }

    void runPendingCapture()
    {
        const int which = pendingCapture;
        pendingCapture = 0;

        if (which == 1)
        {
            owner->capture->refreshWindows();
            captureIndex = 0;
            return;
        }

        if (which != 2)
            return;

        /*
            The title is remembered only when the grab worked.

            It was indented as though it were inside this block and was not, so a failed
            grab still recorded the title - and refindWindow would then spend every later
            attempt chasing a window that had never been read successfully in the first
            place.

            The index is compared as a size_t as well. Against an unsigned size a
            negative index converts to something enormous and passes the test, which is
            the one case the check exists to stop.
        */
        if (owner->capture->grab ((size_t) captureIndex))
        {
            owner->capture->detect();

            const std::vector<CaptureWindow> &found = owner->capture->windows();

            if (captureIndex >= 0 && (size_t) captureIndex < found.size())
                owner->capture->rememberTitle (found[captureIndex].title);
        }
    }

    void runPendingDialog()
    {
        runPendingCapture();

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

            /* Before the load, not inside it: a snapshot taken after the colours have
               already been replaced records the new table and undoes nothing. */
            pushUndo();

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
}

/*
    Anything modal the frame asked for, run after the frame rather than inside it.

    This used to sit at the end of renderEditor, which reads as "once drawing is done"
    and is not: the host calls this between NewFrame and Render, with an ImGui frame
    open and, on Windows, our GL context current on the host's own UI thread. A file
    dialog there pumps its own message loop, so the host redrew its windows into our
    context while its own window sat disabled - a DAW gone black and deaf to the mouse,
    with the editor frozen on its last frame because the re-entrancy guard turned every
    repaint away. Nothing about that points at a file dialog.

    imguiHostSetAfterFrame is called once when the editor is created, and the host layer
    invokes this only when the frame is closed and its drawing state is back as it was.
*/
/*
    Whether this frame would differ from the last one.

    Deliberately generous about saying yes. A wrong yes costs one frame nobody needed; a
    wrong no leaves a stale picture on screen, which is the kind of bug that looks like a
    hang. So anything that could possibly be moving counts: the pointer being over the
    window at all, any mouse button down, a control being edited, a tooltip or popup
    open, and every animation the plugin runs.

    The colour table is folded into a single number and compared with last frame's. That
    covers everything the keyboard shows - notes arriving, effects decaying, a zone's
    colours changing underneath - without the editor needing to know which of them
    happened.

    What is left is the common case: an editor sitting open with nothing playing and the
    mouse elsewhere, redrawing the same picture sixty times a second.
*/
bool editorShouldRender (void *userData)
{
    LumiPaint *self = (LumiPaint *) userData;

    if (self == nullptr || self->editor == nullptr)
        return true;

    return ((LumiEditor *) self->editor)->needsFrame();
}

void editorAfterFrame (void *userData)
{
    LumiPaint *self = (LumiPaint *) userData;

    if (self == nullptr || self->editor == nullptr)
        return;

    ((LumiEditor *) self->editor)->runPendingDialog();
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

    imguiHostSetAfterFrame (editor->hostWindow(), editorAfterFrame);
    imguiHostSetShouldRender (editor->hostWindow(), editorShouldRender);
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
