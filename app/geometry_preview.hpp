#pragma once

#include "rws/document.hpp"
#include "rws/world_recovery.hpp"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rwsman {

class GeometryPreview {
public:
    enum class MissionOverlayKind : std::uint8_t { actor, navigation_point, navigation_connection, dummy, area, light, count };
    struct MissionOverlayPoint {
        MissionOverlayKind kind{MissionOverlayKind::actor};
        std::uint32_t source_entry{};
        rws::Vec3 position{};
        std::string label;
        ImU32 color{};
    };
    struct MissionOverlayLine {
        MissionOverlayKind kind{MissionOverlayKind::navigation_connection};
        std::uint32_t source_entry{};
        rws::Vec3 first{}, second{};
        ImU32 color{};
    };
    void clear();
    void set_mission_overlays(std::vector<MissionOverlayPoint> points,
                              std::vector<MissionOverlayLine> lines);
    [[nodiscard]] std::optional<std::uint32_t> selected_mission_entry() const noexcept {
        return selected_mission_entry_;
    }
    void select_mission_entry(const std::uint32_t entry) noexcept { selected_mission_entry_ = entry; }
    void draw(const rws::Chunk& geometry_chunk, std::span<const std::byte> bytes,
              const std::filesystem::path& source_path);
    [[nodiscard]] bool draw_scene(const std::vector<rws::Chunk>& chunks,
                                  std::span<const std::byte> bytes,
                                  std::span<const rws::SceneInstance> instances,
                                  const std::filesystem::path& source_path,
                                  std::optional<std::uint64_t>& selected_chunk,
                                  const rws::Document* collision_document = nullptr,
                                  bool main_is_collision = false,
                                  std::string_view collision_status = {});
    void draw_scene_tools(std::string_view collision_status);

private:
    struct Face {
        std::uint32_t a{}, b{}, c{};
        std::uint16_t material{};
    };
    struct Uv { float u{}, v{}; };
    enum class PreviewLayer : std::uint8_t { visual_clump, visual_world, collision_world };
    struct GpuVertex {
        float x{}, y{}, z{};
        float base_u{}, base_v{}, lightmap_u{}, lightmap_v{}, debug_u{}, debug_v{};
        float nx{}, ny{}, nz{};
        std::uint32_t source_index{};
    };
    struct DrawBatch {
        std::uint16_t material{};
        std::uint32_t first{}, count{};
        std::uint64_t owner_offset{};
        bool force_opaque{};
        PreviewLayer layer{PreviewLayer::visual_clump};
        std::size_t world_index{}, sector_index{};
    };
    struct RenderedCollisionTriangle { std::uint32_t first{}; std::size_t world{}, sector{}; std::int32_t triangle{}; };

    bool load(const rws::Chunk& geometry_chunk, std::span<const std::byte> bytes,
              const std::filesystem::path& source_path);
    bool load_scene(const std::vector<rws::Chunk>& chunks, std::span<const std::byte> bytes,
                    std::span<const rws::SceneInstance> instances,
                    const std::filesystem::path& source_path,
                    const rws::Document* collision_document = nullptr,
                    bool main_is_collision = false);
    void select_uv_set(std::size_t index);
    void reset_view();
    void frame_bounds(rws::Vec3 center, float radius);
    void pan_camera(float delta_x, float delta_y);
    void update_keyboard_navigation();
    [[nodiscard]] rws::Vec3 camera_offset(float yaw, float pitch) const;
    [[nodiscard]] std::optional<std::uint64_t> pick_scene(float mouse_x, float mouse_y) const;
    [[nodiscard]] std::optional<rws::CollisionRay> viewport_ray(float mouse_x, float mouse_y) const;
    [[nodiscard]] std::optional<rws::CollisionHit> pick_collision(float mouse_x, float mouse_y) const;
    [[nodiscard]] std::optional<ImVec2> project_point(rws::Vec3 point) const;
    static void render_callback(const ImDrawList*, const ImDrawCmd* command);
    void render_gpu();
    bool create_gpu_resources();
    void destroy_gpu_resources();

