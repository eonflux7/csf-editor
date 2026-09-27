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

// ---- Building blocks of the authoring panels (docs/archive/editor/editor-ux-redesign.md, V3-V6) ----

// A card: a raised, rounded block with a header row (an icon in its colour,
// the title, a dim subtitle, a "?" help tooltip). Clicking the header folds
// a collapsible card. Returns whether the body is shown; call end_card()
// only then.
struct CardOptions {
    const char* icon{};
    ImVec4 icon_color{};  // zero alpha: the dim text colour
    const char* subtitle{};
    const char* help{};
    bool collapsible{true};
    bool default_open{true};
};
bool begin_card(const char* id, const char* title, const CardOptions& options = {});
void end_card();

// Button hierarchy: at most one primary button per context (filled accent),
// secondary for the rest, danger for destructive actions (red on hover).
bool primary_button(const char* label, ImVec2 size = {});
bool secondary_button(const char* label, ImVec2 size = {});
bool danger_button(const char* label, ImVec2 size = {});
// A filter chip: a small rounded toggle, filled while active.
bool chip(const char* label, bool active);

// "Nothing here yet" in a panel: an icon, one line of explanation and an
// optional primary action. Returns true when the action was clicked.
bool empty_state(const char* icon, const char* message, const char* action = nullptr);

// A dim "?" on the current line with `text` as a wrapped tooltip.
void help_marker(const char* text);

// Fields over values that live elsewhere (a component's line, a project
// string): they show `value`, keep what the user types or drags while the
// field is active, and return true once when an edit ends with a new value
// in `edited`.
bool edit_text_value(const char* id, const std::string& value, std::string& edited, const char* hint = nullptr);
bool edit_float_value(const char* id, float value, float& edited, float speed = 1.0F, const char* format = "%.2f");
bool edit_int_value(const char* id, int value, int& edited);

// Gives the last item its label for UI scripts, for widgets ImGui does not
// report (combo boxes): "Properties::behaviour" can then click it.
void name_last_item(const char* label);

// A two-column property table: fixed label column, stretching value column.
// property_row starts a row and leaves the cursor in the value column with
// the next item set to fill it.
bool begin_properties(const char* id);
void property_row(const char* label, const char* help = nullptr);
void end_properties();

} // namespace rwsman::ui
