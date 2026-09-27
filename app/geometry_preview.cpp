#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
// clang-format off
// windows.h must precede the headers below; they depend on its declarations.
#include <windows.h>
// clang-format on
#endif

#include "geometry_preview.hpp"
#include "gl_api.hpp"

#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"
#include "rws/physics_inspection.hpp"
#include "rws/texture_image.hpp"
#include "rws/world_recovery.hpp"
#include "rwsman/frame_pacing.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace rwsman {
namespace {

constexpr std::uint64_t actor_owner_mask = 0x8000000000000000ULL;

std::optional<std::uint32_t> actor_owner(const std::uint64_t owner_offset) {
    if ((owner_offset & actor_owner_mask) == 0) return std::nullopt;
    return static_cast<std::uint32_t>(owner_offset & ~actor_owner_mask);
}

struct AffineTransform {
    std::array<float, 9> rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
    rws::Vec3 position{};
};

AffineTransform frame_transform(const rws::FrameInfo& frame) {
    // RwMatrix stores right/up/at vectors; convert those basis columns to a
    // conventional row-major matrix for point multiplication below.
    return {{{frame.rotation[0], frame.rotation[3], frame.rotation[6], frame.rotation[1],
              frame.rotation[4], frame.rotation[7], frame.rotation[2], frame.rotation[5],
              frame.rotation[8]}},
            frame.position};
}

rws::Vec3 rotate(const AffineTransform& transform, const rws::Vec3 value) {
    const auto& m = transform.rotation;
    return {m[0] * value.x + m[1] * value.y + m[2] * value.z,
            m[3] * value.x + m[4] * value.y + m[5] * value.z,
            m[6] * value.x + m[7] * value.y + m[8] * value.z};
}

rws::Vec3 transform_point(const AffineTransform& transform, const rws::Vec3 value) {
    const auto rotated = rotate(transform, value);
    return {rotated.x + transform.position.x, rotated.y + transform.position.y,
            rotated.z + transform.position.z};
}

rws::Vec3 inverse_transform_point(const AffineTransform& transform, const rws::Vec3 value) {
    const auto& m = transform.rotation;
    const rws::Vec3 translated{value.x - transform.position.x, value.y - transform.position.y,
                               value.z - transform.position.z};
    const float c00 = m[4] * m[8] - m[5] * m[7];
    const float c01 = m[2] * m[7] - m[1] * m[8];
    const float c02 = m[1] * m[5] - m[2] * m[4];
    const float determinant =
        m[0] * c00 + m[1] * (m[5] * m[6] - m[3] * m[8]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (std::abs(determinant) < 1.0e-8F) return translated;
    const float inverse = 1.0F / determinant;
    return {
        (c00 * translated.x + c01 * translated.y + c02 * translated.z) * inverse,
        ((m[5] * m[6] - m[3] * m[8]) * translated.x + (m[0] * m[8] - m[2] * m[6]) * translated.y +
         (m[2] * m[3] - m[0] * m[5]) * translated.z) *
            inverse,
        ((m[3] * m[7] - m[4] * m[6]) * translated.x + (m[1] * m[6] - m[0] * m[7]) * translated.y +
         (m[0] * m[4] - m[1] * m[3]) * translated.z) *
            inverse};
}

AffineTransform compose(const AffineTransform& parent, const AffineTransform& local) {
    AffineTransform result;
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 3; ++column) {
            result.rotation[row * 3 + column] = 0.0F;
            for (unsigned k = 0; k < 3; ++k)
                result.rotation[row * 3 + column] +=
                    parent.rotation[row * 3 + k] * local.rotation[k * 3 + column];
        }
    result.position = transform_point(parent, local.position);
    return result;
}

AffineTransform inverse_transform(const AffineTransform& value) {
    const auto origin = inverse_transform_point(value, {});
    const auto x = inverse_transform_point(value, {1, 0, 0});
    const auto y = inverse_transform_point(value, {0, 1, 0});
    const auto z = inverse_transform_point(value, {0, 0, 1});
    return {{{x.x - origin.x, y.x - origin.x, z.x - origin.x, x.y - origin.y, y.y - origin.y,
              z.y - origin.y, x.z - origin.z, y.z - origin.z, z.z - origin.z}},
            origin};
}

std::array<float, 12> draw_transform(const AffineTransform& value) {
    return {value.rotation[0], value.rotation[1], value.rotation[2], value.position.x,
            value.rotation[3], value.rotation[4], value.rotation[5], value.position.y,
            value.rotation[6], value.rotation[7], value.rotation[8], value.position.z};
}

AffineTransform draw_matrix_transform(const std::array<float, 12>& value) {
    return {{{value[0], value[1], value[2], value[4], value[5], value[6], value[8], value[9],
              value[10]}},
            {value[3], value[7], value[11]}};
}

AffineTransform matrix_transform(const std::array<float, 16>& value) {
    return {{{value[0], value[4], value[8], value[1], value[5], value[9], value[2], value[6],
              value[10]}},
            {value[12], value[13], value[14]}};
}

std::string normalized_bone_name(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    value.erase(std::remove_if(value.begin(), value.end(), [](const unsigned char c) {
                    return !std::isalnum(c);
                }),
                value.end());
    return value;
}

std::vector<std::string> frame_labels(const rws::Chunk& frame_list,
                                      const std::span<const std::byte> bytes,
                                      const std::size_t frame_count) {
    std::vector<std::string> result(frame_count);
    std::size_t frame{};
    for (const auto& extension : frame_list.children) {
        if (extension.type != 0x03 || frame >= result.size()) continue;
        for (const auto& plugin : extension.children) {
            if (plugin.type != 0x11F) continue;
            const auto decoded = rws::decode_user_data(plugin, bytes);
            if (!decoded) continue;
            for (const auto& array : decoded.value->arrays) {
                for (const auto& value : array.strings) {
                    const auto normalized = normalized_bone_name(value);
                    if (normalized.find("BONE") != std::string::npos ||
                        normalized.find("BIP") != std::string::npos ||
                        normalized.find("HAND") != std::string::npos ||
                        normalized.find("MANO") != std::string::npos) {
                        result[frame] = value;
                        break;
                    }
                    if (result[frame].empty()) result[frame] = value;
                }
                if (!result[frame].empty()) break;
            }
        }
        ++frame;
    }
    return result;
}

std::optional<std::size_t> find_hand_frame(const rws::HAnimBinding& binding,
                                           const std::span<const std::string> labels,
                                           const bool left_hand) {
    const auto matches = [&](const std::string& value) {
        const auto name = normalized_bone_name(value);
        const bool hand = name.find("HAND") != std::string::npos ||
                          name.find("MANO") != std::string::npos;
        const bool left = name.find("LEFT") != std::string::npos ||
                          name.find("IZQ") != std::string::npos ||
                          name.find("LHAND") != std::string::npos;
        const bool right = name.find("RIGHT") != std::string::npos ||
                           name.find("DER") != std::string::npos ||
                           name.find("RHAND") != std::string::npos;
        return hand && (left_hand ? left && !right : right && !left);
    };
    for (std::size_t frame = 0; frame < labels.size(); ++frame)
        if (matches(labels[frame])) return frame;

    const std::string attachment_tag = left_hand ? "TAG50" : "TAG40";
    for (std::size_t frame = 0; frame < labels.size(); ++frame)
        if (normalized_bone_name(labels[frame]).find(attachment_tag) != std::string::npos)
            return frame;

    // Some unmodified RenderWare humanoids retain the standard hand node IDs.
    const std::int32_t wanted = left_hand ? 23 : 33;
    for (std::size_t frame = 0; frame < binding.frame_node_ids.size(); ++frame)
        if (binding.frame_node_ids[frame] == wanted) return frame;
    return std::nullopt;
}

rws::Vec3 transform_draw_point(const std::array<float, 12>& m, const rws::Vec3 p) {
    return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
            m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
            m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

float read_f32(const std::span<const std::byte> bytes, const std::uint64_t offset) {
    std::uint32_t raw{};
    for (unsigned i = 0; i < 4; ++i)
        raw |= std::to_integer<std::uint32_t>(bytes[static_cast<std::size_t>(offset + i)])
               << (i * 8U);
    return std::bit_cast<float>(raw);
}

std::uint32_t read_span_u32(const std::span<const std::byte> bytes, const std::uint64_t offset) {
    std::uint32_t result{};
    for (unsigned i = 0; i < 4; ++i)
        result |= std::to_integer<std::uint32_t>(bytes[static_cast<std::size_t>(offset + i)])
                  << (i * 8U);
    return result;
}

std::uint16_t read_span_u16(const std::span<const std::byte> bytes, const std::uint64_t offset) {
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes[static_cast<std::size_t>(offset)]) |
        (std::to_integer<std::uint16_t>(bytes[static_cast<std::size_t>(offset + 1)]) << 8U));
}

ImU32 material_color(const std::uint16_t material, const float shade) {
    constexpr std::array<std::array<float, 3>, 12> colors{{{0.36F, 0.67F, 0.91F},
                                                           {0.91F, 0.48F, 0.35F},
                                                           {0.48F, 0.82F, 0.49F},
                                                           {0.84F, 0.67F, 0.31F},
                                                           {0.68F, 0.49F, 0.86F},
                                                           {0.32F, 0.78F, 0.76F},
                                                           {0.91F, 0.43F, 0.67F},
                                                           {0.62F, 0.70F, 0.35F},
                                                           {0.42F, 0.55F, 0.83F},
                                                           {0.86F, 0.58F, 0.39F},
                                                           {0.48F, 0.76F, 0.65F},
                                                           {0.74F, 0.50F, 0.64F}}};
    const auto& color = colors[material % colors.size()];
    return ui::rgba_u32(static_cast<int>(255.0F * color[0] * shade),
                        static_cast<int>(255.0F * color[1] * shade),
                        static_cast<int>(255.0F * color[2] * shade), 255);
}

unsigned int upload_texture(const int width, const int height, const std::uint8_t* rgba) {
    GLuint texture{};
    auto& gl = gl_api();
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    gl.generate_mipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    GLfloat maximum_anisotropy = 1.0F;
    glGetFloatv(gl_max_texture_max_anisotropy, &maximum_anisotropy);
    if (maximum_anisotropy > 1.0F)
        glTexParameterf(GL_TEXTURE_2D, gl_texture_max_anisotropy,
                        std::min(maximum_anisotropy, 8.0F));
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    if (gl.generate_mipmap) gl.generate_mipmap(GL_TEXTURE_2D);
    return texture;
}

bool has_fractional_alpha(const std::span<const std::uint8_t> rgba) {
    for (std::size_t index = 3; index < rgba.size(); index += 4)
        if (rgba[index] != 0 && rgba[index] != 255) return true;
    return false;
}

unsigned int upload_checker_texture(const bool missing) {
    std::array<std::uint8_t, 64 * 64 * 4> checker{};
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const bool bright = ((x / 8) ^ (y / 8)) & 1;
            const auto offset = static_cast<std::size_t>(y * 64 + x) * 4U;
            checker[offset] = missing ? (bright ? 255 : 35) : (bright ? 210 : 65);
            checker[offset + 1] = missing ? 0 : checker[offset];
            checker[offset + 2] = missing ? (bright ? 220 : 35) : checker[offset];
            checker[offset + 3] = 255;
        }
    return upload_texture(64, 64, checker.data());
}

std::filesystem::path find_texture(const std::filesystem::path& source_path,
                                   const std::string& name,
                                   const csf::TextureCatalog* catalog = nullptr,
                                   const std::uint32_t variant = 0) {
    if (name.empty()) return {};
    if (catalog)
        if (const auto resolved = catalog->resolve(name, variant)) return *resolved;
    const auto requested = std::filesystem::path(name);
    std::vector<std::filesystem::path> relative_paths;
    if (requested.extension().empty()) {
        relative_paths.push_back(requested.string() + ".dds");
        relative_paths.push_back(requested.string() + ".png");
    } else
        relative_paths.push_back(requested);
    std::vector<std::filesystem::path> candidates;
    auto directory = source_path.parent_path();
    for (const auto& relative : relative_paths)
        candidates.push_back(directory / relative);
    for (std::size_t depth = 0; depth < 8 && !directory.empty(); ++depth) {
        for (const auto& relative : relative_paths)
            candidates.push_back(directory / "Textures" / relative);
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    std::error_code error;
    for (const auto& candidate : candidates)
        if (std::filesystem::is_regular_file(candidate, error)) return candidate;

    return {};
}

} // namespace

void GeometryPreview::clear() {
    release_geometry();
    texture_catalog_ = {};
    texture_variant_ = 0;
    set_mission_overlays({});
    mission_actor_models_.clear();
    hidden_mission_entries_.clear();
    hidden_sublayers_.clear();
    overlay_filter_.fill('\0');
    slice_.enabled = false;
}

// Drops the loaded model and its GPU data but keeps what the app supplied for
// the mission (overlays, actor models, visibility, texture catalog), so switching
// between the Geometry and Scene workspaces does not lose them.
void GeometryPreview::release_geometry() {
    destroy_gpu_resources();
    if (!owned_texture_ids_.empty())
        glDeleteTextures(static_cast<GLsizei>(owned_texture_ids_.size()),
                         owned_texture_ids_.data());
    if (checker_texture_ != 0) glDeleteTextures(1, &checker_texture_);
    chunk_offset_ = ~std::uint64_t{};
    scene_mode_ = false;
    scene_clump_count_ = scene_instance_count_ = scene_world_sector_count_ = scene_skipped_count_ =
        0;
    scene_world_triangle_count_ = 0;
    scene_custom_instance_count_ = scene_unresolved_instance_count_ = 0;
    collision_sector_count_ = collision_triangle_count_ = collision_material_count_ = 0;
    collision_declared_sector_count_ = 0;
    collision_recovery_status_ = rws::WorldRecoveryStatus::failed;
    collision_diagnostics_.clear();
    collision_surface_labels_.clear();
    collision_source_path_.clear();
    collision_worlds_.clear();
    collision_document_ = nullptr;
    selected_collision_.reset();
    measurement_a_.reset();
    measurement_b_.reset();
    collision_triangle_mapping_.clear();
    instance_root_inverse_.clear();
    vertices_.clear();
    uv_sets_.clear();
    faces_.clear();
    gpu_vertices_.clear();
    draw_batches_.clear();
    batch_bounds_.clear();
    batch_order_.clear();
    visual_triangle_total_ = collision_triangle_total_ = 0;
    material_colors_.clear();
    material_textures_.clear();
    material_lightmap_textures_.clear();
    material_texture_names_.clear();
    material_lightmap_texture_names_.clear();
    owned_texture_ids_.clear();
    translucent_texture_ids_.clear();
    checker_texture_ = 0;
    visual_material_slot_count_ = 0;
    loaded_texture_count_ = missing_texture_count_ = 0;
    texture_status_.clear();
    texture_diagnostics_.clear();
    selected_uv_set_ = 0;
    error_.clear();
    skeleton_lines_.clear();
    physics_lines_.clear();
    animated_actor_ranges_.clear();
    actor_material_layouts_.clear();
    actor_hand_poses_.clear();
    animated_actor_dirty_ = false;
    preserve_view_on_scene_reload_ = false;
}

void GeometryPreview::set_mission_overlays(MissionOverlaySet overlays) {
    mission_points_ = std::move(overlays.points);
    mission_lines_ = std::move(overlays.lines);
    mission_faces_ = std::move(overlays.faces);
    mission_relations_.clear();
    for (const auto& [a, b] : overlays.relations) {
        mission_relations_[a].push_back(b);
        mission_relations_[b].push_back(a);
    }
    selected_mission_entry_.reset();
    rebuild_overlay_indexes();
}

void GeometryPreview::set_texture_catalog(csf::TextureCatalog catalog) {
    texture_catalog_ = std::move(catalog);
    texture_variant_ = std::min(texture_variant_, texture_catalog_.maximum_variant());
}

void GeometryPreview::set_mission_actor_models(std::vector<MissionActorModel> models) {
    mission_actor_models_ = std::move(models);
    scene_mode_ = false;
}

void GeometryPreview::update_mission(MissionOverlaySet overlays, std::vector<MissionActorModel> models,
                                     const std::optional<std::uint32_t> selected) {
    set_mission_overlays(std::move(overlays));
    selected_mission_entry_ = selected;
    // Entries that kept their record, prototype and attachments only need a new
    // placement; any other difference changes the geometry in the scene.
    const bool same_layout =
        scene_mode_ && models.size() == mission_actor_models_.size() &&
        std::ranges::equal(models, mission_actor_models_, [](const auto& a, const auto& b) {
            return a.source_entry == b.source_entry && a.prototype == b.prototype &&
                   std::ranges::equal(a.attachments, b.attachments, [](const auto& x, const auto& y) {
                       return x.model == y.model && x.left_hand == y.left_hand;
                   });
        });
    if (same_layout) {
        for (const auto& model : models) {
            const auto& current = *std::ranges::find(mission_actor_models_, model.source_entry,
                                                     &MissionActorModel::source_entry);
            if (current.position.x != model.position.x || current.position.y != model.position.y ||
                current.position.z != model.position.z || current.heading_radians != model.heading_radians ||
                current.pitch_radians != model.pitch_radians)
                place_mission_actor(model.source_entry, model.position, model.heading_radians,
                                    model.pitch_radians);
        }
        return;
    }
    // Keep playback on actors that still exist; record entries may have moved.
    for (auto& model : models) {
        const auto previous = std::ranges::find(mission_actor_models_, model.source_entry,
                                                &MissionActorModel::source_entry);
        if (previous != mission_actor_models_.end() && previous->prototype == model.prototype) {
            model.animation = previous->animation;
            model.animation_time = previous->animation_time;
            model.animation_loop = previous->animation_loop;
        }
    }
    mission_actor_models_ = std::move(models);
    hidden_mission_entries_.clear();
    preserve_view_on_scene_reload_ = true;
    scene_mode_ = false;
}

void GeometryPreview::place_mission_actor(const std::uint32_t source_entry, const rws::Vec3 position,
                                          const float heading_radians, const float pitch_radians) {
    const auto found = std::ranges::find(mission_actor_models_, source_entry,
                                         &MissionActorModel::source_entry);
    if (found == mission_actor_models_.end()) return;
    found->position = position;
    found->heading_radians = heading_radians;
    found->pitch_radians = pitch_radians;
    if (!scene_mode_) return; // The next scene build uses the new placement.
    const float cy = std::cos(heading_radians), sy = std::sin(heading_radians),
                cp = std::cos(pitch_radians), sp = std::sin(pitch_radians);
    const AffineTransform placement{{{cy, sy * sp, sy * cp, 0, cp, -sp, -sy, cy * sp, cy * cp}}, position};
    auto attachment_placement = placement;
    const auto hand_pose = actor_hand_poses_.find(source_entry);
    if (hand_pose != actor_hand_poses_.end() && hand_pose->second.followed)
        attachment_placement = compose(placement, draw_matrix_transform(hand_pose->second.transform));
    for (std::size_t i = 0; i < draw_batches_.size(); ++i) {
        auto& batch = draw_batches_[i];
        if (actor_owner(batch.owner_offset) != source_entry) continue;
        batch.transform = draw_transform(batch.actor_attachment ? attachment_placement : placement);
        if (i < batch_bounds_.size()) update_batch_bounds(i);
    }
    animating_frame_ = ImGui::GetFrameCount();
}

void GeometryPreview::place_scene_instance(const std::uint64_t offset, const std::array<float, 9>& rotation,
                                           const rws::Vec3 position) {
    const auto root = instance_root_inverse_.find(offset);
    if (!scene_mode_ || root == instance_root_inverse_.end()) return;
    const AffineTransform transform{{{rotation[0], rotation[3], rotation[6], rotation[1], rotation[4], rotation[7],
                                      rotation[2], rotation[5], rotation[8]}},
                                    position};
    const auto placement = draw_transform(compose(transform, draw_matrix_transform(root->second)));
    for (std::size_t i = 0; i < draw_batches_.size(); ++i) {
        if (draw_batches_[i].owner_offset != offset) continue;
        draw_batches_[i].transform = placement;
        if (i < batch_bounds_.size()) update_batch_bounds(i);
    }
    animating_frame_ = ImGui::GetFrameCount();
}

void GeometryPreview::set_mission_entries_visible(const std::span<const std::uint32_t> entries,
                                                   const bool visible) {
    for (const auto entry : entries) {
        if (visible)
            hidden_mission_entries_.erase(entry);
        else
            hidden_mission_entries_.insert(entry);
    }
}

void GeometryPreview::set_mission_entry_states(std::unordered_set<std::uint32_t> hidden,
                                               std::unordered_set<std::uint32_t> locked) {
    hidden_mission_entries_ = std::move(hidden);
    locked_mission_entries_ = std::move(locked);
}

bool GeometryPreview::mission_entry_visible(const std::uint32_t entry) const noexcept {
    return !hidden_mission_entries_.contains(entry);
}

bool GeometryPreview::set_mission_actor_animation(const std::uint32_t source_entry,
                                                  std::shared_ptr<const rws::AnimationClip> clip,
                                                  const float time, const bool loop) {
    const auto found = std::ranges::find_if(mission_actor_models_, [&](const auto& actor) {
        return actor.source_entry == source_entry;
    });
    if (found == mission_actor_models_.end()) return false;
    const bool already_animated =
        std::ranges::any_of(animated_actor_ranges_,
                            [&](const auto& range) { return range.source_entry == source_entry; });
    found->animation = std::move(clip);
    found->animation_time = time;
    found->animation_loop = loop;
    if (already_animated && scene_mode_) {
        // The actor already owns a stable vertex range; re-skin it in place
        // instead of rebuilding the whole scene.
        animated_actor_dirty_ = true;
    } else {
        // First assignment needs a rebuild so the actor gains a vertex range.
        preserve_view_on_scene_reload_ = true;
        scene_mode_ = false;
    }
    return true;
}

void GeometryPreview::refresh_mission_actor_animation() {
    animated_actor_dirty_ = false;
    if (!scene_mode_ || !actor_geometry_builder_ || animated_actor_ranges_.empty() ||
        vertex_buffer_ == 0)
        return;
    auto& gl = gl_api();
    if (!gl.bind_buffer || !gl.buffer_sub_data) return;
    for (const auto& range : animated_actor_ranges_) {
        const auto found = std::ranges::find_if(mission_actor_models_, [&](const auto& actor) {
            return actor.source_entry == range.source_entry;
        });
        if (found == mission_actor_models_.end() || !found->prototype || !found->animation)
            continue;
        // Build into a scratch array, then splice it into the stable range so the
        // layout of gpu_vertices_ never shifts.
        auto saved = std::move(gpu_vertices_);
        gpu_vertices_.clear();
        gpu_vertices_.reserve(range.vertex_count);
        actor_geometry_builder_(*found->prototype, *found);
        auto rebuilt = std::move(gpu_vertices_);
        gpu_vertices_ = std::move(saved);
        if (rebuilt.size() != range.vertex_count ||
            range.vertex_begin + range.vertex_count > gpu_vertices_.size()) {
            scene_mode_ = false; // Unexpected layout change; rebuild safely next frame.
            return;
        }
        std::copy(rebuilt.begin(), rebuilt.end(),
                  gpu_vertices_.begin() + static_cast<std::ptrdiff_t>(range.vertex_begin));
        const float cy = std::cos(found->heading_radians),
                    sy = std::sin(found->heading_radians),
                    cp = std::cos(found->pitch_radians), sp = std::sin(found->pitch_radians);
        const AffineTransform placement{
            {{cy, sy * sp, sy * cp, 0, cp, -sp, -sy, cy * sp, cy * cp}}, found->position};
        auto attachment_placement = placement;
        const auto hand_pose = actor_hand_poses_.find(found->source_entry);
        if (hand_pose != actor_hand_poses_.end() && hand_pose->second.followed)
            attachment_placement =
                compose(placement, draw_matrix_transform(hand_pose->second.transform));
        for (std::size_t i = 0; i < draw_batches_.size(); ++i) {
            auto& batch = draw_batches_[i];
            if (actor_owner(batch.owner_offset) != found->source_entry) continue;
            if (batch.actor_attachment) batch.transform = draw_transform(attachment_placement);
            if (i < batch_bounds_.size()) update_batch_bounds(i); // The pose moved its vertices.
        }
        gl.bind_buffer(gl_array_buffer, vertex_buffer_);
        gl.buffer_sub_data(gl_array_buffer,
                           static_cast<GlSizePtr>(range.vertex_begin * sizeof(GpuVertex)),
                           static_cast<GlSizePtr>(range.vertex_count * sizeof(GpuVertex)),
                           gpu_vertices_.data() + range.vertex_begin);
    }
}

void GeometryPreview::select_uv_set(const std::size_t index) {
    if (index >= uv_sets_.size()) return;
    selected_uv_set_ = index;
    const auto& selected = uv_sets_[selected_uv_set_];
    for (auto& vertex : gpu_vertices_) {
        const auto uv =
            vertex.source_index < selected.size() ? selected[vertex.source_index] : Uv{};
        vertex.debug_u = uv.u;
        vertex.debug_v = uv.v;
    }
    if (vertex_buffer_ != 0) {
        auto& gl = gl_api();
        gl.bind_buffer(gl_array_buffer, vertex_buffer_);
        gl.buffer_data(gl_array_buffer,
                       static_cast<GlSizePtr>(gpu_vertices_.size() * sizeof(GpuVertex)),
                       gpu_vertices_.data(), gl_static_draw);
    }
}

void GeometryPreview::reset_view() {
    yaw_ = -0.65F;
    pitch_ = scene_mode_ ? 0.35F : -0.35F;
    target_yaw_ = yaw_;
    target_pitch_ = pitch_;
    // Fit the framed volume to the vertical 50 degree field of view with a small
    // margin. A box is fitted corner by corner from the default view direction;
    // a sphere only by its radius.
    constexpr float tan_half_fov = 0.46630766F; // tan(25 degrees)
    constexpr float sin_half_fov = 0.42261826F; // sin(25 degrees)
    constexpr float margin = 1.08F;
    const float aspect = canvas_width_ > 1.0F && canvas_height_ > 1.0F
                             ? canvas_width_ / canvas_height_
                             : 16.0F / 9.0F;
    float distance = radius_ / sin_half_fov;
    if (frame_extent_) {
        // Basis vectors of the view in world space: right, up, toward the camera.
        const float cy = std::cos(yaw_), sy = std::sin(yaw_), cp = std::cos(pitch_),
                    sp = std::sin(pitch_);
        const std::array<rws::Vec3, 3> rows =
            scene_mode_ ? std::array<rws::Vec3, 3>{{{cy, 0, -sy},
                                                    {-sp * sy, cp, -sp * cy},
                                                    {cp * sy, sp, cp * cy}}}
                        : std::array<rws::Vec3, 3>{{{cy, -sy, 0},
                                                    {-sp * sy, -sp * cy, cp},
                                                    {cp * sy, cp * cy, sp}}};
        const auto dot = [](const rws::Vec3 a, const rws::Vec3 b) {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        };
        distance = 0.0F;
        for (int corner = 0; corner < 8; ++corner) {
            const rws::Vec3 point{(corner & 1) ? frame_extent_->x : -frame_extent_->x,
                                  (corner & 2) ? frame_extent_->y : -frame_extent_->y,
                                  (corner & 4) ? frame_extent_->z : -frame_extent_->z};
            const float x = dot(point, rows[0]), y = dot(point, rows[1]), z = dot(point, rows[2]);
            distance = std::max({distance, z + std::fabs(x) / (tan_half_fov * aspect),
                                 z + std::fabs(y) / tan_half_fov});
        }
    }
    distance_ = std::max(distance * margin, 0.01F);
    orthographic_scale_ = std::max(radius_ * 1.15F, 0.01F);
    pan_x_ = pan_y_ = 0.0F;
    navigation_offset_ = {};
    target_navigation_offset_ = {};
    preserve_camera_position_ = false;
}