    std::uint64_t chunk_offset_{~std::uint64_t{}};
    bool scene_mode_{};
    std::size_t scene_clump_count_{}, scene_instance_count_{}, scene_world_sector_count_{},
        scene_world_triangle_count_{}, scene_skipped_count_{}, scene_custom_instance_count_{},
        scene_unresolved_instance_count_{};
    std::size_t collision_sector_count_{}, collision_triangle_count_{}, collision_material_count_{};
    std::int32_t collision_declared_sector_count_{};
    rws::WorldRecoveryStatus collision_recovery_status_{rws::WorldRecoveryStatus::failed};
    std::vector<std::string> collision_diagnostics_;
    std::vector<std::vector<std::string>> collision_surface_labels_;
    std::filesystem::path collision_source_path_;
    std::vector<rws::Vec3> vertices_;
    std::vector<std::vector<Uv>> uv_sets_;
    std::vector<Face> faces_;
    std::vector<GpuVertex> gpu_vertices_;
    std::vector<DrawBatch> draw_batches_;
    std::vector<RenderedCollisionTriangle> collision_triangle_mapping_;
    std::vector<rws::RecoveredWorld> collision_worlds_;
    const rws::Document* collision_document_{};
    std::optional<rws::CollisionHit> selected_collision_;
    std::optional<rws::Vec3> measurement_a_, measurement_b_;
    std::array<rws::CollisionClipPlane, 3> clips_{{{false, 0, true, 0}, {false, 1, true, 0}, {false, 2, true, 0}}};
    std::vector<std::array<std::uint8_t, 4>> material_colors_;
    std::vector<unsigned int> material_textures_;
    std::vector<unsigned int> material_lightmap_textures_;
    std::vector<std::string> material_texture_names_;
    std::vector<std::string> material_lightmap_texture_names_;
    std::vector<unsigned int> owned_texture_ids_;
    unsigned int checker_texture_{};
    unsigned int vertex_array_{}, vertex_buffer_{}, shader_program_{};
    std::size_t visual_material_slot_count_{};
    std::size_t loaded_texture_count_{}, missing_texture_count_{};
    std::string texture_status_;
    rws::Vec3 center_{};
    float radius_{1.0F};
    rws::Vec3 visual_center_{}, collision_center_{};
    float visual_radius_{1.0F}, collision_radius_{1.0F};
    rws::Vec3 all_center_{};
    float all_radius_{1.0F};
    float yaw_{-0.65F};
    float pitch_{-0.35F};
    float target_yaw_{-0.65F};
    float target_pitch_{-0.35F};
    float distance_{3.0F};
    float orthographic_scale_{1.0F};
    float pan_x_{}, pan_y_{};
    rws::Vec3 navigation_offset_{};
    rws::Vec3 target_navigation_offset_{};
    bool preserve_camera_position_{};
    float navigation_speed_{1.0F};
    float lightmap_intensity_{2.0F};
    float canvas_x_{}, canvas_y_{}, canvas_width_{}, canvas_height_{};
    int view_style_{};
    std::size_t selected_uv_set_{};
    bool wireframe_{true};
    bool cull_backfaces_{};
    bool show_visual_{true};
    bool show_collision_{true};
    int collision_style_{};
    int collision_color_mode_{};
    float collision_opacity_{0.35F};
    int projection_{};
    bool prefer_collision_{true};
    bool measurement_mode_{};
    bool show_leaf_bounds_{}, show_bsp_path_{};
    std::string error_;
    std::vector<MissionOverlayPoint> mission_points_;
    std::vector<MissionOverlayLine> mission_lines_;
    std::array<bool, static_cast<std::size_t>(MissionOverlayKind::count)> mission_layer_visible_{{true,true,true,true,true,true}};
    std::optional<std::uint32_t> selected_mission_entry_;
};

} // namespace rwsman
