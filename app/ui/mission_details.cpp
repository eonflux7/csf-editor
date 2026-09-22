#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_util.hpp"
#include "navigation.hpp"
#include "references.hpp"
#include "rwsman/mission_lookup.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/property_grid.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace rwsman::ui {

void draw_csf_subtree(const csf::Document& document, const csf::Node& node) {
    std::string name = "(anonymous)";
    if (node.identifier_index)
        if (const auto* value = document.identifier(*node.identifier_index))
            name = value->display_utf8();
    std::string value;
    if (const auto* integer = std::get_if<std::int32_t>(&node.scalar))
        value = " = " + std::to_string(*integer);
    else if (const auto* real = std::get_if<float>(&node.scalar))
        value = " = " + std::to_string(*real);
    else if (const auto* index = std::get_if<std::uint32_t>(&node.scalar))
        if (const auto* text = document.string(*index))
            value = " = \"" + text->display_utf8() + "\"";
    const auto text = name + value + "  [entry " + std::to_string(node.entry_index) + "]";
    ImGui::PushID(static_cast<int>(node.entry_index));
    if (node.children.empty())
        ImGui::BulletText("%s", text.c_str());
    else if (ImGui::TreeNodeEx(text.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const auto& child : node.children)
            draw_csf_subtree(document, child);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

namespace {

using Options = PropertyGrid::Options;

// Everything a record section needs to draw fields with raw-byte disclosure.
struct RecordContext {
    AppState& state;
    const csf::Document& document;
    const csf::Node* record{};
    const csf::CsfSourceId& source;

    // Field value that the projection parsed with the expected type: exact.
    [[nodiscard]] Options proven(const char* field, std::string evidence = {}) const {
        Options options;
        options.provenance = Provenance::proven;
        options.evidence = evidence.empty() ? std::string("Parsed from ") + field + " with the expected type"
                                            : std::move(evidence);
        if (record && field && *field) options.raw = raw_slice_for_field(document, *record, field);
        return options;
    }
    // Field whose value parses but whose meaning is not decoded: preserved raw.
    [[nodiscard]] Options unknown(const char* field, std::string evidence = {}) const {
        Options options = proven(field);
        options.provenance = Provenance::unknown;
        options.evidence = evidence.empty() ? std::string("Stored in ") + field + "; its meaning is not decoded"
                                            : std::move(evidence);
        return options;
    }
    [[nodiscard]] Options inferred(const char* field, std::string evidence) const {
        Options options = proven(field);
        options.provenance = Provenance::inferred;
        options.evidence = std::move(evidence);
        return options;
    }
    [[nodiscard]] Options derived(Provenance provenance, std::string evidence) const {
        Options options;
        options.provenance = provenance;
        options.evidence = std::move(evidence);
        return options;
    }
};

Options absent() {
    Options options;
    options.show_badge = false;
    return options;
}

void opt_int(PropertyGrid& grid, const char* key, const std::optional<std::int32_t>& value,
             const Options& options) {
    if (value)
        grid.integer(key, *value, options);
    else
        grid.text(key, "", absent());
}

void opt_string(PropertyGrid& grid, const char* key, const std::optional<std::string>& value,
                const Options& options) {
    if (value)
        grid.text(key, *value, options);
    else
        grid.text(key, "", absent());
}

void opt_real(PropertyGrid& grid, const char* key, const std::optional<float>& value, const Options& options) {
    if (value)
        grid.real(key, *value, options);
    else
        grid.text(key, "", absent());
}

void opt_vec3(PropertyGrid& grid, const char* key, const std::optional<csf::Vec3>& value,
              const Options& options) {
    if (value)
        grid.vec3(key, value->x, value->y, value->z, options);
    else
        grid.text(key, "", absent());
}

void draw_unsupported(AppState& state, const std::vector<csf::RawField>& fields) {
    if (fields.empty()) return;
    if (!begin_section(state, "Unsupported fields", false)) return;
    for (const auto& field : fields) {
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::raw));
        ImGui::PushFont(font(Font::mono));
        ImGui::Text("? %s", field.name.empty() ? "(anonymous)" : field.name.c_str());
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
        ImGui::Text("entry %u", field.source.entry_index);
        ImGui::PopStyleColor();
    }
    ImGui::TextDisabled("Preserved as raw fields; no meaning is inferred.");
    end_section();
}

void draw_raw_subtree(RecordContext& context) {
    if (!context.record) return;
    if (!begin_section(context.state, "Raw", false)) return;
    const auto slice = raw_slice_for_entry(context.document, context.source.entry_index);
    if (slice.valid()) {
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
        ImGui::PushFont(font(Font::mono));
        ImGui::Text("entry %u @0x%llX, %llu bytes", context.source.entry_index,
                    static_cast<unsigned long long>(slice.offset), static_cast<unsigned long long>(slice.size));
        ImGui::PopFont();
        ImGui::PopStyleColor();
        const auto bytes = context.document.bytes();
        if (slice.offset < bytes.size())
            draw_hex_dump(bytes.subspan(static_cast<std::size_t>(slice.offset),
                                        static_cast<std::size_t>(std::min<std::uint64_t>(
                                            slice.size, bytes.size() - slice.offset))),
                          slice.offset, 64);
    }
    ImGui::Spacing();
    draw_csf_subtree(context.document, *context.record);
    end_section();
}

void draw_references_summary(AppState& state) {
    const auto rows = collect_references(state, state.selection);
    if (rows.empty()) return;
    if (!begin_section(state, "References", true)) return;
    std::size_t by_group[3]{};
    for (const auto& row : rows) {
        if (row.group == "Script uses")
            ++by_group[0];
        else if (row.group == "Definitions and exact uses")
            ++by_group[1];
        else
            ++by_group[2];
    }
    if (by_group[0] > 0)
        dim_text("Used by %zu script instruction%s", by_group[0], by_group[0] == 1 ? "" : "s");
    if (by_group[1] > 0) dim_text("%zu exact definition or use site%s", by_group[1], by_group[1] == 1 ? "" : "s");
    if (by_group[2] > 0) dim_text("%zu other reference%s", by_group[2], by_group[2] == 1 ? "" : "s");
    if (ImGui::SmallButton("Open References panel")) state.commands.run("view.references");
    end_section();
}

const csf::NavPoint* find_point(const csf::MissionScene& scene, const std::optional<std::int32_t> group,
                                const std::optional<std::int32_t> point) {
    if (!group || !point) return nullptr;
    for (const auto& value : scene.navigation())
        for (const auto& candidate : value.points)
            if (candidate.group_id == group && candidate.id == point) return &candidate;
    return nullptr;
}

std::string script_label(const AppState& state, const std::pair<std::size_t, std::size_t>& script) {
    const auto& value = state.mission.programs[script.first].second.scripts()[script.second];
    return value.name + " [" + std::to_string(value.id) + "]";
}

void draw_actor(AppState& state, RecordContext& context, const csf::MissionActor& actor) {
    const auto& scene = *state.mission.scene;
    if (begin_section(state, "Identity", true)) {
        if (PropertyGrid grid(state, "##actor_identity"); grid) {
            opt_string(grid, "name", actor.name, context.proven(".NOMBRE"));
            opt_int(grid, "ID", actor.id, context.proven(".ID"));
            const auto association = std::ranges::find_if(state.mission.actor_associations, [&](const auto& value) {
                return value.actor.entry_index == context.source.entry_index;
            });
            if (actor.class_id) {
                SelectionRef target;
                std::string evidence = "Scene .CLASSID";
                Provenance provenance = Provenance::proven;
                if (association != state.mission.actor_associations.end() && association->definitions.size() == 1) {
                    const auto& definition = *association->definitions.front();
                    target = SelectionRef::database_record(path_utf8(definition.source.file),
                                                           definition.source.entry_index);
                    evidence += "; resolves to exactly one Objetos.bdd record";
                } else if (association != state.mission.actor_associations.end()) {
                    provenance = Provenance::diagnosed;
                    evidence += association->definitions.empty() ? "; no Objetos.bdd record has this class"
                                                                 : "; several Objetos.bdd records share this class";
                }
                char label[48];
                std::snprintf(label, sizeof(label), "%d (0x%X)", *actor.class_id,
                              static_cast<unsigned>(*actor.class_id));
                grid.link("class", label, target, context.derived(provenance, evidence));
            }
            if (actor.faction) grid.text("faction", *actor.faction, context.proven(".BANDO"));
            if (actor.portrait) grid.text("portrait", *actor.portrait, context.proven(".PORTRAIT"));
        }
        end_section();
    }
    if (begin_section(state, "Transform", true)) {
        if (PropertyGrid grid(state, "##actor_transform"); grid) {
            opt_vec3(grid, "authored .POS", actor.position, context.proven(".POS"));
            if (const auto spawn = scene.actor_spawn_position(actor)) {
                const bool from_cell = actor.group && actor.cell && *actor.group >= 0 && *actor.cell >= 0;
                grid.vec3("effective spawn", spawn->x, spawn->y, spawn->z,
                          from_cell ? context.derived(Provenance::proven, "Position of navigation cell .CELDA")
                                    : context.derived(Provenance::inferred,
                                                      "No valid .CELDA; falling back to the authored .POS"));
            }
            if (actor.heading) grid.angle("heading", *actor.heading, true, context.proven(".ANGULO"));
            if (actor.pitch) grid.angle("pitch", *actor.pitch, true, context.proven(".ANGULO_X"));
        }
        end_section();
    }
    if (begin_section(state, "Navigation", true)) {
        if (PropertyGrid grid(state, "##actor_navigation"); grid) {
            char cell[32];
            std::snprintf(cell, sizeof(cell), "%d:%d", actor.group.value_or(-1), actor.cell.value_or(-1));
            const auto* point = find_point(scene, actor.group, actor.cell);
            grid.link("cell", cell, point ? SelectionRef::mission_entry(point->source.entry_index) : SelectionRef{},
                      context.derived(point ? Provenance::proven : Provenance::diagnosed,
                                      point ? "Navigation point exists in the scene"
                                            : "No navigation point has this group:point identity"));
        }
        end_section();
    }
    if (begin_section(state, "Behavior", true)) {
        if (PropertyGrid grid(state, "##actor_behavior"); grid) {
            opt_int(grid, "collision", actor.collision, context.unknown(".COLISION"));
            if (actor.flags) grid.integer("flags", *actor.flags, context.unknown(".FLAGS"));
            opt_int(grid, "secondary explosion", actor.secondary_explosion, context.unknown(".SEGUNDA_EXPLOSION"));
            if (actor.door_box) {
                const auto& box = *actor.door_box;
                grid.vec3("door box min", box[0].x, box[0].y, box[0].z, context.proven(".DOOR_BOX"));
                grid.vec3("door box max", box[1].x, box[1].y, box[1].z, context.proven(".DOOR_BOX"));
            }
        }
        end_section();
    }
    if (!actor.script_ids.empty() || actor.script) {
        if (begin_section(state, "Scripts", true)) {
            if (PropertyGrid grid(state, "##actor_scripts"); grid) {
                if (actor.script) grid.text("script", *actor.script, context.proven(".SCRIPT"));
                for (const auto id : actor.script_ids) {
                    std::vector<std::pair<std::size_t, std::size_t>> found;
                    for (std::size_t document = 0; document < state.mission.programs.size(); ++document) {
                        const auto& scripts = state.mission.programs[document].second.scripts();
                        for (std::size_t script = 0; script < scripts.size(); ++script)
                            if (scripts[script].id == id) found.emplace_back(document, script);
                    }
                    const auto label = "id " + std::to_string(id);
                    if (found.size() == 1)
                        grid.link(label.c_str(), script_label(state, found.front()),
                                  SelectionRef::program_script(found.front().first, found.front().second),
                                  context.derived(Provenance::proven, "Exactly one script has this ID"));
                    else
                        grid.link(label.c_str(), found.empty() ? "no script with this ID" : "ambiguous script ID",
                                  found.empty() ? SelectionRef{}
                                                : SelectionRef::program_script(found.front().first, found.front().second),
                                  context.derived(Provenance::diagnosed,
                                                  found.empty() ? "No loaded script document defines this ID"
                                                                : "Several script documents define this ID"));
                }
            }
            end_section();
        }
    }
    if (!actor.animations.empty()) {
        if (begin_section(state, "Animation bindings", true)) {
            if (PropertyGrid grid(state, "##actor_animations"); grid) {
                for (const auto& binding : actor.animations) {
                    const std::string key = "anim " + std::to_string(binding.id.value_or(-1));
                    SelectionRef target;
                    if (binding.id && state.mission.animations)
                        if (const auto* record = state.mission.animations->find_id(*binding.id))
                            target = SelectionRef::database_record(path_utf8(record->source.file),
                                                                   record->source.entry_index);
                    grid.link(key.c_str(), binding.type.value_or("(unnamed)"), target,
                              context.derived(target.empty() ? Provenance::diagnosed : Provenance::proven,
                                              target.empty() ? "No Anims.bdd record has this ID"
                                                             : "Anims.bdd record found by ID"));
                }
            }
            end_section();
        }
    }
    const auto association = std::ranges::find_if(state.mission.actor_associations, [&](const auto& value) {
        return value.actor.entry_index == context.source.entry_index;
    });
    if (association != state.mission.actor_associations.end()) {
        if (begin_section(state, "Model and physics", true)) {
            if (PropertyGrid grid(state, "##actor_model"); grid) {
                const auto evidence_rows = [&](const char* key, const std::vector<csf::AssociationEvidence>& values) {
                    for (const auto& value : values) {
                        std::string evidence = std::string("Rule: ") + value.rule + "; " +
                                               csf::resolution_status_name(value.resolution.status);
                        grid.link(key, value.resolution.original_reference,
                                  value.resolved_path ? SelectionRef::resource_path(path_utf8(*value.resolved_path))
                                                      : SelectionRef{},
                                  context.derived(provenance_of(value.resolution.status), evidence));
                    }
                };
                evidence_rows("visual model", association->visual_models);
                evidence_rows("LOD variant", association->lod_models);
                evidence_rows("CMO collision", association->collision_models);
                evidence_rows("physics", association->physics_models);
                evidence_rows("ragdoll", association->ragdolls);
                evidence_rows("animation", association->animations);
                if (association->definitions.size() == 1 && !association->definitions.front()->weapon_ids.empty()) {
                    std::string ids;
                    for (const auto id : association->definitions.front()->weapon_ids) {
                        if (!ids.empty()) ids += ", ";
                        ids += std::to_string(id);
                    }
                    grid.text("default weapons", ids, context.derived(Provenance::inferred,
                                                                     "Inventory order in Objetos.bdd; the first "
                                                                     "resolved third-person model is previewed"));
                }
            }
            for (const auto& diagnostic : association->diagnostics)
                token_text(Token::warn, "%s %s", icons::LC_TRIANGLE_ALERT, diagnostic.c_str());
            end_section();
        }
    }
}

void draw_nav_group(AppState& state, RecordContext& context, const csf::NavGroup& group) {
    if (begin_section(state, "Identity", true)) {
        if (PropertyGrid grid(state, "##nav_group"); grid) {
            opt_int(grid, "ID", group.id, context.proven(".ID"));
            opt_string(grid, "name", group.name, context.proven(".NOMBRE"));
            opt_int(grid, "type", group.type, context.unknown(".TIPO"));
            grid.integer("points", static_cast<std::int64_t>(group.points.size()), context.derived(Provenance::proven, "Count of .PUNTOS entries"));
            grid.integer("local links", static_cast<std::int64_t>(group.connections.size()), context.derived(Provenance::proven, "Count of .CONEXIONES entries"));
        }
        end_section();
    }
}

void draw_nav_point(AppState& state, RecordContext& context, const csf::NavPoint& point) {
    const auto& scene = *state.mission.scene;
    if (begin_section(state, "Identity", true)) {
        if (PropertyGrid grid(state, "##nav_point"); grid) {
            char identity[32];
            std::snprintf(identity, sizeof(identity), "%d:%d", point.group_id.value_or(-1), point.id.value_or(-1));
            grid.text("identity", identity, context.derived(Provenance::proven, "group ID : point ID"));
            opt_string(grid, "name", point.name, context.proven(".NOMBRE"));
            opt_vec3(grid, "position", point.position, context.proven(".POS"));
            if (point.heading) grid.angle("heading", *point.heading, false, context.proven(".ROT"));
            if (point.pitch) grid.angle("pitch", *point.pitch, false, context.proven(".ROT_X"));
            std::size_t incoming{}, outgoing{};
            const auto count = [&](const csf::NavConnection& link) {
                if (link.origin_group == point.group_id && link.origin_point == point.id) ++outgoing;
                if (link.destination_group == point.group_id && link.destination_point == point.id) ++incoming;
            };
            for (const auto& owner : scene.navigation())
                for (const auto& link : owner.connections) count(link);
            for (const auto& link : scene.cross_group_connections()) count(link);
            grid.integer("incoming links", static_cast<std::int64_t>(incoming), context.derived(Provenance::proven, "Counted over local and cross-group links"));
            grid.integer("outgoing links", static_cast<std::int64_t>(outgoing), context.derived(Provenance::proven, "Counted over local and cross-group links"));
        }
        end_section();
    }
}

void draw_connection(AppState& state, RecordContext& context, const csf::NavConnection& link) {
    const auto& scene = *state.mission.scene;
    if (begin_section(state, "Link", true)) {
        if (PropertyGrid grid(state, "##nav_link"); grid) {
            const auto describe = [&](const char* key, const std::optional<std::int32_t>& group,
                                      const std::optional<std::int32_t>& point) {
                char label[32];
                std::snprintf(label, sizeof(label), "%d:%d", group.value_or(-1), point.value_or(-1));
                const auto* target = find_point(scene, group, point);
                grid.link(key, label, target ? SelectionRef::mission_entry(target->source.entry_index) : SelectionRef{},
                          context.derived(target ? Provenance::proven : Provenance::diagnosed,
                                          target ? "Navigation point exists" : "No navigation point has this identity"));
            };
            describe("origin", link.origin_group, link.origin_point);
            describe("destination", link.destination_group, link.destination_point);
            grid.text("status", link.valid ? "valid" : link.invalid_reason,
                      context.derived(link.valid ? Provenance::proven : Provenance::diagnosed,
                                      link.valid ? "Both endpoints resolve" : link.invalid_reason));
        }
        end_section();
    }
}

void draw_dummy(AppState& state, RecordContext& context, const csf::MissionDummy& dummy) {
    const auto& scene = *state.mission.scene;
    if (begin_section(state, "Identity", true)) {
        if (PropertyGrid grid(state, "##dummy"); grid) {
            opt_int(grid, "ID", dummy.id, context.proven(".ID"));
            opt_string(grid, "name", dummy.name, context.proven(".NOMBRE"));
            opt_vec3(grid, "position", dummy.position, context.proven(".POS"));
            if (dummy.heading) grid.angle("heading", *dummy.heading, false, context.proven(".ROT"));
            if (dummy.pitch) grid.angle("pitch", *dummy.pitch, false, context.proven(".ROT_X"));
            for (const auto& folder : scene.folders())
                if (dummy.id && std::ranges::find(folder.element_ids, *dummy.id) != folder.element_ids.end())
                    grid.text("folder", folder.path.empty() ? "(root)" : folder.path,
                              context.derived(Provenance::proven, "Folder membership lists this dummy ID"));
            for (const auto& effect : scene.effects())
                if (effect.dummy_id == dummy.id)
                    grid.link("effect", effect.name.value_or("(unnamed)"),
                              SelectionRef::mission_entry(effect.source.entry_index),
                              context.derived(Provenance::proven, "Effect .DUMMY references this dummy"));
        }
        end_section();
    }
}

void draw_area(AppState& state, RecordContext& context, const csf::MissionArea& area) {
    if (begin_section(state, "Identity", true)) {
        if (PropertyGrid grid(state, "##area"); grid) {
            opt_int(grid, "ID", area.id, context.proven(".ID"));
            opt_string(grid, "name", area.name, context.proven(".NOMBRE"));
            grid.integer("vertices", static_cast<std::int64_t>(area.points.size()), context.derived(Provenance::proven, "Count of .PUNTOS entries"));
            opt_real(grid, "height", area.height, context.proven(".HEIGHT"));
            opt_int(grid, "flags", area.flags, context.unknown(".FLAGS"));
            opt_int(grid, "occlusion", area.occlusion, context.unknown(".OCLUSION"));
            opt_int(grid, "reverb", area.reverb, context.unknown(".REVERB"));
            opt_int(grid, "limit reverb", area.limit_reverb, context.unknown(".LIMITREVERB"));
        }
        end_section();
    }
}

void draw_light(AppState& state, RecordContext& context, const csf::MissionLight& light) {
    if (begin_section(state, "Identity", true)) {
        if (PropertyGrid grid(state, "##light"); grid) {
            opt_int(grid, "ID", light.id, context.proven(".ID"));
            opt_string(grid, "name", light.name, context.proven(".NOMBRE"));
            opt_vec3(grid, "position", light.position, context.proven(".POS"));
            opt_real(grid, "radius", light.radius, context.proven(".RADIO"));
            opt_int(grid, "modulation", light.modulate, context.unknown(".MODULATE"));
            const auto packed = light.color.value_or(0);
            grid.custom("color", [&] {
                ImGui::ColorButton("##color", ImGui::ColorConvertU32ToFloat4(rgb_u32(packed)));
                ImGui::SameLine();
                char text[16];
                std::snprintf(text, sizeof(text), "#%06X", packed & 0xFFFFFFU);
                copyable_text(text);
            }, context.proven(".COLOR"));
        }
        end_section();
    }
}

void draw_effect(AppState& state, RecordContext& context, const csf::MissionEffect& effect) {
    const auto& scene = *state.mission.scene;
    if (begin_section(state, "Identity", true)) {
        if (PropertyGrid grid(state, "##effect"); grid) {
            opt_int(grid, "ID", effect.id, context.proven(".ID"));
            opt_string(grid, "name", effect.name, context.proven(".NOMBRE"));
            opt_int(grid, "class", effect.class_id, context.proven(".CLASSID"));
            if (effect.dummy_id) {
                const auto dummy = std::ranges::find_if(scene.dummies(), [&](const auto& value) {
                    return value.id == effect.dummy_id;
                });
                grid.link("placement dummy", "dummy " + std::to_string(*effect.dummy_id),
                          dummy != scene.dummies().end() ? SelectionRef::mission_entry(dummy->source.entry_index)
                                                         : SelectionRef{},
                          context.derived(dummy != scene.dummies().end() ? Provenance::proven : Provenance::diagnosed,
                                          dummy != scene.dummies().end() ? "Dummy with this ID exists"
                                                                         : "No dummy has this ID"));
            }
            opt_int(grid, "priority", effect.priority, context.unknown(".PRIORITY"));
            opt_int(grid, "share group", effect.share_group, context.unknown(".SHARE_GROUP"));
        }
        end_section();
    }
}

void draw_scene_object(AppState& state, RecordContext& context, const csf::SceneObjectAnimation& object) {
    if (begin_section(state, "Identity", true)) {
        if (PropertyGrid grid(state, "##scene_object"); grid) {
            opt_string(grid, "ID", object.id, context.proven(".ID"));
            SelectionRef target;
            if (object.animation_id && state.mission.animations)
                if (const auto* record = state.mission.animations->find_id(*object.animation_id))
                    target = SelectionRef::database_record(path_utf8(record->source.file), record->source.entry_index);
            if (object.animation_id)
                grid.link("animation", std::to_string(*object.animation_id), target,
                          context.derived(target.empty() ? Provenance::diagnosed : Provenance::proven,
                                          target.empty() ? "No Anims.bdd record has this ID" : "Anims.bdd record found by ID"));
            opt_int(grid, "offset type", object.offset_type, context.unknown(".TIPO_OFFSET"));
            opt_real(grid, "offset", object.offset, context.unknown(".OFFSET"));
        }
        end_section();
    }
}

} // namespace