void GeometryPreview::frame_bounds(const rws::Vec3 center, const float radius,
                                   const std::optional<rws::Vec3> half_extent) {
    center_ = center;
    radius_ = std::max(radius, 0.001F);
    frame_extent_ = half_extent;
    reset_view();
}

void GeometryPreview::pan_camera(const float delta_x, const float delta_y) {
    const float focal = std::max(std::min(canvas_width_, canvas_height_) * 0.78F, 1.0F);
    pan_x_ += delta_x / focal;
    pan_y_ += delta_y / focal;
}

rws::Vec3 GeometryPreview::camera_offset(const float yaw, const float pitch) const {
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const std::array<rws::Vec3, 3> rows =
        scene_mode_ ? std::array<rws::Vec3, 3>{{{cy, 0, -sy},
                                                {-sp * sy, cp, -sp * cy},
                                                {cp * sy, sp, cp * cy}}}
                    : std::array<rws::Vec3, 3>{
                          {{cy, -sy, 0}, {-sp * sy, -sp * cy, cp}, {cp * sy, cp * cy, sp}}};
    const rws::Vec3 view_offset{-pan_x_ * distance_, pan_y_ * distance_, distance_};
    return {rows[0].x * view_offset.x + rows[1].x * view_offset.y + rows[2].x * view_offset.z,
            rows[0].y * view_offset.x + rows[1].y * view_offset.y + rows[2].y * view_offset.z,
            rows[0].z * view_offset.x + rows[1].z * view_offset.y + rows[2].z * view_offset.z};
}

std::optional<std::uint64_t> GeometryPreview::pick_scene(const float mouse_x,
                                                         const float mouse_y) const {
    if (!scene_mode_ || canvas_width_ <= 0.0F || canvas_height_ <= 0.0F) return std::nullopt;
    const auto ray = viewport_ray(mouse_x, mouse_y);
    if (!ray) return std::nullopt;
    const auto clip_planes = active_clip_planes();
    const auto origin = ray->origin;
    const auto direction = ray->direction;

    float closest = std::numeric_limits<float>::max();
    std::optional<std::uint64_t> result;
    for (const auto& batch : draw_batches_) {
        if (batch.layer == PreviewLayer::collision_world || !show_visual_) continue;
        const auto end = static_cast<std::size_t>(batch.first) + batch.count;
        for (std::size_t i = batch.first; i + 2 < end; i += 3) {
            const auto& av = gpu_vertices_[i];
            const auto& bv = gpu_vertices_[i + 1];
            const auto& cv = gpu_vertices_[i + 2];
            const auto a = transform_draw_point(batch.transform, {av.x, av.y, av.z});
            const auto b = transform_draw_point(batch.transform, {bv.x, bv.y, bv.z});
            const auto c = transform_draw_point(batch.transform, {cv.x, cv.y, cv.z});
            const rws::Vec3 edge1{b.x - a.x, b.y - a.y, b.z - a.z};
            const rws::Vec3 edge2{c.x - a.x, c.y - a.y, c.z - a.z};
            const rws::Vec3 p{direction.y * edge2.z - direction.z * edge2.y,
                              direction.z * edge2.x - direction.x * edge2.z,
                              direction.x * edge2.y - direction.y * edge2.x};
            const float determinant = edge1.x * p.x + edge1.y * p.y + edge1.z * p.z;
            if (std::abs(determinant) < 0.000001F) continue;
            const float inverse_determinant = 1.0F / determinant;
            const rws::Vec3 offset{origin.x - a.x, origin.y - a.y, origin.z - a.z};
            const float u =
                (offset.x * p.x + offset.y * p.y + offset.z * p.z) * inverse_determinant;
            if (u < 0.0F || u > 1.0F) continue;
            const rws::Vec3 q{offset.y * edge1.z - offset.z * edge1.y,
                              offset.z * edge1.x - offset.x * edge1.z,
                              offset.x * edge1.y - offset.y * edge1.x};
            const float v =
                (direction.x * q.x + direction.y * q.y + direction.z * q.z) * inverse_determinant;
            if (v < 0.0F || u + v > 1.0F) continue;
            const float distance =
                (edge2.x * q.x + edge2.y * q.y + edge2.z * q.z) * inverse_determinant;
            const rws::Vec3 hit_position{origin.x + direction.x * distance,
                                         origin.y + direction.y * distance,
                                         origin.z + direction.z * distance};
            if (distance > 0.0F && distance < closest &&
                rws::collision_point_visible(hit_position, clip_planes)) {
                closest = distance;
                result = batch.owner_offset;
            }
        }
    }
    return result;
}

std::optional<rws::CollisionRay> GeometryPreview::viewport_ray(const float mouse_x,
                                                               const float mouse_y) const {
    if (!scene_mode_ || canvas_width_ <= 0 || canvas_height_ <= 0) return std::nullopt;
    const float x = 2.0F * (mouse_x - canvas_x_) / canvas_width_ - 1.0F;
    const float y = 1.0F - 2.0F * (mouse_y - canvas_y_) / canvas_height_;
    const float aspect = canvas_width_ / canvas_height_;
    const rws::Vec3 target{center_.x + navigation_offset_.x, center_.y + navigation_offset_.y,
                           center_.z + navigation_offset_.z};
    if (projection_ != 0) {
        const float horizontal = (x * aspect - pan_x_) * orthographic_scale_;
        const float vertical = (y + pan_y_) * orthographic_scale_;
        if (projection_ == 1) // Top: source Y is vertical; screen is X/Z.
            return rws::CollisionRay{
                {target.x + horizontal, target.y + distance_, target.z + vertical}, {0, -1, 0}};
        if (projection_ == 2)
            return rws::CollisionRay{
                {target.x + horizontal, target.y + vertical, target.z + distance_}, {0, 0, -1}};
        return rws::CollisionRay{{target.x + distance_, target.y + vertical, target.z - horizontal},
                                 {-1, 0, 0}};
    }
    constexpr float tan_half_fov = 0.46630766F;
    rws::Vec3 view{x * aspect * tan_half_fov, y * tan_half_fov, -1};
    const float length = std::sqrt(view.x * view.x + view.y * view.y + 1);
    view = {view.x / length, view.y / length, view.z / length};
    const float cy = std::cos(yaw_), sy = std::sin(yaw_), cp = std::cos(pitch_),
                sp = std::sin(pitch_);
    const std::array<rws::Vec3, 3> rows{
        {{cy, 0, -sy}, {-sp * sy, cp, -sp * cy}, {cp * sy, sp, cp * cy}}};
    const auto inverse = [&](const rws::Vec3 v) {
        return rws::Vec3{rows[0].x * v.x + rows[1].x * v.y + rows[2].x * v.z,
                         rows[0].y * v.x + rows[1].y * v.y + rows[2].y * v.z,
                         rows[0].z * v.x + rows[1].z * v.y + rows[2].z * v.z};
    };
    const auto offset = camera_offset(yaw_, pitch_);
    return rws::CollisionRay{{target.x + offset.x, target.y + offset.y, target.z + offset.z},
                             inverse(view)};
}

std::optional<rws::CollisionHit> GeometryPreview::pick_collision(const float mouse_x,
                                                                 const float mouse_y) const {
    if (!show_collision_ || !collision_document_) return std::nullopt;
    const auto ray = viewport_ray(mouse_x, mouse_y);
    if (!ray) return std::nullopt;
    return rws::pick_collision_worlds(collision_worlds_, collision_document_->bytes(), *ray,
                                      active_clip_planes());
}

GeometryPreview::ViewRotation GeometryPreview::view_rotation() const {
    const float yaw = projection_ == 3 ? 1.57079632679F : (projection_ == 0 ? yaw_ : 0.0F);
    const float pitch = projection_ == 1 ? -1.57079632679F : (projection_ == 0 ? pitch_ : 0.0F);
    auto& cache = view_rotation_cache_;
    if (cache.yaw != yaw || cache.pitch != pitch) {
        cache.yaw = yaw;
        cache.pitch = pitch;
        cache.rotation = {std::cos(yaw), std::sin(yaw), std::cos(pitch), std::sin(pitch)};
    }
    return cache.rotation;
}

std::optional<ImVec2> GeometryPreview::project_point(const rws::Vec3 point) const {
    if (canvas_width_ <= 0 || canvas_height_ <= 0) return std::nullopt;
    const auto [cy, sy, cp, sp] = view_rotation();
    const rws::Vec3 p{point.x - center_.x - navigation_offset_.x,
                      point.y - center_.y - navigation_offset_.y,
                      point.z - center_.z - navigation_offset_.z};
    const float rx = cy * p.x - sy * p.z, rz = sy * p.x + cy * p.z;
    const float view_scale = projection_ == 0 ? distance_ : orthographic_scale_;
    const rws::Vec3 view{rx + pan_x_ * view_scale, cp * p.y - sp * rz - pan_y_ * view_scale,
                         sp * p.y + cp * rz - distance_};
    const float aspect = canvas_width_ / canvas_height_;
    float nx{}, ny{};
    if (projection_ == 0) {
        if (view.z >= -0.001F) return std::nullopt;
        const float f = 1.0F / 0.46630766F;
        nx = view.x * f / (aspect * -view.z);
        ny = view.y * f / -view.z;
    } else {
        nx = view.x / (orthographic_scale_ * aspect);
        ny = view.y / orthographic_scale_;
    }
    if (!std::isfinite(nx) || !std::isfinite(ny)) return std::nullopt;
    return ImVec2{canvas_x_ + (nx + 1) * 0.5F * canvas_width_,
                  canvas_y_ + (1 - ny) * 0.5F * canvas_height_};
}

void GeometryPreview::update_keyboard_navigation() {
    const auto& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered();
    const bool fast =
        hovered && (ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift));
    // Distance-scaled movement gives level overview speed while zoomed out and
    // precise building/interior movement up close. Keep only a tiny world-scale
    // floor so a near-zero camera distance can never stall navigation entirely.
    const float speed_floor = scene_mode_ ? radius_ * 0.00005F : radius_ * 0.002F;
    const float navigation_scale = projection_ == 0 ? distance_ : orthographic_scale_;
    const float speed = std::max(navigation_scale * 0.35F, std::max(speed_floor, 0.01F)) *
                        navigation_speed_ * (fast ? 4.0F : 1.0F) * io.DeltaTime;
    const float cy = std::cos(yaw_), sy = std::sin(yaw_);
    rws::Vec3 forward = scene_mode_ ? rws::Vec3{-sy, 0.0F, -cy} : rws::Vec3{-sy, -cy, 0.0F};
    rws::Vec3 right = scene_mode_ ? rws::Vec3{cy, 0.0F, -sy} : rws::Vec3{cy, -sy, 0.0F};
    if (scene_mode_ && projection_ == 1) {
        forward = {0.0F, 0.0F, 1.0F};
        right = {1.0F, 0.0F, 0.0F};
    } else if (scene_mode_ && projection_ == 2) {
        forward = {0.0F, 1.0F, 0.0F};
        right = {1.0F, 0.0F, 0.0F};
    } else if (scene_mode_ && projection_ == 3) {
        forward = {0.0F, 1.0F, 0.0F};
        right = {0.0F, 0.0F, -1.0F};
    }
    auto move = [&](const rws::Vec3 direction, const float amount) {
        target_navigation_offset_.x += direction.x * amount;
        target_navigation_offset_.y += direction.y * amount;
        target_navigation_offset_.z += direction.z * amount;
    };
    bool moving = false;
    const auto key_move = [&](const ImGuiKey key, const rws::Vec3 direction, const float amount) {
        // Ctrl chords (Ctrl+D, Ctrl+S, ...) are commands, not movement.
        if (!hovered || io.KeyCtrl || !ImGui::IsKeyDown(key)) return;
        move(direction, amount);
        moving = true;
    };
    key_move(ImGuiKey_W, forward, speed);
    key_move(ImGuiKey_S, forward, -speed);
    key_move(ImGuiKey_D, right, speed);
    key_move(ImGuiKey_A, right, -speed);
    const rws::Vec3 up = scene_mode_ ? rws::Vec3{0, 1, 0} : rws::Vec3{0, 0, 1};
    key_move(ImGuiKey_E, up, speed);
    key_move(ImGuiKey_Q, up, -speed);

    // Easing is exponential, so it would creep toward the target for seconds;
    // it snaps once the remainder is invisible, letting the frame loop go idle.
    const auto ease = [](float& value, const float target, const float alpha, const float epsilon) {
        value += (target - value) * alpha;
        if (std::abs(target - value) <= epsilon) value = target;
    };
    const float rotation_alpha = 1.0F - std::exp(-18.0F * std::min(io.DeltaTime, 0.1F));
    const auto old_camera_offset = camera_offset(yaw_, pitch_);
    ease(yaw_, target_yaw_, rotation_alpha, 1e-5F);
    ease(pitch_, target_pitch_, rotation_alpha, 1e-5F);
    if (preserve_camera_position_) {
        const auto new_camera_offset = camera_offset(yaw_, pitch_);
        const rws::Vec3 correction{old_camera_offset.x - new_camera_offset.x,
                                   old_camera_offset.y - new_camera_offset.y,
                                   old_camera_offset.z - new_camera_offset.z};
        navigation_offset_.x += correction.x;
        navigation_offset_.y += correction.y;
        navigation_offset_.z += correction.z;
        target_navigation_offset_.x += correction.x;
        target_navigation_offset_.y += correction.y;
        target_navigation_offset_.z += correction.z;
        if (std::abs(target_yaw_ - yaw_) < 0.0001F && std::abs(target_pitch_ - pitch_) < 0.0001F)
            preserve_camera_position_ = false;
    }
    const float movement_alpha = 1.0F - std::exp(-12.0F * std::min(io.DeltaTime, 0.1F));
    const float movement_epsilon = std::max(navigation_scale * 1e-5F, 1e-6F);
    ease(navigation_offset_.x, target_navigation_offset_.x, movement_alpha, movement_epsilon);
    ease(navigation_offset_.y, target_navigation_offset_.y, movement_alpha, movement_epsilon);
    ease(navigation_offset_.z, target_navigation_offset_.z, movement_alpha, movement_epsilon);
    if (moving || yaw_ != target_yaw_ || pitch_ != target_pitch_ ||
        navigation_offset_.x != target_navigation_offset_.x ||
        navigation_offset_.y != target_navigation_offset_.y ||
        navigation_offset_.z != target_navigation_offset_.z || animated_actor_dirty_)
        animating_frame_ = ImGui::GetFrameCount();
}

bool GeometryPreview::load(const rws::Chunk& geometry_chunk, const std::span<const std::byte> bytes,
                           const std::filesystem::path& source_path) {
    release_geometry();
    chunk_offset_ = geometry_chunk.offset;
    const auto geometry = rws::decode_geometry(geometry_chunk, bytes);
    if (!geometry) {
        error_ = geometry.error;
        return false;
    }
    if (geometry.value->triangle_layout == rws::TriangleLayout::unknown) {
        error_ = "Triangle word order is ambiguous";
        return false;
    }
    const auto morph =
        std::find_if(geometry.value->morph_targets.begin(), geometry.value->morph_targets.end(),
                     [](const rws::MorphTargetInfo& value) { return value.has_vertices; });
    if (morph == geometry.value->morph_targets.end()) {
        error_ = "Geometry has no non-native vertex array";
        return false;
    }
    uv_sets_.reserve(geometry.value->texcoord_offsets.size());
    for (const auto uv_offset : geometry.value->texcoord_offsets) {
        const auto uv_bytes = static_cast<std::uint64_t>(geometry.value->vertex_count) * 8U;
        if (uv_offset <= bytes.size() && uv_bytes <= bytes.size() - uv_offset) {
            std::vector<Uv> uv_set;
            uv_set.reserve(static_cast<std::size_t>(geometry.value->vertex_count));
            for (std::int32_t i = 0; i < geometry.value->vertex_count; ++i) {
                const auto offset = uv_offset + static_cast<std::uint64_t>(i) * 8U;
                // RenderWare's PC UVs and DDS images both use a top-left origin. DDS rows are
                // uploaded unchanged, so retaining V preserves the original pairing. Flipping
                // only the coordinates mirrors atlas islands into unrelated lightmap regions.
                uv_set.push_back({read_f32(bytes, offset), read_f32(bytes, offset + 4)});
            }
            uv_sets_.push_back(std::move(uv_set));
        }
    }
    if (view_style_ >= 4 && view_style_ <= 6 && uv_sets_.size() > 1) selected_uv_set_ = 1;
    checker_texture_ = upload_checker_texture(true);

    if (const auto* material_list_chunk = rws::find_child(geometry_chunk, 0x08)) {
        const auto material_list = rws::decode_material_list(*material_list_chunk, bytes);
        if (material_list) {
            std::vector<const rws::Chunk*> material_chunks;
            for (const auto& child : material_list_chunk->children)
                if (child.type == 0x07) material_chunks.push_back(&child);
            material_colors_.resize(static_cast<std::size_t>(material_list.value->material_count),
                                    {190, 190, 190, 255});
            material_textures_.resize(material_colors_.size());
            material_lightmap_textures_.resize(material_colors_.size());
            material_texture_names_.resize(material_colors_.size());
            material_lightmap_texture_names_.resize(material_colors_.size());
            std::unordered_map<std::string, unsigned int> texture_cache;
            auto load_texture = [&](const std::string& name) -> unsigned int {
                if (name.empty()) return 0;
                if (const auto cached = texture_cache.find(name); cached != texture_cache.end())
                    return cached->second;
                const auto path =
                    find_texture(source_path, name, &texture_catalog_, texture_variant_);
                if (path.empty()) {
                    ++missing_texture_count_;
                    const auto message = "Missing texture '" + name + "' referenced by " +
                                         source_path.string();
                    texture_diagnostics_.push_back(message);
                    if (texture_status_.empty()) texture_status_ = message;
                    texture_cache.emplace(name, checker_texture_);
                    return checker_texture_;
                }
                int width{}, height{};
                std::vector<std::uint8_t> rgba;
                std::string texture_error;
                if (!rws::decode_texture_image(path, width, height, rgba, texture_error)) {
                    ++missing_texture_count_;
                    texture_diagnostics_.push_back(texture_error);
                    if (texture_status_.empty()) texture_status_ = texture_error;
                    texture_cache.emplace(name, checker_texture_);
                    return checker_texture_;
                }
                const auto id = upload_texture(width, height, rgba.data());
                owned_texture_ids_.push_back(id);
                if (has_fractional_alpha(rgba)) translucent_texture_ids_.insert(id);
                texture_cache.emplace(name, id);
                ++loaded_texture_count_;
                return id;
            };
            std::size_t next_material{};
            for (std::size_t i = 0; i < material_colors_.size(); ++i) {
                const auto remap =
                    i < material_list.value->remap.size() ? material_list.value->remap[i] : -1;
                if (remap >= 0 && static_cast<std::size_t>(remap) < i) {
                    material_colors_[i] = material_colors_[static_cast<std::size_t>(remap)];
                    material_textures_[i] = material_textures_[static_cast<std::size_t>(remap)];
                    material_lightmap_textures_[i] =
                        material_lightmap_textures_[static_cast<std::size_t>(remap)];
                    material_texture_names_[i] =
                        material_texture_names_[static_cast<std::size_t>(remap)];
                    material_lightmap_texture_names_[i] =
                        material_lightmap_texture_names_[static_cast<std::size_t>(remap)];
                    continue;
                }
                if (next_material >= material_chunks.size()) continue;
                const auto& material_chunk = *material_chunks[next_material++];
                const auto material = rws::decode_material(material_chunk, bytes);
                if (material) material_colors_[i] = material.value->color;
                if (const auto* texture_chunk = rws::find_child(material_chunk, 0x06)) {
                    const auto texture = rws::decode_texture(*texture_chunk, bytes);
                    if (texture && !texture.value->name.empty()) {
                        material_texture_names_[i] = texture.value->name;
                        material_textures_[i] = load_texture(texture.value->name);
                    }
                }
                const auto* extension = rws::find_child(material_chunk, 0x03);
                const auto* effects_chunk =
                    extension ? rws::find_child(*extension, 0x120) : nullptr;
                if (!effects_chunk) continue;
                const auto effects = rws::decode_material_effects(*effects_chunk, 0x07, bytes);
                if (!effects || !effects.value->has_dual_texture ||
                    effects.value->dual_texture.name.empty())
                    continue;
                material_lightmap_texture_names_[i] = effects.value->dual_texture.name;
                material_lightmap_textures_[i] = load_texture(effects.value->dual_texture.name);
            }
        }
    }
    const auto vertex_bytes = static_cast<std::uint64_t>(geometry.value->vertex_count) * 12U;
    if (morph->vertices_offset > bytes.size() ||
        vertex_bytes > bytes.size() - morph->vertices_offset) {
        error_ = "Geometry vertex array is outside the file";
        return false;
    }
    vertices_.reserve(static_cast<std::size_t>(geometry.value->vertex_count));
    rws::Vec3 minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max()};
    rws::Vec3 maximum{-minimum.x, -minimum.y, -minimum.z};
    for (std::int32_t i = 0; i < geometry.value->vertex_count; ++i) {
        const auto offset = morph->vertices_offset + static_cast<std::uint64_t>(i) * 12U;
        rws::Vec3 vertex{read_f32(bytes, offset), read_f32(bytes, offset + 4),
                         read_f32(bytes, offset + 8)};
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z)) {
            vertices_.clear();
            faces_.clear();
            error_ = "Geometry contains a non-finite vertex";
            return false;
        }
        vertices_.push_back(vertex);
        minimum.x = std::min(minimum.x, vertex.x);
        minimum.y = std::min(minimum.y, vertex.y);
        minimum.z = std::min(minimum.z, vertex.z);
        maximum.x = std::max(maximum.x, vertex.x);
        maximum.y = std::max(maximum.y, vertex.y);
        maximum.z = std::max(maximum.z, vertex.z);
    }
    center_ = {(minimum.x + maximum.x) * 0.5F, (minimum.y + maximum.y) * 0.5F,
               (minimum.z + maximum.z) * 0.5F};
    radius_ = 0.0F;
    for (const auto& vertex : vertices_) {
        const auto x = vertex.x - center_.x, y = vertex.y - center_.y, z = vertex.z - center_.z;
        radius_ = std::max(radius_, std::sqrt(x * x + y * y + z * z));
    }
    radius_ = std::max(radius_, 0.001F);
    frame_extent_ = rws::Vec3{(maximum.x - minimum.x) * 0.5F, (maximum.y - minimum.y) * 0.5F,
                              (maximum.z - minimum.z) * 0.5F};
    all_center_ = center_;
    all_radius_ = radius_;
    all_extent_ = frame_extent_;
    faces_.reserve(static_cast<std::size_t>(geometry.value->triangle_count));
    for (std::int32_t i = 0; i < geometry.value->triangle_count; ++i) {
        const auto triangle = rws::decode_triangle(*geometry.value, i, bytes);
        if (!triangle) {
            error_ = triangle.error;
            faces_.clear();
            return false;
        }
        if (triangle.value->vertices[0] >= vertices_.size() ||
            triangle.value->vertices[1] >= vertices_.size() ||
            triangle.value->vertices[2] >= vertices_.size()) {
            error_ = "Triangle references a vertex outside the geometry";
            faces_.clear();
            return false;
        }
        faces_.push_back({triangle.value->vertices[0], triangle.value->vertices[1],
                          triangle.value->vertices[2], triangle.value->material});
    }
    std::vector<std::size_t> face_order(faces_.size());
    for (std::size_t i = 0; i < face_order.size(); ++i)
        face_order[i] = i;
    std::stable_sort(face_order.begin(), face_order.end(),
                     [&](const std::size_t a, const std::size_t b) {
                         return faces_[a].material < faces_[b].material;
                     });
    gpu_vertices_.reserve(faces_.size() * 3U);
    for (const auto face_index : face_order) {
        const auto& face = faces_[face_index];
        if (draw_batches_.empty() || draw_batches_.back().material != face.material)
            draw_batches_.push_back({face.material,
                                     static_cast<std::uint32_t>(gpu_vertices_.size()), 0,
                                     geometry_chunk.offset});
        const auto& a = vertices_[face.a];
        const auto& b = vertices_[face.b];
        const auto& c = vertices_[face.c];
        const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
        const float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const float normal_length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (normal_length > 0.0F) {
            nx /= normal_length;
            ny /= normal_length;
            nz /= normal_length;
        }
        const std::array indices{face.a, face.b, face.c};
        for (const auto index : indices) {
            const auto base_uv = !uv_sets_.empty() && index < uv_sets_.front().size()
                                     ? uv_sets_.front()[index]
                                     : Uv{};
            const auto lightmap_uv =
                uv_sets_.size() > 1 && index < uv_sets_[1].size() ? uv_sets_[1][index] : Uv{};
            const auto debug_uv =
                selected_uv_set_ < uv_sets_.size() && index < uv_sets_[selected_uv_set_].size()
                    ? uv_sets_[selected_uv_set_][index]
                    : Uv{};
            const auto& vertex = vertices_[index];
            gpu_vertices_.push_back({vertex.x, vertex.y, vertex.z, base_uv.u, base_uv.v,
                                     lightmap_uv.u, lightmap_uv.v, debug_uv.u, debug_uv.v, nx, ny,
                                     nz, index});
        }
        draw_batches_.back().count += 3;
    }
    if (!create_gpu_resources()) return false;
    reset_view();
    return true;
}

