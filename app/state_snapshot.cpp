#include "state_snapshot.hpp"
#include "viewport_tools.hpp"

#include "app_util.hpp"
#include "authoring.hpp"
#include "mission_editing.hpp"
#include "navigation.hpp"
#include "ui/layout.hpp"

#include "csf/mission_components.hpp"
#include "csf/project_pipeline.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>

namespace rwsman {
namespace {

std::string number(const std::uint64_t value) { return std::to_string(value); }
std::string boolean(const bool value) { return value ? "true" : "false"; }

// FNV-1a over every present mission file (path and bytes): equal hashes mean
// the editor holds the same files.
std::string mission_hash(const csf::MissionEditor& editor) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    const auto mix = [&hash](const auto* data, const std::size_t size) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= bytes[i];
            hash *= 0x100000001b3ULL;
        }
    };
    for (const auto& file : editor.files()) {
        if (!file.present) continue;
        const auto path = file.relative_path.generic_string();
        mix(path.data(), path.size());
        const auto bytes = file.bytes();
        mix(bytes.data(), bytes.size());
    }
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

// Whether a panel window was drawn in the last frame and is not a hidden tab.
bool window_visible(const std::string& name) {
    const auto* window = ImGui::FindWindowByName(name.c_str());
    return window && window->Active && !window->Hidden;
}

} // namespace

const char* mission_record_kind_name(const MissionRecordKey::Kind kind) {
    switch (kind) {
    case MissionRecordKey::Kind::none:
        return "none";
    case MissionRecordKey::Kind::actor:
        return "actor";
    case MissionRecordKey::Kind::dummy:
        return "dummy";
    case MissionRecordKey::Kind::light:
        return "light";
    case MissionRecordKey::Kind::area:
        return "area";
    case MissionRecordKey::Kind::effect:
        return "effect";
    case MissionRecordKey::Kind::nav_group:
        return "nav_group";
    case MissionRecordKey::Kind::nav_point:
        return "nav_point";
    case MissionRecordKey::Kind::placement:
        return "placement";
    }
    return "none";
}

