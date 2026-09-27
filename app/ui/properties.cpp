// Properties (docs/plans/editor-ux-redesign.md, B4): what the selected record
// is and everything an author edits on it, as cards. File offsets, entry
// numbers and provenance appear only with Developer details (Preferences) and
// always in the Inspect mode's Inspector.
#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_util.hpp"
#include "authoring.hpp"
#include "commands.hpp"
#include "mission_authoring.hpp"
#include "mission_editing.hpp"
#include "navigation.hpp"
#include "viewport_tools.hpp"
#include "references.hpp"
#include "rwsman/entity_kind.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <map>

namespace rwsman::ui {
namespace {

// The kind of a selected record, as the Outliner shows it.
EntityKind record_kind(const AppState& state, const MissionRecordKey& key) {
    using Kind = MissionRecordKey::Kind;
    const auto& scene = *state.mission.scene;
    switch (key.kind) {
    case Kind::actor:
        for (const auto& actor : scene.actors())
            if (actor.id == key.id) return classify_mission_actor(scene, state.mission.objects.get(), actor);
        return EntityKind::unresolved;
    case Kind::nav_group:
    case Kind::nav_point:
        for (const auto& group : scene.navigation())
            if (group.id == key.id) return classify_nav_group(group.type, group.name.value_or(""), false);
        return EntityKind::route;
    case Kind::area: return EntityKind::zone;
    case Kind::dummy: return EntityKind::marker;
    case Kind::light: return EntityKind::light;
    case Kind::effect: return EntityKind::effect;
    case Kind::placement:
        if (const auto* project = state.authoring.project.get();
            project && key.id >= 0 && static_cast<std::size_t>(key.id) < project->placements.size() &&
            project->placements[static_cast<std::size_t>(key.id)].kind == csf::ProjectPlacement::Kind::prop)
            return EntityKind::vegetation;
        return EntityKind::building;
    case Kind::none: break;
    }
    return EntityKind::prop;
}

// The kind icon and name in the title size, a caption under it, and (for a
// selected record) quick actions at the right of the title line.
void header(AppState& state, const EntityKind kind, const std::string& title, const std::string& subtitle,
            const bool actions) {
    const float scale = ui_scale();
    const float actions_width = actions ? 3.0F * 30.0F * scale : 0.0F;
    const float top = ImGui::GetCursorPosY();
    ImGui::PushFont(font(Font::title));
    ImGui::PushStyleColor(ImGuiCol_Text, kind_color(kind));
    ImGui::TextUnformatted(kind_icon(kind));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - actions_width);
    ImGui::TextUnformatted(title.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    const float below = ImGui::GetCursorPosY();
    ImGui::PushFont(font(Font::caption));
    ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
    ImGui::TextWrapped("%s", subtitle.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (actions) {
        const float after = ImGui::GetCursorPosY();
        ImGui::SetCursorPos({ImGui::GetContentRegionMax().x - actions_width, top + (below - top) * 0.2F});
        if (icon_button("##frame", icons::LC_SCAN, "Frame in the viewport (F)")) state.commands.run("view.frame_selection");
        ImGui::SameLine(0.0F, 2.0F);
        if (icon_button("##duplicate", icons::LC_COPY, "Duplicate (Ctrl+D)")) state.commands.run("mission.duplicate");
        ImGui::SameLine(0.0F, 2.0F);
        if (icon_button("##references", icons::LC_LINK, "Show every reference")) state.commands.run("mission.references");
        ImGui::SetCursorPosY(after);
    }
    ImGui::Spacing();
}

void used_by(AppState& state) {
    // Scripts that use the record; the database's own cross-references are
    // developer details.
    auto rows = collect_references(state, state.selection);
    if (!state.settings.developer_details)
        std::erase_if(rows, [](const ReferenceRow& row) { return row.group != "Script uses"; });
    CardOptions options{icons::LC_LINK};
    options.subtitle = rows.empty() ? "nothing" : nullptr;
    options.default_open = !rows.empty();
    options.help = "Scripts, objectives and records that refer to this one. Deleting it is refused while scripts "
                   "use it.";
    std::string title = "Used by";
    if (!rows.empty()) title += " (" + std::to_string(rows.size()) + ")";
    if (!begin_card("##used_by", title.c_str(), options)) return;
    if (rows.empty()) dim_text("Nothing in the loaded scripts and databases refers to it.");
    int index = 0;
    for (const auto& row : rows) {
        ImGui::PushID(index++);
        if (ImGui::Selectable(row.label.c_str(), false) && !row.target.empty()) navigate_to(state, row.target);
        if (!row.detail.empty()) {
            ImGui::SameLine();
            ImGui::PushFont(font(Font::caption));
            dim_text("%s", row.detail.c_str());
            ImGui::PopFont();
        }
        ImGui::PopID();
    }
    end_card();
}

// Nothing selected: the mission at a glance, with the next steps.
void overview(AppState& state) {
    const auto& scene = *state.mission.scene;
    const std::string name = state.authoring.project && !state.authoring.project->name.empty()
                                 ? state.authoring.project->name
                                 : path_utf8(state.mission.graph->scene_path().stem());
    header(state, EntityKind::objective, name, "Nothing selected: click something in the viewport or the Outliner.",
           false);
    if (begin_card("##contents", "Contents", {icons::LC_LIST_TREE})) {
        std::map<std::string, std::size_t> counts;
        counts["Actors"] = scene.actors().size();
        counts["Zones"] = scene.areas().size();
        counts["Navigation groups"] = scene.navigation().size();
        counts["Markers"] = scene.dummies().size();
        if (const auto* flow = mission_flow(state)) counts["Objectives"] = flow->objectives().size();
        if (begin_properties("##counts")) {
            for (const auto& [label, count] : counts) {
                property_row(label.c_str());
                ImGui::Text("%zu", count);
            }
            end_properties();
        }
        end_card();
    }
    const auto errors = count_problems(state.problems, Problem::Severity::error);
    const auto warnings = count_problems(state.problems, Problem::Severity::warning);
    if (begin_card("##health", "Problems", {errors ? icons::LC_CIRCLE_ALERT : icons::LC_CIRCLE_CHECK,
                                            color(errors ? Token::error : warnings ? Token::warn : Token::ok)})) {
        if (errors + warnings == 0)
            dim_text("No problems found.");
        else
            ImGui::Text("%zu errors, %zu warnings", errors, warnings);
        if (secondary_button("Show problems")) show_panel(state, Panel::problems);
        end_card();
    }
    if (begin_card("##next", "Add to the mission", {icons::LC_PLUS})) {
        if (secondary_button((std::string(icons::LC_PACKAGE) + " Assets").c_str())) show_panel(state, Panel::assets);
        ImGui::SameLine();
        if (secondary_button((std::string(icons::LC_ROUTE) + " Behaviours").c_str()))
            show_panel(state, Panel::behaviours);
        ImGui::SameLine();
        if (secondary_button((std::string(icons::LC_FLAG) + " Objectives").c_str()))
            show_panel(state, Panel::objectives);
        ImGui::SameLine();
        if (secondary_button((std::string(icons::LC_VIDEO) + " Intro").c_str())) show_panel(state, Panel::timeline);
        end_card();
    }
}

// A project placement: built into the map, so every change rebuilds it.
void placement_editor(AppState& state, const std::size_t index) {
    const auto& placement = state.authoring.project->placements[index];
    const auto edit = [&](const std::string& label, const auto& change) {
        edit_authoring_project(state, label + " " + placement.id, [&](csf::AuthoringProject& project) {
            change(project.placements.at(index));
            return true;
        });
    };
    CardOptions options{icons::LC_MOVE_3D};
    options.help = "Where the map places it. Changes are saved to project.csfproj and rebuild the map in the "
                   "background; undo works as for any other edit.";
    if (begin_card("##placement", "Placement", options)) {
        if (begin_properties("##placement_rows")) {
            if (placement.kind == csf::ProjectPlacement::Kind::piece) {
                // What it was cut from: another mission's map (a donor), or the slot's own.
                property_row("Cut from", "A piece of a shipped map's World: the triangles inside its box, kept "
                                         "with their textures and baked lightmaps.");
                const auto& donors = state.authoring.project->donors;
                const auto donor = std::ranges::find(donors, placement.donor, &csf::ProjectDonor::key);
                if (placement.donor.empty())
                    ImGui::TextUnformatted("the slot's own map");
                else if (donor == donors.end())
                    token_text(Token::error, "donor %s is missing", placement.donor.c_str());
                else
                    ImGui::Text("%s's map  (%s)", donor->mission.c_str(), path_utf8(donor->visual.filename()).c_str());
                property_row("Groups", "The lightmap groups it keeps: a building is its lightmap group "
                                       "(EDIFICIO_5), its inside and its furniture have their own.");
                std::string groups;
                for (const auto& group : placement.lightmaps) groups += (groups.empty() ? "" : ", ") + group;
                ImGui::TextWrapped("%s", groups.empty() ? "everything in the box" : groups.c_str());
                property_row("Box", "The part of the donor map it takes (x, y, z), in the donor's coordinates.");
                dim_text("%.0f, %.0f, %.0f  to  %.0f, %.0f, %.0f", placement.box_min.x, placement.box_min.y,
                         placement.box_min.z, placement.box_max.x, placement.box_max.y, placement.box_max.z);
            } else {
                property_row("Asset");
                ImGui::TextUnformatted(placement.kind == csf::ProjectPlacement::Kind::building ? placement.asset.c_str()
                                                                                               : "donor map props");
            }
            property_row("Position", "Game units (centimetres); Y is up.");
            float position[3]{placement.position.x, placement.position.y, placement.position.z};
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::DragFloat3("##position", position, 5.0F, 0.0F, 0.0F, "%.0f");
            if (ImGui::IsItemDeactivatedAfterEdit())
                edit("Move", [&](csf::ProjectPlacement& edited) { edited.position = {position[0], position[1], position[2]}; });
            property_row("Heading");
            float yaw = placement.yaw_degrees;
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::DragFloat("##yaw", &yaw, 1.0F, -180.0F, 180.0F, "%.0f deg");
            if (ImGui::IsItemDeactivatedAfterEdit()) edit("Turn", [&](csf::ProjectPlacement& edited) { edited.yaw_degrees = yaw; });
            property_row("Height", "Ground: stands on the terrain (plus the offset), rebuilt when the terrain "
                                   "changes. Absolute: stays at its Y. On: stands on another placement.");
            static constexpr const char* modes[]{"Absolute", "Ground", "On another placement"};
            int mode = static_cast<int>(placement.height.mode);
            ImGui::SetNextItemWidth(-1.0F);
            if (ImGui::Combo("##height_mode", &mode, modes, IM_ARRAYSIZE(modes)) &&
                mode != static_cast<int>(csf::HeightRule::Mode::on))
                edit("Height rule", [&](csf::ProjectPlacement& edited) {
                    edited.height.mode = static_cast<csf::HeightRule::Mode>(mode);
                    edited.height.support_kind.clear();
                    edited.height.support_id.clear();
                });
            if (placement.height.mode == csf::HeightRule::Mode::on) {
                property_row("Stands on");
                ImGui::Text("%s %s", placement.height.support_kind.c_str(), placement.height.support_id.c_str());
            }
            if (placement.height.mode != csf::HeightRule::Mode::absolute) {
                property_row("Offset");
                float offset = placement.height.offset;
                ImGui::SetNextItemWidth(-1.0F);
                ImGui::DragFloat("##offset", &offset, 1.0F, 0.0F, 0.0F, "%.0f");
                if (ImGui::IsItemDeactivatedAfterEdit())
                    edit("Height offset", [&](csf::ProjectPlacement& edited) { edited.height.offset = offset; });
            }
            end_properties();
        }
        if (state.authoring.job.valid()) {
            ImGui::PushFont(font(Font::caption));
            dim_text("The map is rebuilding.");
            ImGui::PopFont();
        }
        end_card();
    }
}

// An actor's height rule in the authoring project: the build keeps it on the
// ground, or on another actor or a placement (a radio on a crate).
void anchor_card(AppState& state, const std::int32_t actor_id) {
    auto* project = state.authoring.project.get();
    if (!project) return;
    // A copy: an edit below replaces the project's anchors.
    std::optional<csf::HeightRule> current;
    if (const auto found = std::ranges::find(project->anchors, actor_id, &csf::ProjectAnchor::actor_id);
        found != project->anchors.end())
        current = found->height;
    const bool anchored = current.has_value();
    CardOptions options{icons::LC_MAGNET};
    options.subtitle = !anchored ? "free" : current->mode == csf::HeightRule::Mode::ground ? "on the ground"
                       : current->mode == csf::HeightRule::Mode::on                    ? "on another record"
                                                                                             : "fixed";
    options.default_open = anchored;
    options.help = "What the actor stands on. The project keeps it there when the terrain or its support moves "
                   "(the height report lists the ones that are off).";
    if (!begin_card("##anchor", "Height", options)) return;
    const auto set = [&](const std::string& label, const std::optional<csf::HeightRule>& rule) {
        edit_authoring_project(state, label, [&](csf::AuthoringProject& edited) {
            std::erase_if(edited.anchors, [&](const csf::ProjectAnchor& anchor) { return anchor.actor_id == actor_id; });
            if (rule) edited.anchors.push_back({actor_id, *rule});
            return true;
        });
    };
    static constexpr const char* modes[]{"Free (not kept)", "On the ground", "On another record"};
    int mode = !anchored ? 0 : current->mode == csf::HeightRule::Mode::on ? 2 : 1;
    const float offset = anchored ? current->offset : 0.0F;
    // What the actor stands on, from a pick in the viewport or the Outliner.
    const auto stand_on = [&state, actor_id, offset](const MissionRecordKey& key) {
        csf::HeightRule rule{csf::HeightRule::Mode::on, offset, "actor", std::to_string(key.id)};
        if (key.kind == MissionRecordKey::Kind::placement) {
            const auto& placement =
                state.authoring.project->placements.at(static_cast<std::size_t>(key.id));
            rule.support_kind = placement.kind == csf::ProjectPlacement::Kind::building ? "building"
                                : placement.kind == csf::ProjectPlacement::Kind::piece  ? "piece"
                                                                                       : "prop";
            rule.support_id = placement.id;
        }
        edit_authoring_project(state, "Stand actor " + std::to_string(actor_id) + " on " +
                                          rule.support_kind + " " + rule.support_id,
                               [&](csf::AuthoringProject& edited) {
                                   std::erase_if(edited.anchors, [&](const csf::ProjectAnchor& a) {
                                       return a.actor_id == actor_id;
                                   });
                                   edited.anchors.push_back({actor_id, rule});
                                   return true;
                               });
    };
    const std::vector kinds{MissionRecordKey::Kind::actor, MissionRecordKey::Kind::placement};
    if (begin_properties("##anchor_rows")) {
        property_row("Stands");
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::Combo("##anchor_mode", &mode, modes, IM_ARRAYSIZE(modes))) {
            if (mode == 0) set("Free actor " + std::to_string(actor_id), std::nullopt);
            if (mode == 1)
                set("Keep actor " + std::to_string(actor_id) + " on the ground",
                    csf::HeightRule{csf::HeightRule::Mode::ground, offset, {}, {}});
            // As the eyedropper below: the next record selected is the support.
            if (mode == 2)
                request_pick(state, kinds, "Pick what the actor stands on", stand_on,
                             "##pick_support/" + std::to_string(ImGui::GetID("##pick_support")));
        }
        const bool picking = state.ui.pick && state.ui.pick->field.starts_with("##pick_support/");
        if (picking || (anchored && current->mode == csf::HeightRule::Mode::on)) {
            property_row("On", "Pick the actor or project placement it stands on.");
            const auto support = anchored && current->mode == csf::HeightRule::Mode::on
                                     ? current->support_kind + " " + current->support_id
                                     : std::string("(pick one)");
            ImGui::TextUnformatted(support.c_str());
            ImGui::SameLine();
            pick_button(state, "##pick_support", kinds, "Pick what the actor stands on", stand_on);
        }
        if (anchored) {
            property_row("Offset", "Centimetres above the ground or the support.");
            float value = offset;
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::DragFloat("##anchor_offset", &value, 1.0F, 0.0F, 0.0F, "%.1f");
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                auto rule = *current;
                rule.offset = value;
                set("Height offset of actor " + std::to_string(actor_id), rule);
            }
        }
        end_properties();
    }
    end_card();
}

} // namespace