bool GeometryPreview::load_scene(const std::vector<rws::Chunk>& chunks,
                                 const std::span<const std::byte> bytes,
                                 const std::span<const rws::SceneInstance> instances,
                                 const std::filesystem::path& source_path,
                                 const rws::Document* collision_document,
                                 const bool main_is_collision) {
    const bool preserve_view = preserve_view_on_scene_reload_;
    // A rebuild after an edit keeps the exact camera; the scene bounds (and the
    // orbit center derived from them) may have changed.
    const auto kept_camera = camera();
    release_geometry();
    scene_mode_ = true;
    wireframe_ = false;
    checker_texture_ = upload_checker_texture(true);
    visual_extent_.reset();
    collision_extent_.reset();

    rws::Vec3 minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max()};
    rws::Vec3 maximum{-minimum.x, -minimum.y, -minimum.z};
    std::unordered_map<std::string, unsigned int> texture_cache;
    auto load_texture = [&](const std::filesystem::path& texture_source,
                            const std::string& name) -> unsigned int {
        if (name.empty()) return 0;
        const auto path =
            find_texture(texture_source, name, &texture_catalog_, texture_variant_);
        const auto cache_key = path.empty()
                                   ? (texture_source.parent_path() / (name + ".dds")).string()
                                   : path.string();
        if (const auto cached = texture_cache.find(cache_key); cached != texture_cache.end())
            return cached->second;
        if (path.empty()) {
            ++missing_texture_count_;
            const auto message = "Missing texture '" + name + "' referenced by " +
                                 texture_source.string();
            texture_diagnostics_.push_back(message);
            texture_cache.emplace(cache_key, checker_texture_);
            if (texture_status_.empty()) texture_status_ = message;
            return checker_texture_;
        }
        int width{}, height{};
        std::vector<std::uint8_t> rgba;
        std::string texture_error;
        if (!rws::decode_texture_image(path, width, height, rgba, texture_error)) {
            ++missing_texture_count_;
            texture_diagnostics_.push_back(texture_error);
            if (texture_status_.empty()) texture_status_ = texture_error;
            texture_cache.emplace(cache_key, checker_texture_);
            return checker_texture_;
        }
        const auto id = upload_texture(width, height, rgba.data());
        owned_texture_ids_.push_back(id);
        if (has_fractional_alpha(rgba)) translucent_texture_ids_.insert(id);
        texture_cache.emplace(cache_key, id);
        ++loaded_texture_count_;
        return id;
    };

    struct PrototypeRange {
        std::size_t batch_begin{}, batch_end{};
        AffineTransform original_root;
    };
    std::unordered_map<std::uint32_t, PrototypeRange> prototypes;
    auto source_extension = source_path.extension().string();
    std::ranges::transform(source_extension, source_extension.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    for (const auto& clump_chunk : chunks) {
        if (clump_chunk.type != 0x10) continue;
        ++scene_clump_count_;
        const auto prototype_batch_begin = draw_batches_.size();
        std::optional<std::uint32_t> prototype_id;
        const auto* frame_list_chunk = rws::find_child(clump_chunk, 0x0E);
        const auto* geometry_list_chunk = rws::find_child(clump_chunk, 0x1A);
        if (!frame_list_chunk || !geometry_list_chunk) {
            ++scene_skipped_count_;
            continue;
        }
        const auto frames = rws::decode_frame_list(*frame_list_chunk, bytes);
        if (!frames) {
            ++scene_skipped_count_;
            continue;
        }
        std::vector<const rws::Chunk*> geometries;
        for (const auto& child : geometry_list_chunk->children)
            if (child.type == 0x0F) geometries.push_back(&child);

        std::vector<AffineTransform> world_frames(frames.value->frames.size());
        std::vector<std::uint8_t> frame_state(frames.value->frames.size());
        auto resolve_frame = [&](auto&& self, const std::size_t index) -> bool {
            if (index >= world_frames.size()) return false;
            if (frame_state[index] == 2) return true;
            if (frame_state[index] == 1) return false;
            frame_state[index] = 1;
            const auto& frame = frames.value->frames[index];
            const auto local = frame_transform(frame);
            if (frame.parent >= 0) {
                const auto parent = static_cast<std::size_t>(frame.parent);
                if (!self(self, parent)) return false;
                world_frames[index] = compose(world_frames[parent], local);
            } else {
                world_frames[index] = local;
            }
            frame_state[index] = 2;
            return true;
        };

        for (const auto& atomic_chunk : clump_chunk.children) {
            if (atomic_chunk.type != 0x14) continue;
            if (!prototype_id) {
                const auto* extension = rws::find_child(atomic_chunk, 0x03);
                const auto* pyro = extension ? rws::find_child(*extension, 0xFFFFFF00U) : nullptr;
                const auto metadata = pyro ? rws::decode_pyro_extension(*pyro, 0x14, bytes)
                                           : rws::DecodeResult<rws::PyroExtensionInfo>{};
                if (metadata) {
                    if (const auto index = metadata.value->atomic_object_index())
                        prototype_id = 1000U + *index;
                }
            }
            const auto atomic = rws::decode_atomic(atomic_chunk, bytes);
            if (!atomic || atomic.value->frame_index < 0 || atomic.value->geometry_index < 0 ||
                static_cast<std::size_t>(atomic.value->geometry_index) >= geometries.size() ||
                !resolve_frame(resolve_frame,
                               static_cast<std::size_t>(atomic.value->frame_index))) {
                ++scene_skipped_count_;
                continue;
            }
            const auto geometry = rws::decode_geometry(
                *geometries[static_cast<std::size_t>(atomic.value->geometry_index)], bytes);
            if (!geometry || geometry.value->triangle_layout == rws::TriangleLayout::unknown) {
                ++scene_skipped_count_;
                continue;
            }
            const auto& geometry_chunk =
                *geometries[static_cast<std::size_t>(atomic.value->geometry_index)];
            const auto* material_list_chunk = rws::find_child(geometry_chunk, 0x08);
            const auto material_list = material_list_chunk
                                           ? rws::decode_material_list(*material_list_chunk, bytes)
                                           : rws::DecodeResult<rws::MaterialListInfo>{};
            const std::size_t material_base = material_colors_.size();
            const auto material_count =
                material_list && material_list.value->material_count > 0
                    ? static_cast<std::size_t>(material_list.value->material_count)
                    : 1U;
            material_colors_.resize(material_base + material_count, {190, 190, 190, 255});
            material_textures_.resize(material_base + material_count);
            material_lightmap_textures_.resize(material_base + material_count);
            material_texture_names_.resize(material_base + material_count);
            material_lightmap_texture_names_.resize(material_base + material_count);
            if (material_list_chunk && material_list) {
                std::vector<const rws::Chunk*> material_chunks;
                for (const auto& child : material_list_chunk->children)
                    if (child.type == 0x07) material_chunks.push_back(&child);
                std::size_t next_material{};
                for (std::size_t slot = 0; slot < material_count; ++slot) {
                    const auto destination = material_base + slot;
                    const auto remap = slot < material_list.value->remap.size()
                                           ? material_list.value->remap[slot]
                                           : -1;
                    if (remap >= 0 && static_cast<std::size_t>(remap) < slot) {
                        const auto source = material_base + static_cast<std::size_t>(remap);
                        material_colors_[destination] = material_colors_[source];
                        material_textures_[destination] = material_textures_[source];
                        material_lightmap_textures_[destination] =
                            material_lightmap_textures_[source];
                        material_texture_names_[destination] = material_texture_names_[source];
                        material_lightmap_texture_names_[destination] =
                            material_lightmap_texture_names_[source];
                        continue;
                    }
                    if (next_material >= material_chunks.size()) continue;
                    const auto& material_chunk = *material_chunks[next_material++];
                    const auto material = rws::decode_material(material_chunk, bytes);
                    if (material) material_colors_[destination] = material.value->color;
                    if (const auto* texture_chunk = rws::find_child(material_chunk, 0x06)) {
                        const auto texture = rws::decode_texture(*texture_chunk, bytes);
                        if (texture && !texture.value->name.empty()) {
                            material_texture_names_[destination] = texture.value->name;
                            material_textures_[destination] =
                                load_texture(source_path, texture.value->name);
                        }
                    }
                    const auto* extension = rws::find_child(material_chunk, 0x03);
                    const auto* effects_chunk =
                        extension ? rws::find_child(*extension, 0x120) : nullptr;
                    if (effects_chunk) {
                        const auto effects =
                            rws::decode_material_effects(*effects_chunk, 0x07, bytes);
                        if (effects && effects.value->has_dual_texture &&
                            !effects.value->dual_texture.name.empty()) {
                            material_lightmap_texture_names_[destination] =
                                effects.value->dual_texture.name;
                            material_lightmap_textures_[destination] =
                                load_texture(source_path, effects.value->dual_texture.name);
                        }
                    }
                }
            }
            const auto morph = std::find_if(
                geometry.value->morph_targets.begin(), geometry.value->morph_targets.end(),
                [](const rws::MorphTargetInfo& value) { return value.has_vertices; });
            if (morph == geometry.value->morph_targets.end()) {
                ++scene_skipped_count_;
                continue;
            }
            const auto& transform =
                world_frames[static_cast<std::size_t>(atomic.value->frame_index)];
            std::vector<Uv> base_uvs, lightmap_uvs;
            if (!geometry.value->texcoord_offsets.empty()) {
                base_uvs.reserve(static_cast<std::size_t>(geometry.value->vertex_count));
                const auto offset = geometry.value->texcoord_offsets[0];
                for (std::int32_t i = 0; i < geometry.value->vertex_count; ++i)
                    base_uvs.push_back(
                        {read_f32(bytes, offset + static_cast<std::uint64_t>(i) * 8U),
                         read_f32(bytes, offset + static_cast<std::uint64_t>(i) * 8U + 4)});
            }
            if (geometry.value->texcoord_offsets.size() > 1) {
                lightmap_uvs.reserve(static_cast<std::size_t>(geometry.value->vertex_count));
                const auto offset = geometry.value->texcoord_offsets[1];
                for (std::int32_t i = 0; i < geometry.value->vertex_count; ++i)
                    lightmap_uvs.push_back(
                        {read_f32(bytes, offset + static_cast<std::uint64_t>(i) * 8U),
                         read_f32(bytes, offset + static_cast<std::uint64_t>(i) * 8U + 4)});
            }
            std::vector<rws::Vec3> instance_vertices;
            instance_vertices.reserve(static_cast<std::size_t>(geometry.value->vertex_count));
            for (std::int32_t i = 0; i < geometry.value->vertex_count; ++i) {
                const auto offset = morph->vertices_offset + static_cast<std::uint64_t>(i) * 12U;
                const auto vertex = transform_point(transform, {read_f32(bytes, offset),
                                                                read_f32(bytes, offset + 4),
                                                                read_f32(bytes, offset + 8)});
                instance_vertices.push_back(vertex);
                vertices_.push_back(vertex);
                minimum.x = std::min(minimum.x, vertex.x);
                minimum.y = std::min(minimum.y, vertex.y);
                minimum.z = std::min(minimum.z, vertex.z);
                maximum.x = std::max(maximum.x, vertex.x);
                maximum.y = std::max(maximum.y, vertex.y);
                maximum.z = std::max(maximum.z, vertex.z);
            }
            std::vector<rws::TriangleInfo> triangles;
            triangles.reserve(static_cast<std::size_t>(geometry.value->triangle_count));
            for (std::int32_t i = 0; i < geometry.value->triangle_count; ++i) {
                const auto decoded_triangle = rws::decode_triangle(*geometry.value, i, bytes);
                if (decoded_triangle) triangles.push_back(*decoded_triangle.value);
            }
            std::stable_sort(
                triangles.begin(), triangles.end(),
                [](const auto& left, const auto& right) { return left.material < right.material; });
            for (const auto& triangle : triangles) {
                if (triangle.vertices[0] >= instance_vertices.size() ||
                    triangle.vertices[1] >= instance_vertices.size() ||
                    triangle.vertices[2] >= instance_vertices.size())
                    continue;
                const auto local_material =
                    std::min<std::size_t>(triangle.material, material_count - 1);
                const auto global_material = material_base + local_material;
                if (draw_batches_.empty() || draw_batches_.back().material != global_material ||
                    draw_batches_.back().owner_offset != clump_chunk.offset)
                    draw_batches_.push_back({static_cast<std::uint16_t>(global_material),
                                             static_cast<std::uint32_t>(gpu_vertices_.size()), 0,
                                             clump_chunk.offset});
                const auto& a = instance_vertices[triangle.vertices[0]];
                const auto& b = instance_vertices[triangle.vertices[1]];
                const auto& c = instance_vertices[triangle.vertices[2]];
                float nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
                float ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
                float nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (length > 0.0F) {
                    nx /= length;
                    ny /= length;
                    nz /= length;
                }
                for (const auto index : triangle.vertices) {
                    const auto* vertex = &instance_vertices[index];
                    const auto base_uv = index < base_uvs.size() ? base_uvs[index] : Uv{};
                    const auto lightmap_uv =
                        index < lightmap_uvs.size() ? lightmap_uvs[index] : Uv{};
                    gpu_vertices_.push_back({vertex->x, vertex->y, vertex->z, base_uv.u, base_uv.v,
                                             lightmap_uv.u, lightmap_uv.v, lightmap_uv.u,
                                             lightmap_uv.v, nx, ny, nz, index});
                }
                faces_.push_back({});
                draw_batches_.back().count += 3;
            }
            if (!triangles.empty()) {
                ++scene_instance_count_;
            }
        }
        if (source_extension == ".rpc") {
            const rws::Chunk* hanim_chunk{};
            const auto find_hanim = [&](auto&& self,
                                        const std::vector<rws::Chunk>& values) -> void {
                for (const auto& value : values) {
                    if (!hanim_chunk && value.type == 0x11E) hanim_chunk = &value;
                    self(self, value.children);
                }
            };
            find_hanim(find_hanim, clump_chunk.children);
            if (hanim_chunk) {
                const auto hanim = rws::decode_hanim(*hanim_chunk, bytes);
                std::unordered_map<std::int32_t, std::int32_t> node_ids;
                if (hanim)
                    for (const auto& node : hanim.value->nodes)
                        node_ids[node.node_index] = node.node_id;
                for (std::size_t i = 0; i < frames.value->frames.size(); ++i) {
                    const auto parent = frames.value->frames[i].parent;
                    if (parent < 0 || static_cast<std::size_t>(parent) >= world_frames.size() ||
                        !resolve_frame(resolve_frame, i) ||
                        !resolve_frame(resolve_frame, static_cast<std::size_t>(parent)))
                        continue;
                    skeleton_lines_.push_back(
                        {world_frames[static_cast<std::size_t>(parent)].position,
                         world_frames[i].position, static_cast<std::int32_t>(i),
                         node_ids.contains(static_cast<std::int32_t>(i))
                             ? node_ids[static_cast<std::int32_t>(i)]
                             : -1});
                }
            }
        }
        if (prototype_id && !prototypes.contains(*prototype_id) &&
            draw_batches_.size() > prototype_batch_begin) {
            AffineTransform original_root;
            const auto root =
                std::find_if(frames.value->frames.begin(), frames.value->frames.end(),
                             [](const rws::FrameInfo& frame) { return frame.parent < 0; });
            if (root != frames.value->frames.end()) {
                const auto index = static_cast<std::size_t>(root - frames.value->frames.begin());
                if (resolve_frame(resolve_frame, index)) original_root = world_frames[index];
            }
            prototypes[*prototype_id] = {prototype_batch_begin, draw_batches_.size(),
                                         original_root};
        }
    }

    for (const auto& instance : instances) {
        const auto found = prototypes.find(instance.prototype_id);
        if (found == prototypes.end()) {
            ++scene_unresolved_instance_count_;
            continue;
        }
        const AffineTransform transform{
            {{instance.rotation[0], instance.rotation[3], instance.rotation[6],
              instance.rotation[1], instance.rotation[4], instance.rotation[7],
              instance.rotation[2], instance.rotation[5], instance.rotation[8]}},
            instance.position};
        for (auto batch_index = found->second.batch_begin; batch_index < found->second.batch_end;
             ++batch_index) {
            const auto source_batch = draw_batches_[batch_index];
            DrawBatch output_batch{source_batch.material, source_batch.first, source_batch.count,
                                   instance.offset};
            const auto root_inverse = inverse_transform(found->second.original_root);
            instance_root_inverse_[instance.offset] = draw_transform(root_inverse);
            const auto placement = compose(transform, root_inverse);
            output_batch.transform = draw_transform(placement);
            for (std::uint32_t i = 0; i < source_batch.count; ++i) {
                const auto& vertex =
                    gpu_vertices_[static_cast<std::size_t>(source_batch.first) + i];
                const auto local = inverse_transform_point(found->second.original_root,
                                                           {vertex.x, vertex.y, vertex.z});
                const auto position = transform_point(transform, local);
                minimum.x = std::min(minimum.x, position.x);
                minimum.y = std::min(minimum.y, position.y);
                minimum.z = std::min(minimum.z, position.z);
                maximum.x = std::max(maximum.x, position.x);
                maximum.y = std::max(maximum.y, position.y);
                maximum.z = std::max(maximum.z, position.z);
            }
            draw_batches_.push_back(output_batch);
        }
        ++scene_custom_instance_count_;
    }

    // Mission RPCs use one decoded/uploaded prototype range per resolved document.
    // Import their material tables once, then let static and animated actor draws
    // share the same stable material indices.
    std::unordered_map<const rws::Document*, std::vector<DrawBatch>> actor_prototypes;
    const auto register_actor_materials = [&](const rws::Document& prototype) {
        if (std::ranges::any_of(actor_material_layouts_, [&](const auto& value) {
                return value.prototype == &prototype;
            }))
            return;

        ActorPrototypeMaterials prototype_materials;
        prototype_materials.prototype = &prototype;
        const auto actor_bytes = prototype.bytes();
        for (const auto& clump : prototype.chunks()) {
            if (clump.type != 0x10) continue;
            const auto* geometry_list = rws::find_child(clump, 0x1A);
            if (!geometry_list) continue;
            for (const auto& geometry_chunk : geometry_list->children) {
                if (geometry_chunk.type != 0x0F) continue;
                ActorGeometryMaterials geometry_materials;
                geometry_materials.geometry_offset = geometry_chunk.offset;
                const auto* material_list_chunk = rws::find_child(geometry_chunk, 0x08);
                const auto material_list =
                    material_list_chunk
                        ? rws::decode_material_list(*material_list_chunk, actor_bytes)
                        : rws::DecodeResult<rws::MaterialListInfo>{};
                const auto material_count =
                    material_list && material_list.value->material_count > 0
                        ? static_cast<std::size_t>(material_list.value->material_count)
                        : 1U;
                if (material_colors_.size() + material_count >
                    static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) + 1U) {
                    if (texture_status_.empty())
                        texture_status_ = "Actor material count exceeds the preview limit";
                    continue;
                }

                const auto material_base = material_colors_.size();
                material_colors_.resize(material_base + material_count, {190, 205, 190, 255});
                material_textures_.resize(material_base + material_count);
                material_lightmap_textures_.resize(material_base + material_count);
                material_texture_names_.resize(material_base + material_count);
                material_lightmap_texture_names_.resize(material_base + material_count);
                geometry_materials.slots.reserve(material_count);
                for (std::size_t slot = 0; slot < material_count; ++slot)
                    geometry_materials.slots.push_back(
                        static_cast<std::uint16_t>(material_base + slot));

                if (material_list_chunk && material_list) {
                    std::vector<const rws::Chunk*> material_chunks;
                    for (const auto& child : material_list_chunk->children)
                        if (child.type == 0x07) material_chunks.push_back(&child);
                    std::size_t next_material{};
                    for (std::size_t slot = 0; slot < material_count; ++slot) {
                        const auto destination = material_base + slot;
                        const auto remap = slot < material_list.value->remap.size()
                                               ? material_list.value->remap[slot]
                                               : -1;
                        if (remap >= 0 && static_cast<std::size_t>(remap) < slot) {
                            const auto source = material_base + static_cast<std::size_t>(remap);
                            material_colors_[destination] = material_colors_[source];
                            material_textures_[destination] = material_textures_[source];
                            material_lightmap_textures_[destination] =
                                material_lightmap_textures_[source];
                            material_texture_names_[destination] = material_texture_names_[source];
                            material_lightmap_texture_names_[destination] =
                                material_lightmap_texture_names_[source];
                            continue;
                        }
                        if (next_material >= material_chunks.size()) continue;
                        const auto& material_chunk = *material_chunks[next_material++];
                        if (const auto material = rws::decode_material(material_chunk, actor_bytes))
                            material_colors_[destination] = material.value->color;
                        if (const auto* texture_chunk = rws::find_child(material_chunk, 0x06)) {
                            const auto texture = rws::decode_texture(*texture_chunk, actor_bytes);
                            if (texture && !texture.value->name.empty()) {
                                material_texture_names_[destination] = texture.value->name;
                                material_textures_[destination] =
                                    load_texture(prototype.source_path(), texture.value->name);
                            }
                        }
                        const auto* extension = rws::find_child(material_chunk, 0x03);
                        const auto* effects_chunk =
                            extension ? rws::find_child(*extension, 0x120) : nullptr;
                        if (!effects_chunk) continue;
                        const auto effects =
                            rws::decode_material_effects(*effects_chunk, 0x07, actor_bytes);
                        if (!effects || !effects.value->has_dual_texture ||
                            effects.value->dual_texture.name.empty())
                            continue;
                        material_lightmap_texture_names_[destination] =
                            effects.value->dual_texture.name;
                        material_lightmap_textures_[destination] = load_texture(
                            prototype.source_path(), effects.value->dual_texture.name);
                    }
                }
                prototype_materials.geometries.push_back(std::move(geometry_materials));
            }
        }
        actor_material_layouts_.push_back(std::move(prototype_materials));
    };
    for (const auto& actor : mission_actor_models_) {
        if (actor.prototype) register_actor_materials(*actor.prototype);
        for (const auto& attachment : actor.attachments)
            if (attachment.model) register_actor_materials(*attachment.model);
    }

    actor_geometry_builder_ = [this](const rws::Document& prototype,
                                     const MissionActorModel& actor) {
        std::vector<DrawBatch> batches;
        const auto actor_bytes = prototype.bytes();
        const auto prototype_materials =
            std::ranges::find_if(actor_material_layouts_, [&](const auto& value) {
                return value.prototype == &prototype;
            });
        for (const auto& clump : prototype.chunks()) {
            if (clump.type != 0x10) continue;
            const auto* frame_chunk = rws::find_child(clump, 0x0E);
            const auto* geometry_list = rws::find_child(clump, 0x1A);
            if (!frame_chunk || !geometry_list) continue;
            const auto frames = rws::decode_frame_list(*frame_chunk, actor_bytes);
            if (!frames) continue;
            std::vector<AffineTransform> world_frames(frames.value->frames.size());
            std::vector<unsigned char> states(world_frames.size());
            std::optional<rws::HAnimBinding> binding;
            std::optional<rws::AnimationClip> animation;
            if (actor.animation || !actor.attachments.empty()) {
                const auto decoded_binding = rws::decode_hanim_binding(*frame_chunk, actor_bytes);
                if (decoded_binding) {
                    binding = std::move(*decoded_binding.value);
                }
            }
            if (actor.animation && binding) {
                auto candidate = *actor.animation;
                const auto compatibility =
                    rws::map_animation_tracks(candidate, *binding, frames.value->frames.size());
                if (compatibility.compatible) animation = std::move(candidate);
            }
            const auto resolve = [&](auto&& self, std::size_t i) -> bool {
                if (i >= world_frames.size()) return false;
                if (states[i] == 2) return true;
                if (states[i] == 1) return false;
                states[i] = 1;
                const auto local = frame_transform(frames.value->frames[i]);
                const auto parent = frames.value->frames[i].parent;
                if (parent >= 0) {
                    if (!self(self, static_cast<std::size_t>(parent))) return false;
                    world_frames[i] =
                        compose(world_frames[static_cast<std::size_t>(parent)], local);
                } else
                    world_frames[i] = local;
                states[i] = 2;
                return true;
            };
            std::vector<const rws::Chunk*> geometries;
            for (const auto& child : geometry_list->children)
                if (child.type == 0x0F) geometries.push_back(&child);
            const auto find_skin = [&](auto&& self,
                                       const std::vector<rws::Chunk>& values) -> const rws::Chunk* {
                for (const auto& value : values) {
                    if (value.type == 0x116) return &value;
                    if (const auto* found = self(self, value.children)) return found;
                }
                return nullptr;
            };
            for (std::size_t i = 0; i < world_frames.size(); ++i)
                resolve(resolve, i);
            std::optional<rws::Pose> animated_pose;
            if (animation && binding) {
                for (const auto& atomic_chunk : clump.children) {
                    if (atomic_chunk.type != 0x14) continue;
                    const auto atomic = rws::decode_atomic(atomic_chunk, actor_bytes);
                    if (!atomic || atomic.value->frame_index < 0 ||
                        atomic.value->geometry_index < 0 ||
                        static_cast<std::size_t>(atomic.value->frame_index) >=
                            world_frames.size() ||
                        static_cast<std::size_t>(atomic.value->geometry_index) >= geometries.size())
                        continue;
                    const auto* geometry_chunk =
                        geometries[static_cast<std::size_t>(atomic.value->geometry_index)];
                    const auto* skin_chunk = find_skin(find_skin, geometry_chunk->children);
                    const auto geometry = rws::decode_geometry(*geometry_chunk, actor_bytes);
                    if (!skin_chunk || !geometry) continue;
                    const auto skin =
                        rws::decode_skin(*skin_chunk, geometry.value->vertex_count, actor_bytes);
                    if (!skin) continue;
                    const auto inverse_bind =
                        rws::decode_inverse_bind_matrices(*skin.value, actor_bytes);
                    const auto& frame =
                        world_frames[static_cast<std::size_t>(atomic.value->frame_index)];
                    const std::array<float, 16> atomic_bind{
                        frame.rotation[0], frame.rotation[3], frame.rotation[6], 0,
                        frame.rotation[1], frame.rotation[4], frame.rotation[7], 0,
                        frame.rotation[2], frame.rotation[5], frame.rotation[8], 0,
                        frame.position.x,  frame.position.y,  frame.position.z,  1};
                    const auto bind_pose = rws::recover_skin_bind_pose(*frames.value, *binding,
                                                                       inverse_bind, atomic_bind);
                    if (!bind_pose) continue;
                    animated_pose =
                        rws::evaluate_pose(*animation, *frames.value, bind_pose.value->local,
                                           actor.animation_time, actor.animation_loop);
                    break;
                }
            }
            if (!actor.attachments.empty() && binding) {
                ActorHandPose hand_pose;
                const bool left_hand = actor.attachments.front().left_hand;
                const auto labels = frame_labels(*frame_chunk, actor_bytes,
                                                 frames.value->frames.size());
                const auto hand_frame = find_hand_frame(*binding, labels, left_hand);
                if (hand_frame) {
                    hand_pose.frame_label =
                        !labels[*hand_frame].empty()
                            ? labels[*hand_frame]
                            : std::string(left_hand ? "left hand" : "right hand") +
                                  (*hand_frame < binding->frame_node_ids.size()
                                       ? " (HAnim " +
                                             std::to_string(
                                                 binding->frame_node_ids[*hand_frame]) +
                                             ")"
                                       : "");
                    if (animated_pose && *hand_frame < animated_pose->world.size())
                        hand_pose.transform =
                            draw_transform(matrix_transform(animated_pose->world[*hand_frame]));
                    else if (*hand_frame < world_frames.size())
                        hand_pose.transform = draw_transform(world_frames[*hand_frame]);
                    hand_pose.followed = *hand_frame < world_frames.size();
                }
                actor_hand_poses_[actor.source_entry] = std::move(hand_pose);
            }
            for (const auto& atomic_chunk : clump.children) {
                if (atomic_chunk.type != 0x14) continue;
                const auto atomic = rws::decode_atomic(atomic_chunk, actor_bytes);
                if (!atomic || atomic.value->frame_index < 0 || atomic.value->geometry_index < 0 ||
                    static_cast<std::size_t>(atomic.value->geometry_index) >= geometries.size() ||
                    !resolve(resolve, static_cast<std::size_t>(atomic.value->frame_index)))
                    continue;
                const auto geometry = rws::decode_geometry(
                    *geometries[static_cast<std::size_t>(atomic.value->geometry_index)],
                    actor_bytes);
                if (!geometry || geometry.value->triangle_layout == rws::TriangleLayout::unknown)
                    continue;
                const auto morph =
                    std::ranges::find_if(geometry.value->morph_targets,
                                         [](const auto& value) { return value.has_vertices; });
                if (morph == geometry.value->morph_targets.end()) continue;
                const auto& geometry_chunk =
                    *geometries[static_cast<std::size_t>(atomic.value->geometry_index)];
                const ActorGeometryMaterials* geometry_materials{};
                if (prototype_materials != actor_material_layouts_.end()) {
                    const auto found = std::ranges::find_if(
                        prototype_materials->geometries, [&](const auto& value) {
                            return value.geometry_offset == geometry_chunk.offset;
                        });
                    if (found != prototype_materials->geometries.end())
                        geometry_materials = &*found;
                }
                if (!geometry_materials || geometry_materials->slots.empty()) continue;
                std::vector<rws::Vec3> positions, normals;
                positions.reserve(static_cast<std::size_t>(geometry.value->vertex_count));
                if (morph->has_normals)
                    normals.reserve(static_cast<std::size_t>(geometry.value->vertex_count));
                const auto& frame =
                    world_frames[static_cast<std::size_t>(atomic.value->frame_index)];
                std::vector<rws::SkinVertex> skin_vertices;
                std::vector<std::array<float, 16>> inverse_bind, bone_world;
                const auto* skin_chunk = find_skin(
                    find_skin,
                    geometries[static_cast<std::size_t>(atomic.value->geometry_index)]->children);
                const auto skin =
                    skin_chunk
                        ? rws::decode_skin(*skin_chunk, geometry.value->vertex_count, actor_bytes)
                        : rws::DecodeResult<rws::SkinInfo>{};
                bool used_skinning{};
                if (animated_pose && skin && binding) {
                    skin_vertices.resize(static_cast<std::size_t>(geometry.value->vertex_count));
                    for (std::int32_t i = 0; i < geometry.value->vertex_count; ++i) {
                        const auto offset =
                            morph->vertices_offset + static_cast<std::uint64_t>(i) * 12U;
                        auto& v = skin_vertices[static_cast<std::size_t>(i)];
                        v.position = {read_f32(actor_bytes, offset),
                                      read_f32(actor_bytes, offset + 4),
                                      read_f32(actor_bytes, offset + 8)};
                        if (morph->has_normals) {
                            const auto normal_offset =
                                morph->normals_offset + static_cast<std::uint64_t>(i) * 12U;
                            v.normal = {read_f32(actor_bytes, normal_offset),
                                        read_f32(actor_bytes, normal_offset + 4),
                                        read_f32(actor_bytes, normal_offset + 8)};
                        }
                        for (std::size_t j = 0; j < 4; ++j) {
                            v.bones[j] =
                                std::to_integer<std::uint8_t>(actor_bytes[static_cast<std::size_t>(
                                    skin.value->vertex_indices_offset +
                                    static_cast<std::uint64_t>(i) * 4 + j)]);
                            v.weights[j] = read_f32(actor_bytes,
                                                    skin.value->vertex_weights_offset +
                                                        static_cast<std::uint64_t>(i) * 16 + j * 4);
                        }
                    }
                    inverse_bind = rws::decode_inverse_bind_matrices(*skin.value, actor_bytes);
                    bone_world.resize(skin.value->bone_count);
                    bool complete = true;
                    for (std::size_t b = 0; b < bone_world.size(); ++b) {
                        if (b >= binding->matrix_to_frame.size()) {
                            complete = false;
                            break;
                        }
                        const auto mapped = binding->matrix_to_frame[b];
                        if (mapped < 0 ||
                            static_cast<std::size_t>(mapped) >= animated_pose->world.size()) {
                            complete = false;
                            break;
                        }
                        bone_world[b] = animated_pose->world[static_cast<std::size_t>(mapped)];
                    }
                    if (complete) {
                        const auto skinned = rws::cpu_skin(skin_vertices, inverse_bind, bone_world);
                        for (const auto& v : skinned) {
                            positions.push_back(v.position);
                            if (morph->has_normals) normals.push_back(v.normal);
                        }
                        used_skinning = true;
                    }
                }
                if (!used_skinning) {
                    AffineTransform render_frame = frame;
                    if (!skin && animated_pose &&
                        static_cast<std::size_t>(atomic.value->frame_index) <
                            animated_pose->world.size()) {
                        const auto& matrix =
                            animated_pose
                                ->world[static_cast<std::size_t>(atomic.value->frame_index)];
                        render_frame.rotation = {matrix[0], matrix[4], matrix[8],
                                                 matrix[1], matrix[5], matrix[9],
                                                 matrix[2], matrix[6], matrix[10]};
                        render_frame.position = {matrix[12], matrix[13], matrix[14]};
                    }
                    for (std::int32_t i = 0; i < geometry.value->vertex_count; ++i) {
                        const auto offset =
                            morph->vertices_offset + static_cast<std::uint64_t>(i) * 12U;
                        positions.push_back(
                            transform_point(render_frame, {read_f32(actor_bytes, offset),
                                                           read_f32(actor_bytes, offset + 4),
                                                           read_f32(actor_bytes, offset + 8)}));
                        if (morph->has_normals) {
                            const auto normal_offset =
                                morph->normals_offset + static_cast<std::uint64_t>(i) * 12U;
                            auto normal = rotate(render_frame, {read_f32(actor_bytes, normal_offset),
                                                                read_f32(actor_bytes,
                                                                         normal_offset + 4),
                                                                read_f32(actor_bytes,
                                                                         normal_offset + 8)});
                            const auto normal_length = std::sqrt(normal.x * normal.x +
                                                                 normal.y * normal.y +
                                                                 normal.z * normal.z);
                            if (normal_length > 1.0e-8F) {
                                normal.x /= normal_length;
                                normal.y /= normal_length;
                                normal.z /= normal_length;
                            }
                            normals.push_back(normal);
                        }
                    }
                }
                std::vector<Uv> base_uvs, lightmap_uvs;
                if (!geometry.value->texcoord_offsets.empty()) {
                    base_uvs.reserve(static_cast<std::size_t>(geometry.value->vertex_count));
                    const auto offset = geometry.value->texcoord_offsets[0];
                    for (std::int32_t i = 0; i < geometry.value->vertex_count; ++i)
                        base_uvs.push_back(
                            {read_f32(actor_bytes, offset + static_cast<std::uint64_t>(i) * 8U),
                             read_f32(actor_bytes,
                                      offset + static_cast<std::uint64_t>(i) * 8U + 4)});
                }
                if (geometry.value->texcoord_offsets.size() > 1) {
                    lightmap_uvs.reserve(static_cast<std::size_t>(geometry.value->vertex_count));
                    const auto offset = geometry.value->texcoord_offsets[1];
                    for (std::int32_t i = 0; i < geometry.value->vertex_count; ++i)
                        lightmap_uvs.push_back(
                            {read_f32(actor_bytes, offset + static_cast<std::uint64_t>(i) * 8U),
                             read_f32(actor_bytes,
                                      offset + static_cast<std::uint64_t>(i) * 8U + 4)});
                }
                std::vector<rws::TriangleInfo> triangles;
                triangles.reserve(static_cast<std::size_t>(geometry.value->triangle_count));
                for (std::int32_t i = 0; i < geometry.value->triangle_count; ++i) {
                    const auto triangle = rws::decode_triangle(*geometry.value, i, actor_bytes);
                    if (!triangle || triangle.value->vertices[0] >= positions.size() ||
                        triangle.value->vertices[1] >= positions.size() ||
                        triangle.value->vertices[2] >= positions.size())
                        continue;
                    triangles.push_back(*triangle.value);
                }
                std::stable_sort(triangles.begin(), triangles.end(), [](const auto& left,
                                                                        const auto& right) {
                    return left.material < right.material;
                });
                DrawBatch* batch{};
                for (const auto& triangle : triangles) {
                    const auto local_material = std::min<std::size_t>(
                        triangle.material, geometry_materials->slots.size() - 1);
                    const auto material = geometry_materials->slots[local_material];
                    if (!batch || batch->material != material) {
                        batches.push_back({material,
                                           static_cast<std::uint32_t>(gpu_vertices_.size()), 0,
                                           geometry_chunk.offset});
                        batch = &batches.back();
                    }
                    const auto &a = positions[triangle.vertices[0]],
                               &b = positions[triangle.vertices[1]],
                               &c = positions[triangle.vertices[2]];
                    float nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y),
                          ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z),
                          nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                    const auto length = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (length > 0) {
                        nx /= length;
                        ny /= length;
                        nz /= length;
                    }
                    for (const auto index : triangle.vertices) {
                        const auto& p = positions[index];
                        const auto normal =
                            index < normals.size() ? normals[index] : rws::Vec3{nx, ny, nz};
                        const auto base_uv = index < base_uvs.size() ? base_uvs[index] : Uv{};
                        const auto lightmap_uv =
                            index < lightmap_uvs.size() ? lightmap_uvs[index] : Uv{};
                        gpu_vertices_.push_back({p.x, p.y, p.z, base_uv.u, base_uv.v,
                                                 lightmap_uv.u, lightmap_uv.v, base_uv.u,
                                                 base_uv.v, normal.x, normal.y, normal.z, index});
                    }
                    batch->count += 3;
                }
            }
        }
        return batches;
    };
    for (const auto& actor : mission_actor_models_) {
        if (!actor.prototype) continue;
        const float cy = std::cos(actor.heading_radians), sy = std::sin(actor.heading_radians),
                    cp = std::cos(actor.pitch_radians), sp = std::sin(actor.pitch_radians);
        const AffineTransform placement{{{cy, sy * sp, sy * cp, 0, cp, -sp, -sy, cy * sp, cy * cp}},
                                        actor.position};
        std::vector<DrawBatch> batches;
        if (actor.animation) {
            // Animated actors own a stable vertex range so playback can re-skin only this
            // actor instead of rebuilding the whole scene.
            const auto vertex_begin = gpu_vertices_.size();
            batches = actor_geometry_builder_(*actor.prototype, actor);
            animated_actor_ranges_.push_back(
                {actor.source_entry, vertex_begin, gpu_vertices_.size() - vertex_begin});
        } else {
            auto found = actor_prototypes.find(actor.prototype.get());
            if (found == actor_prototypes.end())
                found = actor_prototypes
                            .emplace(actor.prototype.get(),
                                     actor_geometry_builder_(*actor.prototype, actor))
                            .first;
            batches = found->second;
        }
        MissionActorModel attachment_actor = actor;
        attachment_actor.animation.reset();
        attachment_actor.attachments.clear();
        for (const auto& attachment : actor.attachments) {
            if (!attachment.model) continue;
            auto found = actor_prototypes.find(attachment.model.get());
            if (found == actor_prototypes.end())
                found = actor_prototypes
                            .emplace(attachment.model.get(),
                                     actor_geometry_builder_(*attachment.model, attachment_actor))
                            .first;
            const auto insertion = batches.size();
            batches.insert(batches.end(), found->second.begin(), found->second.end());
            for (auto index = insertion; index < batches.size(); ++index)
                batches[index].actor_attachment = true;
        }
        const auto hand_pose = actor_hand_poses_.find(actor.source_entry);
        const auto attachment_placement =
            hand_pose != actor_hand_poses_.end() && hand_pose->second.followed
                ? compose(placement, draw_matrix_transform(hand_pose->second.transform))
                : placement;
        for (auto batch : batches) {
            batch.owner_offset = 0x8000000000000000ULL | actor.source_entry;
            batch.transform =
                draw_transform(batch.actor_attachment ? attachment_placement : placement);
            for (std::uint32_t i = 0; i < batch.count; ++i) {
                const auto& v = gpu_vertices_[static_cast<std::size_t>(batch.first) + i];
                const auto p = transform_point(placement, {v.x, v.y, v.z});
                minimum = {std::min(minimum.x, p.x), std::min(minimum.y, p.y),
                           std::min(minimum.z, p.z)};
                maximum = {std::max(maximum.x, p.x), std::max(maximum.y, p.y),
                           std::max(maximum.z, p.z)};
            }
            draw_batches_.push_back(batch);
        }
        ++scene_custom_instance_count_;
    }

    const auto append_physics_body = [&](const rws::PhysicsBodyDefInfo& body, const ImU32 color) {
        const auto volumes = rws::flatten_physics_volumes(body.volume);
        for (const auto& instance : volumes) {
            const auto& b = instance.world_bounds;
            if (!b.valid) continue;
            minimum = {std::min(minimum.x, b.minimum.x), std::min(minimum.y, b.minimum.y),
                       std::min(minimum.z, b.minimum.z)};
            maximum = {std::max(maximum.x, b.maximum.x), std::max(maximum.y, b.maximum.y),
                       std::max(maximum.z, b.maximum.z)};
            const auto line = [&](rws::Vec3 a, rws::Vec3 c) {
                physics_lines_.push_back({rws::transform_point(instance.world_transform, a),
                                          rws::transform_point(instance.world_transform, c),
                                          color});
            };
            const auto& volume = *instance.volume;
            const float fatness = std::max(0.0F, volume.fatness);
            if (volume.kind == 0x10 && volume.box_half_extents) {
                const auto e = rws::Vec3{volume.box_half_extents->x + fatness,
                                         volume.box_half_extents->y + fatness,
                                         volume.box_half_extents->z + fatness};
                const std::array<rws::Vec3, 8> p{{{-e.x, -e.y, -e.z},
                                                  {e.x, -e.y, -e.z},
                                                  {-e.x, e.y, -e.z},
                                                  {e.x, e.y, -e.z},
                                                  {-e.x, -e.y, e.z},
                                                  {e.x, -e.y, e.z},
                                                  {-e.x, e.y, e.z},
                                                  {e.x, e.y, e.z}}};
                constexpr std::array<std::array<int, 2>, 12> edges{{{0, 1},
                                                                    {0, 2},
                                                                    {0, 4},
                                                                    {1, 3},
                                                                    {1, 5},
                                                                    {2, 3},
                                                                    {2, 6},
                                                                    {3, 7},
                                                                    {4, 5},
                                                                    {4, 6},
                                                                    {5, 7},
                                                                    {6, 7}}};
                for (const auto& edge : edges)
                    line(p[edge[0]], p[edge[1]]);
            } else if (volume.kind != 0x13) {
                float radius = fatness, half_height{};
                if (volume.kind == 0x11) {
                    radius = volume.cylinder_radius.value_or(0) + fatness;
                    half_height = volume.cylinder_half_height.value_or(0) + fatness;
                } else if (volume.kind == 0x0F)
                    half_height = volume.capsule_half_height.value_or(0);
                constexpr int segments = 24;
                for (int axis = 0; axis < (volume.kind == 0x0E ? 3 : 1); ++axis)
                    for (int i = 0; i < segments; ++i) {
                        const float a = static_cast<float>(i) * 6.283185307F / segments,
                                    c = static_cast<float>(i + 1) * 6.283185307F / segments;
                        auto point = [&](float angle, float height) {
                            if (axis == 1)
                                return rws::Vec3{std::cos(angle) * radius, 0,
                                                 std::sin(angle) * radius};
                            if (axis == 2)
                                return rws::Vec3{0, std::cos(angle) * radius,
                                                 std::sin(angle) * radius};
                            return rws::Vec3{std::cos(angle) * radius, height,
                                             std::sin(angle) * radius};
                        };
                        if (volume.kind == 0x0E)
                            line(point(a, 0), point(c, 0));
                        else {
                            line(point(a, -half_height), point(c, -half_height));
                            line(point(a, half_height), point(c, half_height));
                            if (i % 6 == 0) line(point(a, -half_height), point(a, half_height));
                        }
                    }
            }
        }
        const auto center = rws::transform_point(volumes.empty() ? rws::Matrix34{}
                                                                 : volumes.front().world_transform,
                                                 body.center_of_mass);
        physics_lines_.push_back({{center.x - 0.15F, center.y, center.z},
                                  {center.x + 0.15F, center.y, center.z},
                                  ui::viewport_color(ui::Viewport::physics_center)});
        physics_lines_.push_back({{center.x, center.y - 0.15F, center.z},
                                  {center.x, center.y + 0.15F, center.z},
                                  ui::viewport_color(ui::Viewport::physics_center)});
        physics_lines_.push_back({{center.x, center.y, center.z - 0.15F},
                                  {center.x, center.y, center.z + 0.15F},
                                  ui::viewport_color(ui::Viewport::physics_center)});
        if (rws::has_physics_body_flag(body.flags, rws::PhysicsBodyFlag::finite_rotation_axis))
            physics_lines_.push_back(
                {center,
                 {center.x + body.finite_rotation_axis.x, center.y + body.finite_rotation_axis.y,
                  center.z + body.finite_rotation_axis.z},
                 ui::viewport_color(ui::Viewport::physics_axis)});
    };
    const auto collect_physics = [&](auto&& self, const std::vector<rws::Chunk>& values) -> void {
        for (const auto& value : values) {
            if (value.type == 0x907) {
                const auto body = rws::decode_physics_body_def(value, bytes);
                if (body) append_physics_body(*body.value, ui::viewport_color(ui::Viewport::physics_body));
            } else if (value.type == 0x909) {
                const auto ragdoll = rws::decode_physics_ragdoll_def(value, bytes);
                if (ragdoll) {
                    for (const auto& body : ragdoll.value->bodies)
                        append_physics_body(body, ui::viewport_color(ui::Viewport::physics_ragdoll));
                    for (const auto& pair : ragdoll.value->joint_pairs)
                        if (pair[0] < ragdoll.value->bodies.size() &&
                            pair[1] < ragdoll.value->bodies.size()) {
                            const auto body_center = [&](const std::uint16_t index) {
                                const auto& body = ragdoll.value->bodies[index];
                                const auto volumes = rws::flatten_physics_volumes(body.volume);
                                return rws::transform_point(volumes.empty()
                                                                ? rws::Matrix34{}
                                                                : volumes.front().world_transform,
                                                            body.center_of_mass);
                            };
                            physics_lines_.push_back({body_center(pair[0]), body_center(pair[1]),
                                                      ui::viewport_color(ui::Viewport::physics_joint)});
                        }
                }
            }
            self(self, value.children);
        }
    };
    collect_physics(collect_physics, chunks);

    // CSF stores the terrain as a RenderWare World after its custom instance
    // region. Document recovery exposes the World itself, while its
    // historically short sector/plugin payloads make declared-size BSP walking
    // unreliable. Use the shared core recovery result rather than the parsed tree.
    const auto world_chunk = std::find_if(
        chunks.begin(), chunks.end(), [](const rws::Chunk& chunk) { return chunk.type == 0x0B; });
    if (!main_is_collision && world_chunk != chunks.end()) {
        const auto world = rws::decode_world(*world_chunk, bytes);
        if (world) {
            const auto* material_list_chunk = rws::find_child(*world_chunk, 0x08);
            const auto material_list = material_list_chunk
                                           ? rws::decode_material_list(*material_list_chunk, bytes)
                                           : rws::DecodeResult<rws::MaterialListInfo>{};
            const std::size_t material_base = material_colors_.size();
            const auto material_count =
                material_list && material_list.value->material_count > 0
                    ? static_cast<std::size_t>(material_list.value->material_count)
                    : 1U;
            material_colors_.resize(material_base + material_count, {155, 158, 150, 255});
            material_textures_.resize(material_base + material_count);
            material_lightmap_textures_.resize(material_base + material_count);
            material_texture_names_.resize(material_base + material_count);
            material_lightmap_texture_names_.resize(material_base + material_count);
            if (material_list_chunk && material_list) {
                std::vector<const rws::Chunk*> materials;
                for (const auto& child : material_list_chunk->children)
                    if (child.type == 0x07) materials.push_back(&child);
                std::size_t next_material{};
                for (std::size_t slot = 0; slot < material_count; ++slot) {
                    const auto destination = material_base + slot;
                    const auto remap = material_list.value->remap[slot];
                    if (remap >= 0 && static_cast<std::size_t>(remap) < slot) {
                        const auto source = material_base + static_cast<std::size_t>(remap);
                        material_colors_[destination] = material_colors_[source];
                        material_textures_[destination] = material_textures_[source];
                        material_lightmap_textures_[destination] =
                            material_lightmap_textures_[source];
                        material_texture_names_[destination] = material_texture_names_[source];
                        material_lightmap_texture_names_[destination] =
                            material_lightmap_texture_names_[source];
                        continue;
                    }
                    if (next_material >= materials.size()) continue;
                    const auto& material_chunk = *materials[next_material++];
                    const auto material = rws::decode_material(material_chunk, bytes);
                    if (material) material_colors_[destination] = material.value->color;
                    if (const auto* texture_chunk = rws::find_child(material_chunk, 0x06)) {
                        const auto texture = rws::decode_texture(*texture_chunk, bytes);
                        if (texture && !texture.value->name.empty()) {
                            material_texture_names_[destination] = texture.value->name;
                            material_textures_[destination] =
                                load_texture(source_path, texture.value->name);
                        }
                    }
                    const auto* extension = rws::find_child(material_chunk, 0x03);
                    const auto* effects_chunk =
                        extension ? rws::find_child(*extension, 0x120) : nullptr;
                    if (effects_chunk) {
                        const auto effects =
                            rws::decode_material_effects(*effects_chunk, 0x07, bytes);
                        if (effects && effects.value->has_dual_texture &&
                            !effects.value->dual_texture.name.empty()) {
                            material_lightmap_texture_names_[destination] =
                                effects.value->dual_texture.name;
                            material_lightmap_textures_[destination] =
                                load_texture(source_path, effects.value->dual_texture.name);
                        }
                    }
                }
            }

            const auto recovered_world = rws::recover_world(*world_chunk, bytes);
            for (const auto& recovered_sector : recovered_world.sectors) {
                const auto triangle_count = recovered_sector.triangle_count;
                const auto vertex_count = recovered_sector.vertex_count;
                const auto uv_sets = recovered_sector.texcoord_sets;
                std::vector<rws::Vec3> sector_vertices;
                sector_vertices.reserve(static_cast<std::size_t>(vertex_count));
                for (std::int32_t i = 0; i < vertex_count; ++i) {
                    const auto offset =
                        recovered_sector.vertices_offset + static_cast<std::uint64_t>(i) * 12U;
                    const rws::Vec3 vertex{read_f32(bytes, offset), read_f32(bytes, offset + 4),
                                           read_f32(bytes, offset + 8)};
                    sector_vertices.push_back(vertex);
                    vertices_.push_back(vertex);
                    minimum.x = std::min(minimum.x, vertex.x);
                    minimum.y = std::min(minimum.y, vertex.y);
                    minimum.z = std::min(minimum.z, vertex.z);
                    maximum.x = std::max(maximum.x, vertex.x);
                    maximum.y = std::max(maximum.y, vertex.y);
                    maximum.z = std::max(maximum.z, vertex.z);
                }
                struct SectorTriangle {
                    std::array<std::uint16_t, 3> vertices;
                    std::uint16_t material;
                };
                std::vector<SectorTriangle> sector_triangles;
                sector_triangles.reserve(static_cast<std::size_t>(triangle_count));
                for (std::int32_t i = 0; i < triangle_count; ++i) {
                    const auto triangle =
                        rws::decode_recovered_world_triangle(recovered_sector, i, bytes);
                    if (triangle)
                        sector_triangles.push_back(
                            {triangle.value->vertices, triangle.value->material});
                }
                std::stable_sort(sector_triangles.begin(), sector_triangles.end(),
                                 [](const auto& left, const auto& right) {
                                     return left.material < right.material;
                                 });
                for (const auto& triangle : sector_triangles) {
                    if (triangle.vertices[0] >= sector_vertices.size() ||
                        triangle.vertices[1] >= sector_vertices.size() ||
                        triangle.vertices[2] >= sector_vertices.size())
                        continue;
                    const auto local_material = std::clamp<std::int64_t>(
                        static_cast<std::int64_t>(recovered_sector.material_window_base) +
                            triangle.material,
                        0, static_cast<std::int64_t>(material_count - 1));
                    const auto global_material =
                        material_base + static_cast<std::size_t>(local_material);
                    if (draw_batches_.empty() || draw_batches_.back().material != global_material ||
                        draw_batches_.back().owner_offset != world_chunk->offset) {
                        const auto& texture_name = material_texture_names_[global_material];
                        const bool floor_material =
                            texture_name.size() >= 4 && texture_name.compare(0, 4, "FFLR") == 0;
                        draw_batches_.push_back({static_cast<std::uint16_t>(global_material),
                                                 static_cast<std::uint32_t>(gpu_vertices_.size()),
                                                 0, world_chunk->offset, floor_material,
                                                 false,
                                                 PreviewLayer::visual_world});
                    }
                    const auto& a = sector_vertices[triangle.vertices[0]];
                    const auto& b = sector_vertices[triangle.vertices[1]];
                    const auto& c = sector_vertices[triangle.vertices[2]];
                    float nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
                    float ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
                    float nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                    const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (length > 0.0F) {
                        nx /= length;
                        ny /= length;
                        nz /= length;
                    }
                    for (const auto index : triangle.vertices) {
                        const auto& vertex = sector_vertices[index];
                        const auto base = uv_sets > 0 ? recovered_sector.texcoord_offsets[0] +
                                                            static_cast<std::uint64_t>(index) * 8U
                                                      : 0U;
                        const auto lightmap = uv_sets > 1
                                                  ? recovered_sector.texcoord_offsets[1] +
                                                        static_cast<std::uint64_t>(index) * 8U
                                                  : 0U;
                        const Uv base_uv =
                            base ? Uv{read_f32(bytes, base), read_f32(bytes, base + 4)} : Uv{};
                        const Uv lightmap_uv =
                            lightmap ? Uv{read_f32(bytes, lightmap), read_f32(bytes, lightmap + 4)}
                                     : Uv{};
                        gpu_vertices_.push_back({vertex.x, vertex.y, vertex.z, base_uv.u, base_uv.v,
                                                 lightmap_uv.u, lightmap_uv.v, lightmap_uv.u,
                                                 lightmap_uv.v, nx, ny, nz, index});
                    }
                    faces_.push_back({});
                    draw_batches_.back().count += 3;
                    ++scene_world_triangle_count_;
                }
                ++scene_world_sector_count_;
            }
        }
    }

    if (!vertices_.empty()) {
        visual_center_ = {(minimum.x + maximum.x) * 0.5F, (minimum.y + maximum.y) * 0.5F,
                          (minimum.z + maximum.z) * 0.5F};
        const auto dx = maximum.x - minimum.x, dy = maximum.y - minimum.y,
                   dz = maximum.z - minimum.z;
        visual_radius_ = std::max(0.5F * std::sqrt(dx * dx + dy * dy + dz * dz), 0.001F);
        visual_extent_ = rws::Vec3{dx * 0.5F, dy * 0.5F, dz * 0.5F};
    }

    auto append_collision = [&](const rws::Document& collision) {
        collision_document_ = &collision;
        collision_worlds_ = rws::recover_worlds(collision.chunks(), collision.bytes());
        if (collision_worlds_.empty()) {
            collision_diagnostics_.emplace_back("Collision document has no World");
            return;
        }
        collision_source_path_ = collision.source_path();
        collision_surface_labels_.resize(collision_worlds_.size());
        rws::Vec3 collision_minimum{std::numeric_limits<float>::max(),
                                    std::numeric_limits<float>::max(),
                                    std::numeric_limits<float>::max()};
        rws::Vec3 collision_maximum{std::numeric_limits<float>::lowest(),
                                    std::numeric_limits<float>::lowest(),
                                    std::numeric_limits<float>::lowest()};
        bool have_collision_bounds = false;
        collision_recovery_status_ = rws::WorldRecoveryStatus::complete;

        for (std::size_t world_index = 0; world_index < collision_worlds_.size(); ++world_index) {
            const auto& recovered = collision_worlds_[world_index];
            if (recovered.status == rws::WorldRecoveryStatus::failed)
                collision_recovery_status_ = rws::WorldRecoveryStatus::failed;
            else if (recovered.status == rws::WorldRecoveryStatus::partial &&
                     collision_recovery_status_ == rws::WorldRecoveryStatus::complete)
                collision_recovery_status_ = rws::WorldRecoveryStatus::partial;
            collision_declared_sector_count_ += recovered.header.world_sector_count;
            collision_sector_count_ += recovered.sectors.size();
            collision_triangle_count_ +=
                static_cast<std::size_t>(std::max<std::int64_t>(0, recovered.recovered_triangles));
            collision_material_count_ +=
                static_cast<std::size_t>(std::max(0, recovered.material_count));
            for (const auto& diagnostic : recovered.diagnostics) {
                collision_diagnostics_.push_back(collision_worlds_.size() == 1
                                                     ? diagnostic
                                                     : "World " + std::to_string(world_index) +
                                                           ": " + diagnostic);
            }

            const auto collision_world = std::find_if(
                collision.chunks().begin(), collision.chunks().end(), [&](const rws::Chunk& chunk) {
                    return chunk.type == 0x0B && chunk.offset == recovered.world_offset;
                });
            if (collision_world == collision.chunks().end() || recovered.sectors.empty()) continue;

            const auto material_base = material_colors_.size();
            const auto material_count = std::max<std::size_t>(
                1U, static_cast<std::size_t>(std::max(0, recovered.material_count)));
            material_colors_.resize(material_base + material_count);
            material_textures_.resize(material_base + material_count);
            material_lightmap_textures_.resize(material_base + material_count);
            material_texture_names_.resize(material_base + material_count);
            material_lightmap_texture_names_.resize(material_base + material_count);
            auto& surface_labels = collision_surface_labels_[world_index];
            surface_labels.assign(material_count, "Unknown");
            // A slot without a readable Material keeps a colour of its own.
            for (std::size_t slot = 0; slot < material_count; ++slot) {
                const auto channels = ui::unpack_rgba(material_color(static_cast<std::uint16_t>(slot), 1.0F));
                material_colors_[material_base + slot] = {channels[0], channels[1], channels[2], 255};
            }

            if (const auto* list_chunk = rws::find_child(*collision_world, 0x08)) {
                const auto list = rws::decode_material_list(*list_chunk, collision.bytes());
                std::vector<const rws::Chunk*> materials;
                for (const auto& child : list_chunk->children)
                    if (child.type == 0x07) materials.push_back(&child);
                std::size_t next_material{};
                for (std::size_t slot = 0; list && slot < material_count; ++slot) {
                    const auto remap = list.value->remap[slot];
                    if (remap >= 0 && static_cast<std::size_t>(remap) < slot) {
                        material_colors_[material_base + slot] =
                            material_colors_[material_base + remap];
                        surface_labels[slot] = surface_labels[remap];
                        continue;
                    }
                    if (next_material >= materials.size()) continue;
                    const auto& material_chunk = *materials[next_material++];
                    // The Material's colour is the one Materiales.bdd gives its
                    // surface (Intangible is red).
                    if (const auto material = rws::decode_material(material_chunk, collision.bytes()))
                        material_colors_[material_base + slot] = material.value->color;
                    const auto* extension = rws::find_child(material_chunk, 0x03);
                    const auto* pyro =
                        extension ? rws::find_child(*extension, 0xFFFFFF00U) : nullptr;
                    const auto metadata =
                        pyro ? rws::decode_pyro_extension(*pyro, 0x07, collision.bytes())
                             : rws::DecodeResult<rws::PyroExtensionInfo>{};
                    if (!metadata) continue;
                    const auto name = std::string(metadata.value->object_name());
                    const auto surface = metadata.value->material_surface_type();
                    surface_labels[slot] = name.empty() ? "Unknown" : name;
                    if (surface) surface_labels[slot] += " (ID " + std::to_string(*surface) + ')';
                }
            }

            for (const auto& sector : recovered.sectors) {
                std::vector<rws::Vec3> sector_vertices;
                sector_vertices.reserve(static_cast<std::size_t>(sector.vertex_count));
                for (std::int32_t i = 0; i < sector.vertex_count; ++i) {
                    const auto offset =
                        sector.vertices_offset + static_cast<std::uint64_t>(i) * 12U;
                    const rws::Vec3 vertex{read_f32(collision.bytes(), offset),
                                           read_f32(collision.bytes(), offset + 4U),
                                           read_f32(collision.bytes(), offset + 8U)};
                    sector_vertices.push_back(vertex);
                    vertices_.push_back(vertex);
                    have_collision_bounds = true;
                    collision_minimum.x = std::min(collision_minimum.x, vertex.x);
                    collision_minimum.y = std::min(collision_minimum.y, vertex.y);
                    collision_minimum.z = std::min(collision_minimum.z, vertex.z);
                    collision_maximum.x = std::max(collision_maximum.x, vertex.x);
                    collision_maximum.y = std::max(collision_maximum.y, vertex.y);
                    collision_maximum.z = std::max(collision_maximum.z, vertex.z);
                    minimum.x = std::min(minimum.x, vertex.x);
                    minimum.y = std::min(minimum.y, vertex.y);
                    minimum.z = std::min(minimum.z, vertex.z);
                    maximum.x = std::max(maximum.x, vertex.x);
                    maximum.y = std::max(maximum.y, vertex.y);
                    maximum.z = std::max(maximum.z, vertex.z);
                }
                struct CollisionTriangle {
                    std::array<std::uint16_t, 3> vertices;
                    std::uint16_t material;
                    std::int32_t source;
                };
                std::vector<CollisionTriangle> triangles;
                for (std::int32_t i = 0; i < sector.triangle_count; ++i) {
                    const auto decoded_triangle =
                        rws::decode_recovered_world_triangle(sector, i, collision.bytes());
                    if (!decoded_triangle) continue;
                    const auto resolved = static_cast<std::int64_t>(sector.material_window_base) +
                                          decoded_triangle.value->material;
                    if (decoded_triangle.value->vertices[0] >= sector_vertices.size() ||
                        decoded_triangle.value->vertices[1] >= sector_vertices.size() ||
                        decoded_triangle.value->vertices[2] >= sector_vertices.size() ||
                        resolved < 0 || resolved >= static_cast<std::int64_t>(material_count))
                        continue;
                    triangles.push_back({decoded_triangle.value->vertices,
                                         static_cast<std::uint16_t>(resolved), i});
                }
                std::stable_sort(triangles.begin(), triangles.end(),
                                 [](const auto& left, const auto& right) {
                                     return left.material < right.material;
                                 });
                for (const auto& triangle : triangles) {
                    const auto global_material = material_base + triangle.material;
                    if (draw_batches_.empty() || draw_batches_.back().material != global_material ||
                        draw_batches_.back().owner_offset != sector.chunk_offset ||
                        draw_batches_.back().layer != PreviewLayer::collision_world) {
                        draw_batches_.push_back(
                            {static_cast<std::uint16_t>(global_material),
                             static_cast<std::uint32_t>(gpu_vertices_.size()), 0,
                             sector.chunk_offset, false, false, PreviewLayer::collision_world,
                             world_index,
                             static_cast<std::size_t>(&sector - recovered.sectors.data())});
                    }
                    collision_triangle_mapping_.push_back(
                        {static_cast<std::uint32_t>(gpu_vertices_.size()), world_index,
                         static_cast<std::size_t>(&sector - recovered.sectors.data()),
                         triangle.source});
                    const auto& a = sector_vertices[triangle.vertices[0]];
                    const auto& b = sector_vertices[triangle.vertices[1]];
                    const auto& c = sector_vertices[triangle.vertices[2]];
                    float nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
                    float ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
                    float nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                    const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (length > 0.0F) {
                        nx /= length;
                        ny /= length;
                        nz /= length;
                    }
                    for (const auto index : triangle.vertices) {
                        const auto& vertex = sector_vertices[index];
                        gpu_vertices_.push_back(
                            {vertex.x, vertex.y, vertex.z, 0, 0, 0, 0, 0, 0, nx, ny, nz, index});
                    }
                    faces_.push_back({});
                    draw_batches_.back().count += 3;
                }
            }
        }
        if (have_collision_bounds) {
            collision_center_ = {(collision_minimum.x + collision_maximum.x) * 0.5F,
                                 (collision_minimum.y + collision_maximum.y) * 0.5F,
                                 (collision_minimum.z + collision_maximum.z) * 0.5F};
            const auto dx = collision_maximum.x - collision_minimum.x;
            const auto dy = collision_maximum.y - collision_minimum.y;
            const auto dz = collision_maximum.z - collision_minimum.z;
            collision_radius_ = std::max(0.5F * std::sqrt(dx * dx + dy * dy + dz * dz), 0.001F);
            collision_extent_ = rws::Vec3{dx * 0.5F, dy * 0.5F, dz * 0.5F};
        }
    };

    visual_material_slot_count_ = material_colors_.size();
    if (collision_document) append_collision(*collision_document);
    if (main_is_collision) {
        show_visual_ = false;
        show_collision_ = true;
    } else {
        show_visual_ = true;
    }

    if (gpu_vertices_.empty() && physics_lines_.empty()) {
        error_ = "No renderable standard Clump/Atomic instances were found";
        return false;
    }
    if (vertices_.empty() && collision_extent_) {
        // Collision-only scene: frame the collision world.
        center_ = collision_center_;
        radius_ = collision_radius_;
        frame_extent_ = collision_extent_;
    } else {
        center_ = {(minimum.x + maximum.x) * 0.5F, (minimum.y + maximum.y) * 0.5F,
                   (minimum.z + maximum.z) * 0.5F};
        radius_ = 0.0F;
        for (const auto& vertex : vertices_) {
            const auto x = vertex.x - center_.x, y = vertex.y - center_.y, z = vertex.z - center_.z;
            radius_ = std::max(radius_, std::sqrt(x * x + y * y + z * z));
        }
        radius_ = std::max(radius_, 0.001F);
        frame_extent_.reset();
        if (!vertices_.empty())
            frame_extent_ = rws::Vec3{(maximum.x - minimum.x) * 0.5F, (maximum.y - minimum.y) * 0.5F,
                                      (maximum.z - minimum.z) * 0.5F};
    }
    all_center_ = center_;
    all_radius_ = radius_;
    all_extent_ = frame_extent_;
    if (!gpu_vertices_.empty() && !create_gpu_resources()) return false;
    if (!preserve_view) {
        reset_view();
    } else {
        const auto yaw = yaw_, pitch = pitch_;
        set_camera(kept_camera);
        yaw_ = yaw; // Keep any easing toward the target orientation.
        pitch_ = pitch;
    }
    return true;
}

