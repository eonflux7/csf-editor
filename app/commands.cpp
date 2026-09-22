#include "commands.hpp"

#include "app_actions.hpp"
#include "app_util.hpp"
#include "navigation.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>

namespace rwsman {
namespace {

struct Builder {
    AppState& state;

    Command& add(std::string id, std::string label, std::string category, std::string shortcut,
                 std::function<void()> run, std::function<bool()> enabled = {},
                 std::function<bool()> checked = {}) {
        Command command;
        command.id = std::move(id);
        command.label = std::move(label);
        command.category = std::move(category);
        command.shortcut = std::move(shortcut);
        command.run = std::move(run);
        command.enabled = std::move(enabled);
        command.checked = std::move(checked);
        state.commands.add(std::move(command));
        return state.commands.back();
    }
};

bool has_document(const AppState& state) {
    return state.document != nullptr;
}

} // namespace

void copy_to_clipboard(AppState& state, const std::string& text, const char* what) {
    ImGui::SetClipboardText(text.c_str());
    state.info(std::string("Copied ") + what + ": " + text);
}

void copy_selection_identity(AppState& state) {
    const auto identity = selection_identity(state, state.selection);
    if (!identity.empty()) copy_to_clipboard(state, identity, "identity");
}

void toggle_bottom_panel(AppState& state, bool UiState::*panel) {
    // Opening a specific bottom tab also reveals the dock.
    state.ui.*panel = true;
    state.settings.show_bottom_dock = true;
    state.settings_dirty = true;
    state.ui.focus_bottom_tab = panel == &UiState::show_console       ? 0
                                : panel == &UiState::show_diagnostics ? 1
                                : panel == &UiState::show_references  ? 2
                                : panel == &UiState::show_changes     ? 3
                                                                      : 4;
}

void register_commands(AppState& state) {
    Builder b{state};
    auto& s = state;

    // ---- File ---------------------------------------------------------------
    b.add("file.open_mission", "Open mission...", "File", "Ctrl+Shift+O",
          [&s] { open_mission_dialog(s); })
        .keywords = "scn load";
    b.add("file.open_rws", "Open RWS...", "File", "Ctrl+O", [&s] { open_document_dialog(s); })
        .keywords = "rpc rws anm model load";
    b.add("file.open_companion", "Open collision companion...", "File", "",
          [&s] { open_companion_dialog(s); },
          [&s] { return s.document && !s.main_is_collision; })
        .separator_before = true;
    b.add("file.clear_companion", "Clear collision companion", "File", "",
          [&s] {
              s.collision_document.reset();
              s.collision_status = "Collision companion cleared";
              s.preview.clear();
              restore_mission_overlays(s);
              s.info("Collision companion cleared");
          },
          [&s] { return s.collision_document != nullptr; });
    b.add("file.save_copy", "Save copy", "File", "Ctrl+S", [&s] { save_copy(s); },
          [&s] { return has_document(s); })
        .separator_before = true;
    b.add("file.exit", "Exit", "File", "Ctrl+Q", [&s] { glfwSetWindowShouldClose(s.window, GLFW_TRUE); })
        .separator_before = true;

    // ---- Edit / navigation --------------------------------------------------
    b.add("edit.command_palette", "Command palette...", "Edit", "Ctrl+Shift+P", [&s] {
        s.ui.palette = UiState::PaletteMode::commands;
        s.ui.palette_just_opened = true;
    });
    b.add("edit.go_to", "Go to anything...", "Edit", "Ctrl+P", [&s] {
        s.ui.palette = UiState::PaletteMode::go_to;
        s.ui.palette_just_opened = true;
    });
    b.add("edit.back", "Back", "Edit", "Alt+Left", [&s] { go_back(s); },
          [&s] { return s.history.can_back(); })
        .separator_before = true;
    b.add("edit.forward", "Forward", "Edit", "Alt+Right", [&s] { go_forward(s); },
          [&s] { return s.history.can_forward(); });
    b.add("edit.copy_identity", "Copy selection identity", "Edit", "Ctrl+Shift+C",
          [&s] { copy_selection_identity(s); }, [&s] { return !s.selection.empty(); })
        .separator_before = true;
    b.add("edit.copy_id", "Copy ID", "Edit", "",
          [&s] { copy_to_clipboard(s, selection_id_text(s, s.selection), "ID"); },
          [&s] { return !selection_id_text(s, s.selection).empty(); });
    b.add("edit.copy_path", "Copy source path", "Edit", "",
          [&s] { copy_to_clipboard(s, path_utf8(selection_source_path(s, s.selection)), "path"); },
          [&s] { return !selection_source_path(s, s.selection).empty(); });
    b.add("nav.show_in_hex", "Show in hex", "Edit", "",
          [&s] {
              const auto offset = selection_offset(s, s.selection);
              if (!offset || !s.document) return;
              s.workspace = Workspace::inspector;
              // A chunk or instance highlights its whole record; anything else its offset.
              std::uint64_t end = *offset + 1;
              if (s.selection.kind == SelectionRef::Kind::chunk)
                  if (const auto* chunk = find_chunk(s.document->chunks(), s.selection.a))
                      end = chunk->offset + 12 + chunk->available_size;
              if (s.selection.kind == SelectionRef::Kind::scene_instance)
                  if (const auto* instance = find_instance(s.document->scene_instances(), s.selection.a))
                      end = instance->offset + instance->physical_size;
              s.ui.hex_highlight = {*offset, end};
              s.ui.hex_scroll_to_highlight = true;
          },
          [&s] {
              return s.document && (s.selection.kind == SelectionRef::Kind::chunk ||
                                    s.selection.kind == SelectionRef::Kind::scene_instance);
          });
    b.add("view.isolate_selection", "Isolate actor", "View", "",
          [&s] { s.preview.set_isolate_selected_actor(!s.preview.isolate_selected_actor()); },
          [&s] { return s.selection.kind == SelectionRef::Kind::mission_entry && s.mission.scene; },
          [&s] { return s.preview.isolate_selected_actor(); });
    b.add("edit.preferences", "Preferences...", "Edit", "Ctrl+Comma",
          [&s] { s.ui.show_preferences = true; })
        .separator_before = true;

    // ---- View: workspaces ---------------------------------------------------
    struct WorkspaceCommand {
        Workspace workspace;
        const char* id;
        const char* label;
        const char* shortcut;
    };
    for (const auto& entry : {WorkspaceCommand{Workspace::mission, "view.workspace.mission", "Mission", "Ctrl+1"},
                              WorkspaceCommand{Workspace::script, "view.workspace.script", "Script", "Ctrl+2"},
                              WorkspaceCommand{Workspace::animation, "view.workspace.anim", "Animation", "Ctrl+3"},
                              WorkspaceCommand{Workspace::scene, "view.workspace.scene", "Scene", "Ctrl+4"},
                              WorkspaceCommand{Workspace::geometry, "view.workspace.geom", "Selected geometry", "Ctrl+5"},
                              WorkspaceCommand{Workspace::inspector, "view.workspace.hex", "Inspector / Hex", "Ctrl+6"}}) {
        const auto workspace = entry.workspace;
        auto& command =
            b.add(entry.id, std::string("Workspace: ") + entry.label, "View", entry.shortcut,
                  [&s, workspace] { set_workspace(s, workspace); },
                  [&s, workspace] { return workspace_available(s, workspace); },
                  [&s, workspace] { return s.workspace == workspace; });
        command.keywords = "tab switch workspace";
        command.submenu = "Workspace";
    }
    // Bare digits keep working while the viewport is hovered (existing muscle memory).
    struct Alias {
        const char* id;
        const char* key;
        Workspace workspace;
    };
    for (const auto& alias : {Alias{"view.alias.scene", "1", Workspace::scene},
                              Alias{"view.alias.geometry", "2", Workspace::geometry},
                              Alias{"view.alias.hex", "3", Workspace::inspector},
                              Alias{"view.alias.animation", "4", Workspace::animation}}) {
        const auto workspace = alias.workspace;
        auto& command = b.add(alias.id, std::string("Viewport alias: ") + workspace_name(workspace),
                              "View", alias.key, [&s, workspace] { set_workspace(s, workspace); },
                              [&s, workspace] { return workspace_available(s, workspace); });
        command.scope = ShortcutScope::viewport;
        command.in_palette = false;
        command.in_menu = false;
    }

    // ---- View: panels -------------------------------------------------------
    b.add("view.toggle_explorer", "Toggle Explorer", "View", "Ctrl+B",
          [&s] { s.settings.show_explorer = !s.settings.show_explorer; s.settings_dirty = true; },
          [&s] { return has_document(s); }, [&s] { return s.settings.show_explorer; })
        .separator_before = true;
    b.add("view.toggle_inspector", "Toggle Inspector", "View", "Ctrl+I",
          [&s] { s.settings.show_inspector = !s.settings.show_inspector; s.settings_dirty = true; },
          [&s] { return has_document(s); }, [&s] { return s.settings.show_inspector; });
    b.add("view.toggle_bottom_dock", "Toggle bottom dock", "View", "Ctrl+J",
          [&s] { s.settings.show_bottom_dock = !s.settings.show_bottom_dock; s.settings_dirty = true; },
          {}, [&s] { return s.settings.show_bottom_dock; });
    b.add("view.render_settings", "Render settings panel", "View", "",
          [&s] {
              s.ui.show_render_settings = !s.ui.show_render_settings;
              s.ui.focus_render_settings = s.ui.show_render_settings;
          },
          [&s] { return has_document(s) && (s.workspace == Workspace::scene || s.workspace == Workspace::mission || s.workspace == Workspace::animation); },
          [&s] { return s.ui.show_render_settings; });
    b.add("view.console", "Show Console", "View", "", [&s] { toggle_bottom_panel(s, &UiState::show_console); })
        .submenu = "Bottom dock";
    b.add("view.diagnostics", "Show Diagnostics", "View", "", [&s] { toggle_bottom_panel(s, &UiState::show_diagnostics); })
        .submenu = "Bottom dock";
    b.add("view.references", "Show References", "View", "", [&s] { toggle_bottom_panel(s, &UiState::show_references); })
        .submenu = "Bottom dock";
    b.add("view.changes", "Show Changes", "View", "", [&s] { toggle_bottom_panel(s, &UiState::show_changes); })
        .submenu = "Bottom dock";
    b.add("view.missions", "Show Missions (resource browser)", "View", "", [&s] { toggle_bottom_panel(s, &UiState::show_missions); })
        .submenu = "Bottom dock";
    b.add("view.maximize_viewport", "Maximize viewport", "View", "Ctrl+Space",
          [&s] { s.ui.maximize_viewport = !s.ui.maximize_viewport; },
          [&s] { return has_document(s); }, [&s] { return s.ui.maximize_viewport; });
    b.add("view.pin_inspector", "Pin / unpin the Inspector", "View", "",
          [&s] {
              if (s.ui.inspector_pin)
                  s.ui.inspector_pin.reset();
              else if (!s.selection.empty())
                  s.ui.inspector_pin = s.selection;
          },
          [&s] { return s.ui.inspector_pin.has_value() || !s.selection.empty(); },
          [&s] { return s.ui.inspector_pin.has_value(); });
    b.add("view.duplicate_inspector", "Open a second Inspector on the selection", "View", "",
          [&s] { s.ui.extra_inspectors.push_back({s.ui.next_extra_inspector_id++, s.selection, true}); },
          [&s] { return !s.selection.empty(); });
    b.add("view.reset_layout", "Reset layout", "View", "",
          [&s] { s.ui.reset_layout = true; s.ui.maximize_viewport = false; })
        .separator_before = true;

    // ---- View: viewport -----------------------------------------------------
    {
        auto& c = b.add("view.frame_selection", "Frame selection", "View", "F",
                        [&s] { s.preview.frame_selection(s.selected); },
                        [&s] { return has_document(s); });
        c.scope = ShortcutScope::viewport;
        c.separator_before = true;
    }
    b.add("view.frame_all", "Frame all", "View", "Home", [&s] { s.preview.frame_all(); },
          [&s] { return has_document(s); })
        .scope = ShortcutScope::viewport;
    for (const auto& view : {std::pair<const char*, std::pair<const char*, int>>{"view.ortho_front", {"Front view (X/Y)", 2}},
                             std::pair<const char*, std::pair<const char*, int>>{"view.ortho_side", {"Side view (Z/Y)", 3}},
                             std::pair<const char*, std::pair<const char*, int>>{"view.ortho_top", {"Top view (X/Z)", 1}}}) {
        const int projection = view.second.second;
        auto& command = b.add(view.first, view.second.first, "View",
                              projection == 2 ? "Numpad1" : projection == 3 ? "Numpad3" : "Numpad7",
                              [&s, projection] { s.preview.set_projection(projection); },
                              [&s] { return has_document(s); });
        command.scope = ShortcutScope::viewport;
        command.keywords = "orthographic camera projection";
    }
    b.add("view.toggle_perspective", "Toggle perspective / orthographic", "View", "Numpad5",
          [&s] { s.preview.toggle_perspective(); }, [&s] { return has_document(s); })
        .scope = ShortcutScope::viewport;
    b.add("view.toggle_hud", "Toggle stats HUD", "View", "",
          [&s] {
              s.preview.set_show_hud(!s.preview.show_hud());
              s.settings.show_hud = s.preview.show_hud();
              s.settings_dirty = true;
          },
          [&s] { return has_document(s); }, [&s] { return s.preview.show_hud(); });
    b.add("view.screenshot", "Save screenshot (PNG)", "View", "F12",
          [&s] { s.ui.screenshot_requested = true; })
        .separator_before = true;
    for (int slot = 1; slot <= 9; ++slot) {
        const std::string number = std::to_string(slot);
        auto& store = b.add("view.bookmark_store_" + number, "Store camera bookmark " + number, "View",
                            "Ctrl+Shift+" + number,
                            [&s, slot] {
                                if (s.content_signature.empty()) return;
                                s.settings.set_bookmark(s.content_signature, {slot, s.preview.camera()});
                                s.settings_dirty = true;
                                s.info("Stored camera bookmark " + std::to_string(slot));
                            },
                            [&s] { return has_document(s); });
        store.scope = ShortcutScope::viewport;
        store.in_menu = false;
        store.in_help = false;
        auto& recall = b.add("view.bookmark_recall_" + number, "Recall camera bookmark " + number, "View",
                             "Shift+" + number,
                             [&s, slot] {
                                 if (const auto* bookmark = s.settings.bookmark(s.content_signature, slot)) {
                                     s.preview.set_camera(bookmark->camera);
                                     s.info("Recalled camera bookmark " + std::to_string(slot));
                                 } else {
                                     s.warn("Camera bookmark " + std::to_string(slot) + " is empty (Ctrl+Shift+" +
                                            std::to_string(slot) + " stores it)");
                                 }
                             },
                             [&s] { return has_document(s); });
        recall.scope = ShortcutScope::viewport;
        recall.in_menu = false;
        recall.in_help = false;
    }
    b.add("view.window_maximize", "Maximize window", "View", "", [&s] { glfwMaximizeWindow(s.window); })
        .separator_before = true;
    b.add("view.window_restore", "Restore window", "View", "", [&s] { glfwRestoreWindow(s.window); });

    // ---- Mission ------------------------------------------------------------
    b.add("mission.reload", "Reload mission", "Mission", "",
          [&s] {
              if (s.mission.graph) start_mission_load(s, s.mission.graph->scene_path());
          },
          [&s] { return s.mission.graph != nullptr && !mission_load_active(s); });
    b.add("mission.cancel_load", "Cancel mission load", "Mission", "Escape",
          [&s] { cancel_mission_load(s); }, [&s] { return mission_load_active(s); });
    b.add("mission.references", "Show references of selection", "Mission", "",
          [&s] { toggle_bottom_panel(s, &UiState::show_references); },
          [&s] { return !s.selection.empty(); })
        .separator_before = true;
    b.add("mission.diagnostics", "Show diagnostics", "Mission", "",
          [&s] { toggle_bottom_panel(s, &UiState::show_diagnostics); });

    // ---- Tools --------------------------------------------------------------
    b.add("tools.reload_preview", "Reload preview from edited bytes", "Tools", "",
          [&s] {
              s.preview.clear();
              restore_mission_overlays(s);
              s.info("Preview will reload from the current in-memory document");
          },
          [&s] { return has_document(s); });
    b.add("tools.scene_tools", "Open scene / collision tools", "Tools", "",
          [&s] {
              s.workspace = s.mission.scene ? s.workspace : Workspace::scene;
              if (!(s.workspace == Workspace::scene || s.workspace == Workspace::mission ||
                    s.workspace == Workspace::animation))
                  s.workspace = Workspace::scene;
              s.ui.maximize_viewport = false;
              s.ui.show_render_settings = true;
              s.ui.focus_render_settings = true;
          },
          [&s] { return has_document(s); });

    // ---- Export -------------------------------------------------------------
    b.add("export.scene_gltf", "Whole scene (glTF)", "Export", "", [&s] { export_scene_gltf(s); },
          [&s] { return has_document(s); });
    b.add("export.clump_gltf", "Selected Clump (glTF)", "Export", "",
          [&s] { export_selected_clump_gltf(s); }, [&s] { return selected_clump(s) != nullptr; });
    b.add("export.collision_gltf", "Collision only (glTF)", "Export", "",
          [&s] { export_collision_gltf(s); }, [&s] { return collision_export_document(s) != nullptr; })
        .separator_before = true;
    b.add("export.collision_obj", "Collision only (OBJ)", "Export", "",
          [&s] { export_collision_obj(s); }, [&s] { return collision_export_document(s) != nullptr; });

    // ---- Help ---------------------------------------------------------------
    b.add("help.shortcuts", "Keyboard shortcuts", "Help", "F1", [&s] { s.ui.show_shortcuts = true; });
    b.add("help.about", "About CSF RWS Tools", "Help", "", [&s] { s.ui.show_about = true; });
}

} // namespace rwsman
