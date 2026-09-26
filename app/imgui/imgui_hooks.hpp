#pragma once

#include <imgui.h>
#include <imgui_internal.h>

// Hooks ImGui calls into (IMGUI_ENABLE_TEST_ENGINE and IM_ASSERT). They do
// nothing until the UI test harness installs handlers, and ImGui only calls
// the item hooks while ImGuiContext::TestEngineHookItems is set.
namespace rwsman::imgui_hooks {

struct Handlers {
    void (*item_add)(ImGuiContext* context, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* data){};
    void (*item_info)(ImGuiContext* context, ImGuiID id, const char* label, ImGuiItemStatusFlags flags){};
    // Returns true when the failure was recorded and the program should go on.
    bool (*assert_failed)(const char* expression, const char* file, int line){};
};

Handlers& handlers();

} // namespace rwsman::imgui_hooks
