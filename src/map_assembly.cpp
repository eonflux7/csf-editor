#include "rws/map_assembly.hpp"

#include "rws/decoded.hpp"
#include "rws/scene_export.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <set>

namespace rws {
namespace {

void put_u32(std::vector<std::byte>& out, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
}
void put_f32(std::vector<std::byte>& out, const float value) { put_u32(out, std::bit_cast<std::uint32_t>(value)); }

// Donor collision triangles inside a prop's placed bounds may include ground
// patches under it; a margin keeps triangles that touch the model's surface.
constexpr float collision_margin = 5.0F;

} // namespace

std::vector<std::byte> encode_scene_instance(const SceneInstance& instance, const std::uint32_t library_id) {
    constexpr std::uint32_t record = 0x16FC0U, matrix = 0x0DU, structure = 0x01U;
    std::vector<std::byte> out;
    const auto name_length = static_cast<std::uint32_t>(instance.prototype_name.size());
    put_u32(out, record);
    put_u32(out, 92U + name_length);
    put_u32(out, library_id);
    put_u32(out, instance.prototype_id);
    put_u32(out, instance.instance_id);
    put_f32(out, instance.maximum_visibility_distance);
    put_f32(out, instance.minimum_visibility_distance);
    put_f32(out, instance.visibility_fade_range);
    put_u32(out, instance.flags);
    put_u32(out, matrix);
    put_u32(out, 64U);
    put_u32(out, library_id);
    put_u32(out, structure);
    put_u32(out, 52U);
    put_u32(out, library_id);
    for (const auto value : instance.rotation) put_f32(out, value);
    put_f32(out, instance.position.x);
    put_f32(out, instance.position.y);
    put_f32(out, instance.position.z);
    put_u32(out, instance.matrix_flags);
    put_u32(out, name_length);
    for (const char c : instance.prototype_name) out.push_back(static_cast<std::byte>(c));
    return out;
}

DecodeResult<AssembledProps> assemble_props(const Document& donor_map, const WorldModel& donor_collision,
                                            const std::span<const PlacedProp> props) {
    DecodeResult<AssembledProps> result;
    AssembledProps assembled;
    const auto bytes = donor_map.bytes();
    const auto library = donor_map.chunks().empty() ? donor_collision.library_id
                                                    : donor_map.chunks().front().library_id;
    std::map<std::uint32_t, const Chunk*> clumps;  // prototype id -> Clump
    for (const auto& chunk : donor_map.chunks())
        if (chunk.type == 0x10U)
            if (const auto id = clump_prototype_id(chunk, bytes)) clumps.try_emplace(*id, &chunk);
    const auto donor_triangles = world_build_triangles(donor_collision);
    std::set<std::uint32_t> used_prototypes, used_instance_ids;
    std::vector<SceneInstance> records;
    for (const auto& prop : props) {
        // Resolve the group and its combined donor bounds.
        std::vector<const SceneInstance*> members;
        Vec3 inf{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max()};
        Vec3 sup{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                 std::numeric_limits<float>::lowest()};
        for (const auto id : prop.donor_instance_ids) {
            const auto& instances = donor_map.scene_instances();
            const auto found = std::ranges::find(instances, id, &SceneInstance::instance_id);
            if (found == instances.end()) {
                result.error = "donor map has no scene instance " + std::to_string(id);
                return result;
            }
            const auto clump = clumps.find(found->prototype_id);
            if (clump == clumps.end()) {
                result.error = "donor map has no Clump for prototype " + std::to_string(found->prototype_id);
                return result;
            }
            used_prototypes.insert(found->prototype_id);
            members.push_back(&*found);
            for (const auto& triangle : placed_clump_triangles(*clump->second, bytes, *found))
                for (const auto& p : triangle) {
                    inf = {std::min(inf.x, p.x), std::min(inf.y, p.y), std::min(inf.z, p.z)};
                    sup = {std::max(sup.x, p.x), std::max(sup.y, p.y), std::max(sup.z, p.z)};
                }
        }
        if (members.empty() || inf.x > sup.x) {
            result.error = "prop has no instances or no geometry";
            return result;
        }
        // Turn about +Y around the bottom centre, then move it to `position`.
        const auto angle = prop.yaw_degrees * std::numbers::pi_v<float> / 180.0F;
        const auto c = std::cos(angle), s = std::sin(angle);
        const auto yaw = [&](const Vec3 v) { return Vec3{c * v.x + s * v.z, v.y, -s * v.x + c * v.z}; };
        const Vec3 anchor{(inf.x + sup.x) / 2.0F, inf.y, (inf.z + sup.z) / 2.0F};
        const auto move = [&](const Vec3& p) {
            const auto turned = yaw({p.x - anchor.x, p.y - anchor.y, p.z - anchor.z});
            return Vec3{turned.x + prop.position.x, turned.y + prop.position.y, turned.z + prop.position.z};
        };
        std::string ids;
        for (const auto* member : members) {
            SceneInstance placed = *member;
            // Right, up and at are the rows of `rotation`.
            for (std::size_t row = 0; row < 3; ++row) {
                const auto turned = yaw({member->rotation[row * 3], member->rotation[row * 3 + 1],
                                         member->rotation[row * 3 + 2]});
                placed.rotation[row * 3] = turned.x;
                placed.rotation[row * 3 + 1] = turned.y;
                placed.rotation[row * 3 + 2] = turned.z;
            }
            placed.position = move(member->position);
            if (!used_instance_ids.insert(placed.instance_id).second) {
                placed.instance_id = *used_instance_ids.rbegin() + 1U;
                used_instance_ids.insert(placed.instance_id);
            }
            ids += (ids.empty() ? "" : ",") + std::to_string(placed.instance_id);
            records.push_back(placed);
        }
        // Collision: donor triangles inside the combined donor bounds.
        const auto inside = [&](const Vec3& p) {
            return p.x >= inf.x - collision_margin && p.x <= sup.x + collision_margin &&
                   p.y >= inf.y - collision_margin && p.y <= sup.y + collision_margin &&
                   p.z >= inf.z - collision_margin && p.z <= sup.z + collision_margin;
        };
        std::size_t moved = 0;
        for (auto triangle : donor_triangles) {
            if (!std::ranges::all_of(triangle.vertices, [&](const auto& v) { return inside(v.position); })) continue;
            for (auto& vertex : triangle.vertices) {
                vertex.position = move(vertex.position);
                vertex.normal = yaw(vertex.normal);
            }
            assembled.collision.push_back(triangle);
            ++moved;
        }
        assembled.notes.push_back("prop instances " + ids + " from donor prototype(s) " +
                                  std::to_string(members.front()->prototype_id) +
                                  (members.size() > 1 ? "+" : "") + ", " + std::to_string(moved) +
                                  " collision triangles, donor bounds (" + std::to_string(inf.x) + ", " +
                                  std::to_string(inf.y) + ", " + std::to_string(inf.z) + ")..(" +
                                  std::to_string(sup.x) + ", " + std::to_string(sup.y) + ", " +
                                  std::to_string(sup.z) + ")");
    }
    // The level draws each prototype Clump at its own Frame List transform as
    // well as its clones (KB-world-geometry-11). So the first placement of a
    // prototype is the Clump itself: its root frame takes that placement's
    // matrix (the record Matrix replaces the root frame, FUN_006C4090) and
    // the record is dropped; later placements stay records.
    std::set<std::uint32_t> placed_prototypes;
    std::vector<SceneInstance> remaining;
    for (const auto& chunk : donor_map.chunks()) {
        if (chunk.type != 0x10U) continue;
        const auto id = clump_prototype_id(chunk, bytes);
        if (!id || !used_prototypes.contains(*id) || !placed_prototypes.insert(*id).second) continue;
        const auto end = chunk.offset + 12U + chunk.declared_size;
        std::vector<std::byte> clump(bytes.begin() + static_cast<std::ptrdiff_t>(chunk.offset),
                                     bytes.begin() + static_cast<std::ptrdiff_t>(end));
        const auto first = std::ranges::find(records, *id, &SceneInstance::prototype_id);
        const auto* frames = find_child(chunk, 0x0EU);
        const auto* frame_struct = frames ? find_child(*frames, 0x01U) : nullptr;
        const auto decoded = frames ? decode_frame_list(*frames, bytes) : DecodeResult<FrameListInfo>{};
        std::optional<std::size_t> root;
        if (decoded)
            for (std::size_t i = 0; i < decoded.value->frames.size(); ++i)
                if (decoded.value->frames[i].parent < 0) {
                    root = i;
                    break;
                }
        if (first != records.end() && frame_struct && root) {
            // Frame record: 3x3 rotation (right, up, at), position, parent, flags.
            auto at = frame_struct->payload_offset + 4U + *root * 56U - chunk.offset;
            std::vector<std::byte> matrix;
            for (const auto value : first->rotation) put_f32(matrix, value);
            put_f32(matrix, first->position.x);
            put_f32(matrix, first->position.y);
            put_f32(matrix, first->position.z);
            std::ranges::copy(matrix, clump.begin() + static_cast<std::ptrdiff_t>(at));
            assembled.notes.push_back("prototype " + std::to_string(*id) + " placed by its own root frame (instance " +
                                      std::to_string(first->instance_id) + ")");
            records.erase(first);
        }
        assembled.prefix.insert(assembled.prefix.end(), clump.begin(), clump.end());
    }
    for (const auto& record : records) {
        const auto encoded = encode_scene_instance(record, library);
        assembled.prefix.insert(assembled.prefix.end(), encoded.begin(), encoded.end());
    }
    result.value = std::move(assembled);
    return result;
}

} // namespace rws
