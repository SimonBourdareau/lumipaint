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
    The macOS side of imgui_host.h.

    Written but not run: there is no Mac here to test on, so treat this as a starting
    point rather than a finished port. The structure mirrors the Windows one so the two
    can be compared line for line, and the places most likely to need attention are
    marked.

    Build:
        cmake -B build -DLUMIPAINT_HOST_SOURCES=src/imgui_host_macos.mm
        cmake --build build

    Three things differ from Windows in ways that matter.

    Repaint is driven by an NSTimer on the main run loop rather than WM_TIMER. A CVDisplayLink
    would be smoother but runs on its own thread, and the GL context here is not set up
    for that.

    A plugin editor on macOS is an NSView the host adds to its own window - clap_gui's
    "cocoa" API passes an NSView, not a window - so there is no window to create in the
    embedded case. The floating case makes its own NSWindow.

    OpenGL is deprecated on macOS and gone on Apple silicon in the long run. It still
    works, and rewriting the renderer for Metal is a bigger job than this port; if it
    stops working, imgui's Metal backend is the replacement.
*/

#include "imgui_host.h"

#import <Cocoa/Cocoa.h>
#import <OpenGL/gl3.h>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_osx.h"

#include <cstring>

struct ImGuiHostWindow
{
    NSWindow *window;
    NSOpenGLView *view;
    NSTimer *timer;
    ImGuiContext *imgui;
    ImGuiHostRenderFn render;
    ImGuiHostClosedFn closed;
    void *userData;
    uint32_t w;
    uint32_t h;
    double scale;
    bool floating;
    bool created;
};

@interface LumiPaintView : NSOpenGLView
@property (nonatomic, assign) ImGuiHostWindow *host;
@end

@implementation LumiPaintView

- (BOOL) acceptsFirstResponder
{
    /*
        No.

        The equivalent decision to not consuming key messages on Windows: a plugin
        editor that becomes first responder takes the keyboard from the host, and then
        typing in the DAW's search box goes nowhere. Text fields inside the editor ask
        for focus individually when they are being edited.
    */
    return NO;
}

- (void) drawRect: (NSRect) dirty
{
    (void) dirty;

    if (self.host == nullptr || self.host->render == nullptr)
        return;

    [[self openGLContext] makeCurrentContext];

    ImGuiContext *previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext (self.host->imgui);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplOSX_NewFrame (self);
    ImGui::NewFrame();

    self.host->render (self.host->userData);

    ImGui::Render();
    glViewport (0, 0, (GLsizei) self.bounds.size.width,
                (GLsizei) self.bounds.size.height);
    glClearColor (0.06f, 0.06f, 0.08f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData (ImGui::GetDrawData());

    [[self openGLContext] flushBuffer];
    ImGui::SetCurrentContext (previous);
}

@end

@interface LumiPaintWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, assign) ImGuiHostWindow *host;
@end

@implementation LumiPaintWindowDelegate

- (BOOL) windowShouldClose: (NSWindow *) sender
{
    (void) sender;

    if (self.host != nullptr && self.host->closed != nullptr)
        self.host->closed (self.host->userData);

    return NO;   /* the host decides; hiding is its job, as on Windows */
}

@end

namespace {

void tick (ImGuiHostWindow *c)
{
    if (c != nullptr && c->view != nil)
        [c->view setNeedsDisplay: YES];
}

}

