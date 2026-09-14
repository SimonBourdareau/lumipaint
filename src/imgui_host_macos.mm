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

    Compiles and links, but has never been run: there is no Mac here to test on. The
    structure mirrors imgui_host_win32.cpp closely enough that the two can be read side
    by side, and every place where the platform forces a different answer is marked.

    Build:
        cmake -B build -DLUMIPAINT_HOST_SOURCES=src/imgui_host_macos.mm
        cmake --build build

    Four things differ from Windows in ways that matter.

    Repaint is driven by an NSTimer on the main run loop rather than WM_TIMER. A
    CVDisplayLink would be smoother but runs on its own thread, and the GL context here
    is not set up for that.

    A plugin editor on macOS is an NSView the host adds to its own window - clap_gui's
    "cocoa" API passes an NSView, not a window - so there is no window to create in the
    embedded case. The floating case makes its own NSWindow.

    Drawing happens in drawRect rather than being pushed from the timer, which is the
    equivalent of the WM_PAINT handler on Windows: the first frame does not depend on
    the host having called show.

    OpenGL is deprecated on macOS. It still works, and rewriting the renderer for Metal
    is a bigger job than this port; if it stops working, imgui's Metal backend is the
    replacement.
*/

#include "imgui_host.h"

#import <Cocoa/Cocoa.h>
#import <OpenGL/gl3.h>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_osx.h"

@class LumiPaintWindowDelegate;

struct ImGuiHostWindow
{
    NSWindow *window;
    NSOpenGLView *view;
    NSTimer *timer;

    /*
        The delegate is held here because NSWindow does not hold it.

        setDelegate: is a weak reference. A delegate created locally and handed over was
        released at the end of the function that made it, and the close button then went
        through a dangling pointer - a crash on closing the editor rather than on
        opening it, which is the kind that gets blamed on the host.
    */
    LumiPaintWindowDelegate *delegate;

    ImGuiContext *imgui;
    ImGuiHostRenderFn render;
    ImGuiHostClosedFn closed;
    void *userData;
    uint32_t w;
    uint32_t h;
    double scale;
    bool floating;
    bool created;
    bool visible;

    /*
        Never draw a frame while a frame is already open.

        Saving a map runs NSSavePanel modally, and a modal panel spins its own run loop -
        so the repaint timer keeps firing on the main thread, drawRect is entered again
        with an ImGui frame half built, and ImGui cannot survive that. The same guard as
        the Windows side, for the same reason, reached by a different route.
    */
    bool rendering;
};

@interface LumiPaintView : NSOpenGLView
@property (nonatomic, assign) ImGuiHostWindow *host;
@end

@implementation LumiPaintView

/*
    Focus only while a field is being typed into.

    The equivalent decision to forwarding key messages to the parent on Windows. A view
    that is always willing to become first responder takes the keyboard from the host
    for as long as the editor is open, and then typing in the DAW's search box goes
    nowhere. Answering from WantTextInput means a click lands in the host unless ImGui
    has an active text field, which is the only case that needs it.
*/
- (BOOL) acceptsFirstResponder
{
    if (self.host == nullptr || self.host->imgui == nullptr)
        return NO;

    ImGuiContext *previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext (self.host->imgui);
    const bool wantsText = ImGui::GetIO().WantTextInput;
    ImGui::SetCurrentContext (previous);

    return wantsText ? YES : NO;
}

- (void) drawRect: (NSRect) dirty
{
    (void) dirty;

    ImGuiHostWindow *c = self.host;

    if (c == nullptr || ! c->created || c->imgui == nullptr || c->render == nullptr)
        return;

    if (c->rendering || self.isHiddenOrHasHiddenAncestor)
        return;

    /* Backing pixels, not points. On a Retina display the surface is twice the view's
       point size, and a viewport in points draws into a quarter of it. */
    const NSRect backing = [self convertRectToBacking: self.bounds];

    if (backing.size.width < 8.0 || backing.size.height < 8.0)
        return;

    c->rendering = true;

    [[self openGLContext] makeCurrentContext];

    ImGuiContext *previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext (c->imgui);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplOSX_NewFrame (self);
    ImGui::NewFrame();

    c->render (c->userData);

    ImGui::Render();
    glViewport (0, 0, (GLsizei) backing.size.width, (GLsizei) backing.size.height);
    glClearColor (0.055f, 0.055f, 0.062f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData (ImGui::GetDrawData());

    [[self openGLContext] flushBuffer];
    ImGui::SetCurrentContext (previous);

    c->rendering = false;
}

@end

@interface LumiPaintWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, assign) ImGuiHostWindow *host;
@end

