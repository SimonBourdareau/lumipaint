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
    The Linux side of imgui_host.h, on X11 and GLX.

    Written but never compiled or run: there is no Linux here to test on, and no
    keyboard to test it against. Treat it as a starting point that mirrors the Windows
    one closely enough to compare line by line.

    Build:
        cmake -B build -DLUMIPAINT_HOST_SOURCES=src/imgui_host_x11.cpp
        cmake --build build

    X11 rather than Wayland, deliberately. CLAP's linux API hands over an X11 window id,
    plugin embedding under Wayland is still unresolved across hosts, and most Linux DAW
    users are on X11 or XWayland anyway. Under XWayland this works; under a pure Wayland
    session it will not, and there is no small fix for that.

    Three things differ from Windows in ways that matter.

    There is no WM_TIMER, so repaint runs on a thread that sleeps and asks the main
    thread to redraw. CLAP's timer extension would be tidier but a host is not obliged
    to provide one - the same reason the Windows side does not use it.

    An embedded plugin window is reparented into the host's window with XReparentWindow.
    The host owns the outer window; this is a child inside it.

    Xlib is not thread safe unless told to be. XInitThreads has to be called before any
    other Xlib call, and once per process rather than once per window.
*/

#include "imgui_host.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <GL/gl.h>
#include <GL/glx.h>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

struct ImGuiHostWindow
{
    Display *display;
    Window window;
    Window parent;
    GLXContext glx;
    Colormap colormap;
    Atom deleteMessage;
    ImGuiContext *imgui;
    ImGuiHostRenderFn render;
    ImGuiHostClosedFn closed;
    void *userData;
    uint32_t w;
    uint32_t h;
    double scale;
    bool floating;
    bool visible;
    std::atomic<bool> running;
    std::thread ticker;
};

