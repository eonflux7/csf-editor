#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace csf {

enum class ResourceKind {
    mission_scene,
    mission_script,
    cutscene_script,
    security_data,
    database,
    visual_index,
    model_index,
    physics_index,
    animation_index,
    texture_index,
    visual_map,
    collision_map,
    render_model,
    physics_body,
    ragdoll,
    animation,
    collision_shape,
    texture,
    particle_system,
    audio_bank,
    spatial_data,
    unknown,
};

enum class DependencyKind {
    package_member,
    visual_map,
    collision_map,
    texture_directory,
    sky_model,
    render_model,
    collision_shape,
    physics_body,
    animation,
    texture,
    document_reference,
};

enum class ResolutionStatus {
    explicit_path,
    exact,
    case_mismatch,
    mapped_dff_to_rpc,
    shared_root,
    shared_root_case_mismatch,
    ambiguous,
    missing,
    outside_root,
};

enum class LoadState { metadata_only, available, missing, ambiguous, rejected };

struct SourceLocation {
    std::filesystem::path file;
    std::uint64_t offset{};
    std::uint64_t size{};
    std::optional<std::uint32_t> table_index;
    std::string adapter;
};

struct MissionDiagnostic {
    enum class Severity { note, warning, error };
    Severity severity{Severity::warning};
    std::string code;
    std::string message;
    std::optional<SourceLocation> source;
};

struct IndexedResource {
    std::size_t root_index{};
    std::filesystem::path root;
    std::filesystem::path path;
    std::filesystem::path relative_path;
    std::string normalized_key;
    std::uint64_t size{};
};

struct Resolution {
    ResolutionStatus status{ResolutionStatus::missing};
    std::string original_reference;
    std::string normalized_key;
    std::vector<std::size_t> candidate_indices;
};

class ResourceIndex {
public:
    void add_root(const std::filesystem::path& root);
    void build();
    // Serves `physical` for the package-relative path `relative` of the first
    // root: it replaces a file indexed there or is added as a new one, so every
    // resolution rule (exact, .dff to .rpc, logical) finds it. Kept across build().
    void add_overlay(const std::filesystem::path& relative, const std::filesystem::path& physical);

    [[nodiscard]] const std::vector<std::filesystem::path>& roots() const noexcept {
        return roots_;
    }
    [[nodiscard]] const std::vector<IndexedResource>& resources() const noexcept {
        return resources_;
    }
    [[nodiscard]] Resolution
    resolve(std::string_view reference,
            std::optional<std::size_t> preferred_root = std::nullopt) const;
    [[nodiscard]] std::vector<std::size_t> find_path(const std::filesystem::path& path) const;

    [[nodiscard]] static std::string normalize(std::string_view path);
    [[nodiscard]] static std::string normalize_path(const std::filesystem::path& path);

private:
    void insert(IndexedResource resource);
    void apply_overlay(const std::filesystem::path& relative, const std::filesystem::path& physical);

    std::vector<std::filesystem::path> roots_;
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> overlays_;
    std::vector<IndexedResource> resources_;
    std::unordered_map<std::string, std::vector<std::size_t>> normalized_;
    std::unordered_map<std::string, std::vector<std::size_t>> logical_;
};

struct ResourceNode {
    std::uint64_t id{};
    ResourceKind kind{ResourceKind::unknown};
    std::filesystem::path resolved_path;
    std::string original_reference;
    std::string normalized_key;
    std::uint64_t size{};
    LoadState state{LoadState::metadata_only};
    std::vector<std::filesystem::path> candidates;
    std::vector<MissionDiagnostic> diagnostics;
    std::optional<std::uint32_t> root_type;
    std::optional<std::uint64_t> content_hash;
};

struct DependencyEdge {
    std::uint64_t source{};
    std::optional<std::uint64_t> target;
    DependencyKind kind{DependencyKind::document_reference};
    SourceLocation evidence;
    ResolutionStatus status{ResolutionStatus::missing};
    std::string original_reference;
    std::string normalized_key;
    std::vector<std::filesystem::path> candidates;
};