void GeometryPreview::render_callback(const ImDrawList*, const ImDrawCmd* command) {
    static_cast<GeometryPreview*>(command->UserCallbackData)->render_gpu();
}

bool GeometryPreview::create_gpu_resources() {
    auto& gl = gl_api();
    if (!gl.create_shader || gpu_vertices_.empty()) {
        error_ = "OpenGL 3.3 functions are unavailable";
        return false;
    }
    constexpr const char* vertex_source = R"GLSL(#version 330 core
layout(location=0) in vec3 aPosition;
layout(location=1) in vec2 aBaseUv;
layout(location=2) in vec2 aLightmapUv;
layout(location=3) in vec2 aDebugUv;
layout(location=4) in vec3 aNormal;
uniform vec3 uCenter;
uniform float uYaw, uPitch, uDistance, uOrthographicScale;
uniform vec2 uPan;
uniform float uAspect, uTanHalfFov, uNear, uFar;
uniform bool uYUp;
uniform bool uOrthographic;
uniform vec4 uModel0, uModel1, uModel2;
uniform vec2 uScreenOffset;
uniform vec2 uViewportPixels;
out vec2 vUv;
out vec2 vLightmapUv;
out vec2 vDebugUv;
out vec3 vNormal;
out vec3 vWorld;
void main() {
    vec3 modelNormal=normalize(vec3(dot(uModel0.xyz,aNormal),dot(uModel1.xyz,aNormal),dot(uModel2.xyz,aNormal)));
    vec3 worldPosition=vec3(dot(uModel0.xyz,aPosition)+uModel0.w,dot(uModel1.xyz,aPosition)+uModel1.w,dot(uModel2.xyz,aPosition)+uModel2.w);
    vec3 p = worldPosition - uCenter;
    float cy=cos(uYaw), sy=sin(uYaw), cp=cos(uPitch), sp=sin(uPitch);
    vec3 view;
    if (uYUp) {
        float rx=cy*p.x-sy*p.z, rz=sy*p.x+cy*p.z;
        view=vec3(rx+uPan.x, cp*p.y-sp*rz+uPan.y, sp*p.y+cp*rz-uDistance);
        float nrx=cy*modelNormal.x-sy*modelNormal.z, nrz=sy*modelNormal.x+cy*modelNormal.z;
        vNormal=vec3(nrx, cp*modelNormal.y-sp*nrz, sp*modelNormal.y+cp*nrz);
    } else {
        float rx=cy*p.x-sy*p.y, ry=sy*p.x+cy*p.y;
        view=vec3(rx+uPan.x, cp*p.z-sp*ry+uPan.y, sp*p.z+cp*ry-uDistance);
        float nrx=cy*modelNormal.x-sy*modelNormal.y, nry=sy*modelNormal.x+cy*modelNormal.y;
        vNormal=vec3(nrx, cp*modelNormal.z-sp*nry, sp*modelNormal.z+cp*nry);
    }
    float f=1.0/uTanHalfFov;
    if (uOrthographic)
        gl_Position=vec4(view.x/(uOrthographicScale*uAspect),view.y/uOrthographicScale,
            (-2.0*view.z-uFar-uNear)/(uFar-uNear),1.0);
    else gl_Position=vec4(view.x*f/uAspect, view.y*f,
        ((uFar+uNear)/(uNear-uFar))*view.z+(2.0*uFar*uNear)/(uNear-uFar), -view.z);
    // Outline passes shift the whole mesh by a few pixels in screen space.
    gl_Position.xy+=uScreenOffset*2.0/uViewportPixels*gl_Position.w;
    vUv=aBaseUv;
    vLightmapUv=aLightmapUv;
    vDebugUv=aDebugUv;
    vWorld=worldPosition;
})GLSL";
    constexpr const char* fragment_source = R"GLSL(#version 330 core