@implementation LumiPaintWindowDelegate

- (BOOL) windowShouldClose: (NSWindow *) sender
{
    (void) sender;

    ImGuiHostWindow *c = self.host;

    if (c == nullptr)
        return NO;

    /*
        Ordered out here as well as reported, exactly as the Windows side hides on
        WM_CLOSE. Telling the host and then waiting leaves the window on screen if the
        host does not act, which looks like a close button that does nothing.
    */
    c->visible = false;
    [c->window orderOut: nil];

    if (c->closed != nullptr)
        c->closed (c->userData);

    return NO;
}

@end

namespace {

void tick (ImGuiHostWindow *c)
{
    if (c == nullptr || c->view == nil || ! c->visible)
        return;

    if (c->view.isHiddenOrHasHiddenAncestor)
        return;

    [c->view setNeedsDisplay: YES];
}

void applyStyle()
{
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
}

/*
    A real font, the same intent as the Windows side.

    San Francisco is the system face and sits under a different name depending on the
    release; Helvetica has been there since before any of them. The file is checked for
    first because AddFontFromFileTTF asserts on a missing path rather than returning,
    which would take the host down on a release that names its fonts differently.

    Loaded before the backend initialises, because adding a font afterwards leaves the
    atlas stale and the new face never appears.
*/
void loadFont()
{
    const char *faces[] = {
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/SFNSText.ttf",
        "/System/Library/Fonts/SFNSDisplay.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/Library/Fonts/Arial.ttf"
    };

    for (const char *face : faces)
    {
        NSString *path = [NSString stringWithUTF8String: face];

        if (path != nil && [[NSFileManager defaultManager] fileExistsAtPath: path])
            if (ImGui::GetIO().Fonts->AddFontFromFileTTF (face, 16.0f) != nullptr)
                return;
    }

    ImGui::GetIO().Fonts->AddFontDefault();
}

}

ImGuiHostWindow *imguiHostCreate (uint32_t width, uint32_t height, bool floating,
                                  ImGuiHostRenderFn render, ImGuiHostClosedFn closed,
                                  void *userData)
{
    /*
        Value-initialised rather than memset.

        The struct holds Objective-C object pointers, and writing zeroes over a strong
        reference is not something ARC can account for. new T() zeroes every member
        without going behind the compiler's back.
    */
    ImGuiHostWindow *c = new ImGuiHostWindow();
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

    if (fmt == nil)
    {
        delete c;
        return nullptr;
    }

    LumiPaintView *view = [[LumiPaintView alloc]
                            initWithFrame: NSMakeRect (0, 0, width, height)
                              pixelFormat: fmt];

    if (view == nil)
    {
        delete c;
        return nullptr;
    }

    view.host = c;
    [view setWantsBestResolutionOpenGLSurface: YES];
    c->view = view;

    if (floating)
    {
        /* Titled and closable only: no miniaturise, matching the Windows side, where a
           plugin editor that minimises leaves a stray dock entry and no way back. */
        NSWindow *win = [[NSWindow alloc]
                          initWithContentRect: NSMakeRect (0, 0, width, height)
                                    styleMask: (NSWindowStyleMaskTitled
                                              | NSWindowStyleMaskClosable)
                                      backing: NSBackingStoreBuffered
                                        defer: NO];

        [win setContentView: view];
        [win setReleasedWhenClosed: NO];
        [win setTitle: @"LumiPaint"];

        LumiPaintWindowDelegate *del = [[LumiPaintWindowDelegate alloc] init];
        del.host = c;
        c->delegate = del;
        [win setDelegate: del];
        c->window = win;
    }

    [[view openGLContext] makeCurrentContext];

    IMGUI_CHECKVERSION();
    c->imgui = ImGui::CreateContext();

    ImGuiContext *previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext (c->imgui);
    ImGui::GetIO().IniFilename = nullptr;

    loadFont();
    applyStyle();

    ImGui_ImplOSX_Init (view);
    ImGui_ImplOpenGL3_Init ("#version 150");
    ImGui::SetCurrentContext (previous);

    c->timer = [NSTimer scheduledTimerWithTimeInterval: 1.0 / 60.0
                                               repeats: YES
                                                 block: ^(NSTimer *t) {
                                                     (void) t;
                                                     tick (c);
                                                 }];

    /* Added to the common modes as well. A modal panel spins its own run loop, and a
       timer registered only in the default mode stops firing for as long as one is up -
       so the editor would freeze behind a file dialog rather than carrying on. */
    [[NSRunLoop currentRunLoop] addTimer: c->timer forMode: NSRunLoopCommonModes];

    c->created = true;
    return c;
}

