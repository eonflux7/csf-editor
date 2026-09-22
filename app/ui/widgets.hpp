#pragma once

#include "ui/theme.hpp"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Small themed widgets shared by every panel. Colors come from theme.hpp only.
namespace rwsman::ui {

// Upper-case dim label followed by a hairline: "SELECTION ────────".
void section(const char* label);
// Dim, wrapped secondary text.
void dim_text(const char* format, ...) IM_FMTARGS(1);
// Colored text in a semantic token.
void token_text(Token token, const char* format, ...) IM_FMTARGS(2);

// A text field with a "> " prompt glyph and a hint. Returns true when edited.
bool search_input(const char* id, const char* hint, char* buffer, std::size_t size,
                  float width = -1.0F);

// Small icon-only button with a tooltip. `active` draws it in the accent color.
bool icon_button(const char* id, const char* icon, const char* tooltip, bool active = false);

// Provenance badge (✓ ~ ? !) with an explanatory tooltip. Draws on the current line.
void badge(Provenance provenance, const char* explanation = nullptr);

// Monospace "0x%06X" that copies to the clipboard on click and shows a tooltip.
// Returns true when clicked. Use `follow` to make it a navigation link instead
// of a copy target: the caller receives the click and decides.
bool hex_link(std::uint64_t offset, const char* tooltip = nullptr);

// Text that copies itself to the clipboard when clicked. Returns true on click.
// `copy` defaults to `text`.
bool copyable_text(const char* text, const char* copy = nullptr, const char* tooltip = nullptr);

// Draws `text` with the characters at `positions` (byte offsets) highlighted.
void highlighted_text(const std::string& text, const std::vector<std::uint32_t>& positions);

// Selectable row spanning the available width with a leading icon glyph tinted
// with `icon_token`. Returns true when clicked.
bool icon_row(const char* id, const char* icon, Token icon_token, const char* label,
              bool selected);

} // namespace rwsman::ui
