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

#include "imgui_host.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl3.h"

#include <windows.h>
#include <GL/gl.h>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler (HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

struct ImGuiHostWindow
{
    HWND hwnd;
    HDC hdc;
    HGLRC hglrc;
    ImGuiContext *imgui;
    ImGuiHostRenderFn render;
    ImGuiHostClosedFn closed;
    void *userData;
    uint32_t w;
    uint32_t h;
    float scale;
    int timerFast;
    bool created;
    bool rendering;
};

namespace {

const wchar_t *kClassName = L"LumiPaintImGuiHost";
int g_windowCount = 0;

/* The DLL's own HINSTANCE, not the host executable's. Registering the class
 * against GetModuleHandle(NULL) ties it to a module that may since have been
 * unloaded, and gives Windows no reason to keep this library loaded at all. */
HINSTANCE selfModule()
{
    static HMODULE h = nullptr;

    if (h == nullptr)
        GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                          | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR) &kClassName, &h);

    return (HINSTANCE) h;
}

void releaseInput (HWND hwnd)
{
    if (GetCapture() == hwnd)
        ReleaseCapture();

    if (GetFocus() == hwnd)
        SetFocus (nullptr);
}

void renderFrame (ImGuiHostWindow *c)
{
    if (c == nullptr || ! c->created || c->imgui == nullptr
        || c->hdc == nullptr || c->hglrc == nullptr || c->hwnd == nullptr)
        return;

    /*
        Never draw a frame while a frame is already open.

        Saving a map opens a modal file dialog, which runs its own message loop - so the
        8 ms repaint timer keeps firing, WM_TIMER arrives, and this is called again with
        an ImGui frame half built. ImGui cannot survive that, and the plugin locks up.

        Any modal thing does it: the dialog is simply the first one this plugin had.
        Refusing to re-enter costs a dropped frame while the dialog is up, which is
        exactly what should happen.
    */
    if (c->rendering)
        return;

    c->rendering = true;

    /* Nothing to draw on: hidden, minimised, or collapsed to nothing. Rendering
     * anyway is not merely wasteful - NewFrame drives the backend's mouse
     * handling, which calls SetCursor. Doing that at 120 Hz while the user is in
     * another application is how the pointer goes missing. */
    if (! IsWindowVisible (c->hwnd) || IsIconic (c->hwnd))
    {
        c->rendering = false;
        return;
    }

    RECT box;
    GetClientRect (c->hwnd, &box);

    if (box.right - box.left < 8 || box.bottom - box.top < 8)
    {
        c->rendering = false;
        return;
    }

    ImGui::SetCurrentContext (c->imgui);

    {
        ImGuiIO &io = ImGui::GetIO();
        HWND root = GetAncestor (c->hwnd, GA_ROOT);
        HWND fg = GetForegroundWindow();

        if (fg == c->hwnd || (root != nullptr && fg == root))
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        else
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    }

    /* Put back whatever context the caller had current. The host may be drawing
     * with OpenGL on this thread too, and leaving ours current makes its
     * rendering misbehave in ways that look like the host itself is broken. */
    HGLRC prevRc = wglGetCurrentContext();
    HDC prevDc = wglGetCurrentDC();

    if (! wglMakeCurrent (c->hdc, c->hglrc))
    {
        c->rendering = false;
        return;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (c->render != nullptr)
        c->render (c->userData);

    ImGui::Render();

    const ImGuiIO &io = ImGui::GetIO();
    glViewport (0, 0, (int) io.DisplaySize.x, (int) io.DisplaySize.y);
    glClearColor (0.055f, 0.055f, 0.062f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData (ImGui::GetDrawData());
    SwapBuffers (c->hdc);

    wglMakeCurrent (prevDc, prevRc);
    c->rendering = false;
}

VOID CALLBACK timerProc (HWND h, UINT msg, UINT_PTR id, DWORD tick)
{
    (void) msg;
    (void) id;
    (void) tick;

    ImGuiHostWindow *c = (ImGuiHostWindow *) GetWindowLongPtrW (h, GWLP_USERDATA);

    if (c == nullptr || ! c->created)
    {
        KillTimer (h, 1);
        return;
    }

    /* A hidden window does not need repainting, and the 1 ms system timer
     * resolution should not stay raised for a window nobody can see. */
    if (! IsWindowVisible (h) || IsIconic (h))
    {
        if (c->timerFast)
        {
            timeEndPeriod (1);
            c->timerFast = 0;
        }

        if (GetFocus() == h)
            releaseInput (h);

        return;
    }

    if (! c->timerFast)
    {
        timeBeginPeriod (1);
        c->timerFast = 1;
    }

    renderFrame (c);
}

/* WM_TIMER is quantised to the system tick, which idles around 15.6 ms, so asking for
   8 ms achieves nothing on its own. Raise the resolution for as long as the window is
   up. Only once: Windows counts these. Safe to call repeatedly - any existing timer is
   killed first. */
void startTimer (ImGuiHostWindow *c)
{
    if (c == nullptr || c->hwnd == nullptr)
        return;

    if (! c->timerFast)
    {
        timeBeginPeriod (1);
        c->timerFast = 1;
    }

    KillTimer (c->hwnd, 1);
    SetTimer (c->hwnd, 1, 8, timerProc);
}

LRESULT CALLBACK wndProc (HWND h, UINT m, WPARAM w, LPARAM l)
{
    ImGuiHostWindow *c = (ImGuiHostWindow *) GetWindowLongPtrW (h, GWLP_USERDATA);

    /*
        Keyboard messages go to the host unless a field here is being typed into.

        The backend's handler claims key and character messages whenever this window has
        focus, and returning early on that swallowed them - so with the editor open the
        DAW stopped seeing typing at all, in its search box, its browser, anywhere. A
        plugin window has no business taking the keyboard just for existing.

        Text input is the only case where it should: a value being typed into a field.
        Everything else - shortcuts, transport keys, search - belongs to the host, so it
        is forwarded to the parent window rather than consumed.
    */
    const bool isKeyMessage = m == WM_KEYDOWN || m == WM_KEYUP || m == WM_CHAR
                           || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP || m == WM_SYSCHAR
                           || m == WM_DEADCHAR || m == WM_UNICHAR;

    bool wantsText = false;

    if (c != nullptr && c->imgui != nullptr)
    {
        ImGuiContext *previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext (c->imgui);
        wantsText = ImGui::GetIO().WantTextInput;
        ImGui::SetCurrentContext (previous);
    }

    if (isKeyMessage && ! wantsText)
    {
        HWND parent = GetParent (h);

        if (parent != nullptr)
            return SendMessageW (parent, m, w, l);

        return DefWindowProcW (h, m, w, l);
    }

    if (c != nullptr && c->imgui != nullptr)
    {
        ImGuiContext *previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext (c->imgui);
        const LRESULT handled = ImGui_ImplWin32_WndProcHandler (h, m, w, l);
        ImGui::SetCurrentContext (previous);

        if (handled != 0)
            return handled;
    }

    switch (m)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        /*
            Paint when Windows asks, as well as on the timer.

            There was no handler here at all: erasing was suppressed and painting was
            left entirely to a timer started when the host called show. A CLAP host
            calls show. Wrapped as a VST3 the host attaches the view and makes it
            visible itself, so nothing started the timer and nothing ever drew - a grey
            rectangle, which is the window's uninitialised pixels.

            Handling WM_PAINT means the editor draws whenever Windows says it should,
            whatever the host did or did not call. The timer still drives the animation;
            this makes the first frame independent of it.
        */
        if (c != nullptr)
        {
            PAINTSTRUCT ps;
            BeginPaint (h, &ps);
            renderFrame (c);
            EndPaint (h, &ps);

            /* Belt and braces: if the host never called show, the timer is not running
               and the editor would be a still image. Starting it here costs nothing
               when it is already going. */
            startTimer (c);
            return 0;
        }
        break;

    case WM_SHOWWINDOW:
        if (c != nullptr && w != 0)
        {
            startTimer (c);
            renderFrame (c);
        }
        break;

    case WM_SIZE:
        if (c != nullptr && w != SIZE_MINIMIZED)
        {
            c->w = (uint32_t) LOWORD (l);
            c->h = (uint32_t) HIWORD (l);
            renderFrame (c);
        }
        return 0;

    case WM_CLOSE:
        /* Hide it here as well as telling the host. Reporting "closed" and
         * waiting leaves the window on screen if the host does not act, which
         * looks exactly like an X button that does nothing. */
        if (c != nullptr)
        {
            KillTimer (h, 1);
            releaseInput (h);
            ShowWindow (h, SW_HIDE);

            if (c->closed != nullptr)
                c->closed (c->userData);
        }
        return 0;

    default:
        break;
    }

    return DefWindowProcW (h, m, w, l);
}

bool glInit (ImGuiHostWindow *c, HWND hwnd)
{
    c->hdc = GetDC (hwnd);

    if (c->hdc == nullptr)
        return false;

    PIXELFORMATDESCRIPTOR pfd;
    ZeroMemory (&pfd, sizeof (pfd));
    pfd.nSize = sizeof (pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;

    const int pf = ChoosePixelFormat (c->hdc, &pfd);

    if (pf == 0 || ! SetPixelFormat (c->hdc, pf, &pfd))
        return false;

    c->hglrc = wglCreateContext (c->hdc);

    if (c->hglrc == nullptr)
        return false;

    return wglMakeCurrent (c->hdc, c->hglrc) != FALSE;
}

void glFree (ImGuiHostWindow *c)
{
    if (c->hglrc != nullptr)
    {
        wglMakeCurrent (nullptr, nullptr);
        wglDeleteContext (c->hglrc);
        c->hglrc = nullptr;
    }

    if (c->hdc != nullptr && c->hwnd != nullptr)
    {
        ReleaseDC (c->hwnd, c->hdc);
        c->hdc = nullptr;
    }
}

}

ImGuiHostWindow *imguiHostCreate (uint32_t width, uint32_t height, bool floating,
                                  ImGuiHostRenderFn render, ImGuiHostClosedFn closed,
                                  void *userData)
{
    ImGuiHostWindow *c = new ImGuiHostWindow();
    c->hwnd = nullptr;
    c->hdc = nullptr;
    c->hglrc = nullptr;
    c->imgui = nullptr;
    c->render = render;
    c->closed = closed;
    c->userData = userData;
    c->w = width;
    c->h = height;
    c->scale = 1.0f;
    c->timerFast = 0;
    c->created = false;
    c->rendering = false;

    WNDCLASSEXW wc;
    ZeroMemory (&wc, sizeof (wc));
    wc.cbSize = sizeof (wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = selfModule();
    wc.hCursor = LoadCursor (nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW (&wc);

    /*
        A window with no parent yet cannot be WS_CHILD.

        CreateWindowExW refuses it outright - error 1406, ERROR_TLW_WITH_WSCHILD,
        "cannot create a top-level child window" - because a child window must be
        created with a parent, and there is none until the host calls setParent.

        This never showed up in the CLAP path because Bitwig asks for a floating
        editor, which gets WS_OVERLAPPEDWINDOW and succeeds. The VST3 wrapper asks for
        an embedded one, creation failed, and every later call had nothing to work
        with. The grey was the wrapper's own empty container.

        So it is created as a popup, and setParent converts it to a child - which is
        what setParent already did anyway, since it rewrites the style and reparents in
        the same breath.

        Floating still gets a caption and a close button but no minimise or maximise: a
        plugin editor that minimises leaves a stray taskbar button and no way back.
    */
    const DWORD style = floating ? (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME
                                    & ~WS_MINIMIZEBOX & ~WS_MAXIMIZEBOX)
                                 : WS_POPUP;

    c->hwnd = CreateWindowExW (0, kClassName, L"LumiPaint", style,
                               CW_USEDEFAULT, CW_USEDEFAULT, (int) width, (int) height,
                               nullptr, nullptr, wc.hInstance, nullptr);

    if (c->hwnd == nullptr)
    {
        delete c;
        return nullptr;
    }

    ++g_windowCount;
    SetWindowLongPtrW (c->hwnd, GWLP_USERDATA, (LONG_PTR) c);

    if (! glInit (c, c->hwnd))
    {
        glFree (c);
        DestroyWindow (c->hwnd);
        delete c;
        return nullptr;
    }

    IMGUI_CHECKVERSION();
    c->imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext (c->imgui);
    ImGui::GetIO().IniFilename = nullptr;

    /*
        A real font.

        ImGui's built-in face is a 13-pixel bitmap and looks like one. Segoe UI has
        shipped with every Windows since 7 and Calibri since Office 2007, so one of
        them is always there; Tahoma is the last resort on a stripped install. If none
        loads, AddFontDefault keeps the panel readable rather than leaving it with no
        font at all.

        Loaded here, before the backends are initialised. Adding a font after
        ImGui_ImplOpenGL3_Init has built its atlas leaves the texture stale and the new
        face never appears - which is a quiet way to think you have changed the font
        and be looking at the old one.
    */
    {
        char winDir[MAX_PATH] = { 0 };
        GetWindowsDirectoryA (winDir, MAX_PATH);

        const char *faces[] = { "\\Fonts\\segoeui.ttf",
                                "\\Fonts\\calibri.ttf",
                                "\\Fonts\\tahoma.ttf" };
        bool loaded = false;

        for (const char *face : faces)
        {
            const std::string path = std::string (winDir) + face;

            if (ImGui::GetIO().Fonts->AddFontFromFileTTF (path.c_str(), 16.0f) != nullptr)
            {
                loaded = true;
                break;
            }
        }

        if (! loaded)
            ImGui::GetIO().Fonts->AddFontDefault();
    }

    ImGui::StyleColorsDark();

    ImGuiStyle &style2 = ImGui::GetStyle();
    style2.WindowRounding = 0.0f;
    style2.FrameRounding = 3.0f;
    style2.GrabRounding = 3.0f;
    style2.WindowPadding = ImVec2 (12.0f, 10.0f);
    style2.ItemSpacing = ImVec2 (8.0f, 7.0f);
    style2.Colors[ImGuiCol_WindowBg] = ImVec4 (0.09f, 0.09f, 0.10f, 1.00f);
    style2.Colors[ImGuiCol_ChildBg] = ImVec4 (0.06f, 0.06f, 0.07f, 1.00f);
    style2.Colors[ImGuiCol_FrameBg] = ImVec4 (0.17f, 0.17f, 0.19f, 1.00f);
    style2.Colors[ImGuiCol_Button] = ImVec4 (0.21f, 0.22f, 0.25f, 1.00f);
    style2.Colors[ImGuiCol_ButtonHovered] = ImVec4 (0.30f, 0.32f, 0.38f, 1.00f);
    style2.Colors[ImGuiCol_SliderGrab] = ImVec4 (0.45f, 0.60f, 0.85f, 1.00f);

    ImGui_ImplWin32_InitForOpenGL (c->hwnd);
    ImGui_ImplOpenGL3_Init ("#version 130");

    c->created = true;
    return c;
}

void imguiHostDestroy (ImGuiHostWindow *c)
{
    if (c == nullptr)
        return;

    if (c->hwnd != nullptr)
    {
        KillTimer (c->hwnd, 1);
        releaseInput (c->hwnd);
    }

    if (c->timerFast)
    {
        timeEndPeriod (1);
        c->timerFast = 0;
    }

    if (c->imgui != nullptr)
    {
        ImGui::SetCurrentContext (c->imgui);
        wglMakeCurrent (c->hdc, c->hglrc);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext (c->imgui);
        ImGui::SetCurrentContext (nullptr);
        c->imgui = nullptr;
    }

    glFree (c);

    if (c->hwnd != nullptr)
    {
        SetWindowLongPtrW (c->hwnd, GWLP_USERDATA, 0);
        DestroyWindow (c->hwnd);
        c->hwnd = nullptr;
        --g_windowCount;
    }

    delete c;

    if (g_windowCount <= 0)
    {
        UnregisterClassW (kClassName, selfModule());
        g_windowCount = 0;
    }
}

bool imguiHostSetParent (ImGuiHostWindow *c, void *nativeHandle)
{
    if (c == nullptr || c->hwnd == nullptr || nativeHandle == nullptr)
        return false;

    SetParent (c->hwnd, (HWND) nativeHandle);
    SetWindowLongPtrW (c->hwnd, GWL_STYLE, WS_CHILD | WS_VISIBLE);
    SetWindowPos (c->hwnd, nullptr, 0, 0, (int) c->w, (int) c->h,
                  SWP_NOZORDER | SWP_SHOWWINDOW);

    /*
        Start painting here too, not only in show.

        The repaint timer used to start in imguiHostShow alone, which assumes the host
        parents the view and then asks for it to be shown. A CLAP host does. Wrapped as
        a VST3 the view is attached and made visible by the host itself, show is never
        called, no timer is ever set, and the editor is a grey rectangle - a window that
        exists and is never painted.

        Starting it in both places costs nothing: startTimer kills any existing timer
        first, so being called twice is the same as being called once.
    */
    startTimer (c);
    return true;
}

bool imguiHostSetTransient (ImGuiHostWindow *c, void *nativeHandle)
{
    if (c == nullptr || c->hwnd == nullptr || nativeHandle == nullptr)
        return false;

    SetWindowLongPtrW (c->hwnd, GWLP_HWNDPARENT, (LONG_PTR) nativeHandle);
    return true;
}

void imguiHostSetTitle (ImGuiHostWindow *c, const char *title)
{
    if (c == nullptr || c->hwnd == nullptr || title == nullptr)
        return;

    wchar_t wide[128];
    MultiByteToWideChar (CP_UTF8, 0, title, -1, wide, 128);
    SetWindowTextW (c->hwnd, wide);
}

void imguiHostSetSize (ImGuiHostWindow *c, uint32_t width, uint32_t height)
{
    if (c == nullptr)
        return;

    c->w = width;
    c->h = height;

    if (c->hwnd != nullptr)
        SetWindowPos (c->hwnd, nullptr, 0, 0, (int) width, (int) height,
                      SWP_NOMOVE | SWP_NOZORDER);
}

void imguiHostSetScale (ImGuiHostWindow *c, double scale)
{
    if (c == nullptr || c->imgui == nullptr)
        return;

    c->scale = (float) scale;
}

void imguiHostShow (ImGuiHostWindow *c)
{
    if (c == nullptr || c->hwnd == nullptr)
        return;

    /* Shown without activating, so opening the editor does not pull the keyboard away
       from whatever the host had focused. Clicking in it still gives it focus. */
    ShowWindow (c->hwnd, SW_SHOWNOACTIVATE);
    startTimer (c);
    renderFrame (c);
}

void imguiHostHide (ImGuiHostWindow *c)
{
    if (c == nullptr || c->hwnd == nullptr)
        return;

    KillTimer (c->hwnd, 1);

    if (c->timerFast)
    {
        timeEndPeriod (1);
        c->timerFast = 0;
    }

    releaseInput (c->hwnd);
    ShowWindow (c->hwnd, SW_HIDE);
}