in vec2 vUv;
in vec2 vLightmapUv;
in vec2 vDebugUv;
in vec3 vNormal;
in vec3 vWorld;
uniform sampler2D uTexture;
uniform sampler2D uLightmapTexture;
uniform bool uUseTexture;
uniform bool uUseLightmap;
uniform bool uLightmapOnly;
uniform bool uUseDebugUv;
uniform bool uApplyLighting;
uniform bool uForceOpaque;
uniform float uLightmapIntensity;
uniform float uDim;
uniform vec4 uBaseColor;
uniform vec4 uClip0, uClip1, uClip2;
uniform vec4 uSlice;
out vec4 FragColor;
bool clipped(vec4 c) {
    if (c.x < 0.5) return false;
    float value=vWorld[int(c.y)];
    return c.w > 0.0 ? value < c.z : value > c.z;
}
void main() {
    if (clipped(uClip0)||clipped(uClip1)||clipped(uClip2)) discard;
    if (uSlice.x > 0.5 && (vWorld.y < uSlice.y || vWorld.y > uSlice.z)) discard;
    float light=0.42+0.58*abs(dot(normalize(vNormal), normalize(vec3(0.35,0.55,0.75))));
    vec2 uv=uUseDebugUv ? vDebugUv : vUv;
    vec4 color=uUseTexture ? texture(uTexture,uv) : uBaseColor;
    if (uUseLightmap) {
        vec4 lightmap=texture(uLightmapTexture,vLightmapUv);
        vec3 lit=lightmap.rgb*uLightmapIntensity;
        color=uLightmapOnly ? vec4(lit,1.0) : vec4(color.rgb*lit,color.a);
    }
    if (uForceOpaque) color.a=1.0;
    if (color.a < 0.08) discard;
    FragColor=vec4(color.rgb*(uApplyLighting ? light : 1.0)*uDim,color.a);
})GLSL";
    auto compile = [&](const GLenum type, const char* source) -> GLuint {
        const GLuint shader = gl.create_shader(type);
        gl.shader_source(shader, 1, &source, nullptr);
        gl.compile_shader(shader);
        GLint okay{};
        gl.get_shader_iv(shader, gl_compile_status, &okay);
        if (!okay) {
            std::array<char, 1024> log{};
            gl.get_shader_log(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
            error_ = "OpenGL shader compile failed: " + std::string(log.data());
            gl.delete_shader(shader);
            return 0;
        }
        return shader;
    };
    const GLuint vertex_shader = compile(gl_vertex_shader, vertex_source);
    if (!vertex_shader) return false;
    const GLuint fragment_shader = compile(gl_fragment_shader, fragment_source);
    if (!fragment_shader) {
        gl.delete_shader(vertex_shader);
        return false;
    }
    shader_program_ = gl.create_program();
    gl.attach_shader(shader_program_, vertex_shader);
    gl.attach_shader(shader_program_, fragment_shader);
    gl.link_program(shader_program_);
    gl.delete_shader(vertex_shader);
    gl.delete_shader(fragment_shader);
    GLint linked{};
    gl.get_program_iv(shader_program_, gl_link_status, &linked);
    if (!linked) {
        std::array<char, 1024> log{};
        gl.get_program_log(shader_program_, static_cast<GLsizei>(log.size()), nullptr, log.data());
        error_ = "OpenGL shader link failed: " + std::string(log.data());
        destroy_gpu_resources();
        return false;
    }
    const auto location = [&](const char* name) { return gl.get_uniform_location(shader_program_, name); };
    auto& u = uniforms_;
    u.center = location("uCenter");
    u.yaw = location("uYaw");
    u.pitch = location("uPitch");
    u.distance = location("uDistance");
    u.orthographic_scale = location("uOrthographicScale");
    u.pan = location("uPan");
    u.aspect = location("uAspect");
    u.tan_half_fov = location("uTanHalfFov");
    u.near_plane = location("uNear");
    u.far_plane = location("uFar");
    u.y_up = location("uYUp");
    u.orthographic = location("uOrthographic");
    u.model = {location("uModel0"), location("uModel1"), location("uModel2")};
    u.screen_offset = location("uScreenOffset");
    u.viewport_pixels = location("uViewportPixels");
    u.texture = location("uTexture");
    u.lightmap_texture = location("uLightmapTexture");
    u.use_texture = location("uUseTexture");
    u.use_lightmap = location("uUseLightmap");
    u.lightmap_only = location("uLightmapOnly");
    u.use_debug_uv = location("uUseDebugUv");
    u.apply_lighting = location("uApplyLighting");
    u.force_opaque = location("uForceOpaque");
    u.lightmap_intensity = location("uLightmapIntensity");
    u.dim = location("uDim");
    u.base_color = location("uBaseColor");
    u.slice = location("uSlice");
    u.clip = {location("uClip0"), location("uClip1"), location("uClip2")};
    gl.gen_vertex_arrays(1, &vertex_array_);
    gl.gen_buffers(1, &vertex_buffer_);
    gl.bind_vertex_array(vertex_array_);
    gl.bind_buffer(gl_array_buffer, vertex_buffer_);
    gl.buffer_data(gl_array_buffer,
                   static_cast<GlSizePtr>(gpu_vertices_.size() * sizeof(GpuVertex)),
                   gpu_vertices_.data(), gl_static_draw);
    gl.enable_vertex_attrib_array(0);
    gl.vertex_attrib_pointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                             reinterpret_cast<void*>(offsetof(GpuVertex, x)));
    gl.enable_vertex_attrib_array(1);
    gl.vertex_attrib_pointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                             reinterpret_cast<void*>(offsetof(GpuVertex, base_u)));
    gl.enable_vertex_attrib_array(2);
    gl.vertex_attrib_pointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                             reinterpret_cast<void*>(offsetof(GpuVertex, lightmap_u)));
    gl.enable_vertex_attrib_array(3);
    gl.vertex_attrib_pointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                             reinterpret_cast<void*>(offsetof(GpuVertex, debug_u)));
    gl.enable_vertex_attrib_array(4);
    gl.vertex_attrib_pointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                             reinterpret_cast<void*>(offsetof(GpuVertex, nx)));
    gl.bind_vertex_array(0);
    build_render_cache();
    return true;
}

void GeometryPreview::build_render_cache() {
    batch_bounds_.assign(draw_batches_.size(), {});
    for (std::size_t i = 0; i < draw_batches_.size(); ++i) update_batch_bounds(i);
    // Opaque batches sharing a material share its textures, so grouping them
    // saves texture binds; collision draws last, in its own pass.
    batch_order_.resize(draw_batches_.size());
    std::iota(batch_order_.begin(), batch_order_.end(), 0U);
    std::ranges::stable_sort(batch_order_, [&](const std::uint32_t a, const std::uint32_t b) {
        const auto& first = draw_batches_[a];
        const auto& second = draw_batches_[b];
        const bool first_collision = first.layer == PreviewLayer::collision_world;
        const bool second_collision = second.layer == PreviewLayer::collision_world;
        if (first_collision != second_collision) return second_collision;
        return first.material < second.material;
    });
    visual_triangle_total_ = collision_triangle_total_ = 0;
    for (const auto& batch : draw_batches_)
        (batch.layer == PreviewLayer::collision_world ? collision_triangle_total_ : visual_triangle_total_) +=
            batch.count / 3;
}

void GeometryPreview::update_batch_bounds(const std::size_t batch_index) {
    const auto& batch = draw_batches_[batch_index];
    auto& bounds = batch_bounds_[batch_index];
    bounds = {};
    if (batch.count == 0 || std::size_t{batch.first} + batch.count > gpu_vertices_.size()) return;
    rws::Vec3 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max()};
    rws::Vec3 hi{-lo.x, -lo.y, -lo.z};
    for (std::size_t i = batch.first; i < std::size_t{batch.first} + batch.count; ++i) {
        const auto& vertex = gpu_vertices_[i];
        const auto p = transform_draw_point(batch.transform, {vertex.x, vertex.y, vertex.z});
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    const rws::Vec3 half{(hi.x - lo.x) * 0.5F, (hi.y - lo.y) * 0.5F, (hi.z - lo.z) * 0.5F};
    const float radius = std::sqrt(half.x * half.x + half.y * half.y + half.z * half.z);
    if (!std::isfinite(radius)) return;
    bounds = {{lo.x + half.x, lo.y + half.y, lo.z + half.z}, radius};
}

void GeometryPreview::destroy_gpu_resources() {
    destroy_overlay_program();
    auto& gl = gl_api();
    if (gpu_queries_[0] != 0 && gl.delete_queries)
        gl.delete_queries(static_cast<GLsizei>(gpu_queries_.size()), gpu_queries_.data());
    gpu_queries_ = {};
    gpu_query_pending_ = {};
    gpu_ms_ = -1.0;
    uniforms_ = {};
    if (vertex_buffer_ && gl.delete_buffers) gl.delete_buffers(1, &vertex_buffer_);
    if (vertex_array_ && gl.delete_vertex_arrays) gl.delete_vertex_arrays(1, &vertex_array_);
    if (shader_program_ && gl.delete_program) gl.delete_program(shader_program_);
    vertex_buffer_ = vertex_array_ = shader_program_ = 0;
}

void GeometryPreview::read_gpu_timer() {
    auto& gl = gl_api();
    for (std::size_t i = 0; i < gpu_queries_.size(); ++i) {
        if (!gpu_query_pending_[i]) continue;
        GLint available{};
        gl.get_query_object_iv(gpu_queries_[i], gl_query_result_available, &available);
        if (!available) continue;
        unsigned long long nanoseconds{};
        gl.get_query_object_ui64v(gpu_queries_[i], gl_query_result, &nanoseconds);
        gpu_query_pending_[i] = false;
        const double milliseconds = static_cast<double>(nanoseconds) / 1e6;
        gpu_ms_ = gpu_ms_ < 0.0 ? milliseconds : gpu_ms_ * 0.8 + milliseconds * 0.2;
    }
}

void GeometryPreview::render_gpu() {
    if (canvas_width_ < 1.0F || canvas_height_ < 1.0F) return;
    auto& gl = gl_api();
    // The GPU time of the scene pass, read back a few frames later so the CPU
    // never waits for it. A busy query slot skips timing for that frame.
    bool timing = false;
    if (show_frame_stats_ && gl.has_timer_queries()) {
        if (gpu_queries_[0] == 0)
            gl.gen_queries(static_cast<GLsizei>(gpu_queries_.size()), gpu_queries_.data());
        read_gpu_timer();
        const auto slot = gpu_query_next_;
        if (!gpu_query_pending_[slot]) {
            gl.begin_query(gl_time_elapsed, gpu_queries_[slot]);
            gpu_query_pending_[slot] = true;
            gpu_query_next_ = (slot + 1) % gpu_queries_.size();
            timing = true;
        }
    }
    render_scene_gpu();
    if (timing) gl.end_query(gl_time_elapsed);
}