void draw_properties(AppState& state) {
    if (!state.mission.scene || !state.mission.graph) {
        empty_state(icons::LC_SLIDERS_HORIZONTAL, "Open a mission or a project to see and edit its properties.");
        return;
    }
    const auto key = selected_mission_record(state);
    // Another record starts at its top card (its Behaviour, its component).
    static MissionRecordKey shown;
    if (!(key == shown)) {
        shown = key;
        ImGui::SetScrollY(0.0F);
    }
    if (key.kind == MissionRecordKey::Kind::none) {
        // Scripts, classes and resources keep their Inspector view.
        if (!state.selection.empty() && state.selection.kind != SelectionRef::Kind::mission_entry)
            draw_inspector(state);
        else
            overview(state);
        return;
    }
    if (key.kind == MissionRecordKey::Kind::placement) {
        const auto index = static_cast<std::size_t>(key.id);
        if (!state.authoring.project || index >= state.authoring.project->placements.size()) {
            dim_text("The selected placement is no longer in the project.");
            return;
        }
        header(state, record_kind(state, key), state.authoring.project->placements[index].id,
               "Project placement  ·  part of the map", true);
        placement_editor(state, index);
        return;
    }
    const auto entry = mission_record_entry(*state.mission.scene, key);
    if (!entry) {
        dim_text("The selected record is no longer in the mission.");
        return;
    }
    const auto kind = record_kind(state, key);
    std::string subtitle = entity_kind_name(kind);
    subtitle += "  ·  ID " + std::to_string(key.id);
    if (key.kind == MissionRecordKey::Kind::nav_point) subtitle += "/" + std::to_string(key.sub_id);
    header(state, kind, selection_title(state, state.selection), subtitle, true);
    draw_component_card(state, key);
    if (mission_editable(state))
        draw_record_editor(state, *entry);
    else
        dim_text("The mission is loading.");
    if (key.kind == MissionRecordKey::Kind::actor) anchor_card(state, key.id);
    used_by(state);
    if (state.settings.developer_details) {
        CardOptions options{icons::LC_BINARY};
        options.default_open = false;
        options.help = "Where the record is stored and how sure the decoder is about each value (the Inspect "
                       "mode's view).";
        if (begin_card("##developer", "Developer details", options)) {
            dim_text("%s", selection_identity(state, state.selection).c_str());
            draw_mission_record(state, *entry, false);
            end_card();
        }
    }
}

} // namespace rwsman::ui
