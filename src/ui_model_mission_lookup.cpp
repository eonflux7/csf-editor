#include "rwsman/mission_lookup.hpp"

namespace rwsman {

const csf::CsfSourceId* find_mission_source(const csf::MissionScene& scene,
                                            const std::uint32_t entry, std::string& kind,
                                            std::string& label) {
    for (const auto& value : scene.actors())
        if (value.source.entry_index == entry) {
            kind = "Actor";
            label = value.name.value_or("");
            return &value.source;
        }
    for (const auto& group : scene.navigation()) {
        if (group.source.entry_index == entry) {
            kind = "Navigation group";
            label = group.name.value_or("");
            return &group.source;
        }
        for (const auto& value : group.points)
            if (value.source.entry_index == entry) {
                kind = "Navigation point";
                label = value.name.value_or("");
                return &value.source;
            }
        for (const auto& value : group.connections)
            if (value.source.entry_index == entry) {
                kind = "Navigation connection";
                return &value.source;
            }
    }
    for (const auto& value : scene.cross_group_connections())
        if (value.source.entry_index == entry) {
            kind = "Navigation connection";
            return &value.source;
        }
    for (const auto& value : scene.dummies())
        if (value.source.entry_index == entry) {
            kind = "Dummy";
            label = value.name.value_or("");
            return &value.source;
        }
    for (const auto& value : scene.areas())
        if (value.source.entry_index == entry) {
            kind = "Area";
            label = value.name.value_or("");
            return &value.source;
        }
    for (const auto& value : scene.lights())
        if (value.source.entry_index == entry) {
            kind = "Light";
            label = value.name.value_or("");
            return &value.source;
        }
    for (const auto& value : scene.effects())
        if (value.source.entry_index == entry) {
            kind = "Effect";
            label = value.name.value_or("");
            return &value.source;
        }
    for (const auto& value : scene.folders())
        if (value.source.entry_index == entry) {
            kind = "Folder";
            label = value.path;
            return &value.source;
        }
    for (const auto& value : scene.scene_objects())
        if (value.source.entry_index == entry) {
            kind = "Scene-object animation";
            label = value.id.value_or("");
            return &value.source;
        }
    for (const auto& value : scene.bridges())
        if (value.source.entry_index == entry) {
            kind = "Bridge";
            label = value.visual_rws.value_or("");
            return &value.source;
        }
    for (const auto& value : scene.waters())
        if (value.source.entry_index == entry) {
            kind = "Water";
            return &value.source;
        }
    return nullptr;
}

const csf::Node* find_csf_node(const std::vector<csf::Node>& nodes, const std::uint32_t entry) {
    for (const auto& node : nodes) {
        if (node.entry_index == entry) return &node;
        if (const auto* found = find_csf_node(node.children, entry)) return found;
    }
    return nullptr;
}

} // namespace rwsman