void GeometryPreview::render_scene_gpu() {
    auto& gl = gl_api();
    const auto& io = ImGui::GetIO();
    const int viewport_x = static_cast<int>(canvas_x_ * io.DisplayFramebufferScale.x);
    const int viewport_y = static_cast<int>((io.DisplaySize.y - canvas_y_ - canvas_height_) *
                                            io.DisplayFramebufferScale.y);
    const int viewport_width =
        std::max(1, static_cast<int>(canvas_width_ * io.DisplayFramebufferScale.x));
    const int viewport_height =
        std::max(1, static_cast<int>(canvas_height_ * io.DisplayFramebufferScale.y));

    glViewport(viewport_x, viewport_y, viewport_width, viewport_height);
    glScissor(viewport_x, viewport_y, viewport_width, viewport_height);
    glEnable(GL_SCISSOR_TEST);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    last_draw_calls_ = last_culled_batches_ = 0;
    if (!shader_program_ || !vertex_array_ || draw_batches_.empty()) {
        // Overlays still draw over an empty scene, with nothing to hide them.
        render_overlays_gpu(viewport_width, viewport_height);
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    if (batch_bounds_.size() != draw_batches_.size() || batch_order_.size() != draw_batches_.size())
        build_render_cache();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    if (cull_backfaces_) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    } else
        glDisable(GL_CULL_FACE);
    gl.use_program(shader_program_);
    gl.bind_vertex_array(vertex_array_);
    gl.active_texture(gl_texture0);
    const auto& u = uniforms_;

    const float view_scale = projection_ == 0 ? distance_ : orthographic_scale_;
    const float pan_world_x = pan_x_ * view_scale;
    const float pan_world_z = -pan_y_ * view_scale;
    const float near_plane = std::max(distance_ * 0.001F, std::max(radius_ * 0.000001F, 0.001F));
    const float navigation_distance = std::sqrt(navigation_offset_.x * navigation_offset_.x +
                                                navigation_offset_.y * navigation_offset_.y +
                                                navigation_offset_.z * navigation_offset_.z);
    const float far_plane =
        std::max(distance_ + radius_ * 3.0F + navigation_distance, near_plane + 1.0F);
    // Orthographic depth is linear and must span the whole scene on both sides of the target.
    const float ortho_reach = std::max(all_radius_, radius_) * 3.0F + navigation_distance;
    const float clip_near = projection_ == 0 ? near_plane : distance_ - ortho_reach;
    const float clip_far = projection_ == 0 ? far_plane : distance_ + ortho_reach;
    const float aspect = static_cast<float>(viewport_width) / static_cast<float>(viewport_height);
    const float tan_half_fov = std::tan(25.0F * 3.14159265358979323846F / 180.0F);
    gl.uniform_3f(u.center, center_.x + navigation_offset_.x, center_.y + navigation_offset_.y,
                  center_.z + navigation_offset_.z);
    const float render_yaw = projection_ == 3 ? 1.57079632679F : (projection_ == 0 ? yaw_ : 0.0F);
    const float render_pitch =
        projection_ == 1 ? -1.57079632679F : (projection_ == 0 ? pitch_ : 0.0F);
    gl.uniform_1f(u.yaw, render_yaw);
    gl.uniform_1f(u.pitch, render_pitch);
    gl.uniform_1f(u.distance, distance_);
    gl.uniform_1f(u.orthographic_scale, orthographic_scale_);
    gl.uniform_2f(u.pan, pan_world_x, pan_world_z);
    gl.uniform_1f(u.aspect, aspect);
    gl.uniform_1f(u.tan_half_fov, tan_half_fov);
    gl.uniform_1f(u.near_plane, clip_near);
    gl.uniform_1f(u.far_plane, clip_far);
    gl.uniform_1i(u.y_up, scene_mode_);
    gl.uniform_1i(u.orthographic, projection_ != 0);
    for (std::size_t i = 0; i < clips_.size(); ++i) {
        const auto& clip = clips_[i];
        gl.uniform_4f(u.clip[i], clip.enabled ? 1.0F : 0.0F, static_cast<float>(clip.axis),
                      clip.position, clip.keep_greater ? 1.0F : -1.0F);
    }
    gl.uniform_4f(u.slice, slice_.enabled && slice_.clip_geometry && scene_mode_ ? 1.0F : 0.0F,
                  slice_.low, slice_.high, 0.0F);
    gl.uniform_1i(u.texture, 0);
    gl.uniform_1i(u.lightmap_texture, 1);
    gl.uniform_1f(u.lightmap_intensity, lightmap_intensity_);
    gl.uniform_1i(u.use_debug_uv, view_style_ == 3 || view_style_ == 4);
    gl.uniform_1i(u.lightmap_only, view_style_ == 5);
    gl.uniform_2f(u.screen_offset, 0.0F, 0.0F);
    gl.uniform_2f(u.viewport_pixels, static_cast<float>(viewport_width),
                  static_cast<float>(viewport_height));

    // Uniforms and textures change only when a batch needs different values;
    // consecutive batches mostly share them.
    struct Bound {
        std::optional<std::array<float, 12>> model;
        std::optional<std::array<float, 4>> color;
        std::optional<float> dim;
        int use_texture{-1}, use_lightmap{-1}, force_opaque{-1}, apply_lighting{-1};
        GLuint texture{~GLuint{}}, lightmap{~GLuint{}};
    } bound;
    const auto set_model = [&](const std::array<float, 12>& m) {
        if (bound.model == m) return;
        bound.model = m;
        gl.uniform_4f(u.model[0], m[0], m[1], m[2], m[3]);
        gl.uniform_4f(u.model[1], m[4], m[5], m[6], m[7]);
        gl.uniform_4f(u.model[2], m[8], m[9], m[10], m[11]);
    };
    const auto set_base_color = [&](const float r, const float g, const float b, const float a) {
        const std::array<float, 4> value{r, g, b, a};
        if (bound.color == value) return;
        bound.color = value;
        gl.uniform_4f(u.base_color, r, g, b, a);
    };
    const auto set_dim = [&](const float value) {
        if (bound.dim == value) return;
        bound.dim = value;
        gl.uniform_1f(u.dim, value);
    };
    const auto set_flag = [&](const int location, int& current, const bool value) {
        if (current == static_cast<int>(value)) return;
        current = value;
        gl.uniform_1i(location, value);
    };
    const auto bind_textures = [&](const GLuint texture, const GLuint lightmap) {
        if (bound.texture != texture) {
            gl.active_texture(gl_texture0);
            glBindTexture(GL_TEXTURE_2D, texture);
            bound.texture = texture;
        }
        if (bound.lightmap != lightmap) {
            gl.active_texture(gl_texture1);
            glBindTexture(GL_TEXTURE_2D, lightmap);
            bound.lightmap = lightmap;
        }
    };
    set_flag(u.apply_lighting, bound.apply_lighting, view_style_ < 3);
    std::size_t draw_calls = 0;
    const auto draw_batch = [&](const DrawBatch& batch) {
        glDrawArrays(GL_TRIANGLES, static_cast<GLint>(batch.first), static_cast<GLsizei>(batch.count));
        ++draw_calls;
    };

    // Per batch: whether the selected actor owns it, and whether its bounds reach
    // into the view volume. Only scene mode culls; a single model is one draw.
    const ViewVolume volume{projection_ != 0, tan_half_fov, aspect, clip_near, clip_far,
                            orthographic_scale_};
    bool has_selected_actor = false;
    batch_in_view_.resize(draw_batches_.size());
    std::size_t culled = 0;
    for (std::size_t i = 0; i < draw_batches_.size(); ++i) {
        const auto& batch = draw_batches_[i];
        if (selected_mission_entry_ && actor_owner(batch.owner_offset) == selected_mission_entry_)
            has_selected_actor = true;
        const auto& bounds = batch_bounds_[i];
        bool in_view = true;
        if (scene_mode_ && bounds.radius >= 0.0F) {
            const auto center = view_point(bounds.center);
            in_view = sphere_in_view(volume, center.x, center.y, center.z, bounds.radius);
        }
        batch_in_view_[i] = in_view;
        culled += in_view ? 0 : 1;
    }
    const auto batch_visible = [&](const DrawBatch& batch) {
        if (!isolate_selected_actor_ || !has_selected_actor) return true;
        return batch.layer != PreviewLayer::collision_world &&
               actor_owner(batch.owner_offset) == selected_mission_entry_;
    };
    // A mission's ground without a lightmap is tinted darker, so the markers
    // on it stand out (V7); lit ground and models are drawn as they are.
    const bool tint_ground = scene_mode_ && has_mission_overlays();
    const auto batch_dim = [&](const DrawBatch& batch, const bool lit) {
        if (tint_ground && batch.layer == PreviewLayer::visual_world && !lit) return 0.78F;
        if (!dim_unselected_actors_ || !has_selected_actor || isolate_selected_actor_)
            return 1.0F;
        return actor_owner(batch.owner_offset) == selected_mission_entry_ ? 1.0F : 0.55F;
    };

    auto set_color = [&](const DrawBatch& batch) {
        const auto material = batch.material;
        if ((batch.layer == PreviewLayer::collision_world && collision_color_mode_ == 1) ||
            (batch.layer != PreviewLayer::collision_world && view_style_ == 1)) {
            const auto palette_material =
                batch.layer == PreviewLayer::collision_world &&
                        static_cast<std::size_t>(material) >= visual_material_slot_count_
                    ? static_cast<std::uint16_t>(static_cast<std::size_t>(material) -
                                                 visual_material_slot_count_)
                    : material;
            const auto packed = material_color(palette_material, 1.0F);
            const auto channels = ui::unpack_rgba(packed);
            set_base_color(static_cast<float>(channels[0]) / 255.0F,
                           static_cast<float>(channels[1]) / 255.0F,
                           static_cast<float>(channels[2]) / 255.0F,
                           batch.layer == PreviewLayer::collision_world ? collision_opacity_ : 1.0F);
            return;
        }
        if (batch.layer == PreviewLayer::collision_world && collision_color_mode_ == 2) {
            set_base_color(0.95F, 0.25F, 0.72F, collision_opacity_);
            return;
        }
        const auto index = static_cast<std::size_t>(material);
        const auto rgba = index < material_colors_.size()
                              ? material_colors_[index]
                              : std::array<std::uint8_t, 4>{190, 190, 190, 255};
        set_base_color(rgba[0] / 255.0F, rgba[1] / 255.0F, rgba[2] / 255.0F,
                       batch.layer == PreviewLayer::collision_world ? collision_opacity_
                                                                    : rgba[3] / 255.0F);
    };

    if (view_style_ != 7 || show_collision_) {
        // Opaque visual geometry establishes depth first. Fractional-alpha textures are then
        // composited without writing depth, followed by the optional collision overlay.
        for (auto& draws : pass_draws_) draws.clear();
        for (const auto index : batch_order_) {
            if (!batch_in_view_[index]) continue;
            const auto& batch = draw_batches_[index];
            if (!batch_visible(batch)) continue;
            const bool collision = batch.layer == PreviewLayer::collision_world;
            if ((!collision && !show_visual_) ||
                (collision && (!show_collision_ || collision_style_ == 2)))
                continue;
            if (!collision && view_style_ == 7) continue;
            const auto material = static_cast<std::size_t>(batch.material);
            GLuint texture{}, lightmap{};
            if (!collision && (view_style_ == 0 || view_style_ == 6) &&
                (scene_mode_ || !uv_sets_.empty()) && material < material_textures_.size())
                texture = material_textures_[material];
            else if ((view_style_ == 3 || view_style_ == 4) &&
                     selected_uv_set_ < uv_sets_.size())
                texture = checker_texture_;
            if ((view_style_ == 5 || view_style_ == 6) &&
                (scene_mode_ || uv_sets_.size() > 1) &&
                material < material_lightmap_textures_.size())
                lightmap = material_lightmap_textures_[material];
            const bool translucent = !collision && !batch.force_opaque && texture != 0 &&
                                     translucent_texture_ids_.contains(texture);
            pass_draws_[collision ? 2 : (translucent ? 1 : 0)].push_back({index, texture, lightmap});
        }
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        for (std::size_t pass = 0; pass < pass_draws_.size(); ++pass) {
            if (pass_draws_[pass].empty()) continue;
            if (pass == 0) {
                glDisable(GL_BLEND);
                glDepthMask(GL_TRUE);
                glEnable(GL_DEPTH_TEST);
            } else {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);
                if (pass == 2 && collision_style_ == 3)
                    glDisable(GL_DEPTH_TEST);
                else
                    glEnable(GL_DEPTH_TEST);
            }
            for (const auto& draw : pass_draws_[pass]) {
                const auto& batch = draw_batches_[draw.batch];
                set_color(batch);
                set_model(batch.transform);
                set_dim(batch_dim(batch, draw.lightmap != 0));
                set_flag(u.use_texture, bound.use_texture, draw.texture != 0);
                set_flag(u.use_lightmap, bound.use_lightmap, draw.lightmap != 0);
                set_flag(u.force_opaque, bound.force_opaque, pass != 2 && batch.force_opaque);
                bind_textures(draw.texture, draw.lightmap);
                draw_batch(batch);
            }
        }
        glDepthMask(GL_TRUE);
    }
    if (view_style_ == 7 || wireframe_ ||
        (show_collision_ && (collision_style_ == 1 || collision_style_ == 2))) {
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        set_flag(u.use_texture, bound.use_texture, false);
        set_flag(u.use_lightmap, bound.use_lightmap, false);
        set_flag(u.force_opaque, bound.force_opaque, true);
        set_dim(1.0F);
        const float color = view_style_ == 7 ? 0.84F : 0.09F;
        glDepthFunc(GL_LEQUAL);
        glEnable(GL_POLYGON_OFFSET_LINE);
        glPolygonOffset(-1.0F, -1.0F);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        for (const auto index : batch_order_) {
            if (!batch_in_view_[index]) continue;
            const auto& batch = draw_batches_[index];
            if (!batch_visible(batch)) continue;
            const bool collision = batch.layer == PreviewLayer::collision_world;
            const bool draw_visual_wire = !collision && show_visual_ && (view_style_ == 7 || wireframe_);
            const bool draw_collision_wire =
                collision && show_collision_ && (collision_style_ == 1 || collision_style_ == 2);
            if (!draw_visual_wire && !draw_collision_wire) continue;
            set_base_color(collision ? 0.1F : color,
                           collision ? 0.95F : (view_style_ == 7 ? 0.88F : 0.10F),
                           collision ? 0.95F : (view_style_ == 7 ? 0.95F : 0.13F), 1.0F);
            set_model(batch.transform);
            draw_batch(batch);
        }
        glDisable(GL_POLYGON_OFFSET_LINE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    if (outline_selected_actor_ && has_selected_actor) {
        // A soft glow around the selected actor's silhouette, visible through
        // occluders. The stencil masks the silhouette, then copies of the mesh
        // shifted a few pixels outward paint rings around it, each pixel once so
        // overlapping copies don't stack. Screen-space offsets need no normals,
        // which in these models are often split per face.
        selected_batches_.clear();
        for (std::uint32_t i = 0; i < draw_batches_.size(); ++i) {
            const auto& batch = draw_batches_[i];
            if (batch_in_view_[i] && batch.layer != PreviewLayer::collision_world &&
                actor_owner(batch.owner_offset) == selected_mission_entry_ && batch_visible(batch))
                selected_batches_.push_back(i);
        }
        const auto draw_selected = [&] {
            for (const auto index : selected_batches_) {
                const auto& batch = draw_batches_[index];
                set_model(batch.transform);
                draw_batch(batch);
            }
        };
        glDisable(GL_CULL_FACE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_STENCIL_TEST);
        glStencilMask(0xFF);
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
        set_flag(u.use_texture, bound.use_texture, false);
        set_flag(u.use_lightmap, bound.use_lightmap, false);
        set_flag(u.force_opaque, bound.force_opaque, false);
        set_dim(1.0F);
        set_flag(u.apply_lighting, bound.apply_lighting, false);
        set_base_color(1.0F, 0.74F, 0.22F, 1.0F);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glStencilFunc(GL_ALWAYS, 1, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        draw_selected();
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glStencilFunc(GL_GREATER, 1, 0xFF); // Only pixels still at 0, then marked.
        struct Ring {
            float radius, alpha;
            int steps;
        };
        const float pixels = ui::ui_scale() * io.DisplayFramebufferScale.x;
        for (const auto ring : {Ring{1.5F, 0.95F, 8}, Ring{3.0F, 0.5F, 12}, Ring{4.5F, 0.2F, 16}}) {
            set_base_color(1.0F, 0.74F, 0.22F, ring.alpha);
            for (int step = 0; step < ring.steps; ++step) {
                const float angle =
                    6.28318530718F * static_cast<float>(step) / static_cast<float>(ring.steps);
                gl.uniform_2f(u.screen_offset, std::cos(angle) * ring.radius * pixels,
                              std::sin(angle) * ring.radius * pixels);
                draw_selected();
            }
        }
        gl.uniform_2f(u.screen_offset, 0.0F, 0.0F);
        set_flag(u.apply_lighting, bound.apply_lighting, view_style_ < 3);
        glDisable(GL_STENCIL_TEST);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        if (cull_backfaces_) {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
        }
    }
    if (selected_collision_) {
        const auto mapping =
            std::find_if(collision_triangle_mapping_.begin(), collision_triangle_mapping_.end(),
                         [&](const RenderedCollisionTriangle& item) {
                             return item.world == selected_collision_->world_index &&
                                    item.sector == selected_collision_->sector_index &&
                                    item.triangle == selected_collision_->triangle_index;
                         });
        if (mapping != collision_triangle_mapping_.end()) {
            glEnable(GL_DEPTH_TEST);
            set_flag(u.use_texture, bound.use_texture, false);
            set_flag(u.use_lightmap, bound.use_lightmap, false);
            set_dim(1.0F);
            set_model({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});
            const auto first = static_cast<GLint>(mapping->first);
            // An x-ray fill shows the selection through occluding geometry, a
            // brighter fill picks it out where actually visible, and a bright
            // wire outline on top keeps the triangle's edges crisp at any zoom.
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            set_flag(u.force_opaque, bound.force_opaque, false);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glDisable(GL_DEPTH_TEST);
            set_base_color(1.0F, 0.15F, 0.85F, 0.18F);
            glDrawArrays(GL_TRIANGLES, first, 3);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            set_base_color(1.0F, 0.35F, 0.88F, 0.5F);
            glDrawArrays(GL_TRIANGLES, first, 3);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            set_flag(u.force_opaque, bound.force_opaque, true);
            set_base_color(1.0F, 0.2F, 0.85F, 1.0F);
            glEnable(GL_POLYGON_OFFSET_LINE);
            glPolygonOffset(-2, -2);
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            glLineWidth(3.0F);
            glDrawArrays(GL_TRIANGLES, first, 3);
            glLineWidth(1.0F);
            glDisable(GL_POLYGON_OFFSET_LINE);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }
    }
    last_draw_calls_ = draw_calls;
    last_culled_batches_ = culled;
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_CULL_FACE);
    render_overlays_gpu(viewport_width, viewport_height);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    gl.active_texture(gl_texture0);
    gl.bind_vertex_array(0);
    gl.use_program(0);
}

void GeometryPreview::draw(const rws::Chunk& geometry_chunk, const std::span<const std::byte> bytes,
                           const std::filesystem::path& source_path) {
    if (chunk_offset_ != geometry_chunk.offset) load(geometry_chunk, bytes, source_path);

    constexpr const char* styles[] = {"Textured",        "Material index", "Material color",
                                      "UV checker",      "Lightmap UV",    "Lightmap texture",
                                      "Base + lightmap", "Wireframe"};
    constexpr const char* projections[] = {"Perspective", "Top (X/Z)", "Front (X/Y)", "Side (Z/Y)"};
    // The center panel has no padding (the canvas is edge to edge); the toolbar
    // brings its own.
    const float scale = ui::ui_scale();
    const ImVec2 toolbar_padding{8.0F * scale, 5.0F * scale};
    const float toolbar_height = ImGui::GetFrameHeight() + toolbar_padding.y * 2.0F;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, toolbar_padding);
    ImGui::BeginChild("geometry_toolbar", {0.0F, toolbar_height},
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::SetNextItemWidth(170.0F * scale);
    if (ImGui::Combo("##geometry_style", &view_style_, styles,
                     static_cast<int>(std::size(styles))) &&
        view_style_ >= 4 && view_style_ <= 6 && uv_sets_.size() > 1)
        select_uv_set(1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0F * scale);
    ImGui::Combo("##geometry_projection", &projection_, projections,
                 static_cast<int>(std::size(projections)));
    ImGui::SameLine();
    if (ImGui::Button("Frame")) reset_view();
    ImGui::SameLine();
    if (ImGui::Button("Options")) ImGui::OpenPopup("geometry_options");
    if (ImGui::BeginPopup("geometry_options")) {
        ImGui::Checkbox("Wire overlay", &wireframe_);
        ImGui::Checkbox("Cull backfaces", &cull_backfaces_);
        if (!uv_sets_.empty()) {
            const std::string preview = "UV set " + std::to_string(selected_uv_set_ + 1);
            ImGui::SetNextItemWidth(200.0F * scale);
            if (ImGui::BeginCombo("UV channel", preview.c_str())) {
                for (std::size_t i = 0; i < uv_sets_.size(); ++i) {
                    const std::string label = "UV set " + std::to_string(i + 1) +
                                              (i == 1 ? " (lightmap convention)" : "");
                    if (ImGui::Selectable(label.c_str(), selected_uv_set_ == i)) select_uv_set(i);
                }
                ImGui::EndCombo();
            }
        }
        ImGui::SliderFloat("Lightmap intensity", &lightmap_intensity_, 0.25F, 4.0F, "%.2fx");
        ImGui::Separator();
        ImGui::Text("Textures: %zu loaded, %zu unresolved", loaded_texture_count_,
                    missing_texture_count_);
        ImGui::Text("UV channels: %zu", uv_sets_.size());
        if (!texture_status_.empty()) ImGui::TextWrapped("%s", texture_status_.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Reload edited bytes")) load(geometry_chunk, bytes, source_path);
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    const auto available = ImGui::GetContentRegionAvail();
    const ImVec2 size{std::max(available.x, 64.0F), std::max(available.y, 160.0F)};
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("geometry_canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                               ImGuiButtonFlags_MouseButtonRight);
    canvas_hovered_ = ImGui::IsItemHovered();
    auto* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y},
                             ui::viewport_color(ui::Viewport::canvas));
    draw_list->AddRect(origin, {origin.x + size.x, origin.y + size.y}, ui::viewport_color(ui::Viewport::canvas_border));
    if (ImGui::IsItemHovered()) {
        const auto& io = ImGui::GetIO();
        if (io.KeyCtrl) {
            if (const auto hover = pick_collision(io.MousePos.x, io.MousePos.y)) {
                char coordinate[128]{};
                std::snprintf(coordinate, sizeof(coordinate), "surface %.6g, %.6g, %.6g",
                              hover->position.x, hover->position.y, hover->position.z);
                draw_list->AddText({io.MousePos.x + 14, io.MousePos.y + 14},
                                   ui::viewport_color(ui::Viewport::hud_text), coordinate);
            }
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) reset_view();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            target_yaw_ -= io.MouseDelta.x * 0.01F;
            target_pitch_ = std::clamp(target_pitch_ + io.MouseDelta.y * 0.01F, -1.5F, 1.5F);
            preserve_camera_position_ = true;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            target_yaw_ += io.MouseDelta.x * 0.01F;
            target_pitch_ = std::clamp(target_pitch_ + io.MouseDelta.y * 0.01F, -1.5F, 1.5F);
            preserve_camera_position_ = false;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            pan_camera(io.MouseDelta.x, io.MouseDelta.y);
        }
        if (io.MouseWheel != 0.0F) {
            const float minimum_distance = scene_mode_ ? std::max(radius_ * 0.0001F, 0.05F)
                                                       : std::max(radius_ * 0.02F, 0.001F);
            if (projection_ == 0)
                distance_ = std::clamp(distance_ * std::exp(-io.MouseWheel * 0.16F),
                                       minimum_distance, radius_ * 100.0F);
            else
                orthographic_scale_ =
                    std::clamp(orthographic_scale_ * std::exp(-io.MouseWheel * 0.16F),
                               minimum_distance, radius_ * 100.0F);
        }
    }
    update_keyboard_navigation();
    if (!error_.empty()) {
        draw_list->AddText({origin.x + 12, origin.y + 12}, ui::viewport_color(ui::Viewport::error_text),
                           error_.c_str());
        return;
    }

    canvas_x_ = origin.x;
    canvas_y_ = origin.y;
    canvas_width_ = size.x;
    canvas_height_ = size.y;
    draw_list->AddCallback(render_callback, this);
    draw_list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    const std::string statistics = std::to_string(vertices_.size()) + " vertices | " +
                                   std::to_string(faces_.size()) + " triangles | GPU depth test";
    draw_list->AddText({origin.x + 10, origin.y + 9}, ui::viewport_color(ui::Viewport::hud_text),
                       statistics.c_str());
}

bool GeometryPreview::draw_scene(
    const std::vector<rws::Chunk>& chunks, const std::span<const std::byte> bytes,
    const std::span<const rws::SceneInstance> instances, const std::filesystem::path& source_path,
    std::optional<std::uint64_t>& selected_chunk, const rws::Document* collision_document,
    const bool main_is_collision, const std::string_view collision_status) {
    if (!scene_mode_)
        load_scene(chunks, bytes, instances, source_path, collision_document, main_is_collision);
    else if (animated_actor_dirty_)
        refresh_mission_actor_animation();

    const bool has_collision = collision_sector_count_ != 0;
    const bool has_visual =
        std::any_of(draw_batches_.begin(), draw_batches_.end(), [](const DrawBatch& batch) {
            return batch.layer != PreviewLayer::collision_world;
        });
    bool open_tools = false;

    const auto available = ImGui::GetContentRegionAvail();
    const ImVec2 size{std::max(available.x, 64.0F), std::max(available.y, 160.0F)};
    const auto origin = ImGui::GetCursorScreenPos();
    canvas_x_ = origin.x;
    canvas_y_ = origin.y;
    canvas_width_ = size.x;
    canvas_height_ = size.y;
    ImGui::InvisibleButton("scene_canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                               ImGuiButtonFlags_MouseButtonRight);
    canvas_hovered_ = ImGui::IsItemHovered();
    auto* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y},
                             ui::viewport_color(ui::Viewport::canvas));
    draw_list->AddRect(origin, {origin.x + size.x, origin.y + size.y}, ui::viewport_color(ui::Viewport::canvas_border));
    const auto select_collision = [&](const rws::CollisionHit& hit) {
        selected_collision_ = hit;
        if (!measurement_mode_) return;
        if (!measurement_a_ || measurement_b_) {
            measurement_a_ = hit.position;
            measurement_b_.reset();
        } else {
            measurement_b_ = hit.position;
        }
    };
    // The edit gizmo owns the mouse from the press on a handle to the release.
    const bool gizmo_owns_mouse = update_edit_gizmo();
    const bool authoring = authoring_tool(edit_tool_);
    // An asset dragged from the Assets panel: dropped, it is placed there.
    const auto* payload = ImGui::GetDragDropPayload();
    const bool dragging_asset = payload && payload->IsDataType("RWSMAN_ASSET");
    std::optional<ImVec2> dropped;
    if (ImGui::BeginDragDropTarget()) {
        if (ImGui::AcceptDragDropPayload("RWSMAN_ASSET")) dropped = ImGui::GetIO().MousePos;
        ImGui::EndDragDropTarget();
    }
    hover_ground_.reset();
    // The ground under a screen point: the collision surface, else the plane
    // at the height of the last surface hit (open terrain edges, no collision).
    // Placing stands things on furniture too (a radio on a table).
    const bool placing = edit_tool_ == EditTool::place || dragging_asset;
    const auto ground_at = [&](const ImVec2 screen) -> std::optional<rws::Vec3> {
        if (const auto surface = placing ? placement_point(screen) : surface_point(screen)) {
            last_ground_height_ = surface->y;
            return surface;
        }
        if (const auto ray = viewport_ray(screen.x, screen.y); ray && std::abs(ray->direction.y) > 1e-5F) {
            const float t = (last_ground_height_ - ray->origin.y) / ray->direction.y;
            if (t > 0)
                return rws::Vec3{ray->origin.x + ray->direction.x * t, last_ground_height_,
                                 ray->origin.z + ray->direction.z * t};
        }
        return std::nullopt;
    };
    if ((ImGui::IsItemHovered() && authoring) ||
        (dragging_asset && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)))
        hover_ground_ = ground_at(ImGui::GetIO().MousePos);
    if (dropped) {
        ViewportClick drop{*dropped, ground_at(*dropped)};
        drop.drop = true;
        viewport_clicks_.push_back(drop);
    }
    // Shift+drag with the select tool draws a selection box.
    if (ImGui::IsItemHovered() && !gizmo_owns_mouse && edit_tool_ == EditTool::select && ImGui::GetIO().KeyShift &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        box_start_ = ImGui::GetIO().MousePos;
    if (box_start_) {
        const auto& io = ImGui::GetIO();
        const ImVec2 a = *box_start_, b = io.MousePos;
        const ImVec2 lo{std::min(a.x, b.x), std::min(a.y, b.y)}, hi{std::max(a.x, b.x), std::max(a.y, b.y)};
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            ImGui::GetForegroundDrawList()->AddRectFilled(lo, hi, ui::color_u32(ui::Token::accent, 0.12F));
            ImGui::GetForegroundDrawList()->AddRect(lo, hi, ui::color_u32(ui::Token::accent, 0.8F));
        } else {
            PickEvent event;
            event.is_box = true;
            event.shift = true;
            event.ctrl = io.KeyCtrl;
            for (const auto& marker : overlay_markers_) {
                if (marker.screen.x < lo.x || marker.screen.x > hi.x || marker.screen.y < lo.y || marker.screen.y > hi.y)
                    continue;
                const auto entry = mission_points_[marker.point].source_entry;
                if (!locked_mission_entries_.contains(entry) && std::ranges::find(event.box, entry) == event.box.end())
                    event.box.push_back(entry);
            }
            if ((hi.x - lo.x) * (hi.y - lo.y) > 16.0F) pick_event_ = std::move(event);
            box_start_.reset();
        }
    }
    if (ImGui::IsItemHovered() && !gizmo_owns_mouse) {
        const auto& io = ImGui::GetIO();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const int hit = axis_gizmo_hit(origin, size, io.MousePos);
            if (hit >= 0) {
                set_projection(hit);
                gizmo_press_ = true;
            }
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            const auto drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            if (!gizmo_press_ && drag.x * drag.x + drag.y * drag.y < 16.0F) {
                if (authoring) {
                    // The authoring tools take the click (AppState applies it).
                    viewport_clicks_.push_back({io.MousePos, ground_at(io.MousePos), io.KeyShift, io.KeyCtrl,
                                                io.KeyAlt, io.MouseClickedLastCount[ImGuiMouseButton_Left] >= 2});
                } else {
                    // Shift/Ctrl clicks add or remove the marker under the pointer, never the next one.
                    if (!overlay_click(io.MousePos, !io.KeyShift && !io.KeyCtrl)) {
                        selected_mission_entry_.reset();
                        const auto collision_hit = pick_collision(io.MousePos.x, io.MousePos.y);
                        if (collision_hit && (prefer_collision_ || measurement_mode_ || io.KeyAlt)) {
                            select_collision(*collision_hit);
                        } else if (const auto picked = pick_scene(io.MousePos.x, io.MousePos.y)) {
                            if ((*picked & 0x8000000000000000ULL) != 0)
                                selected_mission_entry_ = static_cast<std::uint32_t>(*picked);
                            else
                                selected_chunk = *picked;
                        } else if (collision_hit) {
                            select_collision(*collision_hit);
                        }
                    }
                    // Shift/Ctrl clicks extend the selection; a plain click resets it.
                    PickEvent event;
                    event.entry = selected_mission_entry_;
                    event.shift = io.KeyShift;
                    event.ctrl = io.KeyCtrl;
                    pick_event_ = std::move(event);
                }
            }
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) gizmo_press_ = false;
        // A double click on empty ground resets the view; on a marker (or with
        // Shift/Ctrl, picking) it is a second click.
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !gizmo_press_ && !authoring && !io.KeyShift &&
            !io.KeyCtrl && overlay_candidates(io.MousePos).empty())
            reset_view();
        const float look_sign = invert_y_ ? -1.0F : 1.0F;
        if (projection_ == 0 && !gizmo_press_ && !box_start_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            target_yaw_ -= io.MouseDelta.x * 0.01F;
            target_pitch_ = std::clamp(target_pitch_ + io.MouseDelta.y * 0.01F * look_sign, -1.5F, 1.5F);
            preserve_camera_position_ = true;
        }
        if (projection_ == 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            target_yaw_ += io.MouseDelta.x * 0.01F;
            target_pitch_ = std::clamp(target_pitch_ + io.MouseDelta.y * 0.01F * look_sign, -1.5F, 1.5F);
            preserve_camera_position_ = false;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            pan_camera(io.MouseDelta.x, io.MouseDelta.y);
        }
        if (io.MouseWheel != 0.0F) {
            const float minimum_distance = scene_mode_ ? std::max(radius_ * 0.0001F, 0.05F)
                                                       : std::max(radius_ * 0.02F, 0.001F);
            if (projection_ == 0)
                distance_ = std::clamp(distance_ * std::exp(-io.MouseWheel * 0.16F),
                                       minimum_distance, radius_ * 100.0F);
            else
                orthographic_scale_ =
                    std::clamp(orthographic_scale_ * std::exp(-io.MouseWheel * 0.16F),
                               minimum_distance, radius_ * 100.0F);
        }
    }
    update_keyboard_navigation();
    if (!error_.empty()) {
        draw_list->AddText({origin.x + 12, origin.y + 12}, ui::viewport_color(ui::Viewport::error_text),
                           error_.c_str());
        return open_tools;
    }
    prepare_overlays(origin, size);
    draw_list->AddCallback(render_callback, this);
    draw_list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    draw_list->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
    auto line3d = [&](const rws::Vec3 a, const rws::Vec3 b, const ImU32 color,
                      float thickness = 1.0F) {
        const auto pa = project_point(a), pb = project_point(b);
        if (pa && pb) draw_list->AddLine(*pa, *pb, color, thickness);
    };
    // Bounded adaptive grid in source RWS units. Y is the confirmed map
    // vertical axis.
    const float target_spacing = std::max(distance_ / 10.0F, 0.001F);
    const float decade = std::pow(10.0F, std::floor(std::log10(target_spacing)));
    const float ratio = target_spacing / decade;
    const float spacing = decade * (ratio > 5 ? 10 : (ratio > 2 ? 5 : (ratio > 1 ? 2 : 1)));
    const float extent = spacing * 10;
    const ImU32 minor = ui::viewport_color(ui::Viewport::grid_minor), major = ui::viewport_color(ui::Viewport::grid_major);
    // The grid fades with distance from its centre (V7): each line is drawn in
    // pieces whose alpha falls off towards the edge, so it has no hard border.
    const auto faded = [](const ImU32 color, const float t) {
        const float keep = std::clamp(1.0F - t * t, 0.0F, 1.0F);
        const auto alpha = static_cast<ImU32>(static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFFU) * keep);
        return (color & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
    };
    // A line of the grid from its (u, v) plane coordinates about the centre.
    const auto grid_line = [&](const float u0, const float v0, const float u1, const float v1, const ImU32 color) {
        constexpr int pieces = 12;
        const auto at = [&](const float u, const float v) -> rws::Vec3 {
            if (projection_ == 2) return {center_.x + u, center_.y + v, center_.z};
            if (projection_ == 3) return {center_.x, center_.y + v, center_.z + u};
            return {center_.x + u, center_.y, center_.z + v};
        };
        for (int k = 0; k < pieces; ++k) {
            const float a = static_cast<float>(k) / pieces, b = static_cast<float>(k + 1) / pieces;
            const float um = u0 + (u1 - u0) * (a + b) * 0.5F, vm = v0 + (v1 - v0) * (a + b) * 0.5F;
            const auto shade = faded(color, std::sqrt(um * um + vm * vm) / extent);
            if ((shade & IM_COL32_A_MASK) == 0) continue;
            line3d(at(u0 + (u1 - u0) * a, v0 + (v1 - v0) * a), at(u0 + (u1 - u0) * b, v0 + (v1 - v0) * b), shade);
        }
    };
    for (int i = -10; i <= 10; ++i) {
        const float v = i * spacing;
        const auto color = i % 5 == 0 ? major : minor;
        grid_line(-extent, v, extent, v, color);
        grid_line(v, -extent, v, extent, color);
    }
    const auto draw_box = [&](const rws::Vec3 lo, const rws::Vec3 hi, const ImU32 color) {
        const std::array<rws::Vec3, 8> p{{{lo.x, lo.y, lo.z},
                                          {hi.x, lo.y, lo.z},
                                          {lo.x, hi.y, lo.z},
                                          {hi.x, hi.y, lo.z},
                                          {lo.x, lo.y, hi.z},
                                          {hi.x, lo.y, hi.z},
                                          {lo.x, hi.y, hi.z},
                                          {hi.x, hi.y, hi.z}}};
        constexpr std::array<std::array<int, 2>, 12> edges{{{0, 1},
                                                            {0, 2},
                                                            {0, 4},
                                                            {1, 3},
                                                            {1, 5},
                                                            {2, 3},
                                                            {2, 6},
                                                            {3, 7},
                                                            {4, 5},
                                                            {4, 6},
                                                            {5, 7},
                                                            {6, 7}}};
        for (const auto& edge : edges)
            line3d(p[edge[0]], p[edge[1]], color, 1.5F);
    };
    const auto draw_measurement_point = [&](const rws::Vec3 point, const char* label,
                                            const ImU32 color) {
        if (const auto screen = project_point(point)) {
            draw_list->AddCircleFilled(*screen, 5.0F, color);
            draw_list->AddText({screen->x + 7.0F, screen->y - 8.0F}, color, label);
        }
    };
    if (measurement_a_) draw_measurement_point(*measurement_a_, "A", ui::viewport_color(ui::Viewport::measure_a));
    if (measurement_b_) draw_measurement_point(*measurement_b_, "B", ui::viewport_color(ui::Viewport::measure_b));
    if (measurement_a_ && measurement_b_) {
        const auto a = project_point(*measurement_a_);
        const auto b = project_point(*measurement_b_);
        if (a && b) {
            draw_list->AddLine(*a, *b, ui::viewport_color(ui::Viewport::measurement), 2.0F);
            const auto measurement = rws::measure_points(*measurement_a_, *measurement_b_);
            char label[64]{};
            std::snprintf(label, sizeof(label), "%.6g", measurement.distance);
            draw_list->AddText({(a->x + b->x) * 0.5F + 5.0F, (a->y + b->y) * 0.5F + 5.0F},
                               ui::viewport_color(ui::Viewport::measurement), label);
        }
    }
    if (show_skeleton_)
        for (const auto& bone : skeleton_lines_) {
            line3d(bone.parent, bone.child, ui::viewport_color(ui::Viewport::skeleton), 2.0F);
            if (show_skeleton_labels_)
                if (const auto point = project_point(bone.child)) {
                    const auto label = bone.node_id >= 0 ? std::to_string(bone.node_id)
                                                         : ("frame " + std::to_string(bone.frame));
                    draw_list->AddText({point->x + 4, point->y - 6}, ui::viewport_color(ui::Viewport::skeleton_label),
                                       label.c_str());
                }
        }
    if (show_physics_ && !isolate_selected_actor_)
        for (const auto& line : physics_lines_)
            line3d(line.first, line.second, line.color, 1.8F);
    if (selected_collision_ && selected_collision_->world_index < collision_worlds_.size()) {
        const auto& hit = *selected_collision_;
        const auto& world = collision_worlds_[hit.world_index];
        if (hit.sector_index < world.sectors.size() && show_leaf_bounds_) {
            const auto& sector = world.sectors[hit.sector_index];
            draw_box(sector.bounding_box_inf, sector.bounding_box_sup, ui::viewport_color(ui::Viewport::leaf_bounds));
        }
        if (show_bsp_path_) {
            auto node = std::find_if(
                world.topology_nodes.begin(), world.topology_nodes.end(), [&](const auto& item) {
                    return item.kind == rws::RecoveredWorldNode::Kind::sector &&
                           item.value_index == hit.sector_index;
                });
            while (node != world.topology_nodes.end()) {
                if (node->kind == rws::RecoveredWorldNode::Kind::plane) {
                    const auto& plane = world.planes[node->value_index];
                    const auto lo = node->bounding_box_inf, hi = node->bounding_box_sup;
                    const ImU32 color = plane.axis == 0
                                            ? ui::viewport_color(ui::Viewport::bsp_x)
                                            : (plane.axis == 4 ? ui::viewport_color(ui::Viewport::bsp_y)
                                                               : ui::viewport_color(ui::Viewport::bsp_z));
                    if (plane.axis == 0) {
                        line3d({plane.split, lo.y, lo.z}, {plane.split, hi.y, lo.z}, color, 2);
                        line3d({plane.split, hi.y, lo.z}, {plane.split, hi.y, hi.z}, color, 2);
                        line3d({plane.split, hi.y, hi.z}, {plane.split, lo.y, hi.z}, color, 2);
                        line3d({plane.split, lo.y, hi.z}, {plane.split, lo.y, lo.z}, color, 2);
                    } else if (plane.axis == 4) {
                        line3d({lo.x, plane.split, lo.z}, {hi.x, plane.split, lo.z}, color, 2);
                        line3d({hi.x, plane.split, lo.z}, {hi.x, plane.split, hi.z}, color, 2);
                        line3d({hi.x, plane.split, hi.z}, {lo.x, plane.split, hi.z}, color, 2);
                        line3d({lo.x, plane.split, hi.z}, {lo.x, plane.split, lo.z}, color, 2);
                    } else {
                        line3d({lo.x, lo.y, plane.split}, {hi.x, lo.y, plane.split}, color, 2);
                        line3d({hi.x, lo.y, plane.split}, {hi.x, hi.y, plane.split}, color, 2);
                        line3d({hi.x, hi.y, plane.split}, {lo.x, hi.y, plane.split}, color, 2);
                        line3d({lo.x, hi.y, plane.split}, {lo.x, lo.y, plane.split}, color, 2);
                    }
                }
                if (!node->parent) break;
                node = world.topology_nodes.begin() + static_cast<std::ptrdiff_t>(*node->parent);
            }
        }
        if (const auto point = project_point(hit.position)) {
            draw_list->AddLine({point->x - 7, point->y}, {point->x + 7, point->y},
                               ui::viewport_color(ui::Viewport::collision_selected), 2);
            draw_list->AddLine({point->x, point->y - 7}, {point->x, point->y + 7},
                               ui::viewport_color(ui::Viewport::collision_selected), 2);
        }
    }
    // Clip planes: a translucent outline at each enabled plane, so the cut is
    // visible in the viewport and not only in the tool panel.
    for (std::size_t i = 0; i < clips_.size(); ++i) {
        const auto& clip = clips_[i];
        if (!clip.enabled) continue;
        const float r = all_radius_;
        const rws::Vec3 c = all_center_;
        const auto corner = [&](const float a, const float b) -> rws::Vec3 {
            if (clip.axis == 0) return {clip.position, c.y + a * r, c.z + b * r};
            if (clip.axis == 1) return {c.x + a * r, clip.position, c.z + b * r};
            return {c.x + a * r, c.y + b * r, clip.position};
        };
        const auto color = ui::viewport_color(ui::Viewport::clip_plane);
        line3d(corner(-1, -1), corner(1, -1), color, 1.5F);
        line3d(corner(1, -1), corner(1, 1), color, 1.5F);
        line3d(corner(1, 1), corner(-1, 1), color, 1.5F);
        line3d(corner(-1, 1), corner(-1, -1), color, 1.5F);
    }
    draw_overlay_decorations(draw_list, origin, size);
    draw_edit_gizmo(draw_list);
    draw_list->PopClipRect();
    draw_viewport_hud(draw_list, origin, size, collision_status);
    if (!tool_hint_.empty()) {
        // The active tool's guidance, centred above the HUD lines at the bottom of the viewport.
        const auto text_size = ImGui::CalcTextSize(tool_hint_.c_str());
        const float pad = 6.0F * ui::ui_scale();
        const ImVec2 at{origin.x + (size.x - text_size.x) * 0.5F, origin.y + size.y - 4.0F * text_size.y - 4.0F * pad};
        draw_list->AddRectFilled({at.x - pad, at.y - pad * 0.6F}, {at.x + text_size.x + pad, at.y + text_size.y + pad * 0.6F},
                                 ui::viewport_color(ui::Viewport::hud_background), 4.0F * ui::ui_scale());
        draw_list->AddText(at, ui::viewport_color(ui::Viewport::hud_text), tool_hint_.c_str());
    }
    draw_overlay_legend(draw_list, origin, size);
    draw_axis_gizmo(draw_list, origin, size);
    draw_overlay_hover_tooltip();
    if (draw_viewport_toolbar(origin, size, has_visual, has_collision)) open_tools = true;
    draw_measure_panel(origin, size);
    draw_overlay_minimap(origin, size);
    draw_overlay_picker();
    if (occlusion_pending_) animating_frame_ = ImGui::GetFrameCount();
    return open_tools;
}

