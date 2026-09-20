#include "csf/cmo.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace csf {
namespace {

bool space(const unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
bool ident_start(const unsigned char c) {
    return std::isalpha(c) || c == '_' || c == '.' || c >= 0x80;
}
bool ident_continue(const unsigned char c) {
    return ident_start(c) || std::isdigit(c) || c == '-' || c == ':';
}
std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}
bool trivia(const CmoToken& token) {
    return token.kind == CmoTokenKind::whitespace || token.kind == CmoTokenKind::comment;
}

std::vector<std::size_t> significant(const std::vector<CmoToken>& tokens) {
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < tokens.size(); ++i)
        if (!trivia(tokens[i])) result.push_back(i);
    return result;
}

std::optional<float> number(const CmoToken& token) {
    if (token.kind != CmoTokenKind::number) return std::nullopt;
    char* end{};
    const auto value = std::strtof(token.spelling.c_str(), &end);
    if (end != token.spelling.c_str() + token.spelling.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

std::string unquote(const CmoToken& token) {
    if (token.kind != CmoTokenKind::string || token.spelling.size() < 2) return token.spelling;
    std::string result;
    for (std::size_t i = 1; i + 1 < token.spelling.size(); ++i) {
        if (token.spelling[i] == '\\' && i + 2 < token.spelling.size()) ++i;
        result.push_back(token.spelling[i]);
    }
    return result;
}

void derive_shapes(const CmoNode& node, const std::vector<CmoToken>& tokens,
                   std::vector<CmoShape>& output, std::vector<CmoDiagnostic>& diagnostics,
                   const bool inherited_external = false) {
    const auto node_name = lower(node.name);
    bool external = inherited_external || node_name.find("extern") != std::string::npos;
    auto find_scalar = [&](std::initializer_list<std::string_view> names) -> const CmoNode* {
        for (const auto& child : node.children) {
            const auto name = lower(child.name);
            if (std::ranges::find(names, name) != names.end()) return &child;
        }
        return nullptr;
    };
    auto values = [&](const CmoNode& field) {
        std::vector<std::size_t> result;
        for (const auto index : field.token_indices) {
            const auto& token = tokens[index];
            if (token.kind == CmoTokenKind::number || token.kind == CmoTokenKind::identifier ||
                token.kind == CmoTokenKind::string)
                result.push_back(index);
        }
        if (!result.empty() && lower(tokens[result.front()].spelling) == lower(field.name))
            result.erase(result.begin());
        return result;
    };
    std::string shape_spelling;
    if (const auto* type = find_scalar({"shape", "shape_type", "type", "tipo", "forma"})) {
        const auto parts = values(*type);
        if (!parts.empty()) shape_spelling = unquote(tokens[parts.front()]);
    }
    if (shape_spelling.empty()) {
        for (const auto candidate : {"box", "sphere", "ellipsoid", "capsule", "cylinder"})
            if (node_name.find(candidate) != std::string::npos) {
                shape_spelling = candidate;
                break;
            }
    }
    CmoShapeKind kind = CmoShapeKind::unknown;
    const auto normalized_shape = lower(shape_spelling);
    if (normalized_shape.find("box") != std::string::npos ||
        normalized_shape.find("caja") != std::string::npos)
        kind = CmoShapeKind::box;
    else if (normalized_shape.find("ellipsoid") != std::string::npos ||
             normalized_shape.find("elipsoid") != std::string::npos)
        kind = CmoShapeKind::ellipsoid;
    else if (normalized_shape.find("sphere") != std::string::npos ||
             normalized_shape.find("esfera") != std::string::npos)
        kind = CmoShapeKind::sphere;
    else if (normalized_shape.find("capsule") != std::string::npos ||
             normalized_shape.find("capsula") != std::string::npos)
        kind = CmoShapeKind::capsule;
    else if (normalized_shape.find("cylinder") != std::string::npos ||
             normalized_shape.find("cilindro") != std::string::npos)
        kind = CmoShapeKind::cylinder;

    if (kind != CmoShapeKind::unknown || !shape_spelling.empty()) {
        CmoShape shape{kind, shape_spelling, node.range};
        shape.external = external;
        auto vec = [&](std::initializer_list<std::string_view> names) -> std::optional<CmoVec3> {
            const auto* field = find_scalar(names);
            if (!field) return std::nullopt;
            const auto parts = values(*field);
            std::vector<float> parsed;
            for (const auto index : parts)
                if (const auto value = number(tokens[index])) parsed.push_back(*value);
            if (parsed.size() < 3) return std::nullopt;
            return CmoVec3{parsed[0], parsed[1], parsed[2]};
        };
        auto scalar = [&](std::initializer_list<std::string_view> names) -> std::optional<float> {
            const auto* field = find_scalar(names);
            if (!field) return std::nullopt;
            for (const auto index : values(*field))
                if (const auto value = number(tokens[index])) return value;
            return std::nullopt;
        };
        shape.center = vec({"center", "centre", "centro", "position", "pos"});
        shape.dimensions = vec({"dimensions", "dimension", "size", "extents", "dimensiones"});
        shape.offset = vec({"offset", "desplazamiento"});
        shape.radius = scalar({"radius", "radio"});
        if (const auto* field = find_scalar({"bone", "bone_index", "boneindex", "hueso"})) {
            for (const auto index : values(*field))
                if (const auto value = number(tokens[index])) {
                    if (*value >= static_cast<float>(std::numeric_limits<std::int32_t>::min()) &&
                        *value <= static_cast<float>(std::numeric_limits<std::int32_t>::max()))
                        shape.bone_index = static_cast<std::int32_t>(*value);
                    break;
                }
        }
        if (const auto* field = find_scalar({"label", "name", "nombre"})) {
            const auto parts = values(*field);
            if (!parts.empty()) shape.label = unquote(tokens[parts.front()]);
        }
        if (const auto* field = find_scalar({"object3d", "object_3d", "use_object_3d"})) {
            const auto parts = values(*field);
            if (!parts.empty()) {
                const auto value = lower(tokens[parts.front()].spelling);
                shape.object_3d = value == "1" || value == "true" || value == "yes";
            }
        }
        for (const auto& child : node.children)
            if (lower(child.name).find("hot") != std::string::npos) {
                const auto parts = values(child);
                std::vector<float> parsed;
                for (const auto index : parts)
                    if (const auto value = number(tokens[index])) parsed.push_back(*value);
                for (std::size_t i = 0; i + 2 < parsed.size(); i += 3)
                    shape.hot_points.push_back({parsed[i], parsed[i + 1], parsed[i + 2]});
            }
        auto invalid_extent = [&](const CmoVec3& value) {
            return value.x < 0 || value.y < 0 || value.z < 0;
        };
        if ((shape.radius && *shape.radius < 0) ||
            (shape.dimensions && invalid_extent(*shape.dimensions)))
            diagnostics.push_back({CmoDiagnostic::Severity::error, shape.range,
                                   "negative-shape-size",
                                   "Shape radii and extents must be nonnegative"});
        if (shape.kind == CmoShapeKind::unknown)
            diagnostics.push_back({CmoDiagnostic::Severity::warning, shape.range,
                                   "unsupported-shape",
                                   "Unsupported CMO shape spelling: " + shape.spelling});
        output.push_back(std::move(shape));
    }
    for (const auto& child : node.children)
        derive_shapes(child, tokens, output, diagnostics, external);
}

} // namespace

CmoDocument CmoDocument::load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open CMO: " + path.string());
    return parse(std::string(std::istreambuf_iterator<char>(input), {}), path);
}

