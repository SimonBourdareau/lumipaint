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

    Compiles and links, but has never been run: there is no Linux here to test on, and
    no keyboard to test it against. It mirrors imgui_host_win32.cpp closely enough to
    compare line by line.

    Build:
        cmake -B build -DCMAKE_BUILD_TYPE=Release
        cmake --build build

    X11 rather than Wayland, deliberately. CLAP's linux API hands over an X11 window id,
    plugin embedding under Wayland is still unresolved across hosts, and most Linux DAW
    users are on X11 or XWayland anyway. Under XWayland this works; under a pure Wayland
    session it will not, and there is no small fix for that.

    Four things differ from Windows in ways that matter.

    There is no WM_TIMER, so repaint runs on a thread that sleeps and draws. CLAP's
    timer extension would be tidier but a host is not obliged to provide one - the same
    reason the Windows side does not use it.

    Because that thread draws, every entry point that touches the window, the GL context
    or the ImGui context has to take a lock. One ImGui context being entered from two
    threads is not something ImGui survives, and the failure is intermittent corruption
    rather than a clean crash.

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
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

struct ImGuiHostWindow
{
    Display *display = nullptr;
    Window window = 0;
    Window parent = 0;
    GLXContext glx = nullptr;
    Colormap colormap = 0;
    Atom deleteMessage = 0;
    ImGuiContext *imgui = nullptr;
    ImGuiHostRenderFn render = nullptr;
    ImGuiHostClosedFn closed = nullptr;
    void *userData = nullptr;
    uint32_t w = 0;
    uint32_t h = 0;
    double scale = 1.0;
    bool floating = false;
    bool visible = false;
    bool rendering = false;
    std::chrono::steady_clock::time_point lastFrame;
    std::atomic<bool> running { false };

    /* Held by the ticker thread while it draws, and by every entry point that touches
       the window or either context. */
    std::mutex lock;
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
    static std::once_flag once;
    std::call_once (once, [] () { XInitThreads(); });
}

ImGuiKey keyFromSym (KeySym sym)
{
    switch (sym)
    {
    case XK_Tab:        return ImGuiKey_Tab;
    case XK_Left:       return ImGuiKey_LeftArrow;
    case XK_Right:      return ImGuiKey_RightArrow;
    case XK_Up:         return ImGuiKey_UpArrow;
    case XK_Down:       return ImGuiKey_DownArrow;
    case XK_Prior:      return ImGuiKey_PageUp;
    case XK_Next:       return ImGuiKey_PageDown;
    case XK_Home:       return ImGuiKey_Home;
    case XK_End:        return ImGuiKey_End;
    case XK_Insert:     return ImGuiKey_Insert;
    case XK_Delete:     return ImGuiKey_Delete;
    case XK_BackSpace:  return ImGuiKey_Backspace;
    case XK_space:      return ImGuiKey_Space;
    case XK_Return:     return ImGuiKey_Enter;
    case XK_KP_Enter:   return ImGuiKey_KeypadEnter;
    case XK_Escape:     return ImGuiKey_Escape;
    case XK_Shift_L:
    case XK_Shift_R:    return ImGuiKey_LeftShift;
    case XK_Control_L:
    case XK_Control_R:  return ImGuiKey_LeftCtrl;
    case XK_Alt_L:
    case XK_Alt_R:      return ImGuiKey_LeftAlt;
    case XK_Super_L:
    case XK_Super_R:    return ImGuiKey_LeftSuper;
    case XK_a:          return ImGuiKey_A;
    case XK_c:          return ImGuiKey_C;
    case XK_v:          return ImGuiKey_V;
    case XK_x:          return ImGuiKey_X;
    case XK_y:          return ImGuiKey_Y;
    case XK_z:          return ImGuiKey_Z;
    default:            return ImGuiKey_None;
    }
}

