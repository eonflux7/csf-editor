#include "imgui_hooks.hpp"

#include <cstdio>
#include <cstdlib>

namespace rwsman::imgui_hooks {

Handlers& handlers() {
    static Handlers instance;
    return instance;
}

} // namespace rwsman::imgui_hooks

void rwsman_imgui_assert_failed(const char* expression, const char* file, const int line) {
    const auto& handlers = rwsman::imgui_hooks::handlers();
    if (handlers.assert_failed && handlers.assert_failed(expression, file, line)) return;
    std::fprintf(stderr, "%s:%d: ImGui assertion failed: %s\n", file, line, expression);
    std::abort();
}

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* context, const ImGuiID id, const ImRect& bb,
                                 const ImGuiLastItemData* data) {
    if (const auto hook = rwsman::imgui_hooks::handlers().item_add) hook(context, id, bb, data);
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext* context, const ImGuiID id, const char* label,
                                  const ImGuiItemStatusFlags flags) {
    if (const auto hook = rwsman::imgui_hooks::handlers().item_info) hook(context, id, label, flags);
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }
