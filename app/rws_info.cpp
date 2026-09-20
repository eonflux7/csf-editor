#include "rws/animation.hpp"
#include "rws/decoded.hpp"
#include "rws/document.hpp"
#include "rws/obj_export.hpp"
#include "rws/scene_export.hpp"
#include "rws/world_recovery.hpp"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>

namespace {

void print_chunks(const std::vector<rws::Chunk>& chunks, const unsigned depth = 0) {
    for (const auto& chunk : chunks) {
        std::cout << std::string(depth * 2, ' ') << "0x" << std::hex << std::setw(8)
                  << std::setfill('0') << chunk.offset << std::dec << std::setfill(' ') << "  "
                  << rws::chunk_name(chunk.type) << " [0x" << std::hex << chunk.type << std::dec
                  << "] size=" << chunk.declared_size;
        if (chunk.truncated) {
            std::cout << " available=" << chunk.available_size << " TRUNCATED";
        }
        std::cout << " stamp=0x" << std::hex << chunk.library_id << std::dec << '\n';
        print_chunks(chunk.children, depth + 1);
    }
}

struct TypeStats {
    std::uint64_t count{};
    std::uint64_t bytes{};
    std::uint64_t truncated{};
};

void collect_stats(const std::vector<rws::Chunk>& chunks,
                   std::map<std::uint32_t, TypeStats>& stats) {
    for (const auto& chunk : chunks) {
        auto& item = stats[chunk.type];
        ++item.count;
        item.bytes += chunk.available_size;
        item.truncated += chunk.truncated ? 1U : 0U;
        collect_stats(chunk.children, stats);
    }
}

void collect_offsets(const std::vector<rws::Chunk>& chunks, const std::uint32_t type,
                     std::set<std::uint64_t>& offsets) {
    for (const auto& chunk : chunks) {
        if (chunk.type == type) offsets.insert(chunk.offset);
        collect_offsets(chunk.children, type, offsets);
    }
}

const rws::Chunk* find_first(const std::vector<rws::Chunk>& chunks, const std::uint32_t type) {
    for (const auto& chunk : chunks) {
        if (chunk.type == type) return &chunk;
        if (const auto* found = find_first(chunk.children, type)) return found;
    }
    return nullptr;
}

void print_animation_report(const rws::Document& document, const bool frames) {
    const auto* root = find_first(document.chunks(), 0x1B);
    if (!root) throw std::runtime_error("Document has no Animation Animation (0x1B) chunk");
    const auto clip = rws::decode_animation(*root, document.bytes());
    std::cout << "Animation version=0x" << std::hex << clip.version << std::dec
              << " interpolator=" << clip.interpolation_type << " ("
              << rws::animation_layout_name(clip.layout) << ") flags=0x" << std::hex << clip.flags
              << std::dec << '\n'
              << "  duration=" << clip.duration << " keyframes=" << clip.keyframes.size() << '/'
              << clip.declared_keyframe_count << " tracks=" << clip.tracks.size()
              << " serialized-record=" << clip.serialized_record_size
              << " logical-stride=" << clip.logical_record_stride << '\n';
    if (clip.layout == rws::AnimationLayout::hanim_compressed_22)
        std::cout << "  translation offset=(" << clip.translation_offset.x << ','
                  << clip.translation_offset.y << ',' << clip.translation_offset.z << ") scale=("
                  << clip.translation_scale.x << ',' << clip.translation_scale.y << ','
                  << clip.translation_scale.z << ")\n";
    for (const auto& track : clip.tracks)
        std::cout << "  track " << track.track_index << ": " << track.keyframes.size()
                  << " keys, first=" << track.keyframes.front()
                  << " last=" << track.keyframes.back() << '\n';
    if (frames)
        for (std::size_t i = 0; i < clip.keyframes.size(); ++i) {
            const auto& value = clip.keyframes[i];
            std::cout << "  key " << i << " @0x" << std::hex << value.source_offset << std::dec
                      << " track=" << value.node_index.value_or(-1) << " time=" << value.time
                      << " previous=" << value.previous_keyframe << " raw=0x" << std::hex
                      << value.raw_previous << std::dec << " t=(" << value.translation.x << ','
                      << value.translation.y << ',' << value.translation.z << ") q=("
                      << value.rotation.x << ',' << value.rotation.y << ',' << value.rotation.z
                      << ',' << value.rotation.w << ")\n";
        }
    for (const auto& diagnostic : clip.diagnostics)
        std::cout << "  "
                  << (diagnostic.severity == rws::AnimationDiagnostic::Severity::error ? "error"
                      : diagnostic.severity == rws::AnimationDiagnostic::Severity::warning
                          ? "warning"
                          : "note")
                  << " @0x" << std::hex << diagnostic.offset << std::dec << " [" << diagnostic.code
                  << "]: " << diagnostic.message << '\n';
}

void print_world_report(const std::vector<rws::RecoveredWorld>& worlds, const bool sectors) {
    if (worlds.empty()) {
        std::cout << "World recovery: no World chunks\n";
        return;
    }
    for (const auto& world : worlds) {
        std::cout << "World at 0x" << std::hex << world.world_offset << " library=0x"
                  << world.library_id << std::dec << '\n'
                  << "  Plane sectors declared: " << world.header.plane_sector_count << '\n'
                  << "  World sectors recovered/declared: " << world.sectors.size() << '/'
                  << world.header.world_sector_count << '\n'
                  << "  Triangles recovered/declared: " << world.recovered_triangles << '/'
                  << world.header.triangle_count << '\n'
                  << "  Vertices recovered/declared: " << world.recovered_vertices << '/'
                  << world.header.vertex_count << '\n'
                  << "  Materials: " << world.material_count << '\n'
                  << "  Invalid candidates: " << world.invalid_candidates
                  << " | invalid triangles: " << world.invalid_triangles
                  << " | invalid materials: " << world.invalid_material_references
                  << " | overlaps: " << world.duplicate_or_overlapping_ranges
                  << " | truncated candidates: " << world.truncated_candidates << '\n'
                  << "  Recovery: " << rws::world_recovery_status_name(world.status) << '\n';
        for (const auto& diagnostic : world.diagnostics)
            std::cout << "  Diagnostic: " << diagnostic << '\n';
        if (sectors) {
            std::size_t index{};
            for (const auto& sector : world.sectors) {
                std::cout << "  Sector " << index++ << " chunk=0x" << std::hex
                          << sector.chunk_offset << " struct=0x" << sector.struct_offset
                          << " end=0x" << sector.range_end << std::dec
                          << " triangles=" << sector.triangle_count
                          << " vertices=" << sector.vertex_count
                          << " material-base=" << sector.material_window_base << " bounds=("
                          << sector.bounding_box_inf.x << ',' << sector.bounding_box_inf.y << ','
                          << sector.bounding_box_inf.z << ")..(" << sector.bounding_box_sup.x << ','
                          << sector.bounding_box_sup.y << ',' << sector.bounding_box_sup.z << ")\n";
            }
        }
    }
}

void print_bsp_report(const std::vector<rws::RecoveredWorld>& worlds, const bool nodes) {
    if (worlds.empty()) {
        std::cout << "BSP recovery: no World chunks\n";
        return;
    }
    for (const auto& world : worlds) {
        const auto& s = world.topology_stats;
        std::cout << "World at 0x" << std::hex << world.world_offset << std::dec << '\n'
                  << "  Planes recovered/declared: " << world.planes.size() << '/'
                  << world.header.plane_sector_count << '\n'
                  << "  Leaves linked/recovered/declared: " << s.linked_sectors << '/'
                  << world.sectors.size() << '/' << world.header.world_sector_count << '\n'
                  << "  Unlinked sectors: " << s.unlinked_sectors << '\n'
                  << "  Maximum validated depth: " << s.maximum_depth << '\n'
                  << "  Candidates invalid=" << s.invalid_candidates
                  << " ambiguous=" << s.ambiguous_candidates
                  << " duplicate=" << s.duplicate_candidates
                  << " overlapping=" << s.overlapping_candidates
                  << " truncated=" << s.truncated_candidates << '\n'
                  << "  Graph unreachable=" << s.unreachable_nodes << " cycles=" << s.cycles
                  << " multiple-parent=" << s.multiple_parents
                  << " bounds-conflicts=" << s.bounds_conflicts << '\n'
                  << "  Topology: " << rws::world_topology_status_name(world.topology_status)
                  << '\n';
        for (const auto& diagnostic : world.topology_diagnostics)
            std::cout << "  Diagnostic: " << diagnostic << '\n';
        if (nodes)
            for (std::size_t i = 0; i < world.topology_nodes.size(); ++i) {
                const auto& node = world.topology_nodes[i];
                std::cout << "  Node " << i << " kind="
                          << (node.kind == rws::RecoveredWorldNode::Kind::plane ? "plane"
                                                                                : "sector")
                          << " value=" << node.value_index << " parent=";
                if (node.parent)
                    std::cout << *node.parent;
                else
                    std::cout << '-';
                std::cout << " side="
                          << (!node.is_left_child ? "root"
                                                  : (*node.is_left_child ? "left" : "right"))
                          << " depth=" << node.depth << " bounds=(" << node.bounding_box_inf.x
                          << ',' << node.bounding_box_inf.y << ',' << node.bounding_box_inf.z
                          << ")..(" << node.bounding_box_sup.x << ',' << node.bounding_box_sup.y
                          << ',' << node.bounding_box_sup.z << ")\n";
            }
    }
}

struct ValidationStats {
    std::uint64_t decoded{}, failed{};
    std::uint64_t triangles_stream{}, triangles_memory{}, triangles_ambiguous{};
};

void validate_types(const std::vector<rws::Chunk>& chunks, const std::span<const std::byte> bytes,
                    ValidationStats& stats,
                    std::optional<std::int32_t> geometry_vertices = std::nullopt,
                    const std::uint32_t enclosing_object_type = 0) {
    for (const auto& chunk : chunks) {
        std::string error;
        bool handled = true;
        auto child_geometry_vertices = geometry_vertices;
        switch (chunk.type) {
        case 0x06: {
            auto value = rws::decode_texture(chunk, bytes);
            error = value.error;
            break;
        }
        case 0x08: {
            auto value = rws::decode_material_list(chunk, bytes);
            error = value.error;
            break;
        }
        case 0x09: {
            auto value = rws::decode_world_sector(chunk, bytes);
            error = value.error;
            break;
        }
        case 0x0A: {
            auto value = rws::decode_plane_sector(chunk, bytes);
            error = value.error;
            break;
        }
        case 0x0B: {
            auto value = rws::decode_world(chunk, bytes);
            error = value.error;
            break;
        }
        case 0x0E: {
            auto value = rws::decode_frame_list(chunk, bytes);
            error = value.error;
            break;
        }
        case 0x0F: {
            auto value = rws::decode_geometry(chunk, bytes);
            error = value.error;
            if (value) {
                child_geometry_vertices = value.value->vertex_count;
                if (value.value->triangle_layout == rws::TriangleLayout::stream_order)
                    ++stats.triangles_stream;
                else if (value.value->triangle_layout == rws::TriangleLayout::memory_order)
                    ++stats.triangles_memory;
                else
                    ++stats.triangles_ambiguous;
            }
            break;
        }
        case 0x10: {
            auto value = rws::decode_clump(chunk, bytes);
            error = value.error;
            break;
        }
        case 0x1B: {
            const auto value = rws::decode_animation(chunk, bytes);
            if (!value.valid()) error = "Animation has unsupported layout or validation errors";
            break;
        }
        case 0x14: {
            auto value = rws::decode_atomic(chunk, bytes);
            error = value.error;
            break;
        }
        case 0x1F: {
            auto value = rws::decode_right_to_render(chunk, bytes);
            error = value.error;
            break;
        }
        case 0x24: {
            auto value = rws::decode_table_of_contents(chunk, bytes);
            error = value.error;
            break;
        }
        case 0x11D: {
            auto value = rws::decode_collision_tree(chunk, bytes);
            error = value.error;
            break;
        }
        case 0x11E: {
            auto value = rws::decode_hanim(chunk, bytes);
            error = value.error;
            break;
        }
        case 0x116: {
            if (!geometry_vertices)
                error = "Skin is not inside a decoded Geometry";
            else {
                auto value = rws::decode_skin(chunk, *geometry_vertices, bytes);
                error = value.error;
            }
            break;
        }
        case 0x11F: {
            auto value = rws::decode_user_data(chunk, bytes);
            error = value.error;
            break;
        }
        case 0x120: {
            auto value = rws::decode_material_effects(chunk, enclosing_object_type, bytes);
            error = value.error;
            break;
        }
        case 0x127: {
            auto value = rws::decode_anisotropy(chunk, bytes);
            error = value.error;
            break;
        }
        case 0x50E: {
            auto value = rws::decode_bin_mesh(chunk, bytes);
            error = value.error;
            break;
        }
        case 0x907: {
            auto value = rws::decode_physics_body_def(chunk, bytes);
            error = value.error;
            break;
        }
        case 0x909: {
            auto value = rws::decode_physics_ragdoll_def(chunk, bytes);
            error = value.error;
            break;
        }
        case 0xFFFFFF00U: {
            // Several collision Worlds under-declare a leaf boundary, leaving its
            // RpWorldSector extensions attached to the enclosing Plane Section.
            const auto owner = enclosing_object_type == 0x0A ? 0x09U : enclosing_object_type;
            auto value = rws::decode_pyro_extension(chunk, owner, bytes);
            error = value.error;
            break;
        }
        case 0x07: {
            auto value = rws::decode_material(chunk, bytes);
            error = value.error;
            break;
        }
        default:
            handled = false;
            break;
        }
        if (handled) {
            if (error.empty())
                ++stats.decoded;
            else {
                ++stats.failed;
                std::cerr << "typed error at 0x" << std::hex << chunk.offset << std::dec << " ("
                          << rws::chunk_name(chunk.type) << "): " << error << '\n';
            }
        }
        const auto child_owner = chunk.type == 0x03 ? enclosing_object_type : chunk.type;
        validate_types(chunk.children, bytes, stats, child_geometry_vertices, child_owner);
    }
}

void export_geometries(const std::vector<rws::Chunk>& chunks,
                       const std::span<const std::byte> bytes,
                       const std::filesystem::path& directory, std::uint64_t& exported) {
    for (const auto& chunk : chunks) {
        if (chunk.type == 0x0F) {
            const auto geometry = rws::decode_geometry(chunk, bytes);
            if (!geometry)
                throw std::runtime_error("Cannot decode Geometry at offset " +
                                         std::to_string(chunk.offset) + ": " + geometry.error);
            std::ostringstream name;
            name << "geometry_" << std::hex << std::setw(8) << std::setfill('0') << chunk.offset
                 << ".obj";
            rws::export_geometry_obj(*geometry.value, bytes, directory / name.str());
            ++exported;
        }
        export_geometries(chunk.children, bytes, directory, exported);
    }
}

void print_instances(const rws::Document& document) {
    struct GroupStats {
        std::uint64_t count{};
        std::uint32_t minimum_id{std::numeric_limits<std::uint32_t>::max()}, maximum_id{};
    };
    using Key =
        std::tuple<std::uint32_t, std::string, float, float, float, std::uint32_t, std::uint32_t>;
    std::map<Key, GroupStats> groups;
    std::set<std::uint32_t> prototype_ids;
    for (const auto& instance : document.scene_instances()) {
        auto& group =
            groups[{instance.prototype_id, instance.prototype_name,
                    instance.maximum_visibility_distance, instance.minimum_visibility_distance,
                    instance.visibility_fade_range, instance.flags, instance.declared_size}];
        ++group.count;
        group.minimum_id = std::min(group.minimum_id, instance.instance_id);
        group.maximum_id = std::max(group.maximum_id, instance.instance_id);
        prototype_ids.insert(instance.prototype_id);
    }
    std::cout << "CSF scene instances: " << document.scene_instances().size() << '\n';
    for (const auto& [key, group] : groups) {
        const auto& [prototype, name, maximum_distance, minimum_distance, fade_range, flags,
                     declared_size] = key;
        std::cout << "  prototype=" << prototype << " count=" << group.count
                  << " ids=" << group.minimum_id << ".." << group.maximum_id << " visibility=(max "
                  << maximum_distance << ", min " << minimum_distance << ", fade " << fade_range
                  << ')' << " flags=0x" << std::hex << flags << std::dec << " ["
                  << rws::scene_instance_flag_names(flags) << ']' << " declared=" << declared_size;
        if (!name.empty()) std::cout << " name=\"" << name << '"';
        std::cout << '\n';
    }

    std::cout << "Prototype correlation (prototype ID = 1000 + Pyro atomic object index):\n";
    std::size_t clump_ordinal{};
    for (const auto& clump : document.chunks()) {
        if (clump.type != 0x10) continue;
        std::size_t atomic_ordinal{};
        for (const auto& atomic : clump.children) {
            if (atomic.type != 0x14) continue;
            const auto* extension = rws::find_child(atomic, 0x03);
            const auto* pyro = extension ? rws::find_child(*extension, 0xFFFFFF00U) : nullptr;
            const auto metadata = pyro ? rws::decode_pyro_extension(*pyro, 0x14, document.bytes())
                                       : rws::DecodeResult<rws::PyroExtensionInfo>{};
            if (metadata) {
                if (const auto object_index = metadata.value->atomic_object_index()) {
                    const auto prototype_id = 1000U + *object_index;
                    if (prototype_ids.contains(prototype_id)) {
                        std::cout << "  clump=" << clump_ordinal << "@0x" << std::hex
                                  << clump.offset << std::dec << " atomic=" << atomic_ordinal
                                  << " object-index=" << *object_index
                                  << " prototype=" << prototype_id;
                        if (!metadata.value->object_name().empty())
                            std::cout << " name=\"" << metadata.value->object_name() << '"';
                        std::cout << '\n';
                    }
                    break; // The game uses the first Atomic with a valid object index.
                }
            }
            ++atomic_ordinal;
        }
        ++clump_ordinal;
    }
}

} // namespace

