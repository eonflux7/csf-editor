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
    void clear();
    void draw(const rws::Chunk& geometry_chunk, std::span<const std::byte> bytes,
              const std::filesystem::path& source_path);
    void draw_scene(const std::vector<rws::Chunk>& chunks, std::span<const std::byte> bytes,
                    std::span<const rws::SceneInstance> instances,
                    const std::filesystem::path& source_path,
                    std::optional<std::uint64_t>& selected_chunk,
                    const rws::Document* collision_document = nullptr,
                    bool main_is_collision = false,
                    std::string_view collision_status = {});

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
    };

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
    std::vector<std::string> collision_surface_labels_;
    std::filesystem::path collision_source_path_;
    std::vector<rws::Vec3> vertices_;
    std::vector<std::vector<Uv>> uv_sets_;
    std::vector<Face> faces_;
    std::vector<GpuVertex> gpu_vertices_;
    std::vector<DrawBatch> draw_batches_;
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
    std::string error_;
};

} // namespace rwsman