struct AdapterReference {
    std::string path;
    DependencyKind kind{DependencyKind::document_reference};
    SourceLocation source;
};

struct AdapterResult {
    std::vector<AdapterReference> references;
    std::uint64_t unknown_tail_offset{};
    std::vector<std::byte> unknown_tail;
    std::vector<MissionDiagnostic> diagnostics;
};

struct TextureCatalogEntry {
    std::string reference;
    std::filesystem::path resolved_path;
    std::string family_key;
    std::uint32_t variant{};
    SourceLocation source;
};

// A mission TXL is a flat list of package-root-relative texture paths.  Material
// texture names are normally bare stems, so the catalog provides the missing
// path association and groups Foo_AltNNN images as variants of Foo.  Extension
// changes between a base texture and an alternative are intentional in shipped
// data and therefore do not affect family matching.
class TextureCatalog {
public:
    void add(const AdapterResult& txl, const ResourceIndex& resources,
             std::optional<std::size_t> preferred_root = std::nullopt);

    [[nodiscard]] const std::vector<TextureCatalogEntry>& entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] std::optional<std::filesystem::path>
    resolve(std::string_view texture_name, std::uint32_t variant = 0) const;
    [[nodiscard]] std::vector<std::uint32_t> variants(std::string_view texture_name) const;
    [[nodiscard]] std::uint32_t maximum_variant() const noexcept;

private:
    std::vector<TextureCatalogEntry> entries_;
};

[[nodiscard]] AdapterResult read_vis(const std::filesystem::path& path);
[[nodiscard]] AdapterResult read_txl(const std::filesystem::path& path);
// A texture list already in memory (an edited .txl); `path` labels its references.
[[nodiscard]] AdapterResult read_txl(std::span<const std::byte> bytes, const std::filesystem::path& path);
[[nodiscard]] AdapterResult read_m3d(const std::filesystem::path& path);
[[nodiscard]] AdapterResult read_and(const std::filesystem::path& path);
[[nodiscard]] AdapterResult read_phd_candidates(const std::filesystem::path& path);

struct MissionOptions {
    std::filesystem::path input;
    std::optional<std::filesystem::path> package_root;
    std::vector<std::filesystem::path> resource_roots;
    const ResourceIndex* prepared_index{};
    bool detect_duplicate_scenes{};
};

class MissionGraph {
public:
    [[nodiscard]] static MissionGraph load(const MissionOptions& options);

    [[nodiscard]] const std::filesystem::path& scene_path() const noexcept { return scene_path_; }
    [[nodiscard]] const std::filesystem::path& package_root() const noexcept {
        return package_root_;
    }
    [[nodiscard]] const ResourceIndex& index() const noexcept { return index_; }
    [[nodiscard]] const TextureCatalog& textures() const noexcept { return textures_; }
    [[nodiscard]] const std::vector<ResourceNode>& nodes() const noexcept { return nodes_; }
    [[nodiscard]] const std::vector<DependencyEdge>& edges() const noexcept { return edges_; }
    [[nodiscard]] const std::vector<MissionDiagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }
    [[nodiscard]] std::vector<const DependencyEdge*> uses(const std::filesystem::path& path) const;

private:
    std::filesystem::path scene_path_;
    std::filesystem::path package_root_;
    ResourceIndex index_;
    TextureCatalog textures_;
    std::vector<ResourceNode> nodes_;
    std::vector<DependencyEdge> edges_;
    std::vector<MissionDiagnostic> diagnostics_;
};

[[nodiscard]] std::string mission_graph_json(const MissionGraph& graph);
[[nodiscard]] const char* resource_kind_name(ResourceKind kind) noexcept;
[[nodiscard]] const char* dependency_kind_name(DependencyKind kind) noexcept;
[[nodiscard]] const char* resolution_status_name(ResolutionStatus status) noexcept;

} // namespace csf