namespace {

/*
    Xlib must be told, once, that more than one thread will touch it.

    Called before anything else opens a display. Getting this wrong does not fail
    cleanly - it produces occasional lockups and corrupted drawing that look like
    problems with the host rather than with this file.
*/
void ensureThreads()
{
    static bool done = false;

    if (! done)
    {
        XInitThreads();
        done = true;
    }
}

/* Pointer position and buttons, since there is no equivalent of the Windows backend
   feeding these in for us. */
void pumpInput (ImGuiHostWindow *c)
{
    ImGuiIO &io = ImGui::GetIO();

    Window root, child;
    int rootX, rootY, winX, winY;
    unsigned int mask = 0;

    if (XQueryPointer (c->display, c->window, &root, &child,
                       &rootX, &rootY, &winX, &winY, &mask))
    {
        io.AddMousePosEvent ((float) winX, (float) winY);
        io.AddMouseButtonEvent (0, (mask & Button1Mask) != 0);
        io.AddMouseButtonEvent (1, (mask & Button3Mask) != 0);
        io.AddMouseButtonEvent (2, (mask & Button2Mask) != 0);
    }

    XEvent event;

    while (XPending (c->display) > 0)
    {
        XNextEvent (c->display, &event);

        switch (event.type)
        {
        case ConfigureNotify:
            c->w = (uint32_t) event.xconfigure.width;
            c->h = (uint32_t) event.xconfigure.height;
            break;

        case ButtonPress:
        case ButtonRelease:
            /* Wheel arrives as buttons four and five. */
            if (event.xbutton.button == 4 || event.xbutton.button == 5)
                if (event.type == ButtonPress)
                    io.AddMouseWheelEvent (0.0f, event.xbutton.button == 4 ? 1.0f : -1.0f);
            break;

        case ClientMessage:
            if ((Atom) event.xclient.data.l[0] == c->deleteMessage && c->closed != nullptr)
                c->closed (c->userData);
            break;

        default:
            break;
        }
    }
}

void renderFrame (ImGuiHostWindow *c)
{
    if (c == nullptr || c->imgui == nullptr || ! c->visible)
        return;

    if (c->w < 8 || c->h < 8)
        return;

    ImGuiContext *previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext (c->imgui);

    if (! glXMakeCurrent (c->display, c->window, c->glx))
    {
        ImGui::SetCurrentContext (previous);
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2 ((float) c->w, (float) c->h);
    io.DeltaTime = 1.0f / 60.0f;

    pumpInput (c);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    if (c->render != nullptr)
        c->render (c->userData);

    ImGui::Render();
    glViewport (0, 0, (int) c->w, (int) c->h);
    glClearColor (0.055f, 0.055f, 0.062f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData (ImGui::GetDrawData());
    glXSwapBuffers (c->display, c->window);

    glXMakeCurrent (c->display, None, nullptr);
    ImGui::SetCurrentContext (previous);
}

}

ImGuiHostWindow *imguiHostCreate (uint32_t width, uint32_t height, bool floating,
                                  ImGuiHostRenderFn render, ImGuiHostClosedFn closed,
                                  void *userData)
{
    ensureThreads();

    ImGuiHostWindow *c = new ImGuiHostWindow();
    std::memset ((void *) c, 0, sizeof (ImGuiHostWindow) - sizeof (std::thread));
    c->render = render;
    c->closed = closed;
    c->userData = userData;
    c->w = width;
    c->h = height;
    c->scale = 1.0;
    c->floating = floating;
    c->visible = false;
    c->running.store (false);

    c->display = XOpenDisplay (nullptr);

    if (c->display == nullptr)
    {
        delete c;
        return nullptr;
    }

    const int screen = DefaultScreen (c->display);

    int attribs[] = {
        GLX_RGBA,
        GLX_DEPTH_SIZE, 24,
        GLX_DOUBLEBUFFER,
        None
    };

    XVisualInfo *visual = glXChooseVisual (c->display, screen, attribs);

    if (visual == nullptr)
    {
        XCloseDisplay (c->display);
        delete c;
        return nullptr;
    }

    c->parent = RootWindow (c->display, screen);
    c->colormap = XCreateColormap (c->display, c->parent, visual->visual, AllocNone);

    XSetWindowAttributes swa;
    std::memset (&swa, 0, sizeof (swa));
    swa.colormap = c->colormap;
    swa.event_mask = ExposureMask | StructureNotifyMask | ButtonPressMask
                   | ButtonReleaseMask | PointerMotionMask;

    c->window = XCreateWindow (c->display, c->parent, 0, 0, width, height, 0,
                               visual->depth, InputOutput, visual->visual,
                               CWColormap | CWEventMask, &swa);

    if (c->window == 0)
    {
        XFree (visual);
        XCloseDisplay (c->display);
        delete c;
        return nullptr;
    }

    XStoreName (c->display, c->window, "LumiPaint");

    c->deleteMessage = XInternAtom (c->display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols (c->display, c->window, &c->deleteMessage, 1);

    c->glx = glXCreateContext (c->display, visual, nullptr, GL_TRUE);
    XFree (visual);

    if (c->glx == nullptr)
    {
        XDestroyWindow (c->display, c->window);
        XCloseDisplay (c->display);
        delete c;
        return nullptr;
    }

    glXMakeCurrent (c->display, c->window, c->glx);

    IMGUI_CHECKVERSION();
    c->imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext (c->imgui);
    ImGui::GetIO().IniFilename = nullptr;

    /*
        A real font, the same intent as the Windows side.

        DejaVu is on essentially every distribution, Liberation and Noto cover the rest.
        If none is found AddFontDefault keeps the panel readable with the built-in
        bitmap face rather than leaving it with no font at all. Loaded before the
        backend initialises, because adding a font afterwards leaves the atlas stale
        and the new face never appears.
    */
    {
        const char *faces[] = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/noto/NotoSans-Regular.ttf"
        };

        bool loaded = false;

        for (const char *face : faces)
            if (ImGui::GetIO().Fonts->AddFontFromFileTTF (face, 16.0f) != nullptr)
            {
                loaded = true;
                break;
            }

        if (! loaded)
            ImGui::GetIO().Fonts->AddFontDefault();
    }

    ImGui::StyleColorsDark();

    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.WindowPadding = ImVec2 (12.0f, 10.0f);
    style.ItemSpacing = ImVec2 (8.0f, 7.0f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4 (0.09f, 0.09f, 0.10f, 1.00f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4 (0.06f, 0.06f, 0.07f, 1.00f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4 (0.17f, 0.17f, 0.19f, 1.00f);
    style.Colors[ImGuiCol_Button] = ImVec4 (0.21f, 0.22f, 0.25f, 1.00f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4 (0.30f, 0.32f, 0.38f, 1.00f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4 (0.45f, 0.60f, 0.85f, 1.00f);

    ImGui_ImplOpenGL3_Init ("#version 130");

    glXMakeCurrent (c->display, None, nullptr);
    return c;
}

void imguiHostDestroy (ImGuiHostWindow *c)
{
    if (c == nullptr)
        return;

    c->running.store (false);

    if (c->ticker.joinable())
        c->ticker.join();

    if (c->imgui != nullptr)
    {
        ImGui::SetCurrentContext (c->imgui);
        glXMakeCurrent (c->display, c->window, c->glx);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext (c->imgui);
        ImGui::SetCurrentContext (nullptr);
        c->imgui = nullptr;
    }

    if (c->display != nullptr)
    {
        glXMakeCurrent (c->display, None, nullptr);

        if (c->glx != nullptr)
            glXDestroyContext (c->display, c->glx);

        if (c->window != 0)
            XDestroyWindow (c->display, c->window);

        if (c->colormap != 0)
            XFreeColormap (c->display, c->colormap);

        XCloseDisplay (c->display);
        c->display = nullptr;
    }

    delete c;
}

bool imguiHostSetParent (ImGuiHostWindow *c, void *nativeHandle)
{
    if (c == nullptr || c->display == nullptr || nativeHandle == nullptr)
        return false;

    /* CLAP's linux API passes an X11 window id, not a pointer, so it arrives as an
       integer widened into a void pointer. */
    const Window host = (Window) (uintptr_t) nativeHandle;

    XReparentWindow (c->display, c->window, host, 0, 0);
    XFlush (c->display);
    c->parent = host;
    return true;
}

bool imguiHostSetTransient (ImGuiHostWindow *c, void *nativeHandle)
{
    if (c == nullptr || c->display == nullptr || nativeHandle == nullptr)
        return false;

    XSetTransientForHint (c->display, c->window, (Window) (uintptr_t) nativeHandle);
    XFlush (c->display);
    return true;
}

void imguiHostSetTitle (ImGuiHostWindow *c, const char *title)
{
    if (c == nullptr || c->display == nullptr || title == nullptr)
        return;

    XStoreName (c->display, c->window, title);
    XFlush (c->display);
}

void imguiHostSetSize (ImGuiHostWindow *c, uint32_t width, uint32_t height)
{
    if (c == nullptr || c->display == nullptr)
        return;

    c->w = width;
    c->h = height;
    XResizeWindow (c->display, c->window, width, height);
    XFlush (c->display);
}

void imguiHostSetScale (ImGuiHostWindow *c, double scale)
{
    if (c == nullptr)
        return;

    c->scale = scale;
}

void imguiHostShow (ImGuiHostWindow *c)
{
    if (c == nullptr || c->display == nullptr)
        return;

    XMapWindow (c->display, c->window);
    XFlush (c->display);
    c->visible = true;

    /*
        Repaint from a thread, because there is no WM_TIMER here.

        It sleeps and draws, which is the simplest thing that works. A host that offers
        CLAP's timer extension would be tidier, but a host is not obliged to offer one -
        the same reason the Windows side drives its own repaint.
    */
    if (! c->running.load())
    {
        c->running.store (true);
        c->ticker = std::thread ([c]()
        {
            while (c->running.load())
            {
                renderFrame (c);
                std::this_thread::sleep_for (std::chrono::milliseconds (16));
            }
        });
    }
}

void imguiHostHide (ImGuiHostWindow *c)
{
    if (c == nullptr || c->display == nullptr)
        return;

    c->visible = false;
    c->running.store (false);

    if (c->ticker.joinable())
        c->ticker.join();

    XUnmapWindow (c->display, c->window);
    XFlush (c->display);
}
