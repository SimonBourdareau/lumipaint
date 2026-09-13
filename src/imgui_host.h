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

/*
 * The window and OpenGL context behind the editor.
 *
 * The host layer owns the render loop, not the caller: it decides when a frame
 * happens and calls `render` with ImGui's frame already open, so a platform can
 * drive repaint however it must. On Windows that is a WM_TIMER, because relying
 * on the host's CLAP timer extension leaves the window blank in any host that
 * does not provide one.
 */

struct ImGuiHostWindow;

typedef void (*ImGuiHostRenderFn) (void *userData);
typedef void (*ImGuiHostClosedFn) (void *userData);

ImGuiHostWindow *imguiHostCreate (uint32_t width, uint32_t height, bool floating,
                                  ImGuiHostRenderFn render, ImGuiHostClosedFn closed,
                                  void *userData);
void imguiHostDestroy (ImGuiHostWindow *window);
bool imguiHostSetParent (ImGuiHostWindow *window, void *nativeHandle);
bool imguiHostSetTransient (ImGuiHostWindow *window, void *nativeHandle);
void imguiHostSetTitle (ImGuiHostWindow *window, const char *title);
void imguiHostSetSize (ImGuiHostWindow *window, uint32_t width, uint32_t height);
void imguiHostSetScale (ImGuiHostWindow *window, double scale);
void imguiHostShow (ImGuiHostWindow *window);
void imguiHostHide (ImGuiHostWindow *window);