StateSnapshot snapshot_state(const AppState& state) {
    StateSnapshot s;
    s["app.workspace"] = ui::workspace_key(state.workspace);
    s["app.document"] = boolean(state.document != nullptr);
    s["document.name"] = state.document ? path_utf8(state.document->source_path().filename()) : "";
    s["document.dirty"] = boolean(state.document && state.document->dirty());

    const auto& selection = state.selection;
    s["selection.kind"] = selection_kind_name(selection.kind);
    s["selection.title"] = selection.empty() ? "" : selection_title(state, selection);
    const auto record = state.mission.scene ? selected_mission_record(state) : MissionRecordKey{};
    s["selection.record"] = mission_record_kind_name(record.kind);
    s["selection.id"] = record.kind == MissionRecordKey::Kind::none ? "" : std::to_string(record.id);
    s["selection.sub_id"] = record.kind == MissionRecordKey::Kind::nav_point ? std::to_string(record.sub_id) : "";
    // Position of the selected actor, dummy or navigation point, to the centimetre.
    std::optional<csf::Vec3> position;
    std::optional<float> heading;
    if (const auto* scene = state.mission.scene.get()) {
        using Kind = MissionRecordKey::Kind;
        if (record.kind == Kind::actor) {
            for (const auto& actor : scene->actors())
                if (actor.id == record.id) {
                    position = actor.position;
                    heading = actor.heading;
                }
        } else if (record.kind == Kind::dummy) {
            for (const auto& dummy : scene->dummies())
                if (dummy.id == record.id) position = dummy.position;
        } else if (record.kind == Kind::nav_point) {
            if (const auto* point = scene->navigation_point(record.id, record.sub_id)) position = point->position;
        } else if (record.kind == Kind::placement) {
            if (const auto* placement = selected_placement(state)) {
                position = csf::Vec3{placement->position.x, placement->position.y, placement->position.z};
                heading = placement->yaw_degrees;
            }
        }
    }
    s["selection.count"] = number(record.kind == MissionRecordKey::Kind::none ? 0 : 1 + state.ui.selection_extra.size());
    char text[96] = "";
    if (position) std::snprintf(text, sizeof(text), "%.0f %.0f %.0f", position->x, position->y, position->z);
    s["selection.position"] = text;
    text[0] = '\0';
    if (heading) std::snprintf(text, sizeof(text), "%.1f", *heading);
    s["selection.heading"] = text;

    const auto& mission = state.mission;
    s["mission.open"] = boolean(mission.scene != nullptr);
    s["mission.name"] = mission.graph ? path_utf8(mission.graph->scene_path().filename()) : "";
    s["mission.editable"] = boolean(mission.editor && mission_editable(state));
    s["mission.project"] = mission.project ? path_utf8(mission.project->workspace_root) : "";
    if (const auto* editor = mission.editor.get()) {
        s["mission.dirty"] = boolean(editor->dirty());
        s["mission.revision"] = number(editor->revision());
        const auto labels = editor->history_labels();
        const auto position = editor->history_position();
        s["mission.history.size"] = number(labels.size());
        s["mission.history.position"] = number(position);
        s["mission.history.top"] = position > 0 && position <= labels.size() ? labels[position - 1] : "";
        s["mission.modified_files"] = number(editor->modified_files().size());
        s["mission.hash"] = mission_hash(*editor);
        // Components (csf/mission_components.hpp) and the one that made the selection.
        const auto components = csf::mission_components(*editor);
        s["count.components"] = number(components.size());
        std::optional<csf::MissionRecordId> owned;
        using Type = csf::MissionRecordId::Type;
        using Kind = MissionRecordKey::Kind;
        if (record.kind == Kind::actor) owned = csf::MissionRecordId{Type::actor, record.id};
        if (record.kind == Kind::nav_group || record.kind == Kind::nav_point)
            owned = csf::MissionRecordId{Type::navigation_group, record.id};
        if (record.kind == Kind::dummy) owned = csf::MissionRecordId{Type::dummy, record.id};
        if (record.kind == Kind::area) owned = csf::MissionRecordId{Type::area, record.id};
        s["selection.component"] = "";
        s["selection.component_state"] = "";
        for (const auto& component : components)
            if (owned && std::ranges::find(component.owns, *owned) != component.owns.end()) {
                s["selection.component"] = csf::component_title(component);
                s["selection.component_state"] =
                    csf::component_state(*editor, component) == csf::ComponentState::clean ? "clean" : "modified";
            }
    }
    if (const auto* scene = mission.scene.get()) {
        std::size_t points = 0;
        for (const auto& group : scene->navigation()) points += group.points.size();
        s["count.actors"] = number(scene->actors().size());
        s["count.dummies"] = number(scene->dummies().size());
        s["count.areas"] = number(scene->areas().size());
        s["count.lights"] = number(scene->lights().size());
        s["count.effects"] = number(scene->effects().size());
        s["count.nav_groups"] = number(scene->navigation().size());
        s["count.nav_points"] = number(points);
    }
    std::size_t scripts = 0;
    for (const auto& [path, program] : mission.programs) scripts += program.scripts().size();
    s["count.scripts"] = number(scripts);

    s["project.open"] = boolean(state.authoring.project != nullptr);
    s["project.name"] = state.authoring.project ? state.authoring.project->name : "";
    s["viewport.grid_preview_points"] = number(state.tools.preview_grid ? state.tools.grid_points.size() : 0);
    s["project.busy"] = boolean(authoring_pending(state));
    s["project.dirty"] = boolean(authoring_dirty(state));
    if (const auto* project = state.authoring.project.get()) {
        s["project.slot"] = project->slot.mission;
        s["project.builds"] = number(csf::archive_builds(*project).size());
        s["project.playtests"] = number(project->playtests.size());
        std::size_t active = 0;
        for (const auto& deployment : project->local.deployments)
            active += csf::deployment_state(deployment) == csf::DeploymentState::active;
        s["project.deployed"] = number(active);
    }
    s["count.placements"] = number(state.authoring.project ? state.authoring.project->placements.size() : 0);
    s["project.history.size"] = number(state.authoring.steps.size());
    s["project.history.position"] = number(state.authoring.cursor);
    s["project.history.top"] = state.authoring.cursor > 0 ? state.authoring.steps[state.authoring.cursor - 1].label : "";
    s["project.height_findings"] = number(state.authoring.findings.size());

    s["diagnostics.problems"] = number(state.diagnostic_problem_count());
    s["log.errors"] = number(state.log.count(LogLevel::error));
    s["log.warnings"] = number(state.log.count(LogLevel::warn));
    const auto latest = state.log.latest();
    s["log.last"] = latest ? latest->message : "";
    s["toast.count"] = number(state.toasts.size());
    s["toast.last"] = state.toasts.empty() ? "" : state.toasts.back().message;

    s["ui.palette"] = state.ui.palette == UiState::PaletteMode::closed     ? "closed"
                      : state.ui.palette == UiState::PaletteMode::commands ? "commands"
                                                                           : "go_to";
    s["ui.bottom_dock"] = boolean(state.settings.show_bottom_dock);
    for (int i = 0; i < static_cast<int>(ui::Panel::count); ++i) {
        const auto panel = static_cast<ui::Panel>(i);
        auto key = lower_ascii(ui::panel_title(panel));
        for (auto& c : key)
            if (c == ' ') c = '_';
        s["panel." + key] = boolean(window_visible(ui::panel_window_name(state.workspace, panel)));
    }
    const auto tool = state.preview.edit_tool();
    using Tool = GeometryPreview::EditTool;
    s["viewport.tool"] = tool == Tool::move     ? "move"
                         : tool == Tool::rotate ? "rotate"
                         : tool == Tool::place  ? "place"
                         : tool == Tool::route  ? "route"
                         : tool == Tool::zone   ? "zone"
                         : tool == Tool::cover  ? "cover"
                                                : "select";
    s["viewport.sketch"] = number(state.tools.sketch.size());
    s["viewport.picking"] = boolean(state.ui.pick.has_value());
    std::string targets;
    for (const auto& form : state.tools.objectives) targets += (targets.empty() ? "" : ",") + std::to_string(form.target);
    s["form.objective_targets"] = targets;
    s["viewport.place_asset"] = !state.tools.place_building_asset.empty() ? state.tools.place_building_asset
                                : state.tools.place_entry                ? state.tools.place_entry->name
                                                                         : "";
    s["viewport.projection"] = state.preview.projection() == 0 ? "perspective" : "orthographic";
    return s;
}

} // namespace rwsman
