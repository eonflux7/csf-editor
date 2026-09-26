#pragma once

#include "rwsman/commands.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// UI scenario scripts (docs/plans/editor-ux-redesign.md, T2): one step per
// line, `<op> <argument>...`, run by `rws-man --run-script` one step per frame.
// Arguments are separated by spaces; double quotes group an argument and
// accept \" and \\. '#' outside quotes starts a comment and ';' separates
// steps on one line.
//
//   open <path>                 open-project <folder>
//   copy <folder> <name>        (copies a folder to <name> in the output directory; paths
//                               starting $UI_OUTPUT are in the output directory)
//   wait idle | wait frames <n>
//   command <id>                undo | redo
//   goto [<kind>:]<text>        palette <text> | goto-palette <text>
//   click <target> [<modifiers>]   double-click <target> [<modifiers>] | hover <target>
//   click-world <x> <y> <z> [<modifiers>]   drag-world <x0> <y0> <z0> <x1> <y1> <z1> [<modifiers>]
//                               (modifiers held during the click or drag: Shift, Ctrl, Shift+Ctrl...)
//   drag-to-world <target> <x> <y> <z>   (drags a widget into the viewport: an asset onto the ground)
//   key <shortcut>              type <text>
//   expect <key> <value>        expect-not <key> <value> | expect-contains <key> <text>
//   remember <key>              expect-same <key> | expect-changed <key>   (against the remembered value)
//   screenshot <file>           compare <golden> [<max changed pixels %>]
//   dump-state <file>           lint commands | lint theme
//   resize <width>x<height>     list-items [<text>]   (prints visible widgets, for writing scripts)
//   monkey <seed> <steps>       (a seeded random walk of tool commands, clicks, drags,
//                               keys, undo and redo; fails if the selection stops resolving)
//   undo-all                    (undoes every mission and project edit)
//   add-component <lines>       (adds a component from mission operation lines, '|' between lines)
//   setting <key> <value>       (resource_root, game_root, projects_root or blender, for this run)
//   remove <path>               (deletes a file or folder in the output directory)
//   `type` expands a text starting with $ or ~/ as a path ($UI_OUTPUT/name).
//   require <VARIABLE>          (skips the script, exit 77, unless the environment variable is set)
//
// A <target> is a widget label, optionally qualified by the window it is in:
// "Save", "Inspector::name", "Changes::History". It matches the visible label
// (the text before "##"), the ID part after "##", or the full label.
namespace rwsman {

struct UiScriptStep {
    std::size_t line{};
    std::string op;
    std::vector<std::string> args;
};

struct UiScript {
    std::vector<UiScriptStep> steps;
    std::vector<std::string> errors;  // "line N: ..." for unknown ops and wrong argument counts
};

[[nodiscard]] UiScript parse_ui_script(std::string_view text);
// The steps for the older `--commands a,goto:kind:text,palette:text` list.
[[nodiscard]] std::vector<UiScriptStep> ui_script_from_commands(const std::vector<std::string>& commands);
[[nodiscard]] std::string format_ui_script_step(const UiScriptStep& step);

// A widget target split into its window qualifier and label.
struct UiTarget {
    std::string window;  // empty: any window
    std::string label;
};
[[nodiscard]] UiTarget parse_ui_target(std::string_view text);
// Whether a widget with `label` (as submitted to ImGui, "Text##id" or "##id")
// in a window named `window` ("Title###id", child windows "Parent/Child_ID")
// is the one `target` names.
[[nodiscard]] bool ui_target_matches(const UiTarget& target, std::string_view window, std::string_view label);
// Whether a window qualifier ("Inspector") names the window `window`, or a
// window it is a child of.
[[nodiscard]] bool ui_window_matches(std::string_view qualifier, std::string_view window);

// A flat view of the application state for `expect` and `dump-state`
// (T5): dotted keys ("selection.kind", "count.actors") to text values.
using StateSnapshot = std::map<std::string, std::string>;
[[nodiscard]] std::string state_snapshot_json(const StateSnapshot& snapshot);

// Problems with a command registry that the UI tests treat as failures
// (T11): IDs that are not dotted lower-case words, missing labels or
// categories, commands without an action, two palette commands with the same
// label, and unparsable or conflicting shortcuts.
[[nodiscard]] std::vector<std::string> lint_commands(const CommandRegistry& registry);

} // namespace rwsman
