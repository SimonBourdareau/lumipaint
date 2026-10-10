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

/*
 * Called once the frame is finished and the platform's drawing state is back as
 * it was found: no ImGui frame open, no GL context of ours left current, no
 * re-entrancy guard held.
 *
 * It exists for one thing, and that thing was a bug. A file dialog opened from
 * inside `render` is opened in the middle of a frame - and a modal dialog pumps
 * its own message loop, so for as long as it is up the host redraws its own
 * windows on this thread while our GL context is still current and its window is
 * disabled. The DAW goes black and stops answering the mouse, the editor keeps
 * showing its last frame because the re-entrancy guard turns every repaint away,
 * and none of it looks like it came from a file dialog.
 *
 * Deferring within `render` is not enough - that is still inside the frame. It
 * has to be after it, which only the host layer can arrange.
 */
typedef void (*ImGuiHostAfterFrameFn) (void *userData);

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

/* Optional. Not passed to imguiHostCreate so that the existing signature, and every
   caller of it, stays as it is. */
void imguiHostSetAfterFrame (ImGuiHostWindow *window, ImGuiHostAfterFrameFn afterFrame);

/*
    Asked before each frame, and the frame is skipped when it says no.

    An editor that redraws whether or not anything has changed spends most of its time
    drawing the same picture again. The plugin is nearly free when its window is closed
    and expensive when it is open, and almost all of that is frames nobody asked for.

    Returning false skips the frame entirely - no new frame, no draw, no swap - and
    leaves whatever was last drawn on screen, which is the same picture it would have
    produced anyway.
*/
typedef bool (*ImGuiHostShouldRenderFn) (void *userData);

void imguiHostSetShouldRender (ImGuiHostWindow *window, ImGuiHostShouldRenderFn fn);
