#pragma once

#include "csf/mission.hpp"
#include "rws/animation.hpp"
#include "rws/document.hpp"
#include "rws/world_recovery.hpp"
#include "rwsman/history.hpp"
#include "rwsman/viewport_overlays.hpp"

#include <imgui.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rwsman {

class GeometryPreview {
public:
    enum class MissionOverlayKind : std::uint8_t {
        actor,
        navigation_point,
        navigation_connection,
        dummy,
        cutscene_camera,
        area,
        light,
        effect,
        actor_cmo,
        actor_physics,
        count
    };
    struct MissionOverlayPoint {
        MissionOverlayKind kind{MissionOverlayKind::actor};
        std::uint32_t source_entry{};
        rws::Vec3 position{};
        std::string label;
        ImU32 color{};
        // Finer layer within the kind (a nav group, an actor faction), or empty.
        std::string sublayer;
        std::optional<rws::Vec3> heading; // Unit facing direction.
        float radius{};                   // Light range in world units, or 0.
    };
    struct MissionOverlayLine {
        MissionOverlayKind kind{MissionOverlayKind::navigation_connection};
        std::uint32_t source_entry{};
        rws::Vec3 first{}, second{};
        ImU32 color{};
        bool directed{};
        // The reverse navigation link folded into this one, drawn without arrows.
        std::optional<std::uint32_t> partner_entry;
        // Shown only per the overlay detail mode (cutscene camera frusta).
        bool detail{};
        std::string sublayer;
    };
    struct MissionOverlayFace {
        MissionOverlayKind kind{MissionOverlayKind::area};
        std::uint32_t source_entry{};
        rws::Vec3 a{}, b{}, c{};
        ImU32 color{};
    };
    struct MissionOverlaySet {
        std::vector<MissionOverlayPoint> points;
        std::vector<MissionOverlayLine> lines;
        std::vector<MissionOverlayFace> faces;
        // Undirected links between entries, for focus mode (actor and its spawn
        // point, effect and its dummy, a link and its points, an area and its contents).
        std::vector<std::pair<std::uint32_t, std::uint32_t>> relations;
    };
    struct MissionActorModel {
        struct Attachment {
            std::shared_ptr<const rws::Document> model;
            std::string label;
            bool left_hand{};
        };
        std::uint32_t source_entry{};
        std::shared_ptr<const rws::Document> prototype;
        rws::Vec3 position{};
        float heading_radians{}, pitch_radians{};
        std::vector<Attachment> attachments;
        std::shared_ptr<const rws::AnimationClip> animation;
        float animation_time{};
        bool animation_loop{true};
    };
    void clear();
    void set_mission_overlays(MissionOverlaySet overlays);
    void set_mission_actor_models(std::vector<MissionActorModel> models);
    // Replaces overlays and actor models after a mission edit without moving
    // the camera. Models that differ only in placement are moved in place;
    // anything else rebuilds the scene. `selected` is the entry to keep selected.
    void update_mission(MissionOverlaySet overlays, std::vector<MissionActorModel> models,
                        std::optional<std::uint32_t> selected);