void GeometryPreview::set_projection(const int projection) noexcept {
    projection_ = std::clamp(projection, 0, 3);
    if (projection_ != 0) last_orthographic_projection_ = projection_;
}

void GeometryPreview::toggle_perspective() noexcept {
    set_projection(projection_ == 0 ? last_orthographic_projection_ : 0);
}

namespace {

std::string compact_count(const std::size_t value) {
    char buffer[24];
    if (value >= 1000000)
        std::snprintf(buffer, sizeof(buffer), "%.2fM", static_cast<double>(value) / 1.0e6);
    else if (value >= 1000)
        std::snprintf(buffer, sizeof(buffer), "%.0fk", static_cast<double>(value) / 1.0e3);
    else
        std::snprintf(buffer, sizeof(buffer), "%zu", value);
    return buffer;
}

} // namespace

bool GeometryPreview::draw_viewport_toolbar(const ImVec2 origin, const ImVec2 size,
                                            const bool has_visual, const bool has_collision) {
    bool open_tools = false;
    constexpr std::array scene_styles{0, 1, 2, 5, 6, 7};
    constexpr const char* scene_style_names[] = {"Textured",        "Material index",
                                                 "Material color",  "Lightmap texture",
                                                 "Base + lightmap", "Wireframe"};
    constexpr const char* projection_names[] = {"Perspective", "Top (X/Z)", "Front (X/Y)",
                                                "Side (Z/Y)"};
    const auto selected_style = std::find(scene_styles.begin(), scene_styles.end(), view_style_);
    const std::size_t style_index =
        selected_style == scene_styles.end()
            ? 1
            : static_cast<std::size_t>(std::distance(scene_styles.begin(), selected_style));
    // Mission overlays add marker controls and the filter field to the toolbar.
    const bool compact = size.x < (has_mission_overlays() ? 1080.0F : 640.0F) * ui::ui_scale();
    const float pad = 8.0F * ui::ui_scale();
    const std::string chevron = ui::icons::LC_CHEVRON_DOWN;

    ImGui::SetCursorScreenPos({origin.x + pad, origin.y + pad});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::color(ui::Token::bg0, 0.88F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {3.0F, 3.0F});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {2.0F, 0.0F});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 3.0F);
    ImGui::PushStyleColor(ImGuiCol_Button, ui::transparent());
    const bool visible = ImGui::BeginChild(
        "##viewport_toolbar", {0.0F, 0.0F},
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY |
            ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings);
    if (visible) {
        // A toolbar button that opens a popup below itself.
        const auto popup_button = [&](const char* id, const std::string& label, const char* tooltip,
                                      const bool active = false) {
            if (active) ImGui::PushStyleColor(ImGuiCol_Text, ui::color(ui::Token::accent));
            const bool clicked = ImGui::Button((label + "##" + id).c_str());
            if (active) ImGui::PopStyleColor();
            if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
            const ImVec2 anchor{ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 2.0F};
            if (clicked) ImGui::OpenPopup(id);
            ImGui::SetNextWindowPos(anchor, ImGuiCond_Always);
            return ImGui::BeginPopup(id);
        };

        // Three labelled menus (docs/archive/editor/editor-ux-redesign.md, C2): View
        // (shading, projection, framing, options), Show (layers, clipping) and
        // Markers; then the quick toggles and the marker filter.
        if (popup_button("view", std::string(ui::icons::LC_EYE) + " View " + chevron,
                         "Shading, projection (numpad 1/3/7, 5), framing and view options")) {
            ui::section("Shading");
            for (std::size_t i = 0; i < std::size(scene_style_names); ++i)
                if (ImGui::Selectable(scene_style_names[i], i == style_index)) view_style_ = scene_styles[i];
            ui::section("Projection");
            for (int i = 0; i < 4; ++i)
                if (ImGui::Selectable(projection_names[i], i == projection_)) set_projection(i);
            ui::section("Frame");
            if (ImGui::MenuItem("All", "Home")) frame_all();
            if (ImGui::MenuItem("Visual map", nullptr, false, has_visual))
                frame_bounds(visual_center_, visual_radius_, visual_extent_);
            if (ImGui::MenuItem("Collision map", nullptr, false, has_collision))
                frame_bounds(collision_center_, collision_radius_, collision_extent_);
            if (ImGui::MenuItem("Selection", "F", false, selected_mission_entry_.has_value()))
                frame_selection(std::nullopt);
            ui::section("Options");
            ImGui::Checkbox("Stats HUD", &show_hud_);
            ImGui::Checkbox("Wire overlay", &wireframe_);
            ImGui::Checkbox("Cull backfaces", &cull_backfaces_);
            ImGui::Checkbox("Prefer collision on click", &prefer_collision_);
            ImGui::SetNextItemWidth(160.0F * ui::ui_scale());
            ImGui::SliderFloat("Move speed", &navigation_speed_, 0.05F, 4.0F, "%.2fx", ImGuiSliderFlags_Logarithmic);
            ImGui::Separator();
            if (ImGui::MenuItem("Render settings panel...")) open_tools = true;
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        bool any_clip = false;
        for (const auto& clip : clips_) any_clip = any_clip || clip.enabled;
        if (popup_button("show", std::string(ui::icons::LC_LAYERS) + " Show " + chevron,
                         "Map layers, collision, clip planes", any_clip)) {
            draw_layers_popup(has_visual, has_collision);
            ui::section("Clip planes");
            const char* axes = "XYZ";
            for (std::size_t i = 0; i < clips_.size(); ++i) {
                auto& clip = clips_[i];
                ImGui::PushID(static_cast<int>(i));
                const std::string label = std::string("Clip ") + axes[i];
                ImGui::Checkbox(label.c_str(), &clip.enabled);
                if (clip.enabled) {
                    ImGui::SameLine();
                    ImGui::Checkbox("keep greater", &clip.keep_greater);
                    ImGui::SetNextItemWidth(180.0F * ui::ui_scale());
                    ImGui::DragFloat("##clip_position", &clip.position, 0.5F);
                }
                ImGui::PopID();
            }
            ImGui::Checkbox("Leaf bounds", &show_leaf_bounds_);
            ImGui::Checkbox("Selected BSP path", &show_bsp_path_);
            ImGui::EndPopup();
        }
        if (has_mission_overlays()) {
            ImGui::SameLine();
            if (popup_button("markers", std::string(ui::icons::LC_MAP_PIN) + " Markers " + chevron,
                             "Marker display: labels, depth, fading, merging, height slice")) {
                draw_markers_popup();
                ImGui::EndPopup();
            }
            const auto toggle = [&](const char* id, const char* icon, bool& value, const char* tooltip) {
                if (value) ImGui::PushStyleColor(ImGuiCol_Text, ui::color(ui::Token::accent));
                const bool clicked = ImGui::Button((std::string(icon) + "##" + id).c_str());
                if (value) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
                return clicked;
            };
            ImGui::SameLine();
            if (toggle("focus", ui::icons::LC_TARGET, focus_mode_,
                       "Focus mode (Z): dim markers unrelated to the selection"))
                focus_mode_ = !focus_mode_;
            ImGui::SameLine();
            bool slice = slice_.enabled;
            if (toggle("slice", ui::icons::LC_MOUNTAIN, slice,
                       "Height slice (Y): show one floor around the selection or camera target"))
                toggle_floor_slice();
            ImGui::SameLine();
            if (focus_filter_request_) {
                ImGui::SetKeyboardFocusHere();
                focus_filter_request_ = false;
            }
            ImGui::SetNextItemWidth((compact ? 90.0F : 150.0F) * ui::ui_scale());
            ImGui::InputTextWithHint("##overlay_filter",
                                     (std::string(ui::icons::LC_FILTER) + " Filter markers (/)").c_str(),
                                     overlay_filter_.data(), overlay_filter_.size());
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("Name, sublayer or layer text; #123 for an entry; kind:nav for a layer.\n"
                                  "Escape clears. Non-matching markers are %s.",
                                  overlay_options_.dim_filtered ? "dimmed" : "hidden");
            if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape)) overlay_filter_.fill('\0');
        }
        collision_loaded_for_measure_ = has_collision;
        if (!tool_strip_) {
        ImGui::SameLine();
        ImGui::BeginDisabled(!has_collision);
        // The measure button is a plain toggle, not a popup.
        if (measurement_mode_) ImGui::PushStyleColor(ImGuiCol_Text, ui::color(ui::Token::accent));
        const bool measure_clicked = ImGui::Button((std::string(ui::icons::LC_RULER) + "##measure").c_str());
        if (measurement_mode_) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Measure two points on the level collision");
        if (measure_clicked) measurement_mode_ = !measurement_mode_;
        ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ui::icon_button("screenshot", ui::icons::LC_CAMERA, "Save a PNG screenshot (F12)"))
            screenshot_requested_ = true;
    }
    ImGui::EndChild();
    toolbar_bottom_ = ImGui::GetItemRectMax().y;
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    if (tool_strip_) draw_tool_strip(origin);
    return open_tools;
}

