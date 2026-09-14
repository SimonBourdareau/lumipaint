// Compile against either the upstream or patched Cocoa backend. No MIDI or
// plugin activation: exercise the exact app-deactivation notification in the crash.
#include "imgui_internal.h"
#include "imgui_impl_osx.mm"
#include <cstdio>
#include <cstring>

static void postFocus()
{
    auto center = [NSNotificationCenter defaultCenter];
    [center postNotificationName:NSApplicationDidResignActiveNotification object:NSApp];
    [center postNotificationName:NSApplicationDidBecomeActiveNotification object:NSApp];
}

static bool hasFocusPair(ImGuiContext *context)
{
    auto &events = context->InputEventsQueue;
    return events.Size == 2 && events[0].Type == ImGuiInputEventType_Focus &&
        !events[0].AppFocused.Focused && events[1].Type == ImGuiInputEventType_Focus &&
        events[1].AppFocused.Focused;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSView *firstView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
        NSView *secondView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
        auto first = ImGui::CreateContext();
        ImGui::SetCurrentContext(first);
        ImGui_ImplOSX_Init(firstView);
        auto second = ImGui::CreateContext();
        ImGui::SetCurrentContext(second);
        ImGui_ImplOSX_Init(secondView);
        auto unrelated = ImGui::CreateContext();
        const bool nullContext = argc > 1 && !std::strcmp(argv[1], "--null-context");
        ImGui::SetCurrentContext(nullContext ? nullptr : unrelated);
        postFocus();
        bool passed = hasFocusPair(first) && hasFocusPair(second) &&
            unrelated->InputEventsQueue.empty() &&
            ImGui::GetCurrentContext() == (nullContext ? nullptr : unrelated);
        if (!passed) {
            std::puts("FAIL: app focus delivered to wrong editor/context");
            return 1;
        }

        ImGui::SetCurrentContext(first);
        ImGui_ImplOSX_Shutdown();
        ImGui::DestroyContext(first);
        passed = firstView.subviews.count == 0;
        [firstView release];

        ImGui::SetCurrentContext(second);
        ImGui::GetIO().ClearEventsQueue();
        KeyEventResponder *responder = ImGui_ImplOSX_GetBackendData()->KeyEventResponder;
        ImGui::SetCurrentContext(nullptr);
        [responder insertText:@"x" replacementRange:NSMakeRange(NSNotFound, 0)];
        passed &= ImGui::GetCurrentContext() == nullptr && second->InputEventsQueue.Size == 1 &&
            second->InputEventsQueue[0].Type == ImGuiInputEventType_Text;
        ImGui::SetCurrentContext(second);
        ImGui::GetIO().ClearEventsQueue();
        ImGui::SetCurrentContext(nullptr);
        postFocus();
        passed &= hasFocusPair(second) && ImGui::GetCurrentContext() == nullptr;

        ImGui::SetCurrentContext(second);
        ImGui_ImplOSX_Shutdown();
        ImGui::DestroyContext(second);
        passed &= secondView.subviews.count == 0;
        [secondView release];
        ImGui::SetCurrentContext(unrelated);
        postFocus();
        passed &= unrelated->InputEventsQueue.empty();
        ImGui::DestroyContext(unrelated);
        ImGui::SetCurrentContext(nullptr);
        for (int i = 0; i < 100; ++i) postFocus();
        std::puts(passed ? "PASS: focus isolation, null context, text input and post-destruction notifications" : "FAIL: context lifetime");
        return passed ? 0 : 1;
    }
}
