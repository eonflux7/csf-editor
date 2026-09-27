// The mission's flow as a graph (docs/archive/editor/editor-ux-redesign.md, S3): events
// on the left, the scripts they start in the middle, the objectives on the
// right; edges for "starts", "raises" and "completes". An event that starts
// scripts but that nothing raises, and an objective that nothing completes,
// are drawn as problems. Read-only; a click on a script opens it.
#include "app_state.hpp"
#include "ui/ui.hpp"

#include "mission_authoring.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <map>

namespace rwsman::ui {
namespace {

struct Node {
    ImVec2 min, max;
    bool problem{};
};

ImVec2 left_of(const Node& node) { return {node.min.x, (node.min.y + node.max.y) / 2}; }
ImVec2 right_of(const Node& node) { return {node.max.x, (node.min.y + node.max.y) / 2}; }

void edge(ImDrawList* draw, const ImVec2 from, const ImVec2 to, const ImU32 colour, const float thickness) {
    const float bend = std::max(30.0F, (to.x - from.x) * 0.45F);
    draw->AddBezierCubic(from, {from.x + bend, from.y}, {to.x - bend, to.y}, to, colour, thickness);
    // An arrowhead at the target.
    draw->AddTriangleFilled(to, {to.x - 7.0F, to.y - 4.0F}, {to.x - 7.0F, to.y + 4.0F}, colour);
}

} // namespace

bool flow_event_unraised(const csf::FlowEvent& event) {
    return !event.builtin && event.senders.empty() && !event.listeners.empty();
}

void draw_flow_graph(AppState& state) {
    const auto* flow = mission_flow(state);
    if (!flow) return;
    const float scale = ui_scale();
    const float row = ImGui::GetFrameHeight() + 10.0F * scale;
    const float width = std::max(ImGui::GetContentRegionAvail().x, 420.0F * scale);
    const float gap = 70.0F * scale;
    const float column = (width - 2 * gap) / 3.0F;
    const auto& events = flow->events();
    // Scripts in the order of the first event that starts them, so edges cross less.
    std::vector<const csf::FlowScript*> scripts;
    for (const auto& event : events)
        for (const auto id : event.listeners)
            for (const auto& script : flow->scripts())
                if (script.id == id && std::ranges::find(scripts, &script) == scripts.end()) scripts.push_back(&script);
    for (const auto& script : flow->scripts())
        if (std::ranges::find(scripts, &script) == scripts.end()) scripts.push_back(&script);
    const auto& objectives = flow->objectives();
    const auto rows = std::max({events.size(), scripts.size(), objectives.size(), std::size_t{1}});
    ImGui::BeginChild("##flow_graph", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy({width, row * static_cast<float>(rows + 1)});
    auto* draw = ImGui::GetWindowDrawList();
    const auto node_at = [&](const int col, const std::size_t index) {
        const ImVec2 min{origin.x + static_cast<float>(col) * (column + gap), origin.y + row * static_cast<float>(index + 1)};
        return Node{min, {min.x + column, min.y + ImGui::GetFrameHeight()}};
    };
    // Column titles.
    ImGui::PushFont(font(Font::caption));
    for (int col = 0; col < 3; ++col)
        draw->AddText({origin.x + static_cast<float>(col) * (column + gap), origin.y + 2.0F}, color_u32(Token::text_dim),
                      col == 0 ? "EVENTS" : col == 1 ? "SCRIPTS" : "OBJECTIVES");
    ImGui::PopFont();
    std::map<std::string, Node> event_nodes;
    std::map<std::pair<std::string, std::int32_t>, Node> script_nodes;
    std::map<std::int32_t, Node> objective_nodes;
    for (std::size_t i = 0; i < events.size(); ++i) {
        auto node = node_at(0, i);
        node.problem = flow_event_unraised(events[i]);
        event_nodes[events[i].name] = node;
    }
    for (std::size_t i = 0; i < scripts.size(); ++i) script_nodes[{scripts[i]->program, scripts[i]->id}] = node_at(1, i);
    for (std::size_t i = 0; i < objectives.size(); ++i) {
        auto node = node_at(2, i);
        node.problem = objectives[i].completed_by.empty() || objectives[i].defined_by.empty();
        objective_nodes[objectives[i].number] = node;
    }
    const auto script_node = [&](const std::int32_t id) -> const Node* {
        for (const auto* program : {"mission", "cutscene"})
            if (const auto found = script_nodes.find({program, id}); found != script_nodes.end()) return &found->second;
        return nullptr;
    };
    // Hovering a node brings out its edges.
    const auto mouse = ImGui::GetIO().MousePos;
    const auto hovered = [&](const Node& node) {
        return ImGui::IsWindowHovered() && mouse.x >= node.min.x && mouse.x <= node.max.x && mouse.y >= node.min.y &&
               mouse.y <= node.max.y;
    };
    const auto strength = [&](const Node& a, const Node& b) { return hovered(a) || hovered(b) ? 2.5F * scale : 1.2F * scale; };
    for (const auto& event : events) {
        const auto& from = event_nodes[event.name];
        for (const auto id : event.listeners)
            if (const auto* to = script_node(id))
                edge(draw, right_of(from), left_of(*to), color_u32(Token::inferred, 0.75F), strength(from, *to));
        for (const auto id : event.senders)
            if (const auto* by = script_node(id))
                edge(draw, {by->min.x, by->max.y - 4.0F}, {from.max.x, from.max.y - 4.0F}, color_u32(Token::accent, 0.75F),
                     strength(from, *by));
    }
    for (const auto& objective : objectives) {
        const auto& to = objective_nodes[objective.number];
        for (const auto id : objective.completed_by)
            if (const auto* from = script_node(id))
                edge(draw, right_of(*from), left_of(to), color_u32(Token::ok, 0.8F), strength(*from, to));
        for (const auto id : objective.defined_by)
            if (const auto* from = script_node(id))
                edge(draw, right_of(*from), {to.min.x, to.min.y + 4.0F}, color_u32(Token::text_dim, 0.5F), strength(*from, to));
    }
    const auto box = [&](const Node& node, const std::string& label, const ImU32 accent) {
        const bool over = hovered(node);
        draw->AddRectFilled(node.min, node.max, color_u32(over ? Token::bg2 : Token::bg1), 4.0F * scale);
        draw->AddRect(node.min, node.max, node.problem ? color_u32(Token::error) : color_u32(Token::line), 4.0F * scale, 0,
                      node.problem ? 2.0F * scale : 1.0F);
        draw->AddRectFilled(node.min, {node.min.x + 3.0F * scale, node.max.y}, accent, 4.0F * scale,
                            ImDrawFlags_RoundCornersLeft);
        const auto text = (node.problem ? std::string(icons::LC_TRIANGLE_ALERT) + " " : std::string()) + label;
        draw->PushClipRect(node.min, {node.max.x - 4.0F, node.max.y}, true);
        draw->AddText({node.min.x + 9.0F * scale, node.min.y + ImGui::GetStyle().FramePadding.y},
                      node.problem ? color_u32(Token::error) : color_u32(Token::text), text.c_str());
        draw->PopClipRect();
        return over;
    };
    for (const auto& event : events) {
        const auto& node = event_nodes[event.name];
        if (box(node, event.name + (event.builtin ? "" : "  (custom)"), color_u32(Token::inferred)))
            ImGui::SetTooltip("%s\n%zu script(s) start on it, %zu raise it%s", event.name.c_str(), event.listeners.size(),
                              event.senders.size(),
                              flow_event_unraised(event) ? "\nNothing raises it: its scripts never start" : "");
    }
    for (const auto* script : scripts) {
        const auto& node = script_nodes[{script->program, script->id}];
        const auto label = std::to_string(script->id) + " " + script->name;
        if (box(node, label, script->program == "cutscene" ? kind_color_u32(EntityKind::camera_path)
                                                           : kind_color_u32(EntityKind::script))) {
            ImGui::SetTooltip("%s script %d %s%s\nClick to open it", script->program.c_str(), script->id, script->name.c_str(),
                              script->trigger ? " (trigger)" : "");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) open_flow_script(state, script->program, script->id);
        }
    }
    for (const auto& objective : objectives) {
        const auto& node = objective_nodes[objective.number];
        const auto label = "Objective " + std::to_string(objective.number) + "  " + text_label(state, objective.label);
        if (box(node, label, kind_color_u32(EntityKind::objective)))
            ImGui::SetTooltip("Objective %d: set up by %zu script(s), completed by %zu%s", objective.number,
                              objective.defined_by.size(), objective.completed_by.size(),
                              objective.completed_by.empty() ? "\nNothing completes it" : "");
    }
    ImGui::EndChild();
}

} // namespace rwsman::ui