/*
    Pointer, wheel and keyboard, since there is no imgui backend for raw Xlib.

    The pointer is polled rather than tracked through motion events, which is cheaper
    and cannot fall behind. Keys have to come from events, and characters from
    XLookupString so that a layout other than US produces what the user actually typed.
*/
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
        io.AddKeyEvent (ImGuiMod_Shift, (mask & ShiftMask) != 0);
        io.AddKeyEvent (ImGuiMod_Ctrl, (mask & ControlMask) != 0);
        io.AddKeyEvent (ImGuiMod_Alt, (mask & Mod1Mask) != 0);
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
            /* Wheel arrives as buttons four and five, and as a press and a release
               each; only the press is counted or every notch would move two steps. */
            if (event.xbutton.button == 4)
                io.AddMouseWheelEvent (0.0f, 1.0f);
            else if (event.xbutton.button == 5)
                io.AddMouseWheelEvent (0.0f, -1.0f);
            else if (event.xbutton.button == 6)
                io.AddMouseWheelEvent (-1.0f, 0.0f);
            else if (event.xbutton.button == 7)
                io.AddMouseWheelEvent (1.0f, 0.0f);

            break;

        case KeyPress:
        case KeyRelease:
        {
            const bool down = event.type == KeyPress;
            char text[32] = { 0 };
            KeySym sym = 0;
            const int length = XLookupString (&event.xkey, text, sizeof (text) - 1,
                                              &sym, nullptr);

            const ImGuiKey key = keyFromSym (sym);

            if (key != ImGuiKey_None)
                io.AddKeyEvent (key, down);

            /*
                Characters only while a field is being typed into.

                The Windows side forwards key messages to the host unless
                WantTextInput is set, for the same reason: an editor that swallows
                typing means the DAW's search box stops working while it is open.
                Here nothing is swallowed - X delivers to whichever window has focus -
                but feeding characters in regardless would let a stray keypress land in
                a numeric field that merely happens to be hovered.
            */
            if (down && io.WantTextInput)
                for (int i = 0; i < length; ++i)
                    if ((unsigned char) text[i] >= 32)
                        io.AddInputCharacter ((unsigned int) (unsigned char) text[i]);

            break;
        }

        case ClientMessage:
            if ((Atom) event.xclient.data.l[0] == c->deleteMessage)
            {
                /*
                    Unmapped here as well as reported, exactly as the Windows side hides
                    on WM_CLOSE. Telling the host and waiting leaves the window on
                    screen if the host does not act, which looks like a close button
                    that does nothing.
                */
                c->visible = false;
                XUnmapWindow (c->display, c->window);
                XFlush (c->display);

                if (c->closed != nullptr)
                    c->closed (c->userData);
            }

            break;

        default:
            break;
        }
    }
}

/*
    One frame. Called only from the ticker thread, with the lock already held.

    The re-entrancy guard is the same one the Windows side needs: a file dialog here is
    zenity or kdialog run through popen from inside the render callback, so the callback
    does not return until the dialog closes. It cannot re-enter on this thread, but the
    guard costs a comparison and means a second caller can never open a second frame.
*/
void renderFrame (ImGuiHostWindow *c)
{
    if (c == nullptr || c->imgui == nullptr || ! c->visible || c->rendering)
        return;

    if (c->w < 8 || c->h < 8)
        return;

    c->rendering = true;

    ImGuiContext *previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext (c->imgui);

    if (! glXMakeCurrent (c->display, c->window, c->glx))
    {
        ImGui::SetCurrentContext (previous);
        c->rendering = false;
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2 ((float) c->w, (float) c->h);

    /* Measured rather than assumed. A fixed 1/60 makes every animation in the plugin
       run at whatever rate this thread happens to achieve, so a loaded machine slows
       the ripples down instead of dropping frames. */
    const auto now = std::chrono::steady_clock::now();
    double delta = std::chrono::duration<double> (now - c->lastFrame).count();
    c->lastFrame = now;

    if (delta <= 0.0 || delta > 0.5)
        delta = 1.0 / 60.0;

    io.DeltaTime = (float) delta;

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
    c->rendering = false;
}

void startTicker (ImGuiHostWindow *c)
{
    if (c->running.load())
        return;

    c->running.store (true);
    c->lastFrame = std::chrono::steady_clock::now();

    c->ticker = std::thread ([c] ()
    {
        while (c->running.load())
        {
            {
                std::lock_guard<std::mutex> held (c->lock);
                renderFrame (c);
            }

            std::this_thread::sleep_for (std::chrono::milliseconds (16));
        }
    });
}

/*
    Stopped from outside the ticker thread only.

    Joining is what makes the entry points safe: once this returns, nothing else is
    touching the display or either context, so teardown can proceed without racing a
    frame that is halfway through.
*/
void stopTicker (ImGuiHostWindow *c)
{
    c->running.store (false);

    if (c->ticker.joinable())
        c->ticker.join();
}

}

