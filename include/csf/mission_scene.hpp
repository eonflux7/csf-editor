#pragma once

#include "csf/document.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace csf {

struct CsfSourceId {
    std::filesystem::path file;
    std::uint32_t entry_index{};
    SourceRange range{};
};

struct Vec3 { float x{}, y{}, z{}; };

struct TypedDiagnostic {
    Diagnostic::Severity severity{Diagnostic::Severity::warning};
    CsfSourceId source;
    std::string code;
    std::string message;
};

struct RawField {
    std::string name;
    CsfSourceId source;
};

struct EnvironmentField {
    std::string name;
    CsfSourceId source;
    std::variant<std::int32_t, float, std::string> value;
};

struct MissionActor {
    CsfSourceId source;
    std::optional<std::string> name;
    std::optional<std::int32_t> id;
    std::optional<std::int32_t> class_id;
    std::optional<Vec3> position;
    std::optional<float> heading;
    std::optional<float> pitch;
    std::optional<std::string> script;
    std::optional<std::int32_t> group;
    std::optional<std::int32_t> cell;
    std::optional<std::int32_t> collision;
    std::optional<std::uint32_t> flags;
    std::vector<RawField> unknown_fields;
};

struct PlayerMetadata {
    std::optional<std::int32_t> active_player;
    std::optional<std::int32_t> commando_start;
    std::optional<std::int32_t> sniper_start;
    std::optional<std::int32_t> spy_start;
};

struct NavPoint {
    CsfSourceId source;
    std::optional<std::int32_t> group_id;
    std::optional<std::int32_t> id;
    std::optional<std::string> name;
    std::optional<Vec3> position;
    std::optional<float> heading;
    std::optional<float> pitch;
    std::vector<RawField> unknown_fields;
};

struct NavConnection {
    CsfSourceId source;
    std::optional<std::int32_t> origin_group;
    std::optional<std::int32_t> origin_point;
    std::optional<std::int32_t> destination_group;
    std::optional<std::int32_t> destination_point;
    bool valid{};
    std::string invalid_reason;
};

struct NavGroup {
    CsfSourceId source;
    std::optional<std::int32_t> id;
    std::optional<std::string> name;
    std::optional<std::int32_t> type;
    std::vector<NavPoint> points;
    std::vector<NavConnection> connections;
    std::vector<RawField> unknown_fields;
};

struct NavigationStats {
    std::size_t groups{}, points{}, connections{}, connected_components{}, orphan_points{};
    std::size_t invalid_connections{}, duplicate_group_ids{}, duplicate_point_ids{};
};

struct MissionDummy {
    CsfSourceId source;
    std::optional<std::int32_t> id;
    std::optional<std::string> name;
    std::optional<Vec3> position;
    std::optional<float> heading;
    std::optional<float> pitch;
    std::vector<RawField> unknown_fields;
};

struct MissionArea {
    CsfSourceId source;
    std::optional<std::int32_t> id;
    std::optional<std::string> name;
    std::optional<std::int32_t> flags;
    std::optional<std::int32_t> occlusion;
    std::optional<float> height;
    std::vector<Vec3> points;
    std::vector<RawField> unknown_fields;
};

struct MissionLight {
    CsfSourceId source;
    std::optional<std::int32_t> id;
    std::optional<std::string> name;
    std::optional<Vec3> position;
    std::optional<std::uint32_t> color;
    std::optional<std::int32_t> modulate;
    std::optional<float> radius;
    std::vector<RawField> unknown_fields;
};

struct FolderMembership {
    CsfSourceId source;
    std::string path;
    std::vector<std::int32_t> element_ids;
};

class MissionScene {
public:
    [[nodiscard]] static MissionScene project(const Document& document);

    [[nodiscard]] const std::filesystem::path& source_path() const noexcept { return source_path_; }
    [[nodiscard]] const PlayerMetadata& player() const noexcept { return player_; }
    [[nodiscard]] const std::vector<EnvironmentField>& environment() const noexcept { return environment_; }
    [[nodiscard]] const std::vector<MissionActor>& actors() const noexcept { return actors_; }
    [[nodiscard]] const std::vector<NavGroup>& navigation() const noexcept { return navigation_; }
    [[nodiscard]] const std::vector<NavConnection>& cross_group_connections() const noexcept { return cross_group_; }
    [[nodiscard]] const NavigationStats& navigation_stats() const noexcept { return navigation_stats_; }
    [[nodiscard]] const std::vector<MissionDummy>& dummies() const noexcept { return dummies_; }
    [[nodiscard]] const std::vector<MissionArea>& areas() const noexcept { return areas_; }
    [[nodiscard]] const std::vector<MissionLight>& lights() const noexcept { return lights_; }
    [[nodiscard]] const std::vector<FolderMembership>& folders() const noexcept { return folders_; }
    [[nodiscard]] const std::vector<TypedDiagnostic>& diagnostics() const noexcept { return diagnostics_; }

private:
    std::filesystem::path source_path_;
    PlayerMetadata player_;
    std::vector<EnvironmentField> environment_;
    std::vector<MissionActor> actors_;
    std::vector<NavGroup> navigation_;
    std::vector<NavConnection> cross_group_;
    NavigationStats navigation_stats_;
    std::vector<MissionDummy> dummies_;
    std::vector<MissionArea> areas_;
    std::vector<MissionLight> lights_;
    std::vector<FolderMembership> folders_;
    std::vector<TypedDiagnostic> diagnostics_;
};

enum class SymbolCategory { actor, navigation_group, navigation_point, dummy, area, light, script, database_record, unknown };
enum class SymbolRole { definition, typed_reference, candidate };

struct SymbolSite {
    std::string symbol;
    SymbolCategory category{SymbolCategory::unknown};
    SymbolRole role{SymbolRole::candidate};
    CsfSourceId source;
    std::string field;
};

class MissionSymbolIndex {
public:
    void add_scene(const MissionScene& scene);
    void add_document(const Document& document);
    [[nodiscard]] std::vector<const SymbolSite*> exact(std::string_view symbol) const;
    [[nodiscard]] const std::vector<SymbolSite>& sites() const noexcept { return sites_; }
private:
    std::vector<SymbolSite> sites_;
};

[[nodiscard]] std::string mission_scene_json(const MissionScene& scene);
[[nodiscard]] const char* symbol_category_name(SymbolCategory) noexcept;
[[nodiscard]] const char* symbol_role_name(SymbolRole) noexcept;

} // namespace csf