CmoDocument CmoDocument::parse(std::string source, std::filesystem::path path) {
    CmoDocument result;
    result.source_path_ = std::move(path);
    result.source_ = std::move(source);
    const auto& text = result.source_;
    for (std::size_t i = 0; i < text.size();) {
        const auto begin = i;
        CmoTokenKind kind = CmoTokenKind::invalid;
        const auto c = static_cast<unsigned char>(text[i]);
        if (space(c)) {
            kind = CmoTokenKind::whitespace;
            while (i < text.size() && space(static_cast<unsigned char>(text[i])))
                ++i;
        } else if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            kind = CmoTokenKind::comment;
            i += 2;
            while (i < text.size() && text[i] != '\n')
                ++i;
        } else if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            kind = CmoTokenKind::comment;
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/'))
                ++i;
            if (i + 1 < text.size())
                i += 2;
            else
                result.diagnostics_.push_back({CmoDiagnostic::Severity::error,
                                               {begin, text.size() - begin},
                                               "unterminated-comment",
                                               "Unterminated block comment"});
        } else if (text[i] == '#') {
            kind = CmoTokenKind::comment;
            while (i < text.size() && text[i] != '\n')
                ++i;
        } else if (text[i] == '"' || text[i] == '\'') {
            kind = CmoTokenKind::string;
            const char quote = text[i++];
            bool closed = false;
            while (i < text.size()) {
                if (text[i] == '\\' && i + 1 < text.size()) {
                    i += 2;
                    continue;
                }
                if (text[i++] == quote) {
                    closed = true;
                    break;
                }
            }
            if (!closed)
                result.diagnostics_.push_back({CmoDiagnostic::Severity::error,
                                               {begin, i - begin},
                                               "unterminated-string",
                                               "Unterminated string literal"});
        } else if (ident_start(c)) {
            kind = CmoTokenKind::identifier;
            ++i;
            while (i < text.size() && ident_continue(static_cast<unsigned char>(text[i])))
                ++i;
        } else if (std::isdigit(c) ||
                   ((text[i] == '+' || text[i] == '-') && i + 1 < text.size() &&
                    (std::isdigit(static_cast<unsigned char>(text[i + 1])) ||
                     text[i + 1] == '.')) ||
                   (text[i] == '.' && i + 1 < text.size() &&
                    std::isdigit(static_cast<unsigned char>(text[i + 1])))) {
            kind = CmoTokenKind::number;
            char* end{};
            (void)std::strtof(text.c_str() + i, &end);
            i = static_cast<std::size_t>(end - text.c_str());
        } else if (std::string_view("{}[]()=,:<>;").find(text[i]) != std::string_view::npos) {
            kind = CmoTokenKind::punctuation;
            ++i;
        } else {
            ++i;
            result.diagnostics_.push_back({CmoDiagnostic::Severity::warning,
                                           {begin, 1},
                                           "unexpected-character",
                                           "Unexpected character in CMO source"});
        }
        result.tokens_.push_back({kind, {begin, i - begin}, text.substr(begin, i - begin)});
    }

    const auto sig = significant(result.tokens_);
    std::size_t cursor{};
    auto parse_block = [&](auto&& self, const char closing) -> std::vector<CmoNode> {
        std::vector<CmoNode> nodes;
        while (cursor < sig.size()) {
            const auto token_index = sig[cursor];
            const auto& token = result.tokens_[token_index];
            if (closing && token.spelling.size() == 1 && token.spelling.front() == closing) {
                ++cursor;
                return nodes;
            }
            if (token.spelling == "}" || token.spelling == "]" || token.spelling == ")") {
                result.diagnostics_.push_back({CmoDiagnostic::Severity::error, token.range,
                                               "unmatched-close", "Unmatched closing bracket"});
                ++cursor;
                continue;
            }
            CmoNode node;
            node.range.offset = token.range.offset;
            node.name = token.kind == CmoTokenKind::identifier || token.kind == CmoTokenKind::string
                            ? unquote(token)
                            : token.spelling;
            std::size_t block_end{};
            while (cursor < sig.size()) {
                const auto current_index = sig[cursor];
                const auto& current = result.tokens_[current_index];
                if (current.spelling == "{" || current.spelling == "[" || current.spelling == "(") {
                    node.token_indices.push_back(current_index);
                    ++cursor;
                    const char wanted = current.spelling == "{"   ? '}'
                                        : current.spelling == "[" ? ']'
                                                                  : ')';
                    node.children = self(self, wanted);
                    break;
                }
                if (current.spelling == ";" || current.spelling == "}" || current.spelling == "]" ||
                    current.spelling == ")")
                    break;
                node.token_indices.push_back(current_index);
                ++cursor;
                if (cursor < sig.size()) {
                    const auto& next = result.tokens_[sig[cursor]];
                    if (next.range.offset > current.range.offset + current.range.size &&
                        text.substr(current.range.offset + current.range.size,
                                    next.range.offset - current.range.offset - current.range.size)
                                .find('\n') != std::string::npos &&
                        current.spelling != "=" && current.spelling != ",")
                        break;
                }
            }
            std::size_t end = node.range.offset;
            if (!node.token_indices.empty()) {
                const auto& last = result.tokens_[node.token_indices.back()];
                end = last.range.offset + last.range.size;
            }
            if (!node.children.empty())
                end = node.children.back().range.offset + node.children.back().range.size;
            if (cursor > 0) {
                const auto& previous = result.tokens_[sig[cursor - 1]];
                if (previous.spelling == "}" || previous.spelling == "]" ||
                    previous.spelling == ")")
                    block_end = previous.range.offset + previous.range.size;
            }
            end = std::max(end, block_end);
            node.range.size = std::max<std::size_t>(1, end - node.range.offset);
            nodes.push_back(std::move(node));
            if (cursor < sig.size() && result.tokens_[sig[cursor]].spelling == ";") ++cursor;
        }
        if (closing)
            result.diagnostics_.push_back({CmoDiagnostic::Severity::error,
                                           {text.size(), 0},
                                           "unclosed-bracket",
                                           "CMO bracket was not closed"});
        return nodes;
    };
    result.roots_ = parse_block(parse_block, 0);
    for (const auto& root : result.roots_)
        derive_shapes(root, result.tokens_, result.shapes_, result.diagnostics_);
    return result;
}