ImGuiHostWindow *imguiHostCreate (uint32_t width, uint32_t height, bool floating,
                                  ImGuiHostRenderFn render, ImGuiHostClosedFn closed,
                                  void *userData)
{
    ensureThreads();

    /*
        Value-initialised by its member initialisers rather than memset.

        The previous memset spanned sizeof(ImGuiHostWindow) - sizeof(std::thread),
        which assumed the thread was the last member, wrote over a std::atomic, and
        would have written over the mutex added since. Default member initialisers do
        the same job and cannot be invalidated by reordering the struct.
    */
    ImGuiHostWindow *c = new ImGuiHostWindow();
    c->render = render;
    c->closed = closed;
    c->userData = userData;
    c->w = width;
    c->h = height;
    c->floating = floating;

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
                   | ButtonReleaseMask | PointerMotionMask
                   | KeyPressMask | KeyReleaseMask | FocusChangeMask;

    c->window = XCreateWindow (c->display, c->parent, 0, 0, width, height, 0,
                               visual->depth, InputOutput, visual->visual,
                               CWColormap | CWEventMask, &swa);

    if (c->window == 0)
    {
        XFree (visual);
        XFreeColormap (c->display, c->colormap);
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
        XFreeColormap (c->display, c->colormap);
        XCloseDisplay (c->display);
        delete c;
        return nullptr;
    }

    glXMakeCurrent (c->display, c->window, c->glx);

    IMGUI_CHECKVERSION();
    c->imgui = ImGui::CreateContext();

    ImGuiContext *previous = ImGui::GetCurrentContext();
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
            "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"
        };

        bool loaded = false;

        for (const char *face : faces)
        {
            FILE *probe = std::fopen (face, "rb");

            if (probe == nullptr)
                continue;

            std::fclose (probe);

            if (ImGui::GetIO().Fonts->AddFontFromFileTTF (face, 16.0f) != nullptr)
            {
                loaded = true;
                break;
            }
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
    ImGui::SetCurrentContext (previous);

    glXMakeCurrent (c->display, None, nullptr);
    return c;
}

void imguiHostDestroy (ImGuiHostWindow *c)
{
    if (c == nullptr)
        return;

    stopTicker (c);

    if (c->imgui != nullptr)
    {
        ImGuiContext *previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext (c->imgui);
        glXMakeCurrent (c->display, c->window, c->glx);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext (c->imgui);
        ImGui::SetCurrentContext (previous == c->imgui ? nullptr : previous);
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

    std::lock_guard<std::mutex> held (c->lock);

    /* CLAP's linux API passes an X11 window id, not a pointer, so it arrives as an
       integer widened into a void pointer. */
    const Window host = (Window) (uintptr_t) nativeHandle;

    XReparentWindow (c->display, c->window, host, 0, 0);
    XResizeWindow (c->display, c->window, c->w, c->h);
    XMapWindow (c->display, c->window);
    XFlush (c->display);
    c->parent = host;
    c->visible = true;

    /*
        Drawing from here, not only from show.

        The Windows side starts its repaint timer in setParent as well as in show,
        because a host that attaches the view and makes it visible itself never calls
        show - and the editor is then a window that exists and is never painted. The
        VST3 wrapper does exactly that, on every platform.
    */
    startTicker (c);
    return true;
}

bool imguiHostSetTransient (ImGuiHostWindow *c, void *nativeHandle)
{
    if (c == nullptr || c->display == nullptr || nativeHandle == nullptr)
        return false;

    std::lock_guard<std::mutex> held (c->lock);
    XSetTransientForHint (c->display, c->window, (Window) (uintptr_t) nativeHandle);
    XFlush (c->display);
    return true;
}

void imguiHostSetTitle (ImGuiHostWindow *c, const char *title)
{
    if (c == nullptr || c->display == nullptr || title == nullptr)
        return;

    std::lock_guard<std::mutex> held (c->lock);
    XStoreName (c->display, c->window, title);
    XFlush (c->display);
}

void imguiHostSetSize (ImGuiHostWindow *c, uint32_t width, uint32_t height)
{
    if (c == nullptr || c->display == nullptr)
        return;

    std::lock_guard<std::mutex> held (c->lock);
    c->w = width;
    c->h = height;
    XResizeWindow (c->display, c->window, width, height);
    XFlush (c->display);
}

void imguiHostSetScale (ImGuiHostWindow *c, double scale)
{
    if (c == nullptr)
        return;

    std::lock_guard<std::mutex> held (c->lock);
    c->scale = scale;
}

void imguiHostShow (ImGuiHostWindow *c)
{
    if (c == nullptr || c->display == nullptr)
        return;

    {
        std::lock_guard<std::mutex> held (c->lock);
        XMapWindow (c->display, c->window);
        XFlush (c->display);
        c->visible = true;
    }

    /*
        Repaint from a thread, because there is no WM_TIMER here.

        It sleeps and draws, which is the simplest thing that works. A host that offers
        CLAP's timer extension would be tidier, but a host is not obliged to offer one -
        the same reason the Windows side drives its own repaint.
    */
    startTicker (c);
}

void imguiHostHide (ImGuiHostWindow *c)
{
    if (c == nullptr || c->display == nullptr)
        return;

    {
        std::lock_guard<std::mutex> held (c->lock);
        c->visible = false;
    }

    /*
        Stopped before unmapping, and outside the lock.

        stopTicker joins, and the ticker takes the same lock every frame - so joining
        while holding it is a deadlock on the first iteration. The visible flag is set
        under the lock, the join happens after it is dropped.
    */
    stopTicker (c);

    std::lock_guard<std::mutex> held (c->lock);
    XUnmapWindow (c->display, c->window);
    XFlush (c->display);
}