    // ---- Viewport editing (geometry_preview_editing.cpp) ----
    enum class EditTool : std::uint8_t { select, move, rotate };
    // What the gizmo manipulates: the selected record's pivot and heading.
    struct EditHandle {
        std::uint32_t source_entry{};
        rws::Vec3 position{};
        float heading_radians{};
        bool rotatable{};
        // A static map prop (scene instance at this offset) instead of a
        // mission record; `rotation` is its RwMatrix basis at the drag start.
        std::optional<std::uint64_t> map_instance;
        std::array<float, 9> rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
    };
    struct EditDrag {
        enum class Phase : std::uint8_t { active, finished, cancelled };
        Phase phase{Phase::active};
        std::uint32_t source_entry{};
        rws::Vec3 position{};
        float heading_radians{};
        std::optional<std::uint64_t> map_instance;
        std::array<float, 9> rotation{1, 0, 0, 0, 1, 0, 0, 0, 1}; // At the drag start.
        float start_heading_radians{};
    };
    // The rotation a finished map-instance drag asks for.
    [[nodiscard]] static std::array<float, 9> instance_rotation_for(const EditDrag& drag);
    // Moves a map scene instance without rebuilding the scene.
    void place_scene_instance(std::uint64_t offset, const std::array<float, 9>& rotation,
                              rws::Vec3 position);
    void set_edit_tool(EditTool tool) noexcept { edit_tool_ = tool; }
    [[nodiscard]] EditTool edit_tool() const noexcept { return edit_tool_; }
    // Set every frame by the app; nullopt hides the gizmo.
    void set_edit_handle(std::optional<EditHandle> handle);
    // The drag in progress (each frame while active), then its end once.
    [[nodiscard]] std::optional<EditDrag> take_edit_drag();
    [[nodiscard]] bool edit_drag_active() const noexcept { return edit_drag_.has_value(); }
    // Snap moves to the collision surface under the pointer.
    void set_snap_to_surface(const bool value) noexcept { snap_to_surface_ = value; }
    [[nodiscard]] bool snap_to_surface() const noexcept { return snap_to_surface_; }
    // Collision surface point under a screen position, if any.
    [[nodiscard]] std::optional<rws::Vec3> surface_point(ImVec2 screen) const;
    // World point in front of the camera at the view center (placement target).
    [[nodiscard]] rws::Vec3 view_target() const;
    void set_texture_catalog(csf::TextureCatalog catalog);
    [[nodiscard]] bool set_mission_actor_animation(std::uint32_t source_entry,
                                                   std::shared_ptr<const rws::AnimationClip> clip,
                                                   float time, bool loop);
    [[nodiscard]] std::optional<std::uint32_t> selected_mission_entry() const noexcept {
        return selected_mission_entry_;
    }
    void select_mission_entry(const std::uint32_t entry) noexcept {
        selected_mission_entry_ = entry;
    }
    void clear_mission_selection() noexcept { selected_mission_entry_.reset(); }
    // True while the pointer is over the 3D canvas (bare-key viewport shortcuts).
    [[nodiscard]] bool viewport_hovered() const noexcept { return canvas_hovered_; }
    [[nodiscard]] bool isolate_selected_actor() const noexcept { return isolate_selected_actor_; }
    void set_isolate_selected_actor(const bool value) noexcept { isolate_selected_actor_ = value; }
    // Mission overlay display options; the shell mirrors them into settings.ini.
    [[nodiscard]] const OverlayOptions& overlay_options() const noexcept { return overlay_options_; }
    void set_overlay_options(OverlayOptions options);
    // Focus mode dims every marker unrelated to the selection.
    [[nodiscard]] bool focus_mode() const noexcept { return focus_mode_; }
    void set_focus_mode(const bool value) noexcept { focus_mode_ = value; }
    // Height slice: overlays (and, optionally, geometry) outside a Y band are hidden.
    [[nodiscard]] bool slice_enabled() const noexcept { return slice_.enabled; }
    // Enables a band around the selected marker or the camera target, or disables it.
    void toggle_floor_slice();
    void cycle_label_mode() noexcept;
    void toggle_minimap() noexcept { overlay_options_.show_minimap = !overlay_options_.show_minimap; }
    // Moves keyboard focus to the overlay filter field on the next frame.
    void focus_overlay_filter() noexcept { focus_filter_request_ = true; }
    [[nodiscard]] bool has_mission_overlays() const noexcept {
        return !mission_points_.empty() || !mission_lines_.empty();
    }
    void frame_all();
    // Projection: 0 perspective, 1 top (X/Z), 2 front (X/Y), 3 side (Z/Y).
    void set_projection(int projection) noexcept;
    [[nodiscard]] int projection() const noexcept { return projection_; }
    // Numpad 5: switch between perspective and the last orthographic view.
    void toggle_perspective() noexcept;
    void set_show_hud(const bool value) noexcept { show_hud_ = value; }
    void set_show_frame_stats(const bool value) noexcept { show_frame_stats_ = value; }
    // The frame loop's measurements, for the stats HUD.
    void set_frame_timing(const double fps, const double cpu_ms) noexcept {
        frame_fps_ = fps;
        frame_cpu_ms_ = cpu_ms;
    }
    // True when the latest frame left something in motion (camera easing, held
    // movement keys, marker occlusion still being counted): draw another frame.
    [[nodiscard]] bool animating() const noexcept {
        return animating_frame_ == ImGui::GetFrameCount();
    }
    [[nodiscard]] bool show_hud() const noexcept { return show_hud_; }
    void set_view_style(const int style) noexcept { view_style_ = style; }
    void set_navigation_speed(const float speed) noexcept { navigation_speed_ = speed; }
    void set_invert_y(const bool value) noexcept { invert_y_ = value; }
    // True once after the toolbar's screenshot button is pressed.
    [[nodiscard]] bool take_screenshot_request() noexcept {
        return std::exchange(screenshot_requested_, false);
    }
    // Frames the selected mission record, or the scene chunk at `chunk_offset`
    // when no mission record is selected. Returns whether anything was framed.
    bool frame_selection(std::optional<std::uint64_t> chunk_offset);
    [[nodiscard]] rwsman::CameraSnapshot camera() const;
    void set_camera(const rwsman::CameraSnapshot& snapshot);
    // The scene camera's eye and the point it orbits (game units), and a
    // camera placed at `eye` looking at `target` (cutscene shots).
    [[nodiscard]] rws::Vec3 eye_position() const;
    [[nodiscard]] rws::Vec3 orbit_target() const;
    void look_from(rws::Vec3 eye, rws::Vec3 target);
    void set_mission_entries_visible(std::span<const std::uint32_t> entries, bool visible);
    [[nodiscard]] bool mission_entry_visible(std::uint32_t entry) const noexcept;
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
    struct Uv {
        float u{}, v{};
    };
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
        bool actor_attachment{};
        PreviewLayer layer{PreviewLayer::visual_clump};
        std::size_t world_index{}, sector_index{};
        std::array<float, 12> transform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    };
    // A world-space sphere around a batch's transformed vertices; radius < 0
    // when there is nothing to bound (never culled).
    struct BatchBounds {
        rws::Vec3 center{};
        float radius{-1.0F};
    };
    // A visible batch for this frame, with the textures it samples.
    struct PassDraw {
        std::uint32_t batch{};
        unsigned int texture{}, lightmap{};
    };
    // Scene shader uniform locations, looked up once after linking.
    struct SceneUniforms {
        int center{-1}, yaw{-1}, pitch{-1}, distance{-1}, orthographic_scale{-1}, pan{-1};
        int aspect{-1}, tan_half_fov{-1}, near_plane{-1}, far_plane{-1}, y_up{-1}, orthographic{-1};
        std::array<int, 3> model{{-1, -1, -1}};
        int screen_offset{-1}, viewport_pixels{-1};
        int texture{-1}, lightmap_texture{-1}, use_texture{-1}, use_lightmap{-1}, lightmap_only{-1};
        int use_debug_uv{-1}, apply_lighting{-1}, force_opaque{-1}, lightmap_intensity{-1};
        int dim{-1}, base_color{-1}, slice{-1};
        std::array<int, 3> clip{{-1, -1, -1}};
    };
    struct RenderedCollisionTriangle {
        std::uint32_t first{};
        std::size_t world{}, sector{};
        std::int32_t triangle{};
    };
    struct SkeletonLine {
        rws::Vec3 parent{}, child{};
        std::int32_t frame{}, node_id{-1};
    };
    struct PhysicsLine {
        rws::Vec3 first{}, second{};
        ImU32 color{};
    };
    struct AnimatedActorRange {
        std::uint32_t source_entry{};
        std::size_t vertex_begin{};
        std::size_t vertex_count{};
    };
    struct ActorGeometryMaterials {
        std::uint64_t geometry_offset{};
        std::vector<std::uint16_t> slots;
    };
    struct ActorPrototypeMaterials {
        const rws::Document* prototype{};
        std::vector<ActorGeometryMaterials> geometries;
    };
    struct ActorHandPose {
        std::array<float, 12> transform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        std::string frame_label;
        bool followed{};
    };

    bool load(const rws::Chunk& geometry_chunk, std::span<const std::byte> bytes,
              const std::filesystem::path& source_path);
    bool load_scene(const std::vector<rws::Chunk>& chunks, std::span<const std::byte> bytes,
                    std::span<const rws::SceneInstance> instances,
                    const std::filesystem::path& source_path,
                    const rws::Document* collision_document = nullptr,
                    bool main_is_collision = false);
    void refresh_mission_actor_animation();
    void select_uv_set(std::size_t index);
    void reset_view();
    // Frames a sphere, or, when `half_extent` is given, the box it bounds (a
    // tighter fit for flat levels seen from above).
    void frame_bounds(rws::Vec3 center, float radius,
                      std::optional<rws::Vec3> half_extent = std::nullopt);
    void pan_camera(float delta_x, float delta_y);
    void update_keyboard_navigation();
    [[nodiscard]] rws::Vec3 camera_offset(float yaw, float pitch) const;
    [[nodiscard]] std::optional<std::uint64_t> pick_scene(float mouse_x, float mouse_y) const;
    [[nodiscard]] std::optional<rws::CollisionRay> viewport_ray(float mouse_x, float mouse_y) const;
    [[nodiscard]] std::optional<rws::CollisionHit> pick_collision(float mouse_x,
                                                                  float mouse_y) const;
    [[nodiscard]] std::optional<ImVec2> project_point(rws::Vec3 point) const;
    static void render_callback(const ImDrawList*, const ImDrawCmd* command);
    void render_gpu();
    void render_scene_gpu();
    bool draw_viewport_toolbar(ImVec2 origin, ImVec2 size, bool has_visual, bool has_collision);
    void draw_viewport_hud(ImDrawList* draw_list, ImVec2 origin, ImVec2 size,
                           std::string_view collision_status);
    void draw_axis_gizmo(ImDrawList* draw_list, ImVec2 origin, ImVec2 size);
    // Returns the projection to switch to when `mouse` is over an axis of the
    // gizmo (0 = center: back to perspective), or -1.
    [[nodiscard]] int axis_gizmo_hit(ImVec2 origin, ImVec2 size, ImVec2 mouse) const;
    void draw_overlay_hover_tooltip();
    // Moves an actor model (and its attachments) without rebuilding the scene.
    void place_mission_actor(std::uint32_t source_entry, rws::Vec3 position, float heading_radians,
                             float pitch_radians);
    // Moves the overlay markers of one entry during a drag.
    void move_mission_markers(std::uint32_t source_entry, rws::Vec3 position, float heading_radians);
    // Starts, continues or ends a gizmo drag; returns true when it owns the mouse.
    bool update_edit_gizmo();
    void draw_edit_gizmo(ImDrawList* draw_list);
    // ---- Mission overlays (geometry_preview_overlays.cpp) ----
    struct OverlayVertex {
        float a[3];       // Anchor (world).
        float b[3];       // Second endpoint, or a direction for heading ticks.
        float corner[2];  // Quad corner or line end/side.
        float size[2];    // Pixel radius / thickness / world radius; heading length.
        std::uint32_t color;
        float params[4];  // Mode, shape, occluded-opacity floor, unused.
    };
    struct ProjectedMarker {
        std::uint32_t point{}; // Index into mission_points_.
        ImVec2 screen{};
        float depth{};         // View-space distance along the view axis.
        bool pinned{};
    };
    struct OverlayCluster {
        ImVec2 screen{};
        std::vector<std::uint32_t> markers; // Indices into overlay_markers_.
        std::uint32_t representative{};      // The member shown for the group.
    };
    struct OverlaySlice {
        bool enabled{};
        float low{}, high{};
        bool clip_geometry{true};
    };
    enum class Emphasis : std::uint8_t { dimmed, normal, related, hovered, selected };
    void rebuild_overlay_indexes();
    [[nodiscard]] bool overlay_layer_visible(MissionOverlayKind kind, const std::string& sublayer) const;
    [[nodiscard]] bool overlay_entry_matches_filter(std::uint32_t entry) const;
    [[nodiscard]] bool overlay_position_visible(rws::Vec3 point) const;
    [[nodiscard]] Emphasis overlay_emphasis(std::uint32_t entry) const;
    [[nodiscard]] rwsman::MarkerFadeRange overlay_fade_range() const;
    [[nodiscard]] rws::Vec3 view_point(rws::Vec3 point) const;
    [[nodiscard]] std::vector<rws::CollisionClipPlane> active_clip_planes() const;
    // Walls between the camera and a marker; nullopt while not yet computed or
    // without collision. Counted on a per-frame time budget once the camera settles, cached per pose.
    [[nodiscard]] std::optional<int> overlay_point_walls(std::uint32_t point_index);
    [[nodiscard]] std::optional<bool> overlay_point_occluded(std::uint32_t point_index);
    [[nodiscard]] float overlay_occlusion_alpha(std::uint32_t point_index);
    [[nodiscard]] float overlay_eye_distance(rws::Vec3 point) const;
    void prepare_overlays(ImVec2 origin, ImVec2 size);
    void render_overlays_gpu(int viewport_width, int viewport_height);
    bool create_overlay_program();
    void destroy_overlay_program();
    void draw_overlay_decorations(ImDrawList* draw_list, ImVec2 origin, ImVec2 size);
    void draw_overlay_legend(ImDrawList* draw_list, ImVec2 origin, ImVec2 size);
    void draw_overlay_minimap(ImVec2 origin, ImVec2 size);
    void draw_overlay_picker();
    void draw_layers_popup(bool has_visual, bool has_collision);
    void draw_markers_popup();
    // Handles a left click on the canvas; returns true when an overlay consumed it.
    bool overlay_click(ImVec2 mouse);
    [[nodiscard]] std::vector<std::uint32_t> overlay_candidates(ImVec2 mouse) const;
    [[nodiscard]] std::optional<std::size_t> overlay_cluster_at(ImVec2 mouse) const;
    void draw_measure_panel(ImVec2 origin, ImVec2 size);
    bool create_gpu_resources();
    // Per-batch bounds, the material-grouped draw order, and triangle counts;
    // rebuilt whenever the batches or their vertices change.
    void build_render_cache();
    void update_batch_bounds(std::size_t batch_index);
    // Sine and cosine of the rendered yaw and pitch, cached while they are unchanged.
    struct ViewRotation {
        float cy{1.0F}, sy{}, cp{1.0F}, sp{};
    };
    [[nodiscard]] ViewRotation view_rotation() const;
    void read_gpu_timer();
    void release_geometry();
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
    std::vector<BatchBounds> batch_bounds_;      // Parallel to draw_batches_.
    std::vector<std::uint32_t> batch_order_;     // Collision last, grouped by material.
    std::size_t visual_triangle_total_{}, collision_triangle_total_{};
    // Per frame, reused to avoid allocations.
    std::vector<std::uint8_t> batch_in_view_;
    std::array<std::vector<PassDraw>, 3> pass_draws_; // Opaque, translucent, collision.
    std::vector<std::uint32_t> selected_batches_;
    SceneUniforms uniforms_;
    mutable struct {
        float yaw{std::numeric_limits<float>::quiet_NaN()}, pitch{};
        ViewRotation rotation;
    } view_rotation_cache_;
    // Frame statistics: draws submitted and culled by the latest render, and the
    // GPU time of the scene pass from timer queries (read a few frames late).
    std::size_t last_draw_calls_{}, last_culled_batches_{};
    std::array<unsigned int, 4> gpu_queries_{};
    std::array<bool, 4> gpu_query_pending_{};
    std::size_t gpu_query_next_{};
    double gpu_ms_{-1.0};
    bool show_frame_stats_{};
    double frame_fps_{}, frame_cpu_ms_{};
    int animating_frame_{-1}; // The ImGui frame that last left something in motion.
    std::vector<RenderedCollisionTriangle> collision_triangle_mapping_;
    std::vector<rws::RecoveredWorld> collision_worlds_;
    const rws::Document* collision_document_{};
    std::optional<rws::CollisionHit> selected_collision_;
    std::optional<rws::Vec3> measurement_a_, measurement_b_;
    std::array<rws::CollisionClipPlane, 3> clips_{
        {{false, 0, true, 0}, {false, 1, true, 0}, {false, 2, true, 0}}};
    std::vector<std::array<std::uint8_t, 4>> material_colors_;
    std::vector<unsigned int> material_textures_;
    std::vector<unsigned int> material_lightmap_textures_;
    std::vector<std::string> material_texture_names_;
    std::vector<std::string> material_lightmap_texture_names_;
    std::vector<unsigned int> owned_texture_ids_;
    std::unordered_set<unsigned int> translucent_texture_ids_;
    unsigned int checker_texture_{};
    unsigned int vertex_array_{}, vertex_buffer_{}, shader_program_{};
    std::size_t visual_material_slot_count_{};
    std::size_t loaded_texture_count_{}, missing_texture_count_{};
    std::string texture_status_;
    std::vector<std::string> texture_diagnostics_;
    csf::TextureCatalog texture_catalog_;
    std::uint32_t texture_variant_{};
    rws::Vec3 center_{};
    float radius_{1.0F};
    std::optional<rws::Vec3> frame_extent_; // Half extent of the framed box, if any.
    rws::Vec3 visual_center_{}, collision_center_{};
    float visual_radius_{1.0F}, collision_radius_{1.0F};
    std::optional<rws::Vec3> visual_extent_, collision_extent_;
    rws::Vec3 all_center_{};
    float all_radius_{1.0F};
    std::optional<rws::Vec3> all_extent_;
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
    bool preserve_view_on_scene_reload_{};
    float navigation_speed_{1.0F};
    float lightmap_intensity_{2.0F};
    float canvas_x_{}, canvas_y_{}, canvas_width_{}, canvas_height_{};
    bool canvas_hovered_{};
    bool show_hud_{true}, invert_y_{}, screenshot_requested_{}, gizmo_press_{};
    int last_orthographic_projection_{2};
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
    std::vector<MissionActorModel> mission_actor_models_;
    std::optional<std::uint32_t> selected_mission_entry_;
    std::unordered_set<std::uint32_t> hidden_mission_entries_;
    std::vector<SkeletonLine> skeleton_lines_;
    bool show_skeleton_{true}, show_skeleton_labels_{};
    bool isolate_selected_actor_{}, dim_unselected_actors_{true}, outline_selected_actor_{true};
    std::vector<PhysicsLine> physics_lines_;
    bool show_physics_{true};
    std::vector<AnimatedActorRange> animated_actor_ranges_;
    std::vector<ActorPrototypeMaterials> actor_material_layouts_;
    std::unordered_map<std::uint32_t, ActorHandPose> actor_hand_poses_;
    std::function<std::vector<DrawBatch>(const rws::Document&, const MissionActorModel&)>
        actor_geometry_builder_;
    bool animated_actor_dirty_{};

    EditTool edit_tool_{EditTool::select};
    std::optional<EditHandle> edit_handle_;
    // Per scene instance: its prototype's inverse root frame, as a draw matrix.
    std::unordered_map<std::uint64_t, std::array<float, 12>> instance_root_inverse_;
    struct ActiveDrag {
        enum class Mode : std::uint8_t { plane, axis_x, axis_y, axis_z, rotate };
        Mode mode{Mode::plane};
        EditHandle start;
        ImVec2 start_mouse{};
        rws::Vec3 plane_start{}; // Ray/plane hit at the press, for plane drags.
        float start_angle{};     // Pointer bearing at the press, for rotation.
        rws::Vec3 position{};
        float heading{};
        float pitch{};           // Kept for actor placement during the drag.
    };
    std::optional<ActiveDrag> edit_drag_;
    void apply_drag_pose(const ActiveDrag& drag);
    std::optional<EditDrag> edit_result_;
    bool snap_to_surface_{true};

    OverlayOptions overlay_options_;
    bool focus_mode_{};
    OverlaySlice slice_;
    std::array<char, 128> overlay_filter_{};
    bool focus_filter_request_{};
    std::set<std::pair<std::uint8_t, std::string>> hidden_sublayers_;
    std::vector<MissionOverlayFace> mission_faces_;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> mission_relations_;
    std::unordered_map<std::uint32_t, MissionOverlayKind> entry_kinds_;
    // Per kind: distinct entries, and (sublayer, entries) in name order.
    std::array<std::size_t, static_cast<std::size_t>(MissionOverlayKind::count)> layer_entry_counts_{};
    std::array<std::vector<std::pair<std::string, std::size_t>>,
               static_cast<std::size_t>(MissionOverlayKind::count)>
        layer_sublayers_;
    std::unordered_set<std::uint32_t> filter_matches_; // Valid while the filter is non-empty.
    std::string filter_matches_query_;
    // Per frame.
    std::vector<ProjectedMarker> overlay_markers_;
    std::vector<OverlayCluster> overlay_clusters_;
    struct ProjectedLine {
        std::uint32_t line{};
        ImVec2 a{}, b{};
    };
    std::vector<ProjectedLine> overlay_screen_lines_;
    std::unordered_set<std::uint64_t> overlay_counted_; // Kind and entry pairs counted this frame.
    std::array<std::size_t, static_cast<std::size_t>(MissionOverlayKind::count)> visible_layer_counts_{};
    std::optional<std::uint32_t> hovered_mission_entry_;
    std::optional<std::size_t> hovered_cluster_;
    std::optional<std::uint32_t> focus_source_entry_;
    std::unordered_set<std::uint32_t> focus_entries_;
    bool show_all_labels_{}; // Held key.
    std::vector<OverlayVertex> overlay_vertices_;
    unsigned int overlay_program_{}, overlay_vertex_array_{}, overlay_vertex_buffer_{};
    std::size_t overlay_buffer_capacity_{};
    bool overlay_program_failed_{};
    // Repeated clicks on one spot cycle through the markers under it.
    ImVec2 last_overlay_click_{-1e9F, -1e9F};
    std::size_t overlay_click_cycle_{};
    std::vector<std::uint32_t> overlay_picker_entries_;
    bool open_overlay_picker_{};
    std::optional<ImVec4> offscreen_indicator_rect_; // x0, y0, x1, y1 of last frame's arrow.
    // Cached walls between the camera and each mission point: -1 unknown.
    std::vector<std::int8_t> point_occlusion_;
    std::vector<std::uint32_t> point_occlusion_generation_;
    std::uint32_t occlusion_generation_{1};
    std::array<float, 12> occlusion_camera_key_{};
    // Wall counts pause while the camera moves and resume once it has settled,
    // each frame spending at most a fixed slice of time.
    std::chrono::steady_clock::time_point occlusion_camera_changed_{};
    std::chrono::steady_clock::time_point occlusion_deadline_{}; // Set by the frame's first count.
    bool occlusion_settled_{};
    bool occlusion_pending_{};          // Wall counts are stale or unfinished this frame.
    double occlusion_count_seconds_{};  // Running average cost of one wall count.
    float slice_height_{};                 // Band height for "current floor"; 0 until first use.
    rws::Vec3 overlay_bounds_min_{}, overlay_bounds_max_{};
    bool overlay_bounds_valid_{};
    static constexpr int minimap_cells = 48;
    std::vector<std::uint16_t> minimap_density_;
    std::array<char, 48> new_preset_name_{};
};

} // namespace rwsman
