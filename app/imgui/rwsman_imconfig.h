#pragma once

// rws-man's Dear ImGui configuration (IMGUI_USER_CONFIG, see imconfig.h).
// Assertions and the test-engine item hooks forward to rws-man's UI test
// harness (app/ui_automation.hpp) through app/imgui/imgui_hooks.hpp.

// Returns only when a UI test run records the failure; otherwise prints it
// and aborts, as assert() would.
void rwsman_imgui_assert_failed(const char* expression, const char* file, int line);

#ifndef NDEBUG
#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : rwsman_imgui_assert_failed(#_EXPR, __FILE__, __LINE__))
#endif