void imguiHostDestroy (ImGuiHostWindow *c)
{
    if (c == nullptr)
        return;

    c->created = false;
    c->visible = false;

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
        [c->window orderOut: nil];
        [c->window close];
        c->window = nil;
    }

    /*
        Taken out of the host's view hierarchy in the embedded case.

        The host owns the window this was added to and destroys it when it likes. A view
        of ours left inside it outlives this object and draws through a freed host
        pointer, which is a crash some time after the editor was closed rather than at
        the moment it was.
    */
    if (c->view != nil)
    {
        c->view.host = nullptr;
        [c->view removeFromSuperview];
        c->view = nil;
    }

    c->delegate = nil;
    delete c;
}

bool imguiHostSetParent (ImGuiHostWindow *c, void *nativeHandle)
{
    if (c == nullptr || c->view == nil || nativeHandle == nullptr)
        return false;

    /* clap_gui's cocoa API hands over an NSView to add ourselves to. */
    NSView *parent = (__bridge NSView *) nativeHandle;

    [c->view removeFromSuperview];
    [c->view setFrame: NSMakeRect (0, 0, c->w, c->h)];
    [c->view setAutoresizingMask: NSViewWidthSizable | NSViewHeightSizable];
    [parent addSubview: c->view];

    /*
        Painting from here, not only from show.

        The Windows side starts its repaint timer in setParent as well as in show,
        because a host that attaches the view and makes it visible itself never calls
        show - and the editor is then a window that exists and is never painted. An
        embedded view has no show call of its own either.
    */
    c->visible = true;
    [c->view setNeedsDisplay: YES];
    return true;
}

bool imguiHostSetTransient (ImGuiHostWindow *c, void *nativeHandle)
{
    if (c == nullptr || c->window == nil || nativeHandle == nullptr)
        return false;

    /* Keeps the editor above the host's window without making it a child of it, so
       closing the host does not leave it orphaned on screen. */
    NSWindow *parent = (__bridge NSWindow *) nativeHandle;
    [parent addChildWindow: c->window ordered: NSWindowAbove];
    return true;
}

void imguiHostSetTitle (ImGuiHostWindow *c, const char *title)
{
    if (c == nullptr || c->window == nil || title == nullptr)
        return;

    NSString *text = [NSString stringWithUTF8String: title];

    if (text != nil)
        [c->window setTitle: text];
}

void imguiHostSetSize (ImGuiHostWindow *c, uint32_t width, uint32_t height)
{
    if (c == nullptr)
        return;

    c->w = width;
    c->h = height;

    if (c->view != nil)
        [c->view setFrameSize: NSMakeSize (width, height)];

    if (c->window != nil)
        [c->window setContentSize: NSMakeSize (width, height)];
}

void imguiHostSetScale (ImGuiHostWindow *c, double scale)
{
    if (c == nullptr)
        return;

    c->scale = scale;
}

void imguiHostShow (ImGuiHostWindow *c)
{
    if (c == nullptr)
        return;

    if (c->view != nil)
        [c->view setHidden: NO];

    /* Ordered in without taking focus, as on Windows: opening the editor should not
       pull the keyboard away from the host. */
    if (c->window != nil)
        [c->window orderFront: nil];

    c->visible = true;

    if (c->view != nil)
        [c->view setNeedsDisplay: YES];
}

void imguiHostHide (ImGuiHostWindow *c)
{
    if (c == nullptr)
        return;

    c->visible = false;

    if (c->view != nil)
    {
        /*
            The keyboard is given back if we hold it.

            Equivalent to releaseInput on the Windows side. Hiding the editor while a
            field was being edited otherwise leaves this view as first responder in a
            window nobody can see, and the host's typing goes nowhere with no visible
            cause.
        */
        NSWindow *owner = [c->view window];

        if (owner != nil && [owner firstResponder] == c->view)
            [owner makeFirstResponder: nil];

        [c->view setHidden: YES];
    }

    if (c->window != nil)
        [c->window orderOut: nil];
}
