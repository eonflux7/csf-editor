#include "commands.hpp"

#include "app_actions.hpp"
#include "app_util.hpp"
#include "authoring.hpp"
#include "file_dialogs.hpp"
#include "mission_authoring.hpp"
#include "mission_editing.hpp"
#include "navigation.hpp"
#include "project_actions.hpp"
#include "viewport_tools.hpp"
#include "ui/fonts.hpp"
#include "ui/layout.hpp"
#include "ui/theme.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <cmath>
#include <cstdio>

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

void change_ui_scale(AppState& state, const float step) {
    // Steps land on whole 10% values (1.0, 1.1, ...) whatever the starting scale.
    const float scaled = state.settings.ui_scale * 10.0F;
    const float next = step > 0.0F ? std::floor(scaled + 0.001F) + 1.0F
                     : step < 0.0F ? std::ceil(scaled - 0.001F) - 1.0F
                                   : 10.0F;
    const float before = state.settings.ui_scale;
    state.settings.ui_scale = next / 10.0F;
    state.settings.clamp();
    if (state.settings.ui_scale == before) return;
    state.settings_dirty = true;
    char text[48];
    std::snprintf(text, sizeof(text), "UI scale %.0f%%", static_cast<double>(state.settings.ui_scale * 100.0F));
    state.info(text);
}

void show_panel(AppState& state, const Panel panel) {
    // A panel of another workspace (Objectives while inspecting) switches to
    // the first available workspace that has it, the Mission workspace first.
    if (!ui::workspace_has_panel(state.workspace, panel))
        for (const auto workspace : {Workspace::mission, Workspace::script, Workspace::scene, Workspace::animation,
                                     Workspace::geometry, Workspace::inspector})
            if (ui::workspace_has_panel(workspace, panel) && set_workspace(state, workspace)) break;
    state.ui.panel_open[static_cast<std::size_t>(panel)] = true;
    for (const auto& slot : ui::workspace_panels(state.workspace))
        if (slot.panel == panel) {
            auto& shown = slot.dock == ui::Dock::right    ? state.settings.show_inspector
                          : slot.dock == ui::Dock::bottom ? state.settings.show_bottom_dock
                                                          : state.settings.show_explorer;
            if (!shown) state.settings_dirty = true;
            shown = true;
        }
    state.ui.maximize_viewport = false;
    state.ui.focus_panel = panel;
    state.ui.focus_panel_frames = 4;
}

