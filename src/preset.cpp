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

#include "preset.h"
#include "lumipaint.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#if defined (_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <commdlg.h>
 #include <shlobj.h>
 #undef far
 #undef near
#else
 #include <sys/stat.h>
 #include <sys/types.h>
 #include <unistd.h>
#endif

namespace lumipaint {
namespace {

char g_error[256] = "";

void setError (const char *text)
{
    std::snprintf (g_error, sizeof (g_error), "%s", text);
}

std::string hex (uint32_t rgb)
{
    char buf[8];
    std::snprintf (buf, sizeof (buf), "%06x", (unsigned int) (rgb & 0x00ffffffu));
    return buf;
}

uint32_t unhex (const std::string &s)
{
    return (uint32_t) std::strtoul (s.c_str(), nullptr, 16) & 0x00ffffffu;
}

#if defined (_WIN32)

std::string runDialog (bool saving)
{
    wchar_t file[MAX_PATH] = L"";
    const std::string dir = PresetIO::defaultDirectory();
    std::wstring wdir (dir.begin(), dir.end());

    OPENFILENAMEW ofn;
    ZeroMemory (&ofn, sizeof (ofn));
    ofn.lStructSize = sizeof (ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = L"LumiPaint map\0*.lumimap\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"lumimap";
    ofn.lpstrInitialDir = wdir.empty() ? nullptr : wdir.c_str();

    /* OFN_NOCHANGEDIR matters in a plugin: without it the dialog changes the working
       directory of the whole host, which breaks whatever the host does with relative
       paths afterwards. */
    ofn.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST
              | (saving ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);

    const BOOL ok = saving ? GetSaveFileNameW (&ofn) : GetOpenFileNameW (&ofn);

    if (! ok)
        return std::string();

    char narrow[MAX_PATH * 2] = "";
    WideCharToMultiByte (CP_UTF8, 0, file, -1, narrow, sizeof (narrow), nullptr, nullptr);
    return narrow;
}

#endif

}

#if defined (__APPLE__)

/*
    Implemented in preset_macos.mm, so this file stays plain C++ and only the part that
    must be Objective-C is.

    Declared after the anonymous namespace closes, which is not where it used to sit. An
    anonymous namespace gives internal linkage, so the declaration could never resolve
    against the definition in the .mm - the macOS build compiled both files and then
    failed at the link with an undefined symbol, which is not where anyone looks for a
    file-dialog problem.
*/
std::string macDialog (bool saving);

#endif

const char *PresetIO::lastError()
{
    return g_error;
}

std::string PresetIO::defaultDirectory()
{
#if defined (_WIN32)
    wchar_t docs[MAX_PATH] = L"";

    if (! SUCCEEDED (SHGetFolderPathW (nullptr, CSIDL_PERSONAL, nullptr, 0, docs)))
        return std::string();

    std::wstring path = std::wstring (docs) + L"\\LumiPaint";
    CreateDirectoryW (path.c_str(), nullptr);

    char narrow[MAX_PATH * 2] = "";
    WideCharToMultiByte (CP_UTF8, 0, path.c_str(), -1, narrow, sizeof (narrow),
                         nullptr, nullptr);
    return narrow;
#else
    const char *home = getenv ("HOME");

    if (home == nullptr)
        return std::string();

    /*
        Created if it is not there, which the Windows branch has always done and this
        one did not. A save panel pointed at a directory that does not exist is ignored
        and opens wherever the system last was, so the first save of a fresh install
        landed somewhere arbitrary while the panel gave no hint why.
    */
    const std::string path = std::string (home) + "/Documents/LumiPaint";
    mkdir (path.c_str(), 0755);
    return path;
#endif
}

#if defined (__linux__)

namespace {

/*
    A dialog through whatever the desktop provides.

    zenity and kdialog are both single commands that print the chosen path, and one of
    them is present on most desktops. Neither is a dependency: if both are missing the
    caller is told to type a path instead, which is worse but not broken.

    Deliberately not a GTK or Qt dependency. Linking a toolkit into an audio plugin
    invites symbol clashes with whatever the host already loaded, and a file dialog is
    not worth that risk.
*/
std::string runCommand (const std::string &command)
{
    FILE *pipe = popen (command.c_str(), "r");

    if (pipe == nullptr)
        return std::string();

    char buffer[1024] = { 0 };
    std::string result;

    while (fgets (buffer, sizeof (buffer), pipe) != nullptr)
        result += buffer;

    pclose (pipe);

    while (! result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();

    return result;
}

bool haveTool (const char *name)
{
    return runCommand (std::string ("command -v ") + name + " 2>/dev/null").size() > 0;
}

std::string linuxDialog (bool saving)
{
    const std::string dir = PresetIO::defaultDirectory();

    if (haveTool ("zenity"))
        return runCommand ("zenity --file-selection"
                           + std::string (saving ? " --save --confirm-overwrite" : "")
                           + " --file-filter='LumiPaint map | *.lumimap'"
                           + " --filename='" + dir + "/' 2>/dev/null");

    if (haveTool ("kdialog"))
        return runCommand (std::string (saving ? "kdialog --getsavefilename "
                                               : "kdialog --getopenfilename ")
                           + "'" + dir + "/' '*.lumimap' 2>/dev/null");

    setError ("install zenity or kdialog for a file dialog");
    return std::string();
}

}

#endif

std::string PresetIO::askForSavePath()
{
#if defined (_WIN32)
    return runDialog (true);
#elif defined (__APPLE__)
    return macDialog (true);
#elif defined (__linux__)
    return linuxDialog (true);
#else
    setError ("no file dialog on this platform");
    return std::string();
#endif
}

std::string PresetIO::askForOpenPath()
{
#if defined (_WIN32)
    return runDialog (false);
#elif defined (__APPLE__)
    return macDialog (false);
#elif defined (__linux__)
    return linuxDialog (false);
#else
    setError ("no file dialog on this platform");
    return std::string();
#endif
}

bool PresetIO::save (const std::string &path, const LumiLink &link,
                     double brightness, double unlitLevel)
{
    std::ofstream out (path.c_str());

    if (! out)
    {
        setError ("could not write that file");
        return false;
    }

    out << "lumipaint-map 1\n";
    out << "# one line per note: note number, then rrggbb\n";

    for (int note = 0; note < 128; ++note)
        out << "note " << note << ' ' << hex (link.getColour (note)) << '\n';

    out << "highlight " << hex (link.getHighlightColour()) << '\n';
    out << "pressed " << hex (link.getPressColour()) << '\n';
    out << "pressure " << hex (link.getPressureGradColour()) << '\n';
    out << "bend " << hex (link.getBendGradColour()) << '\n';
    out << "ripple " << hex (link.getRippleColour()) << '\n';
    out << "splash " << hex (link.getSplashColour()) << '\n';
    out << "afterglow " << hex (link.getAfterglowColour()) << '\n';
    out << "pulse " << hex (link.getPulseColour()) << '\n';
    out << "halo " << hex (link.getHaloColour()) << '\n';
    out << "tension-home " << hex (link.getTensionHome()) << '\n';
    out << "tension-far " << hex (link.getTensionFar()) << '\n';

    for (int i = 0; i < 8; ++i)
        out << "degree " << i << ' ' << hex (link.getDegreeColour (i)) << '\n';

    /*
        The settings as well as the colours.

        A map used to be the 128 colours and the effect tints, which meant loading one
        gave you the right palette and none of the behaviour - the ripple that made the
        palette worth having was still set to whatever the last session left. A map is
        the whole look of an instance, so it saves the whole look.
    */
    out << "ripple " << (link.getRippleEnabled() ? 1 : 0) << '\n';
    out << "ripple-speed " << link.getRippleSpeed() << '\n';
    out << "ripple-trail " << link.getRippleTrail() << '\n';
    out << "ripple-source " << link.getRippleSource() << '\n';
    out << "splash " << (link.getSplashEnabled() ? 1 : 0) << '\n';
    out << "splash-cc " << link.getSplashCC() << '\n';
    out << "afterglow " << (link.getAfterglowEnabled() ? 1 : 0) << '\n';
    out << "afterglow-decay " << link.getAfterglowDecay() << '\n';
    out << "pulse " << (link.getPulseEnabled() ? 1 : 0) << '\n';
    out << "halo " << (link.getHaloEnabled() ? 1 : 0) << '\n';
    out << "degrees " << (link.getDegreeEnabled() ? 1 : 0) << '\n';
    out << "degrees-strength " << link.getDegreeAlpha() << '\n';
    out << "tension " << (link.getTensionEnabled() ? 1 : 0) << '\n';
    out << "tension-strength " << link.getTensionAlpha() << '\n';
    out << "waves " << (link.getWavesEnabled() ? 1 : 0) << '\n';
    out << "waves-delay " << link.getWavesDelay() << '\n';
    out << "bend-path " << (link.getBendPathEnabled() ? 1 : 0) << '\n';
    out << "velocity " << (link.getVelocityEnabled() ? 1 : 0) << '\n';
    out << "bend-scale " << link.getBendFullScale() << '\n';

    out << "brightness " << brightness << '\n';
    out << "unlit " << unlitLevel << '\n';

    setError ("");
    return out.good();
}

bool PresetIO::load (const std::string &path, LumiLink &link,
                     double &brightness, double &unlitLevel)
{
    std::ifstream in (path.c_str());

    if (! in)
    {
        setError ("could not read that file");
        return false;
    }

    std::string first;
    std::getline (in, first);

    if (first.rfind ("lumipaint-map", 0) != 0)
    {
        setError ("that is not a LumiPaint map");
        return false;
    }

    std::string line;

    while (std::getline (in, line))
    {
        if (line.empty() || line[0] == '#')
            continue;

        std::istringstream parts (line);
        std::string key;
        parts >> key;

        if (key == "note")
        {
            int note = -1;
            std::string colour;
            parts >> note >> colour;

            if (note >= 0 && note < 128)
                link.setColour (note, unhex (colour));
        }
        else if (key == "degree")
        {
            int index = -1;
            std::string colour;
            parts >> index >> colour;
            link.setDegreeColour (index, unhex (colour));
        }
        else if (key == "ripple") { int v; parts >> v; link.setRippleEnabled (v != 0); }
        else if (key == "ripple-speed") { int v; parts >> v; link.setRippleSpeed (v); }
        else if (key == "ripple-trail") { int v; parts >> v; link.setRippleTrail (v); }
        else if (key == "ripple-source") { int v; parts >> v; link.setRippleSource (v); }
        else if (key == "splash") { int v; parts >> v; link.setSplashEnabled (v != 0); }
        else if (key == "splash-cc") { int v; parts >> v; link.setSplashCC (v); }
        else if (key == "afterglow") { int v; parts >> v; link.setAfterglowEnabled (v != 0); }
        else if (key == "afterglow-decay") { int v; parts >> v; link.setAfterglowDecay (v); }
        else if (key == "pulse") { int v; parts >> v; link.setPulseEnabled (v != 0); }
        else if (key == "halo") { int v; parts >> v; link.setHaloEnabled (v != 0); }
        else if (key == "degrees") { int v; parts >> v; link.setDegreeEnabled (v != 0); }
        else if (key == "degrees-strength") { int v; parts >> v; link.setDegreeAlpha (v); }
        else if (key == "tension") { int v; parts >> v; link.setTensionEnabled (v != 0); }
        else if (key == "tension-strength") { int v; parts >> v; link.setTensionAlpha (v); }
        else if (key == "waves") { int v; parts >> v; link.setWavesEnabled (v != 0); }
        else if (key == "waves-delay") { int v; parts >> v; link.setWavesDelay (v); }
        else if (key == "bend-path") { int v; parts >> v; link.setBendPathEnabled (v != 0); }
        else if (key == "velocity") { int v; parts >> v; link.setVelocityEnabled (v != 0); }
        else if (key == "bend-scale") { int v; parts >> v; link.setBendFullScale (v); }
        else if (key == "brightness")
        {
            parts >> brightness;
        }
        else if (key == "unlit")
        {
            parts >> unlitLevel;
        }
        else
        {
            std::string colour;
            parts >> colour;
            const uint32_t rgb = unhex (colour);

            if (key == "highlight") link.setHighlightColour (rgb);
            else if (key == "pressed") link.setPressColour (rgb);
            else if (key == "pressure") link.setPressureGradColour (rgb);
            else if (key == "bend") link.setBendGradColour (rgb);
            else if (key == "ripple") link.setRippleColour (rgb);
            else if (key == "splash") link.setSplashColour (rgb);
            else if (key == "afterglow") link.setAfterglowColour (rgb);
            else if (key == "pulse") link.setPulseColour (rgb);
            else if (key == "halo") link.setHaloColour (rgb);
            else if (key == "tension-home") link.setTensionHome (rgb);
            else if (key == "tension-far") link.setTensionFar (rgb);
        }
    }

    setError ("");
    return true;
}

}
