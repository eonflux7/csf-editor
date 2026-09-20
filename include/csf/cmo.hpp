#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace csf {

struct CmoRange {
    std::size_t offset{}, size{};
};

enum class CmoTokenKind { whitespace, comment, identifier, string, number, punctuation, invalid };

struct CmoToken {
    CmoTokenKind kind{CmoTokenKind::invalid};
    CmoRange range;
    std::string spelling;
};

struct CmoNode {
    std::string name;
    CmoRange range;
    std::vector<std::size_t> token_indices;
    std::vector<CmoNode> children;
};

enum class CmoShapeKind { unknown, box, sphere, ellipsoid, capsule, cylinder };

struct CmoVec3 {
    float x{}, y{}, z{};
};

struct CmoShape {
    CmoShapeKind kind{CmoShapeKind::unknown};
    std::string spelling;
    CmoRange range;
    std::optional<CmoVec3> center;
    std::optional<CmoVec3> dimensions;
    std::optional<CmoVec3> offset;
    std::optional<float> radius;
    std::optional<std::int32_t> bone_index;
    std::optional<std::string> label;
    bool external{};
    bool object_3d{};
    std::vector<CmoVec3> hot_points;
};

struct CmoDiagnostic {
    enum class Severity { warning, error };
    Severity severity{Severity::warning};
    CmoRange range;
    std::string code;
    std::string message;
};

class CmoDocument {
public:
    [[nodiscard]] static CmoDocument load(const std::filesystem::path& path);
    [[nodiscard]] static CmoDocument parse(std::string source, std::filesystem::path path = {});

    [[nodiscard]] const std::filesystem::path& source_path() const noexcept { return source_path_; }
    [[nodiscard]] const std::string& source() const noexcept { return source_; }
    [[nodiscard]] const std::vector<CmoToken>& tokens() const noexcept { return tokens_; }
    [[nodiscard]] const std::vector<CmoNode>& roots() const noexcept { return roots_; }
    [[nodiscard]] const std::vector<CmoShape>& shapes() const noexcept { return shapes_; }
    [[nodiscard]] const std::vector<CmoDiagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }
    [[nodiscard]] std::string_view text(CmoRange range) const noexcept;
    [[nodiscard]] std::vector<CmoDiagnostic> validate_bones(std::size_t skeleton_size) const;

private:
    std::filesystem::path source_path_;
    std::string source_;
    std::vector<CmoToken> tokens_;
    std::vector<CmoNode> roots_;
    std::vector<CmoShape> shapes_;
    std::vector<CmoDiagnostic> diagnostics_;
};

[[nodiscard]] const char* cmo_shape_kind_name(CmoShapeKind) noexcept;

} // namespace csf