ImGuiHostWindow *imguiHostCreate (uint32_t width, uint32_t height, bool floating,
                                  ImGuiHostRenderFn render, ImGuiHostClosedFn closed,
                                  void *userData)
{
    ImGuiHostWindow *c = new ImGuiHostWindow();
    std::memset (c, 0, sizeof (*c));
    c->render = render;
    c->closed = closed;
    c->userData = userData;
    c->w = width;
    c->h = height;
    c->scale = 1.0;
    c->floating = floating;

    NSOpenGLPixelFormatAttribute attrs[] = {
        NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion3_2Core,
        NSOpenGLPFAColorSize, 24,
        NSOpenGLPFAAlphaSize, 8,
        NSOpenGLPFADoubleBuffer,
        NSOpenGLPFAAccelerated,
        0
    };

    NSOpenGLPixelFormat *fmt = [[NSOpenGLPixelFormat alloc] initWithAttributes: attrs];
    LumiPaintView *view = [[LumiPaintView alloc]
                            initWithFrame: NSMakeRect (0, 0, width, height)
                              pixelFormat: fmt];
    view.host = c;
    c->view = view;

    if (floating)
    {
        NSRect frame = NSMakeRect (0, 0, width, height);
        NSWindow *win = [[NSWindow alloc]
                          initWithContentRect: frame
                                    styleMask: (NSWindowStyleMaskTitled
                                              | NSWindowStyleMaskClosable)
                                      backing: NSBackingStoreBuffered
                                        defer: NO];

        /* Titled and closable only: no miniaturise, matching the Windows side, where a
           plugin editor that minimises leaves a stray dock entry and no way back. */
        [win setContentView: view];
        [win setReleasedWhenClosed: NO];

        LumiPaintWindowDelegate *del = [[LumiPaintWindowDelegate alloc] init];
        del.host = c;
        [win setDelegate: del];
        c->window = win;
    }

    [[view openGLContext] makeCurrentContext];

    IMGUI_CHECKVERSION();
    c->imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext (c->imgui);
    ImGui::GetIO().IniFilename = nullptr;

    /* The plugin loads Segoe UI on Windows; the nearest thing here. */
    ImGui::GetIO().Fonts->AddFontFromFileTTF (
        "/System/Library/Fonts/SFNS.ttf", 16.0f);

    if (ImGui::GetIO().Fonts->Fonts.empty())
        ImGui::GetIO().Fonts->AddFontDefault();

    ImGui_ImplOSX_Init (view);
    ImGui_ImplOpenGL3_Init ("#version 150");

    c->timer = [NSTimer scheduledTimerWithTimeInterval: 1.0 / 60.0
                                               repeats: YES
                                                 block: ^(NSTimer *t) {
                                                     (void) t;
                                                     tick (c);
                                                 }];

    c->created = true;
    return c;
}

void imguiHostDestroy (ImGuiHostWindow *c)
{
    if (c == nullptr)
        return;

    if (c->timer != nil)
    {
        [c->timer invalidate];
        c->timer = nil;
    }

    if (c->imgui != nullptr)
    {
        ImGui::SetCurrentContext (c->imgui);
        [[c->view openGLContext] makeCurrentContext];
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplOSX_Shutdown();
        ImGui::DestroyContext (c->imgui);
        ImGui::SetCurrentContext (nullptr);
        c->imgui = nullptr;
    }

    if (c->window != nil)
    {
        [c->window setDelegate: nil];
        [c->window close];
        c->window = nil;
    }

    c->view = nil;
    delete c;
}

bool imguiHostSetParent (ImGuiHostWindow *c, void *parentHandle)
{
    if (c == nullptr || parentHandle == nullptr)
        return false;

    /* clap_gui's cocoa API hands over an NSView to add ourselves to. */
    NSView *parent = (__bridge NSView *) parentHandle;
    [parent addSubview: c->view];
    return true;
}

bool imguiHostSetSize (ImGuiHostWindow *c, uint32_t width, uint32_t height)
{
    if (c == nullptr)
        return false;

    c->w = width;
    c->h = height;
    [c->view setFrameSize: NSMakeSize (width, height)];

    if (c->window != nil)
        [c->window setContentSize: NSMakeSize (width, height)];

    return true;
}

bool imguiHostSetScale (ImGuiHostWindow *c, double scale)
{
    if (c == nullptr)
        return false;

    c->scale = scale;
    return true;
}

void imguiHostShow (ImGuiHostWindow *c)
{
    if (c == nullptr)
        return;

    if (c->window != nil)
    {
        /* Ordered in without taking focus, as on Windows: opening the editor should not
           pull the keyboard away from the host. */
        [c->window orderFront: nil];
    }

    [c->view setHidden: NO];
}

void imguiHostHide (ImGuiHostWindow *c)
{
    if (c == nullptr)
        return;

    [c->view setHidden: YES];

    if (c->window != nil)
        [c->window orderOut: nil];
}

bool imguiHostSetTransient (ImGuiHostWindow *c, void *parentHandle)
{
    if (c == nullptr || c->window == nil || parentHandle == nullptr)
        return false;

    /* Keeps the editor above the host's window without making it a child of it, so
       closing the host does not leave it orphaned on screen. */
    NSWindow *parent = (__bridge NSWindow *) parentHandle;
    [parent addChildWindow: c->window ordered: NSWindowAbove];
    return true;
}

void imguiHostSetTitle (ImGuiHostWindow *c, const char *title)
{
    if (c == nullptr || c->window == nil || title == nullptr)
        return;

    [c->window setTitle: [NSString stringWithUTF8String: title]];
}

void *imguiHostNativeHandle (ImGuiHostWindow *c)
{
    return c != nullptr ? (__bridge void *) c->view : nullptr;
}