void toggle_panel(AppState& state, const Panel panel) {
    auto& open = state.ui.panel_open[static_cast<std::size_t>(panel)];
    if (open && ui::workspace_has_panel(state.workspace, panel)) {
        open = false;
        return;
    }
    show_panel(state, panel);
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
    b.add("file.new_project", "New project...", "File", "Ctrl+N", [&s] { s.ui.show_new_project = true; })
        .keywords = "create checkpoint slot wizard";
    b.add("file.open_project", "Open mission project...", "File", "",
          [&s] { request_file_dialog(s, DialogKind::open_project, s.settings.projects_root); })
        .keywords = "mod edit workspace";
    // Ctrl+S saves what is being edited: the mission project while a mission is
    // open, otherwise a copy of the RenderWare document.
    b.add("file.save", "Save", "File", "Ctrl+S",
          [&s] {
              if (mission_editable(s)) save_mission_project(s);
              else save_copy(s);
          },
          [&s] { return mission_editable(s) || has_document(s); })
        .separator_before = true;
    b.add("file.save_mission", "Save mission project", "File", "",
          [&s] { save_mission_project(s); }, [&s] { return mission_editable(s); });
    b.add("file.save_mission_as", "Save mission project as...", "File", "",
          [&s] {
              // A new folder becomes the project; the edits are written there in full.
              s.mission.project.reset();
              request_file_dialog(s, DialogKind::save_project, s.settings.projects_root);
          },
          [&s] { return mission_editable(s); });
    b.add("file.export_mission", "Export mission archive (.pak)...", "Build", "Ctrl+E",
          [&s] { s.ui.show_export_dialog = true; }, [&s] { return mission_editable(s); })
        .keywords = "pak package mod build install";
    b.add("file.save_copy", "Save copy of the RenderWare document", "File", "", [&s] { save_copy(s); },
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
    // Text fields keep Ctrl+Z / Ctrl+Y for their own undo.
    // Undo covers mission edits and project edits (placements, texts) alike.
    const auto undo_ready = [&s] { return can_undo_edit(s) && !ImGui::GetIO().WantTextInput; };
    const auto redo_ready = [&s] { return can_redo_edit(s) && !ImGui::GetIO().WantTextInput; };
    b.add("edit.undo", "Undo", "Edit", "Ctrl+Z", [&s] { undo_edit(s); }, undo_ready).separator_before = true;
    b.add("edit.redo", "Redo", "Edit", "Ctrl+Y", [&s] { redo_edit(s); }, redo_ready);
    {
        auto& redo = b.add("edit.redo_alternate", "Redo", "Edit", "Ctrl+Shift+Z", [&s] { redo_edit(s); }, redo_ready);
        redo.in_menu = false;
        redo.in_palette = false;
        redo.in_help = false;
    }
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
    b.add("nav.show_in_hex", "Show in hex", "Inspect", "",
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

    // ---- View: modes and workspaces -----------------------------------------
    // Mission and Script are modes of their own; Inspect groups the four
    // inspection workspaces and returns to the one used last.
    struct WorkspaceCommand {
        Workspace workspace;
        const char* id;
        const char* label;
        const char* shortcut;
    };
    for (const auto& entry : {WorkspaceCommand{Workspace::mission, "view.workspace.mission", "Mission mode", "Ctrl+1"},
                              WorkspaceCommand{Workspace::script, "view.workspace.script", "Script mode", "Ctrl+2"},
                              WorkspaceCommand{Workspace::scene, "view.workspace.scene", "Inspect: scene", "Ctrl+4"},
                              WorkspaceCommand{Workspace::geometry, "view.workspace.geom", "Inspect: selected geometry", "Ctrl+5"},
                              WorkspaceCommand{Workspace::animation, "view.workspace.anim", "Inspect: animation", "Ctrl+6"},
                              WorkspaceCommand{Workspace::inspector, "view.workspace.hex", "Inspect: hex", "Ctrl+7"}}) {
        const auto workspace = entry.workspace;
        auto& command =
            b.add(entry.id, entry.label, "View", entry.shortcut,
                  [&s, workspace] { set_workspace(s, workspace); },
                  [&s, workspace] { return workspace_available(s, workspace); },
                  [&s, workspace] { return s.workspace == workspace; });
        command.keywords = "tab switch workspace mode";
        command.submenu = "Mode";
    }
    {
        auto& inspect = b.add("view.mode.inspect", "Inspect mode", "View", "Ctrl+3",
                              [&s] {
                                  if (!set_workspace(s, s.ui.inspect_workspace))
                                      for (const auto workspace : {Workspace::scene, Workspace::inspector})
                                          if (set_workspace(s, workspace)) break;
                              },
                              [&s] { return has_document(s); },
                              [&s] { return mode_of(s.workspace) == Mode::inspect; });
        inspect.keywords = "tab switch workspace chunks hex geometry";
        inspect.submenu = "Mode";
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
    b.add("view.toggle_explorer", "Toggle left panels", "View", "Ctrl+B",
          [&s] { s.settings.show_explorer = !s.settings.show_explorer; s.settings_dirty = true; },
          [&s] { return has_document(s); }, [&s] { return s.settings.show_explorer; })
        .separator_before = true;
    b.add("view.toggle_inspector", "Toggle right panels", "View", "Ctrl+I",
          [&s] { s.settings.show_inspector = !s.settings.show_inspector; s.settings_dirty = true; },
          [&s] { return has_document(s); }, [&s] { return s.settings.show_inspector; });
    b.add("view.toggle_bottom_dock", "Toggle bottom panels", "View", "Ctrl+J",
          [&s] { s.settings.show_bottom_dock = !s.settings.show_bottom_dock; s.settings_dirty = true; },
          {}, [&s] { return s.settings.show_bottom_dock; });
    // One "show" command per panel; showing never hides (the panel's close
    // button and its dock toggle do).
    for (std::size_t i = 0; i < panel_count; ++i) {
        const auto panel = static_cast<Panel>(i);
        if (panel == Panel::center) continue;
        auto& command = b.add(std::string("view.panel.") + ui::panel_key(panel),
                              std::string("Show ") + ui::panel_title(panel), "View", "",
                              [&s, panel] { show_panel(s, panel); },
                              [&s, panel] {
                                  return panel == Panel::missions || panel == Panel::console || has_document(s);
                              });
        command.submenu = "Panels";
        command.keywords = "panel window dock tab";
    }
    b.add("view.render_settings", "Toggle render settings", "View", "",
          [&s] { toggle_panel(s, Panel::render_settings); },
          [&s] { return has_document(s) && ui::has_viewport(s.workspace) && s.workspace != Workspace::geometry; },
          [&s] { return s.ui.panel_open[static_cast<std::size_t>(Panel::render_settings)]; });
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
    for (const auto& [id, label, theme] : {std::tuple{"view.theme_dark", "Theme: dark", "dark"},
                                           std::tuple{"view.theme_high_contrast", "Theme: high contrast", "high-contrast"}})
        b.add(id, label, "View", "",
              [&s, theme] {
                  ui::set_theme(theme);
                  s.settings.theme = theme;
                  s.settings_dirty = true;
                  ui::apply_theme(ui::ui_scale());
              },
              {}, [theme] { return ui::theme_name() == theme; })
            .submenu = "Theme";
    b.add("view.zoom_in", "Zoom in (UI scale)", "View", "Ctrl+=", [&s] { change_ui_scale(s, 1.0F); },
          [&s] { return s.settings.ui_scale < 2.0F; })
        .keywords = "bigger larger font size text";
    b.add("view.zoom_out", "Zoom out (UI scale)", "View", "Ctrl+-", [&s] { change_ui_scale(s, -1.0F); },
          [&s] { return s.settings.ui_scale > 0.8F; })
        .keywords = "smaller font size text";
    b.add("view.zoom_reset", "Reset zoom (100%)", "View", "Ctrl+0", [&s] { change_ui_scale(s, 0.0F); },
          [&s] { return s.settings.ui_scale != 1.0F; })
        .keywords = "font size text default";

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
    // ---- View: mission markers ---------------------------------------------
    {
        const auto markers = [&s] { return s.mission.scene != nullptr && s.preview.has_mission_overlays(); };
        auto& focus = b.add("view.markers_focus", "Focus mode (dim unrelated markers)", "View", "Z",
                            [&s] { s.preview.set_focus_mode(!s.preview.focus_mode()); }, markers,
                            [&s] { return s.preview.focus_mode(); });
        focus.scope = ShortcutScope::viewport;
        focus.separator_before = true;
        focus.submenu = "Markers";
        focus.keywords = "overlay related selection declutter";
        auto& slice = b.add("view.markers_slice", "Height slice: current floor", "View", "Y",
                            [&s] { s.preview.toggle_floor_slice(); }, markers,
                            [&s] { return s.preview.slice_enabled(); });
        slice.scope = ShortcutScope::viewport;
        slice.submenu = "Markers";
        slice.keywords = "storey level vertical band clip overlay";
        auto& filter = b.add("view.markers_filter", "Filter markers", "View", "/",
                             [&s] { s.preview.focus_overlay_filter(); }, markers);
        filter.scope = ShortcutScope::viewport;
        filter.submenu = "Markers";
        filter.keywords = "search overlay find";
        auto& minimap = b.add("view.markers_minimap", "Minimap", "View", "M",
                              [&s] { s.preview.toggle_minimap(); }, markers,
                              [&s] { return s.preview.overlay_options().show_minimap; });
        minimap.scope = ShortcutScope::viewport;
        minimap.submenu = "Markers";
        minimap.keywords = "overview density heatmap map";
        auto& labels = b.add("view.markers_labels", "Cycle marker labels", "View", "",
                             [&s] { s.preview.cycle_label_mode(); }, markers);
        labels.submenu = "Markers";
        labels.keywords = "names text overlay";
        auto& legend = b.add("view.markers_legend", "Marker legend", "View", "",
                             [&s] {
                                 auto options = s.preview.overlay_options();
                                 options.show_legend = !options.show_legend;
                                 s.preview.set_overlay_options(std::move(options));
                             },
                             markers, [&s] { return s.preview.overlay_options().show_legend; });
        legend.submenu = "Markers";
        legend.keywords = "counts overlay key";
        for (const auto& preset : builtin_overlay_presets()) {
            auto& command = b.add("view.markers_preset." + lower_ascii(preset.name), "Marker preset: " + preset.name, "View", "",
                                  [&s, hidden = preset.hidden_layers] {
                                      auto options = s.preview.overlay_options();
                                      options.hidden_layers = hidden;
                                      s.preview.set_overlay_options(std::move(options));
                                  },
                                  markers);
            command.submenu = "Markers";
            command.keywords = "layers overlay preset";
        }
    }
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
    b.add("mission.reload", "Reload mission", "File", "",
          [&s] {
              if (!s.mission.graph) return;
              std::optional<std::filesystem::path> project;
              if (s.mission.project) project = s.mission.project->workspace_root;
              start_mission_load(s, s.mission.graph->scene_path(), project);
          },
          [&s] { return s.mission.graph != nullptr && !mission_load_active(s); });
    b.add("mission.cancel_load", "Cancel mission load", "File", "Escape",
          [&s] { cancel_mission_load(s); }, [&s] { return mission_load_active(s); });
    {
        // Authoring projects: the map is rebuilt from the Blender exports and
        // placements; the height report checks what stands where.
        const auto idle = [&s] { return authoring_open(s) && !authoring_pending(s) && !mission_load_active(s); };
        auto& rebuild = b.add("mission.project_rebuild", "Rebuild project map", "Build", "",
                              [&s] { rebuild_authoring_map(s, true); }, idle);
        rebuild.separator_before = true;
        rebuild.keywords = "blender terrain world build";
        auto& heights = b.add("mission.project_heights", "Height report", "Build", "",
                              [&s] {
                                  s.authoring.show_heights = true;
                                  rebuild_authoring_map(s, false);
                              },
                              idle);
        heights.keywords = "ground snap float buried terrain";
        auto& resnap = b.add("mission.project_resnap", "Resnap heights", "Build", "",
                             [&s] { resnap_authoring_heights(s); },
                             [&s, idle] { return idle() && !s.authoring.findings.empty(); });
        resnap.keywords = "ground snap";
        auto& blender = b.add("build.edit_in_blender", "Edit in Blender", "Build", "", [&s] { edit_in_blender(s); },
                              [&s] { return authoring_open(s); });
        blender.keywords = "terrain model blend";
        auto& archives = b.add("build.archives", "Build archives", "Build", "Ctrl+Shift+B",
                               [&s] { build_project_archives(s); }, idle);
        archives.separator_before = true;
        archives.keywords = "pak package dist";
        b.add("build.deploy", "Deploy to the test install...", "Build", "",
              [&s] {
                  if (s.ui.selected_build.empty() && s.authoring.project) {
                      const auto builds = csf::archive_builds(*s.authoring.project);
                      if (!builds.empty()) s.ui.selected_build = builds.front();
                  }
                  s.ui.confirm_deploy = s.ui.selected_build;
                  show_panel(s, Panel::build);
              },
              [&s] { return authoring_open(s) && s.authoring.project && !csf::archive_builds(*s.authoring.project).empty(); })
            .keywords = "install game test play";
    }
    {
        const auto editable = [&s] { return mission_editable(s); };
        const auto record = [&s] {
            return mission_editable(s) && selected_mission_record(s).kind != MissionRecordKey::Kind::none;
        };
        auto& select = b.add("mission.tool_select", "Select tool", "Place", "Q",
                             [&s] { set_viewport_tool(s, GeometryPreview::EditTool::select); }, editable,
                             [&s] { return s.preview.edit_tool() == GeometryPreview::EditTool::select; });
        select.scope = ShortcutScope::viewport;
        select.separator_before = true;
        select.keywords = "pointer pick cursor";
        auto& move = b.add("mission.tool_move", "Move tool", "Place", "G",
                           [&s] {
                               const auto tool = s.preview.edit_tool() == GeometryPreview::EditTool::move
                                                     ? GeometryPreview::EditTool::select
                                                     : GeometryPreview::EditTool::move;
                               s.preview.set_edit_tool(tool);
                           },
                           editable, [&s] { return s.preview.edit_tool() == GeometryPreview::EditTool::move; });
        move.scope = ShortcutScope::viewport;
        move.keywords = "gizmo translate drag position";
        auto& rotate = b.add("mission.tool_rotate", "Rotate tool", "Place", "R",
                             [&s] {
                                 const auto tool = s.preview.edit_tool() == GeometryPreview::EditTool::rotate
                                                       ? GeometryPreview::EditTool::select
                                                       : GeometryPreview::EditTool::rotate;
                                 s.preview.set_edit_tool(tool);
                             },
                             editable, [&s] { return s.preview.edit_tool() == GeometryPreview::EditTool::rotate; });
        rotate.scope = ShortcutScope::viewport;
        rotate.keywords = "gizmo heading turn angle";
        b.add("mission.snap_surface", "Snap moves to the ground", "Place", "",
              [&s] { s.preview.set_snap_to_surface(!s.preview.snap_to_surface()); }, editable,
              [&s] { return s.preview.snap_to_surface(); })
            .keywords = "ground floor drop";
        // Viewport authoring tools (Phase 2): each click adds to the mission.
        using Tool = GeometryPreview::EditTool;
        for (const auto& [id, label, key, tool, keywords] :
             {std::tuple{"mission.tool_place", "Place tool", "P", Tool::place, "asset prop actor add put drop"},
              std::tuple{"mission.tool_route", "Route tool", "N", Tool::route, "navigation patrol path points"},
              std::tuple{"mission.tool_zone", "Zone tool", "B", Tool::zone, "area trigger region polygon"},
              std::tuple{"mission.tool_cover", "Cover tool", "C", Tool::cover, "cover points parapeto hide"}}) {
            auto& command = b.add(id, label, "Place", key,
                                  [&s, tool] {
                                      set_viewport_tool(s, s.preview.edit_tool() == tool ? Tool::select : tool);
                                      if (tool == Tool::place && !s.tools.place_entry &&
                                          s.tools.place_building_asset.empty())
                                          show_panel(s, Panel::assets);
                                  },
                                  editable, [&s, tool] { return s.preview.edit_tool() == tool; });
            command.scope = ShortcutScope::viewport;
            command.keywords = keywords;
        }
        b.add("mission.align_ground", "Align to the ground", "Edit", "",
              [&s] { align_selection_to_ground(s); }, record)
            .keywords = "snap drop floor height";
        b.add("mission.select_none", "Select nothing", "Edit", "",
              [&s] {
                  s.ui.selection_extra.clear();
                  s.preview.clear_mission_selection();
                  s.selection = {};
              },
              record)
            .keywords = "deselect clear";
        b.add("mission.duplicate", "Duplicate", "Edit", "Ctrl+D",
              [&s] { duplicate_selection(s); }, record)
            .separator_before = true;
        auto& remove = b.add("mission.delete", "Delete", "Edit", "Delete",
                             [&s] { delete_selection(s, false); }, record);
        remove.scope = ShortcutScope::viewport;
        auto& force = b.add("mission.delete_force", "Delete, leaving references", "Edit",
                            "Shift+Delete", [&s] { delete_selection(s, true); }, record);
        force.scope = ShortcutScope::viewport;
        b.add("mission.assets", "Asset browser", "Place", "", [&s] { show_panel(s, Panel::assets); }, editable)
            .keywords = "class import place prop actor browser add";
        b.add("mission.presets", "Behaviours: guards, patrols, cover, walk grid", "Place", "",
              [&s] { show_panel(s, Panel::behaviours); }, editable)
            .keywords = "patrol guard cover walk grid dog idle preset";
        // One palette entry per behaviour preset: the form opens set up for it.
        using Preset = AuthoringTools::Preset;
        for (const auto& [id, label, preset, keywords] :
             {std::tuple{"place.guard_patrol", "Add guard on patrol", Preset::guard_patrol, "soldier route walk"},
              std::tuple{"place.guard_idle", "Add idle guard", Preset::guard_idle, "soldier stand smoke animation"},
              std::tuple{"place.animal_patrol", "Add animal on patrol", Preset::animal_patrol, "dog doberman route"},
              std::tuple{"place.cover_group", "Add cover points", Preset::cover_group, "parapeto hide combat"},
              std::tuple{"place.walk_grid", "Generate walk grid", Preset::walk_grid, "malla navigation mesh"}}) {
            auto& command = b.add(id, label, "Place", "",
                                  [&s, preset] {
                                      s.tools.preset = preset;
                                      show_panel(s, Panel::behaviours);
                                  },
                                  editable);
            command.keywords = keywords;
            command.submenu = "Behaviour";
        }
        b.add("mission.add_objective", "Add objective", "Mission", "",
              [&s] {
                  s.tools.objectives.emplace_back();
                  show_panel(s, Panel::objectives);
              },
              editable)
            .keywords = "goal zone kill use";
        for (const auto& [id, label, panel, keywords] :
             {std::tuple{"mission.settings", "Mission settings", Panel::mission_settings,
                         "players start score environment fog"},
              std::tuple{"mission.objectives", "Objectives and starting equipment", Panel::objectives,
                         "goal success kit weapons tips"},
              std::tuple{"mission.flow", "Mission flow", Panel::flow, "events scripts graph findings objectives"},
              std::tuple{"mission.cutscene", "Intro cutscene", Panel::timeline, "camera shots travelling intro timeline"},
              std::tuple{"mission.texts", "Mission text", Panel::texts, "strings fli globalek localization"}})
            b.add(id, label, "Mission", "", [&s, panel] { show_panel(s, panel); }, editable).keywords = keywords;
        b.add("mission.capture_shot", "Capture cutscene shot from view", "Mission", "",
              [&s] {
                  capture_shot(s);
                  s.info("Captured shot " + std::to_string(s.tools.shots.size()));
              },
              editable)
            .keywords = "camera intro cutscene";
        b.add("mission.play_shots", "Play cutscene shots", "Mission", "", [&s] { play_shots(s); },
              [&s] { return !s.tools.shots.empty(); })
            .keywords = "camera intro cutscene preview";
        b.add("mission.new_mission", "Start a new mission in this slot", "Mission", "",
              [&s] { start_new_mission(s); }, editable)
            .keywords = "empty clear slot";
    }
    b.add("mission.references", "Show references of selection", "Mission", "",
          [&s] { show_panel(s, Panel::references); },
          [&s] { return !s.selection.empty(); })
        .separator_before = true;
    b.add("mission.problems", "Show problems", "Mission", "", [&s] { show_panel(s, Panel::problems); })
        .keywords = "diagnostics errors warnings findings";

    // ---- Tools --------------------------------------------------------------
    b.add("tools.reload_preview", "Reload preview from edited bytes", "Inspect", "",
          [&s] {
              s.preview.clear();
              restore_mission_overlays(s);
              s.info("Preview will reload from the current in-memory document");
          },
          [&s] { return has_document(s); });
    b.add("tools.scene_tools", "Open scene / collision tools", "Inspect", "",
          [&s] {
              s.workspace = s.mission.scene ? s.workspace : Workspace::scene;
              if (!(s.workspace == Workspace::scene || s.workspace == Workspace::mission ||
                    s.workspace == Workspace::animation))
                  s.workspace = Workspace::scene;
              s.ui.maximize_viewport = false;
              show_panel(s, Panel::render_settings);
          },
          [&s] { return has_document(s); });

    // ---- Export -------------------------------------------------------------
    b.add("export.scene_gltf", "Whole scene (glTF)", "Inspect", "", [&s] { export_scene_gltf(s); },
          [&s] { return has_document(s); });
    b.add("export.clump_gltf", "Selected Clump (glTF)", "Inspect", "",
          [&s] { export_selected_clump_gltf(s); }, [&s] { return selected_clump(s) != nullptr; });
    b.add("export.collision_gltf", "Collision only (glTF)", "Inspect", "",
          [&s] { export_collision_gltf(s); }, [&s] { return collision_export_document(s) != nullptr; })
        .separator_before = true;
    b.add("export.collision_obj", "Collision only (OBJ)", "Inspect", "",
          [&s] { export_collision_obj(s); }, [&s] { return collision_export_document(s) != nullptr; });

    // ---- Help ---------------------------------------------------------------
    b.add("help.shortcuts", "Keyboard shortcuts", "Help", "F1", [&s] { s.ui.show_shortcuts = true; });
    b.add("help.about", "About CSF Mission Editor", "Help", "", [&s] { s.ui.show_about = true; });
}

} // namespace rwsman