void GeometryPreview::draw_tool_strip(const ImVec2 origin) {
    // The editing tools (docs/archive/editor/editor-ux-redesign.md, C1), a column on
    // the viewport's left edge under the toolbar.
    const float scale = ui::ui_scale();
    ImGui::SetCursorScreenPos({origin.x + 8.0F * scale, toolbar_bottom_ + 8.0F * scale});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::color(ui::Token::bg0, 0.88F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {3.0F, 3.0F});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0F, 2.0F});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 3.0F);
    if (ImGui::BeginChild("##tool_strip", {0.0F, 0.0F},
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY |
                              ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                              ImGuiWindowFlags_NoSavedSettings)) {
        const auto tool = [&](const char* id, const char* icon, const EditTool value, const char* tooltip) {
            if (ui::icon_button(id, icon, tooltip, edit_tool_ == value)) edit_tool_ = value;
        };
        tool("select_tool", ui::icons::LC_MOUSE_POINTER, EditTool::select, "Select (Q): click to select, F frames it");
        tool("move_tool", ui::icons::LC_MOVE_3D, EditTool::move,
             "Move (G): drag the centre to slide on the ground, an arrow for one axis; Alt toggles snapping");
        tool("rotate_tool", ui::icons::LC_REFRESH_CW, EditTool::rotate,
             "Rotate (R): drag around the ring; Ctrl snaps to 15 degrees");
        ImGui::Separator();
        if (ui::icon_button("snap_tool", ui::icons::LC_MAGNET,
                            snap_to_surface_ ? "Snap to the ground: on (moves follow the collision surface)"
                                             : "Snap to the ground: off (moves keep their height)",
                            snap_to_surface_))
            snap_to_surface_ = !snap_to_surface_;
        ImGui::BeginDisabled(!collision_loaded_for_measure_);
        if (ui::icon_button("measure_tool", ui::icons::LC_RULER, "Measure two points on the ground", measurement_mode_))
            measurement_mode_ = !measurement_mode_;
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

void GeometryPreview::draw_viewport_hud(ImDrawList* draw_list, const ImVec2 origin, const ImVec2 size,
                                        const std::string_view collision_status) {
    if (show_hud_) {
        const std::size_t triangles = (show_visual_ ? visual_triangle_total_ : 0) +
                                      (show_collision_ ? collision_triangle_total_ : 0);
        char first[96];
        // A negative rate hides it (UI test runs).
        if (frame_fps_ < 0)
            std::snprintf(first, sizeof(first), "%s tris \xC2\xB7 cam %d", compact_count(triangles).c_str(),
                          static_cast<int>(projection_ == 0 ? distance_ : orthographic_scale_));
        else
            std::snprintf(first, sizeof(first), "fps %.0f \xC2\xB7 %s tris \xC2\xB7 cam %d", frame_fps_,
                          compact_count(triangles).c_str(),
                          static_cast<int>(projection_ == 0 ? distance_ : orthographic_scale_));
        std::ostringstream second;
        second << scene_clump_count_ << " clumps \xC2\xB7 " << scene_instance_count_ << " instances \xC2\xB7 "
               << scene_custom_instance_count_ << " placements";
        if (collision_sector_count_ != 0)
            second << " \xC2\xB7 collision " << collision_sector_count_ << " sectors / "
                   << compact_count(collision_triangle_count_) << " tris";
        std::vector<std::string> lines{first, second.str()};
        if (show_frame_stats_) {
            // CPU covers the whole app frame; GPU only the scene pass, read back late.
            char timing[128];
            char gpu[24] = "n/a";
            if (gpu_ms_ >= 0.0) std::snprintf(gpu, sizeof(gpu), "%.2f ms", gpu_ms_);
            std::snprintf(timing, sizeof(timing), "cpu %.2f ms \xC2\xB7 gpu %s \xC2\xB7 %zu draws \xC2\xB7 %zu/%zu culled",
                          frame_cpu_ms_, gpu, last_draw_calls_, last_culled_batches_, draw_batches_.size());
            lines.emplace_back(timing);
        }
        ImGui::PushFont(ui::font(ui::Font::mono));
        const float line = ImGui::GetTextLineHeightWithSpacing();
        float width = 0.0F;
        for (const auto& text : lines) width = std::max(width, ImGui::CalcTextSize(text.c_str()).x);
        width += 16.0F;
        const float height = line * static_cast<float>(lines.size());
        const ImVec2 lo{origin.x + 8.0F, origin.y + size.y - height - 14.0F};
        draw_list->AddRectFilled(lo, {lo.x + width, lo.y + height + 6.0F},
                                 ui::viewport_color(ui::Viewport::hud_background), 3.0F);
        for (std::size_t i = 0; i < lines.size(); ++i)
            draw_list->AddText({lo.x + 8.0F, lo.y + 3.0F + line * static_cast<float>(i)},
                               ui::viewport_color(i == 0 ? ui::Viewport::hud_text : ui::Viewport::hud_text_dim),
                               lines[i].c_str());
        ImGui::PopFont();
    }
    if (!texture_status_.empty() || !collision_diagnostics_.empty() ||
        (collision_sector_count_ == 0 && !collision_status.empty())) {
        const char* warning = !texture_status_.empty()
                                  ? "Rendering warning - see Render settings"
                                  : (!collision_diagnostics_.empty() ? "Collision warning - see Render settings"
                                                                     : "No collision companion");
        const auto warning_size = ImGui::CalcTextSize(warning);
        draw_list->AddRectFilled({origin.x + size.x - warning_size.x - 25.0F, origin.y + 8.0F},
                                 {origin.x + size.x - 8.0F, origin.y + 31.0F},
                                 ui::viewport_color(ui::Viewport::warning_background), 3.0F);
        draw_list->AddText({origin.x + size.x - warning_size.x - 17.0F, origin.y + 12.0F},
                           ui::viewport_color(ui::Viewport::warning_text), warning);
    }
}

int GeometryPreview::axis_gizmo_hit(const ImVec2 origin, const ImVec2 size, const ImVec2 mouse) const {
    if (!scene_mode_ || canvas_width_ <= 0.0F) return -1;
    const float scale = ui::ui_scale();
    const ImVec2 center{origin.x + size.x - 58.0F * scale, origin.y + size.y - 58.0F * scale};
    const float length = 32.0F * scale;
    const auto base = project_point(center_);
    if (!base) return -1;
    const float step = std::max(radius_ * 0.2F, 0.001F);
    const std::array<rws::Vec3, 3> axes{{{step, 0, 0}, {0, step, 0}, {0, 0, step}}};
    // X -> side view (looking along X), Y -> top view, Z -> front view.
    constexpr std::array<int, 3> targets{3, 1, 2};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto tip = project_point({center_.x + axes[i].x, center_.y + axes[i].y, center_.z + axes[i].z});
        if (!tip) continue;
        float dx = tip->x - base->x, dy = tip->y - base->y;
        const float norm = std::sqrt(dx * dx + dy * dy);
        if (norm < 0.5F) continue;
        dx = dx / norm * length;
        dy = dy / norm * length;
        const float distance = std::hypot(mouse.x - (center.x + dx), mouse.y - (center.y + dy));
        if (distance < 10.0F * scale) return targets[i];
    }
    return std::hypot(mouse.x - center.x, mouse.y - center.y) < 8.0F * scale ? 0 : -1;
}

void GeometryPreview::draw_axis_gizmo(ImDrawList* draw_list, const ImVec2 origin, const ImVec2 size) {
    if (!scene_mode_ || canvas_width_ <= 0.0F) return;
    const float scale = ui::ui_scale();
    const ImVec2 center{origin.x + size.x - 58.0F * scale, origin.y + size.y - 58.0F * scale};
    const float length = 32.0F * scale;
    const auto base = project_point(center_);
    if (!base) return;
    const float step = std::max(radius_ * 0.2F, 0.001F);
    const std::array<rws::Vec3, 3> axes{{{step, 0, 0}, {0, step, 0}, {0, 0, step}}};
    constexpr std::array<ui::Viewport, 3> colors{ui::Viewport::axis_x, ui::Viewport::axis_y, ui::Viewport::axis_z};
    constexpr const char* names[] = {"X", "Y", "Z"};
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const int hovered = canvas_hovered_ ? axis_gizmo_hit(origin, size, mouse) : -1;
    draw_list->AddCircleFilled(center, length + 14.0F * scale, ui::viewport_color(ui::Viewport::hud_background, 0.6F), 32);
    struct Axis {
        float dx, dy;
        std::size_t index;
    };
    std::array<Axis, 3> drawn{};
    std::size_t count = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        const auto tip = project_point({center_.x + axes[i].x, center_.y + axes[i].y, center_.z + axes[i].z});
        if (!tip) continue;
        float dx = tip->x - base->x, dy = tip->y - base->y;
        const float norm = std::sqrt(dx * dx + dy * dy);
        if (norm < 0.5F) {
            // The axis points along the view direction: draw it as a dot.
            draw_list->AddCircleFilled(center, 5.0F * scale, ui::viewport_color(colors[i]));
            continue;
        }
        drawn[count++] = {dx / norm * length, dy / norm * length, i};
    }
    for (std::size_t n = 0; n < count; ++n) {
        const auto& axis = drawn[n];
        const auto color = ui::viewport_color(colors[axis.index]);
        draw_list->AddLine(center, {center.x + axis.dx, center.y + axis.dy}, color, 2.0F);
        draw_list->AddCircleFilled({center.x - axis.dx * 0.6F, center.y - axis.dy * 0.6F}, 3.0F * scale,
                                   ui::viewport_color(colors[axis.index], 0.35F));
        const ImVec2 tip{center.x + axis.dx, center.y + axis.dy};
        constexpr std::array<int, 3> targets{3, 1, 2};
        const bool hot = hovered == targets[axis.index];
        draw_list->AddCircleFilled(tip, 9.0F * scale, color);
        if (hot) draw_list->AddCircle(tip, 11.0F * scale, ui::viewport_color(ui::Viewport::selection_secondary), 16, 2.0F);
        draw_list->AddText({tip.x - ImGui::CalcTextSize(names[axis.index]).x * 0.5F, tip.y - ImGui::GetTextLineHeight() * 0.5F},
                           ui::viewport_color(ui::Viewport::canvas), names[axis.index]);
    }
    if (hovered == 0) draw_list->AddCircle(center, 8.0F * scale, ui::viewport_color(ui::Viewport::selection_secondary), 16);
    if (hovered >= 0) ImGui::SetTooltip(hovered == 0 ? "Perspective" : "Snap to %s view", hovered == 3 ? "side" : hovered == 1 ? "top" : "front");
}

void GeometryPreview::draw_measure_panel(const ImVec2 origin, const ImVec2 size) {
    if (!measurement_mode_) return;
    const float line = ImGui::GetTextLineHeightWithSpacing();
    ImGui::SetCursorScreenPos({origin.x + 8.0F, origin.y + size.y - line * 9.0F - 26.0F * ui::ui_scale()});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::color(ui::Token::bg0, 0.88F));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 3.0F);
    if (ImGui::BeginChild("##measure_panel", {0.0F, 0.0F},
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY |
                              ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        ui::section("Measure");
        ImGui::PushFont(ui::font(ui::Font::mono));
        const auto point_text = [&](const char* name, const std::optional<rws::Vec3>& point, const ui::Viewport color) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::viewport_color_f(color));
            if (point)
                ImGui::Text("%s  %.6g, %.6g, %.6g", name, point->x, point->y, point->z);
            else
                ImGui::Text("%s  click a collision surface", name);
            ImGui::PopStyleColor();
        };
        point_text("A", measurement_a_, ui::Viewport::measure_a);
        point_text("B", measurement_b_, ui::Viewport::measure_b);
        if (measurement_a_ && measurement_b_) {
            const auto measurement = rws::measure_points(*measurement_a_, *measurement_b_);
            ImGui::Text("distance %.6g", measurement.distance);
            ImGui::Text("|dX| %.6g  |dY| %.6g  |dZ| %.6g", measurement.absolute_delta.x,
                        measurement.absolute_delta.y, measurement.absolute_delta.z);
        }
        ImGui::PopFont();
        if (ImGui::SmallButton("Clear")) {
            measurement_a_.reset();
            measurement_b_.reset();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!measurement_a_ || !measurement_b_);
        if (ImGui::SmallButton("Swap")) std::swap(measurement_a_, measurement_b_);
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy distance")) {
            const auto measurement = rws::measure_points(*measurement_a_, *measurement_b_);
            char text[48];
            std::snprintf(text, sizeof(text), "%.9g", measurement.distance);
            ImGui::SetClipboardText(text);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Done")) measurement_mode_ = false;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void GeometryPreview::frame_all() {
    frame_bounds(all_center_, all_radius_, all_extent_);
}

bool GeometryPreview::frame_selection(const std::optional<std::uint64_t> chunk_offset) {
    rws::Vec3 minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max()};
    rws::Vec3 maximum{-minimum.x, -minimum.y, -minimum.z};
    bool found{};
    const auto include = [&](const rws::Vec3 point) {
        minimum = {std::min(minimum.x, point.x), std::min(minimum.y, point.y),
                   std::min(minimum.z, point.z)};
        maximum = {std::max(maximum.x, point.x), std::max(maximum.y, point.y),
                   std::max(maximum.z, point.z)};
        found = true;
    };
    const auto include_batches = [&](const auto& owned) {
        for (const auto& batch : draw_batches_) {
            if (!owned(batch)) continue;
            const auto& m = batch.transform;
            for (std::uint32_t i = 0; i < batch.count; ++i) {
                const auto index = static_cast<std::size_t>(batch.first) + i;
                if (index >= gpu_vertices_.size()) break;
                const auto& vertex = gpu_vertices_[index];
                include({m[0] * vertex.x + m[1] * vertex.y + m[2] * vertex.z + m[3],
                         m[4] * vertex.x + m[5] * vertex.y + m[6] * vertex.z + m[7],
                         m[8] * vertex.x + m[9] * vertex.y + m[10] * vertex.z + m[11]});
            }
        }
    };
    float minimum_radius = 0.001F;
    if (selected_mission_entry_) {
        include_batches([&](const DrawBatch& batch) {
            return actor_owner(batch.owner_offset) == selected_mission_entry_;
        });
        if (!found) {
            for (const auto& point : mission_points_)
                if (point.source_entry == *selected_mission_entry_) include(point.position);
            for (const auto& line : mission_lines_)
                if (line.source_entry == *selected_mission_entry_) {
                    include(line.first);
                    include(line.second);
                }
            // A lone marker has no extent; keep the surroundings visible.
            minimum_radius = std::max(radius_ * 0.02F, 1.0F);
        }
    } else if (chunk_offset) {
        include_batches([&](const DrawBatch& batch) {
            return batch.owner_offset == *chunk_offset && batch.layer != PreviewLayer::collision_world;
        });
    }
    if (!found) return false;
    const rws::Vec3 focus{(minimum.x + maximum.x) * 0.5F, (minimum.y + maximum.y) * 0.5F,
                          (minimum.z + maximum.z) * 0.5F};
    const auto dx = maximum.x - minimum.x, dy = maximum.y - minimum.y, dz = maximum.z - minimum.z;
    const float radius = std::max(0.5F * std::sqrt(dx * dx + dy * dy + dz * dz), minimum_radius);
    // A lone marker keeps its surroundings: fit the sphere, not the empty box.
    frame_bounds(focus, radius,
                 radius > minimum_radius ? std::optional(rws::Vec3{dx * 0.5F, dy * 0.5F, dz * 0.5F})
                                         : std::nullopt);
    return true;
}

rwsman::CameraSnapshot GeometryPreview::camera() const {
    rwsman::CameraSnapshot snapshot;
    snapshot.yaw = target_yaw_;
    snapshot.pitch = target_pitch_;
    snapshot.distance = distance_;
    snapshot.orthographic_scale = orthographic_scale_;
    snapshot.pan_x = pan_x_;
    snapshot.pan_y = pan_y_;
    snapshot.navigation[0] = target_navigation_offset_.x;
    snapshot.navigation[1] = target_navigation_offset_.y;
    snapshot.navigation[2] = target_navigation_offset_.z;
    snapshot.center[0] = center_.x;
    snapshot.center[1] = center_.y;
    snapshot.center[2] = center_.z;
    snapshot.radius = radius_;
    snapshot.projection = projection_;
    snapshot.valid = true;
    return snapshot;
}

rws::Vec3 GeometryPreview::orbit_target() const {
    return {center_.x + navigation_offset_.x, center_.y + navigation_offset_.y, center_.z + navigation_offset_.z};
}

rws::Vec3 GeometryPreview::eye_position() const {
    const auto target = orbit_target();
    const auto offset = camera_offset(yaw_, pitch_);
    return {target.x + offset.x, target.y + offset.y, target.z + offset.z};
}

void GeometryPreview::look_from(const rws::Vec3 eye, const rws::Vec3 target) {
    // In scene mode the eye sits at target + distance * (cos p sin y, sin p, cos p cos y).
    const rws::Vec3 d{eye.x - target.x, eye.y - target.y, eye.z - target.z};
    const auto distance = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (!(distance > 0.001F)) return;
    pan_x_ = pan_y_ = 0.0F;
    distance_ = distance;
    yaw_ = target_yaw_ = std::atan2(d.x, d.z);
    pitch_ = target_pitch_ = std::asin(std::clamp(d.y / distance, -1.0F, 1.0F));
    navigation_offset_ = target_navigation_offset_ = {target.x - center_.x, target.y - center_.y, target.z - center_.z};
    preserve_camera_position_ = false;
    animating_frame_ = ImGui::GetFrameCount();
}

void GeometryPreview::set_camera(const rwsman::CameraSnapshot& snapshot) {
    if (!snapshot.valid) return;
    yaw_ = target_yaw_ = snapshot.yaw;
    pitch_ = target_pitch_ = snapshot.pitch;
    distance_ = snapshot.distance;
    orthographic_scale_ = snapshot.orthographic_scale;
    pan_x_ = snapshot.pan_x;
    pan_y_ = snapshot.pan_y;
    navigation_offset_ = target_navigation_offset_ = {snapshot.navigation[0], snapshot.navigation[1],
                                                      snapshot.navigation[2]};
    center_ = {snapshot.center[0], snapshot.center[1], snapshot.center[2]};
    radius_ = std::max(snapshot.radius, 0.001F);
    projection_ = snapshot.projection;
    preserve_camera_position_ = false;
}

void GeometryPreview::draw_scene_tools(const std::string_view collision_status) {
    const bool has_collision = collision_sector_count_ != 0;
    const bool has_visual =
        std::any_of(draw_batches_.begin(), draw_batches_.end(), [](const DrawBatch& batch) {
            return batch.layer != PreviewLayer::collision_world;
        });
    const bool selected_actor_available =
        selected_mission_entry_ &&
        std::ranges::any_of(draw_batches_, [&](const auto& batch) {
            return actor_owner(batch.owner_offset) == selected_mission_entry_;
        });

    ImGui::SeparatorText("Rendering");
    ImGui::BeginDisabled(!has_visual);
    ImGui::Checkbox("Show visual scene", &show_visual_);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!has_collision);
    ImGui::Checkbox("Show level collision", &show_collision_);
    ImGui::EndDisabled();
    ImGui::SeparatorText("Actor focus");
    ImGui::BeginDisabled(!selected_actor_available);
    if (ImGui::Button("Frame selected actor", {-1.0F, 0.0F})) frame_selection(std::nullopt);
    ImGui::Checkbox("Isolate selected actor", &isolate_selected_actor_);
    ImGui::Checkbox("Dim unselected scene", &dim_unselected_actors_);
    ImGui::Checkbox("Outline selected actor", &outline_selected_actor_);
    if (selected_mission_entry_) {
        const auto hand = actor_hand_poses_.find(*selected_mission_entry_);
        if (hand != actor_hand_poses_.end()) {
            if (hand->second.followed)
                ImGui::TextWrapped("Weapon follows %s", hand->second.frame_label.c_str());
            else if (!hand->second.frame_label.empty())
                ImGui::TextWrapped("Weapon uses authored pose; %s is ready when an animation is active",
                                   hand->second.frame_label.c_str());
            else
                ImGui::TextWrapped("Weapon uses authored pose; no matching hand frame was found");
        }
    }
    ImGui::EndDisabled();
    if (view_style_ != 7) ImGui::Checkbox("Wire overlay", &wireframe_);
    ImGui::Checkbox("Cull backfaces", &cull_backfaces_);
    ImGui::TextDisabled("Move speed");
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::SliderFloat("##move_speed", &navigation_speed_, 0.05F, 4.0F, "%.2fx",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::TextDisabled("Camera distance %.1f", distance_);

    if (view_style_ == 5 || view_style_ == 6) {
        ImGui::TextDisabled("Lightmap intensity");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::SliderFloat("##lightmap_intensity", &lightmap_intensity_, 0.25F, 4.0F);
    }
    const auto maximum_texture_variant = texture_catalog_.maximum_variant();
    if (maximum_texture_variant != 0) {
        const auto variant_label = texture_variant_ == 0
                                       ? std::string("Base")
                                       : "Alt " + [&] {
                                             std::ostringstream value;
                                             value << std::setw(3) << std::setfill('0')
                                                   << texture_variant_;
                                             return value.str();
                                         }();
        ImGui::TextDisabled("TXL texture variant");
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::BeginCombo("##texture_variant", variant_label.c_str())) {
            for (std::uint32_t variant = 0; variant <= maximum_texture_variant; ++variant) {
                const auto label = variant == 0
                                       ? std::string("Base")
                                       : "Alt " + [&] {
                                             std::ostringstream value;
                                             value << std::setw(3) << std::setfill('0') << variant;
                                             return value.str();
                                         }();
                if (ImGui::Selectable(label.c_str(), texture_variant_ == variant)) {
                    texture_variant_ = variant;
                    scene_mode_ = false;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Falls back to Base for textures without this alternative");
    }
    ImGui::Text("Textures: %zu loaded", loaded_texture_count_);
    ImGui::Text("Unresolved slots: %zu", missing_texture_count_);
    if (!texture_status_.empty()) {
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextColored(ui::color(ui::Token::warn), "%s", texture_status_.c_str());
        ImGui::PopTextWrapPos();
    }
    if (!texture_diagnostics_.empty() && ImGui::TreeNode("Texture diagnostics")) {
        for (const auto& diagnostic : texture_diagnostics_)
            ImGui::BulletText("%s", diagnostic.c_str());
        ImGui::TreePop();
    }

    ImGui::SeparatorText("Collision");
    if (has_collision) {
        ImGui::TextWrapped("%s", collision_source_path_.filename().string().c_str());
        ImGui::Text("%zu/%d sectors | %zu triangles", collision_sector_count_,
                    collision_declared_sector_count_, collision_triangle_count_);
        ImGui::Text("%zu materials | %s", collision_material_count_,
                    rws::world_recovery_status_name(collision_recovery_status_));
        constexpr const char* collision_styles[] = {"Solid", "Solid + wire", "Wireframe", "X-ray"};
        constexpr const char* collision_colors[] = {"Surface", "Material index", "Single color"};
        ImGui::TextDisabled("Style");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::Combo("##collision_style", &collision_style_, collision_styles,
                     static_cast<int>(std::size(collision_styles)));
        ImGui::TextDisabled("Color by");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::Combo("##collision_color", &collision_color_mode_, collision_colors,
                     static_cast<int>(std::size(collision_colors)));
        ImGui::TextDisabled("Opacity");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::SliderFloat("##collision_opacity", &collision_opacity_, 0.05F, 1.0F, "%.2f");
        if (!collision_diagnostics_.empty()) {
            ImGui::PushTextWrapPos(0.0F);
            for (const auto& diagnostic : collision_diagnostics_)
                ImGui::TextColored(ui::color(ui::Token::warn), "%s", diagnostic.c_str());
            ImGui::PopTextWrapPos();
        }
        if (ImGui::TreeNode("Surface labels")) {
            for (std::size_t world_index = 0; world_index < collision_surface_labels_.size();
                 ++world_index) {
                const auto& labels = collision_surface_labels_[world_index];
                ImGui::PushID(static_cast<int>(world_index));
                if (collision_surface_labels_.size() == 1 ||
                    ImGui::TreeNode("world_surfaces", "World %zu", world_index)) {
                    for (std::size_t slot = 0; slot < labels.size(); ++slot)
                        ImGui::TextWrapped("%zu: %s", slot, labels[slot].c_str());
                    if (collision_surface_labels_.size() != 1) ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    } else {
        ImGui::TextDisabled("%.*s", static_cast<int>(collision_status.size()),
                            collision_status.data());
    }

    ImGui::SeparatorText("Inspection");
    ImGui::BeginDisabled(!has_collision);
    ImGui::Checkbox("Prefer collision on click", &prefer_collision_);
    ImGui::Checkbox("Measure two points", &measurement_mode_);
    if (ImGui::Button("Clear measurement")) {
        measurement_a_.reset();
        measurement_b_.reset();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!measurement_a_ || !measurement_b_);
    if (ImGui::Button("Swap endpoints")) std::swap(measurement_a_, measurement_b_);
    ImGui::EndDisabled();
    if (measurement_a_ && measurement_b_) {
        const auto measurement = rws::measure_points(*measurement_a_, *measurement_b_);
        ImGui::TextWrapped("Distance %.6g RWS units", measurement.distance);
        ImGui::Text("|dX| %.6g", measurement.absolute_delta.x);
        ImGui::Text("|dY| %.6g", measurement.absolute_delta.y);
        ImGui::Text("|dZ| %.6g", measurement.absolute_delta.z);
    }
    const char* axes = "XYZ";
    for (std::size_t i = 0; i < clips_.size(); ++i) {
        auto& clip = clips_[i];
        ImGui::PushID(static_cast<int>(i));
        const std::string label = std::string("Clip ") + axes[i];
        ImGui::Checkbox(label.c_str(), &clip.enabled);
        if (clip.enabled) {
            ImGui::SameLine();
            ImGui::Checkbox("Keep greater", &clip.keep_greater);
            ImGui::TextDisabled("Position");
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::DragFloat("##clip_position", &clip.position, 0.5F);
        }
        ImGui::PopID();
    }
    ImGui::Checkbox("Leaf bounds", &show_leaf_bounds_);
    ImGui::Checkbox("Selected BSP path", &show_bsp_path_);
    ImGui::EndDisabled();

    if (selected_collision_ && collision_document_ &&
        selected_collision_->world_index < collision_worlds_.size()) {
        const auto& hit = *selected_collision_;
        const auto& world = collision_worlds_[hit.world_index];
        const auto triangle = rws::decode_recovered_world_triangle_resolved(
            world, hit.sector_index, hit.triangle_index, collision_document_->bytes());
        ImGui::SeparatorText("Selected collision");
        ImGui::Text("World %zu @ 0x%llX", hit.world_index,
                    static_cast<unsigned long long>(world.world_offset));
        ImGui::Text("Sector %zu @ 0x%llX", hit.sector_index,
                    static_cast<unsigned long long>(hit.sector_offset));
        ImGui::Text("Triangle %d @ 0x%llX", hit.triangle_index,
                    static_cast<unsigned long long>(hit.triangle_offset));
        ImGui::Text("Material %d", hit.material_slot);
        if (hit.material_slot >= 0 && hit.world_index < collision_surface_labels_.size() &&
            static_cast<std::size_t>(hit.material_slot) <
                collision_surface_labels_[hit.world_index].size())
            ImGui::TextWrapped(
                "Surface %s",
                collision_surface_labels_[hit.world_index][hit.material_slot].c_str());
        ImGui::Text("Position %.6g, %.6g, %.6g", hit.position.x, hit.position.y, hit.position.z);
        ImGui::Text("Normal %.6g, %.6g, %.6g", hit.geometric_normal.x, hit.geometric_normal.y,
                    hit.geometric_normal.z);
        ImGui::Text("Barycentric %.4g, %.4g, %.4g", hit.barycentric[0], hit.barycentric[1],
                    hit.barycentric[2]);
        if (!rws::collision_point_visible(hit.position, clips_))
            ImGui::TextColored(ui::color(ui::Token::warn),
                               "Selection is outside the clip region");
        if (triangle && ImGui::TreeNode("Vertices")) {
            for (std::size_t i = 0; i < 3; ++i)
                ImGui::Text("%zu: %.6g, %.6g, %.6g", i, triangle.value->vertices[i].x,
                            triangle.value->vertices[i].y, triangle.value->vertices[i].z);
            ImGui::TreePop();
        }
        if (ImGui::Button("Frame sector")) {
            const auto& sector = world.sectors[hit.sector_index];
            const rws::Vec3 center{(sector.bounding_box_inf.x + sector.bounding_box_sup.x) * 0.5F,
                                   (sector.bounding_box_inf.y + sector.bounding_box_sup.y) * 0.5F,
                                   (sector.bounding_box_inf.z + sector.bounding_box_sup.z) * 0.5F};
            const auto dx = sector.bounding_box_sup.x - sector.bounding_box_inf.x;
            const auto dy = sector.bounding_box_sup.y - sector.bounding_box_inf.y;
            const auto dz = sector.bounding_box_sup.z - sector.bounding_box_inf.z;
            frame_bounds(center, std::max(0.5F * std::sqrt(dx * dx + dy * dy + dz * dz), 0.001F),
                         rws::Vec3{dx * 0.5F, dy * 0.5F, dz * 0.5F});
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy coordinate")) {
            char coordinate[128]{};
            std::snprintf(coordinate, sizeof(coordinate), "%.9g, %.9g, %.9g", hit.position.x,
                          hit.position.y, hit.position.z);
            ImGui::SetClipboardText(coordinate);
        }
        ImGui::Text("BSP %s | planes %zu/%d | depth %zu",
                    rws::world_topology_status_name(world.topology_status), world.planes.size(),
                    world.header.plane_sector_count, world.topology_stats.maximum_depth);
    }
}

} // namespace rwsman