void draw_mission_record(AppState& state, const std::uint32_t entry) {
    const auto& scene = *state.mission.scene;
    std::string kind, label;
    const auto* source = find_mission_source(scene, entry, kind, label);
    if (!source || !state.mission.document) return;
    const auto& document = *state.mission.document;
    RecordContext context{state, document, find_csf_node(document.roots(), entry), *source};

    if (const auto found = std::ranges::find_if(scene.scene_objects(), [&](const auto& value) { return value.source.entry_index == entry; });
        found != scene.scene_objects().end()) {
        draw_scene_object(state, context, *found);
        draw_unsupported(state, found->unknown_fields);
    } else if (const auto actor = std::ranges::find_if(scene.actors(), [&](const auto& value) { return value.source.entry_index == entry; });
               actor != scene.actors().end()) {
        draw_actor(state, context, *actor);
        draw_unsupported(state, actor->unknown_fields);
    } else if (const auto dummy = std::ranges::find_if(scene.dummies(), [&](const auto& value) { return value.source.entry_index == entry; });
               dummy != scene.dummies().end()) {
        draw_dummy(state, context, *dummy);
        draw_unsupported(state, dummy->unknown_fields);
    } else if (const auto area = std::ranges::find_if(scene.areas(), [&](const auto& value) { return value.source.entry_index == entry; });
               area != scene.areas().end()) {
        draw_area(state, context, *area);
        draw_unsupported(state, area->unknown_fields);
    } else if (const auto light = std::ranges::find_if(scene.lights(), [&](const auto& value) { return value.source.entry_index == entry; });
               light != scene.lights().end()) {
        draw_light(state, context, *light);
        draw_unsupported(state, light->unknown_fields);
    } else if (const auto effect = std::ranges::find_if(scene.effects(), [&](const auto& value) { return value.source.entry_index == entry; });
               effect != scene.effects().end()) {
        draw_effect(state, context, *effect);
        draw_unsupported(state, effect->unknown_fields);
    } else {
        bool handled = false;
        for (const auto& group : scene.navigation()) {
            if (group.source.entry_index == entry) {
                draw_nav_group(state, context, group);
                draw_unsupported(state, group.unknown_fields);
                handled = true;
                break;
            }
            for (const auto& point : group.points)
                if (point.source.entry_index == entry) {
                    draw_nav_point(state, context, point);
                    draw_unsupported(state, point.unknown_fields);
                    handled = true;
                }
            for (const auto& link : group.connections)
                if (link.source.entry_index == entry) {
                    draw_connection(state, context, link);
                    handled = true;
                }
            if (handled) break;
        }
        if (!handled)
            for (const auto& link : scene.cross_group_connections())
                if (link.source.entry_index == entry) draw_connection(state, context, link);
    }
    draw_references_summary(state);
    draw_raw_subtree(context);
}

} // namespace rwsman::ui