std::string_view CmoDocument::text(const CmoRange range) const noexcept {
    if (range.offset > source_.size() || range.size > source_.size() - range.offset) return {};
    return std::string_view(source_).substr(range.offset, range.size);
}

std::vector<CmoDiagnostic> CmoDocument::validate_bones(const std::size_t skeleton_size) const {
    std::vector<CmoDiagnostic> result;
    for (const auto& shape : shapes_)
        if (shape.bone_index &&
            (*shape.bone_index < 0 || static_cast<std::size_t>(*shape.bone_index) >= skeleton_size))
            result.push_back(
                {CmoDiagnostic::Severity::warning, shape.range, "unresolved-bone",
                 "CMO bone index " + std::to_string(*shape.bone_index) +
                     " is outside skeleton range 0.." +
                     (skeleton_size ? std::to_string(skeleton_size - 1) : std::string("empty"))});
    return result;
}

const char* cmo_shape_kind_name(const CmoShapeKind kind) noexcept {
    switch (kind) {
    case CmoShapeKind::box:
        return "box";
    case CmoShapeKind::sphere:
        return "sphere";
    case CmoShapeKind::ellipsoid:
        return "ellipsoid";
    case CmoShapeKind::capsule:
        return "capsule";
    case CmoShapeKind::cylinder:
        return "cylinder";
    case CmoShapeKind::unknown:
        return "unknown";
    }
    return "unknown";
}

} // namespace csf