int main(const int argc, char** argv) {
    if (argc < 2 || argc > 5) {
        std::cerr << "Usage: rws-info <file.rws|file.rpc|file.anm> "
                     "[--summary|--animation-report[=frames]|--world-report[=sectors]|--bsp-report["
                     "=nodes]|--instances|--validate-types|--export-animation-gltf <model.rpc> "
                     "<file.gltf>|--export-obj <directory>|--export-scene-gltf "
                     "<file.gltf>|--export-collision-gltf <file.gltf>|--export-collision-obj "
                     "<file.obj>|--export-clump-gltf <offset> <file.gltf>]\n";
        return 2;
    }
    try {
        const auto document = rws::Document::load(argv[1]);
        std::cout << argv[1] << ": " << document.bytes().size() << " bytes, "
                  << document.chunks().size() << " top-level chunks\n";
        if (!document.chunks().empty()) {
            auto extension = std::filesystem::path(argv[1]).extension().string();
            std::ranges::transform(extension, extension.begin(), [](const unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (extension == ".rpc" && document.chunks().front().type != 0x10)
                throw std::runtime_error("RPC root is not a RenderWare Clump (0x10)");
            if (extension == ".rpc") std::cout << "Format: RPC Clump\n";
            const auto version = rws::decode_library_id(document.chunks().front().library_id);
            std::cout << "RenderWare " << version.major << '.' << version.minor << '.'
                      << version.revision << '.' << version.binary << " build " << version.build
                      << " (stamp 0x" << std::hex << document.chunks().front().library_id
                      << std::dec << ")\n";
        }
        const auto mode = argc >= 3 ? std::string_view(argv[2]) : std::string_view{};
        const auto recovered_worlds = rws::recover_worlds(document.chunks(), document.bytes());
        if (mode == "--summary") {
            std::map<std::uint32_t, TypeStats> stats;
            collect_stats(document.chunks(), stats);
            std::cout << "type        name                                  count       payload "
                         "bytes  truncated\n";
            for (const auto& [type, item] : stats) {
                std::cout << "0x" << std::hex << std::setw(8) << std::setfill('0') << type
                          << std::dec << std::setfill(' ') << "  " << std::left << std::setw(36)
                          << rws::chunk_name(type) << std::right << std::setw(9) << item.count
                          << std::setw(20) << item.bytes << std::setw(11) << item.truncated << '\n';
            }
            if (!document.scene_instances().empty())
                std::cout << "CSF scene instances: " << document.scene_instances().size() << '\n';
            for (const auto& world : recovered_worlds) {
                std::cout << "Recovered World at 0x" << std::hex << world.world_offset << std::dec
                          << ": sectors " << world.sectors.size() << '/'
                          << world.header.world_sector_count << ", triangles "
                          << world.recovered_triangles << '/' << world.header.triangle_count
                          << ", vertices " << world.recovered_vertices << '/'
                          << world.header.vertex_count << ", "
                          << rws::world_recovery_status_name(world.status) << "; BSP planes "
                          << world.planes.size() << '/' << world.header.plane_sector_count << ", "
                          << rws::world_topology_status_name(world.topology_status) << '\n';
            }
        } else if (mode == "--animation-report" || mode == "--animation-report=frames") {
            print_animation_report(document, mode == "--animation-report=frames");
        } else if (mode == "--export-animation-gltf" && argc == 5) {
            const auto* animation = find_first(document.chunks(), 0x1B);
            if (!animation)
                throw std::runtime_error("Input document has no Animation Animation chunk");
            auto clip = rws::decode_animation(*animation, document.bytes());
            const auto model = rws::Document::load(argv[3]);
            const auto* frame_chunk = find_first(model.chunks(), 0x0E);
            if (!frame_chunk) throw std::runtime_error("Model has no Frame List/HAnim hierarchy");
            const auto frames = rws::decode_frame_list(*frame_chunk, model.bytes());
            const auto binding = rws::decode_hanim_binding(*frame_chunk, model.bytes());
            if (!frames || !binding)
                throw std::runtime_error("Model Frame List/HAnim hierarchy could not be decoded");
            const auto compatibility =
                rws::map_animation_tracks(clip, *binding.value, frames.value->frames.size());
            if (!compatibility.compatible)
                throw std::runtime_error("Animation is not compatible with the model hierarchy");
            std::vector<std::array<float, 16>> inverse_bind;
            if (const auto* geometry_chunk = find_first(model.chunks(), 0x0F)) {
                const auto geometry = rws::decode_geometry(*geometry_chunk, model.bytes());
                if (geometry)
                    if (const auto* skin_chunk = find_first(geometry_chunk->children, 0x116)) {
                        const auto skin = rws::decode_skin(
                            *skin_chunk, geometry.value->vertex_count, model.bytes());
                        if (skin)
                            inverse_bind =
                                rws::decode_inverse_bind_matrices(*skin.value, model.bytes());
                    }
            }
            const std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            inverse_bind.resize(frames.value->frames.size(), identity);
            rws::export_animation_gltf(clip, *frames.value, binding.value->hierarchy, inverse_bind,
                                       argv[4]);
            std::cout << "Exported animation skeleton, skin joints, and " << clip.tracks.size()
                      << " tracks to " << argv[4] << '\n';
        } else if (mode == "--world-report" || mode == "--world-report=sectors") {
            print_world_report(recovered_worlds, mode == "--world-report=sectors");
        } else if (mode == "--bsp-report" || mode == "--bsp-report=nodes") {
            print_bsp_report(recovered_worlds, mode == "--bsp-report=nodes");
        } else if (mode == "--instances") {
            print_instances(document);
        } else if (mode == "--validate-types") {
            ValidationStats stats;
            validate_types(document.chunks(), document.bytes(), stats);
            std::set<std::uint64_t> nested_sectors;
            std::set<std::uint64_t> nested_planes;
            collect_offsets(document.chunks(), 0x09, nested_sectors);
            collect_offsets(document.chunks(), 0x0A, nested_planes);
            for (const auto& world : recovered_worlds) {
                for (const auto& sector : world.sectors)
                    if (!nested_sectors.contains(sector.chunk_offset)) ++stats.decoded;
                for (const auto& plane : world.planes)
                    if (!nested_planes.contains(plane.chunk_offset)) ++stats.decoded;
                auto recovery_failures = world.invalid_candidates + world.invalid_triangles +
                                         world.invalid_material_references +
                                         world.duplicate_or_overlapping_ranges;
                recovery_failures += world.topology_stats.invalid_candidates +
                                     world.topology_stats.ambiguous_candidates +
                                     world.topology_stats.bounds_conflicts;
                // A partial result can consist solely of aggregate count mismatches,
                // and a failed result can have no recognizable sector candidate at all.
                if (world.status != rws::WorldRecoveryStatus::complete && recovery_failures == 0)
                    recovery_failures = 1;
                stats.failed += recovery_failures;
            }
            std::cout << "Typed structures decoded: " << stats.decoded
                      << ", failed: " << stats.failed << '\n';
            std::cout << "Geometry triangle layouts: stream=" << stats.triangles_stream
                      << ", memory=" << stats.triangles_memory
                      << ", ambiguous=" << stats.triangles_ambiguous << '\n';
            if (stats.failed != 0) return 3;
        } else if (mode == "--export-obj" && argc == 4) {
            const std::filesystem::path directory(argv[3]);
            std::filesystem::create_directories(directory);
            std::uint64_t exported{};
            export_geometries(document.chunks(), document.bytes(), directory, exported);
            std::cout << "Exported " << exported << " geometries to " << directory.string() << '\n';
        } else if (mode == "--export-scene-gltf" && argc == 4) {
            const auto stats =
                rws::export_scene_gltf(document.chunks(), document.scene_instances(),
                                       document.bytes(), std::filesystem::path(argv[3]));
            std::cout << "Exported assembled scene: " << stats.atomic_instances
                      << " atomic meshes, " << stats.custom_instances << " resolved CSF instances, "
                      << stats.unresolved_instances << " unresolved CSF instances, "
                      << stats.recovered_world_sectors << " recovered World sectors ("
                      << stats.recovered_world_triangles << " triangles, "
                      << stats.recovered_world_vertices << " vertices), " << stats.world_sectors
                      << " exported World meshes, " << stats.vertices << " vertices, "
                      << stats.triangles << " triangles, " << stats.materials << " materials ("
                      << stats.skipped << " skipped)\n";
        } else if (mode == "--export-collision-gltf" && argc == 4) {
            const auto stats =
                rws::export_collision_gltf(document.chunks(), document.bytes(), argv[3]);
            std::cout << "Exported collision glTF: " << stats.world_sectors << " sectors, "
                      << stats.triangles << " triangles, " << stats.materials << " materials ("
                      << stats.skipped << " skipped)\n";
        } else if (mode == "--export-collision-obj" && argc == 4) {
            const auto stats =
                rws::export_collision_obj(document.chunks(), document.bytes(), argv[3]);
            std::cout << "Exported collision OBJ: " << stats.sectors << " sectors, "
                      << stats.triangles << " triangles, " << stats.materials << " materials ("
                      << stats.skipped_triangles << " skipped triangles)\n";
        } else if (mode == "--export-clump-gltf" && argc == 5) {
            const auto offset = std::stoull(argv[3], nullptr, 0);
            const auto* clump = [&]() -> const rws::Chunk* {
                for (const auto& chunk : document.chunks())
                    if (chunk.type == 0x10 && chunk.offset == offset) return &chunk;
                return nullptr;
            }();
            if (!clump)
                throw std::runtime_error("No top-level Clump found at the requested offset");
            const auto stats =
                rws::export_clump_gltf(*clump, document.bytes(), std::filesystem::path(argv[4]));
            std::cout << "Exported Clump at 0x" << std::hex << offset << std::dec << ": "
                      << stats.atomic_instances << " Atomics, " << stats.vertices << " vertices, "
                      << stats.triangles << " triangles, " << stats.materials << " materials\n";
        } else {
            print_chunks(document.chunks());
        }
        for (const auto& diagnostic : document.diagnostics()) {
            std::cerr << (diagnostic.severity == rws::Diagnostic::Severity::error ? "error"
                                                                                  : "warning")
                      << " at 0x" << std::hex << diagnostic.offset << std::dec << ": "
                      << diagnostic.message << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "rws-info: " << error.what() << '\n';
        return 1;
    }
}
