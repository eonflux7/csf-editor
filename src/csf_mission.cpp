#include "csf/mission.hpp"

#include "csf/document.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace csf {
namespace {

std::string slashes(std::string_view value) {
    std::string result(value);
    std::ranges::replace(result, '\\', '/');
    while (result.starts_with("./")) result.erase(0, 2);
    std::string compact;
    compact.reserve(result.size());
    for (const auto character : result) {
        if (character != '/' || compact.empty() || compact.back() != '/') compact.push_back(character);
    }
    while (compact.size() > 1 && compact.back() == '/') compact.pop_back();
    return compact;
}

std::string lower_ascii(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string extension_lower(std::string_view value) {
    return lower_ascii(std::filesystem::path(value).extension().string());
}

bool traversal_or_absolute(std::string_view reference) {
    const auto normalized = slashes(reference);
    if (normalized.empty()) return false;
    if (normalized.front() == '/' ||
        (normalized.size() >= 2 && std::isalpha(static_cast<unsigned char>(normalized[0])) && normalized[1] == ':'))
        return true;
    std::stringstream stream(normalized);
    std::string component;
    while (std::getline(stream, component, '/')) {
        if (component == "..") return true;
    }
    return false;
}

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Cannot open " + path.string());
    const auto end = input.tellg();
    if (end < 0) throw std::runtime_error("Cannot size " + path.string());
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input && !bytes.empty()) throw std::runtime_error("Cannot read " + path.string());
    return bytes;
}

std::uint64_t content_hash(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot hash " + path.string());
    std::uint64_t hash = 14695981039346656037ULL;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        for (std::streamsize index = 0; index < input.gcount(); ++index) {
            hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]);
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

std::optional<std::uint32_t> u32(std::span<const std::byte> bytes, const std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) return std::nullopt;
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U);
}

bool plausible_path(std::string_view value, const std::set<std::string>& extensions) {
    if (value.empty() || value.size() > 4096 || !extensions.contains(extension_lower(value))) return false;
    return std::ranges::all_of(value, [](const unsigned char character) {
        return character >= 0x20 && character != 0x7f;
    });
}

AdapterResult read_records(const std::filesystem::path& path, const std::size_t suffix_size,
                           const std::set<std::string>& extensions, const char* adapter,
                           const DependencyKind kind) {
    const auto bytes = read_file(path);
    AdapterResult result;
    std::size_t cursor{};
    while (cursor < bytes.size()) {
        const auto length = u32(bytes, cursor);
        if (!length || *length > 4096 || cursor + 4ULL + *length + suffix_size > bytes.size()) break;
        const auto text_offset = cursor + 4;
        std::string value(*length, '\0');
        if (*length) std::memcpy(value.data(), bytes.data() + text_offset, *length);
        if (!plausible_path(value, extensions)) break;
        result.references.push_back({std::move(value), kind,
            {path, text_offset, *length, std::nullopt, adapter}});
        cursor = text_offset + *length + suffix_size;
    }
    result.unknown_tail_offset = cursor;
    result.unknown_tail.assign(bytes.begin() + static_cast<std::ptrdiff_t>(cursor), bytes.end());
    if (!result.unknown_tail.empty()) {
        result.diagnostics.push_back({MissionDiagnostic::Severity::note, "adapter-unknown-tail",
            std::string(adapter) + " retained " + std::to_string(result.unknown_tail.size()) +
                " unparsed bytes",
            SourceLocation{path, cursor, result.unknown_tail.size(), std::nullopt, adapter}});
    }
    return result;
}

std::string json_escape(std::string_view value) {
    std::ostringstream output;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
        case '\\': output << "\\\\"; break;
        case '"': output << "\\\""; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (byte < 0x20)
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned>(byte) << std::dec;
            else output << character;
        }
    }
    return output.str();
}

const char* load_state_name(const LoadState state) noexcept {
    switch (state) {
    case LoadState::metadata_only: return "metadata-only";
    case LoadState::available: return "available";
    case LoadState::missing: return "missing";
    case LoadState::ambiguous: return "ambiguous";
    case LoadState::rejected: return "rejected";
    }
    return "metadata-only";
}

ResourceKind kind_for_extension(const std::filesystem::path& path, const DependencyKind hint) {
    if (hint == DependencyKind::visual_map) return ResourceKind::visual_map;
    if (hint == DependencyKind::collision_map) return ResourceKind::collision_map;
    if (hint == DependencyKind::texture_directory || hint == DependencyKind::texture) return ResourceKind::texture;
    if (hint == DependencyKind::sky_model || hint == DependencyKind::render_model) return ResourceKind::render_model;
    if (hint == DependencyKind::physics_body) return ResourceKind::physics_body;
    if (hint == DependencyKind::animation) return ResourceKind::animation;
    const auto extension = extension_lower(path.string());
    if (extension == ".scn") return ResourceKind::mission_scene;
    if (extension == ".gsc") return ResourceKind::mission_script;
    if (extension == ".csc") return ResourceKind::cutscene_script;
    if (extension == ".sec") return ResourceKind::security_data;
    if (extension == ".bdd") return ResourceKind::database;
    if (extension == ".vis") return ResourceKind::visual_index;
    if (extension == ".m3d") return ResourceKind::model_index;
    if (extension == ".phd") return ResourceKind::physics_index;
    if (extension == ".and") return ResourceKind::animation_index;
    if (extension == ".txl") return ResourceKind::texture_index;
    if (extension == ".rpc" || extension == ".dff") return ResourceKind::render_model;
    if (extension == ".anm") return ResourceKind::animation;
    if (extension == ".cmo") return ResourceKind::collision_shape;
    if (extension == ".dds" || extension == ".png" || extension == ".tga") return ResourceKind::texture;
    if (extension == ".wad") return ResourceKind::audio_bank;
    if (extension == ".rws") {
        if (hint == DependencyKind::collision_map) return ResourceKind::collision_map;
        if (hint == DependencyKind::visual_map) return ResourceKind::visual_map;
        if (hint == DependencyKind::physics_body) return ResourceKind::physics_body;
    }
    return ResourceKind::unknown;
}

std::filesystem::path infer_package_root(const std::filesystem::path& scene) {
    auto current = scene.parent_path();
    while (!current.empty()) {
        if (std::filesystem::is_directory(current / "Maps") || std::filesystem::is_directory(current / "BDD"))
            return current;
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return scene.parent_path();
}

std::filesystem::path choose_scene(const std::filesystem::path& input) {
    if (std::filesystem::is_regular_file(input)) return std::filesystem::absolute(input).lexically_normal();
    if (!std::filesystem::is_directory(input)) throw std::runtime_error("Mission input does not exist: " + input.string());
    std::vector<std::filesystem::path> scenes;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             input, std::filesystem::directory_options::skip_permission_denied)) {
        if (entry.is_regular_file() && extension_lower(entry.path().string()) == ".scn") scenes.push_back(entry.path());
    }
    std::ranges::sort(scenes);
    if (scenes.empty()) throw std::runtime_error("Mission directory contains no SCN file");
    const auto wanted = lower_ascii(input.filename().string());
    const auto match = std::ranges::find_if(scenes, [&](const auto& path) {
        return lower_ascii(path.stem().string()) == wanted;
    });
    if (match != scenes.end()) return std::filesystem::absolute(*match).lexically_normal();
    if (scenes.size() != 1) throw std::runtime_error("Mission directory contains multiple SCN files; select one explicitly");
    return std::filesystem::absolute(scenes.front()).lexically_normal();
}

} // namespace

void ResourceIndex::add_root(const std::filesystem::path& root) {
    const auto absolute = std::filesystem::absolute(root).lexically_normal();
    if (!std::filesystem::is_directory(absolute)) throw std::runtime_error("Resource root is not a directory: " + absolute.string());
    if (std::ranges::find(roots_, absolute) == roots_.end()) roots_.push_back(absolute);
}

void ResourceIndex::build() {
    resources_.clear();
    normalized_.clear();
    logical_.clear();
    for (std::size_t root_index = 0; root_index < roots_.size(); ++root_index) {
        std::vector<std::filesystem::path> paths;
        std::error_code error;
        std::filesystem::recursive_directory_iterator iterator(
            roots_[root_index], std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::recursive_directory_iterator end;
        for (; iterator != end; iterator.increment(error)) {
            if (error) { error.clear(); continue; }
            const auto link_status = iterator->symlink_status(error);
            if (error) { error.clear(); continue; }
            if (std::filesystem::is_symlink(link_status)) {
                if (iterator->is_directory(error)) iterator.disable_recursion_pending();
                error.clear();
                continue;
            }
            if ((iterator->is_regular_file(error) || iterator->is_directory(error)) && !error)
                paths.push_back(iterator->path());
            error.clear();
        }
        std::ranges::sort(paths, {}, [](const auto& path) { return slashes(path.generic_string()); });
        for (const auto& path : paths) {
            const auto relative = path.lexically_relative(roots_[root_index]);
            const auto key = normalize(relative.generic_string());
            const auto index = resources_.size();
            const auto size = std::filesystem::is_regular_file(path, error)
                                  ? static_cast<std::uint64_t>(std::filesystem::file_size(path, error)) : 0;
            resources_.push_back({root_index, roots_[root_index], path, relative, key, size});
            normalized_[key].push_back(index);
            static const std::array anchors{"anims/", "bdd/", "gfx/", "maps/", "menus/",
                                             "models/", "sounds/", "texts/"};
            for (const auto anchor : anchors) {
                const auto position = key.find(anchor);
                if (position == 0 || (position != std::string::npos && key[position - 1] == '/')) {
                    logical_[key.substr(position)].push_back(index);
                    break;
                }
            }
            error.clear();
        }
    }
}

std::string ResourceIndex::normalize(const std::string_view path) {
    return lower_ascii(slashes(path));
}

Resolution ResourceIndex::resolve(const std::string_view reference,
                                  const std::optional<std::size_t> preferred_root) const {
    Resolution result;
    result.original_reference = std::string(reference);
    result.normalized_key = normalize(reference);
    if (traversal_or_absolute(reference)) {
        result.status = ResolutionStatus::outside_root;
        return result;
    }
    const auto exact_key = slashes(reference);
    auto candidates_for = [&](const std::string& key, const std::optional<std::size_t> root) {
        std::vector<std::size_t> candidates;
        if (const auto found = normalized_.find(key); found != normalized_.end()) {
            for (const auto index : found->second)
                if (!root || resources_[index].root_index == *root) candidates.push_back(index);
        }
        return candidates;
    };
    auto exact_candidates = [&](const std::optional<std::size_t> root, const std::string& wanted) {
        auto candidates = candidates_for(normalize(wanted), root);
        std::erase_if(candidates, [&](const auto index) {
            return slashes(resources_[index].relative_path.generic_string()) != wanted;
        });
        return candidates;
    };
    auto decide = [&](std::vector<std::size_t> candidates, const ResolutionStatus unique_status) -> bool {
        if (candidates.empty()) return false;
        std::set<std::string> physical_paths;
        std::erase_if(candidates, [&](const auto index) {
            const auto identity = normalize(std::filesystem::absolute(resources_[index].path)
                                                .lexically_normal().generic_string());
            return !physical_paths.insert(identity).second;
        });
        result.candidate_indices = std::move(candidates);
        result.status = result.candidate_indices.size() == 1 ? unique_status : ResolutionStatus::ambiguous;
        return true;
    };

    if (preferred_root) {
        if (decide(exact_candidates(preferred_root, exact_key), ResolutionStatus::exact)) return result;
        if (decide(candidates_for(result.normalized_key, preferred_root), ResolutionStatus::case_mismatch)) return result;
    }
    if (extension_lower(reference) == ".dff") {
        auto mapped = std::filesystem::path(exact_key);
        mapped.replace_extension(".rpc");
        const auto mapped_key = normalize(mapped.generic_string());
        if (preferred_root && decide(candidates_for(mapped_key, preferred_root), ResolutionStatus::mapped_dff_to_rpc)) return result;
        for (std::size_t root = 0; root < roots_.size(); ++root)
            if ((!preferred_root || root != *preferred_root) &&
                decide(candidates_for(mapped_key, root), ResolutionStatus::mapped_dff_to_rpc)) return result;
    }
    for (std::size_t root = 0; root < roots_.size(); ++root) {
        if (preferred_root && root == *preferred_root) continue;
        if (decide(exact_candidates(root, exact_key), ResolutionStatus::shared_root)) return result;
        if (decide(candidates_for(result.normalized_key, root), ResolutionStatus::shared_root_case_mismatch)) return result;
    }
    if (extension_lower(reference) == ".dff") {
        auto mapped = std::filesystem::path(exact_key);
        mapped.replace_extension(".rpc");
        if (const auto found = logical_.find(normalize(mapped.generic_string())); found != logical_.end()) {
            decide(found->second, ResolutionStatus::mapped_dff_to_rpc);
            return result;
        }
    }
    if (const auto found = logical_.find(result.normalized_key); found != logical_.end()) {
        decide(found->second, ResolutionStatus::shared_root);
        return result;
    }
    result.status = ResolutionStatus::missing;
    return result;
}

std::vector<std::size_t> ResourceIndex::find_path(const std::filesystem::path& path) const {
    std::vector<std::size_t> result;
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    for (std::size_t index = 0; index < resources_.size(); ++index)
        if (resources_[index].path.lexically_normal() == absolute) result.push_back(index);
    return result;
}

AdapterResult read_vis(const std::filesystem::path& path) {
    const auto bytes = read_file(path);
    AdapterResult result;
    const std::array kinds{DependencyKind::visual_map, DependencyKind::collision_map,
                           DependencyKind::texture_directory, DependencyKind::sky_model};
    std::size_t cursor{};
    for (std::size_t field = 0; field < kinds.size(); ++field) {
        const auto length = u32(bytes, cursor);
        if (!length || *length > 4096 || cursor + 4ULL + *length > bytes.size()) {
            result.diagnostics.push_back({MissionDiagnostic::Severity::error, "vis-truncated",
                "VIS ended before its four leading references", SourceLocation{path, cursor, 0, std::nullopt, "vis"}});
            break;
        }
        const auto text_offset = cursor + 4;
        std::string value(*length, '\0');
        if (*length) std::memcpy(value.data(), bytes.data() + text_offset, *length);
        result.references.push_back({std::move(value), kinds[field],
            {path, text_offset, *length, static_cast<std::uint32_t>(field), "vis"}});
        cursor = text_offset + *length;
    }
    result.unknown_tail_offset = cursor;
    result.unknown_tail.assign(bytes.begin() + static_cast<std::ptrdiff_t>(cursor), bytes.end());
    return result;
}

AdapterResult read_txl(const std::filesystem::path& path) {
    const auto bytes = read_file(path);
    AdapterResult result;
    std::size_t line_start{};
    std::uint32_t line{};
    while (line_start < bytes.size()) {
        auto line_end = line_start;
        while (line_end < bytes.size() && bytes[line_end] != std::byte{'\n'} && bytes[line_end] != std::byte{'\r'}) ++line_end;
        std::string value(line_end - line_start, '\0');
        if (!value.empty()) std::memcpy(value.data(), bytes.data() + line_start, value.size());
        if (!value.empty()) result.references.push_back({std::move(value), DependencyKind::texture,
            {path, line_start, line_end - line_start, line, "txl"}});
        if (line_end < bytes.size()) {
            const auto first = bytes[line_end++];
            if (first == std::byte{'\r'} && line_end < bytes.size() && bytes[line_end] == std::byte{'\n'})
                ++line_end;
        }
        line_start = line_end;
        ++line;
    }
    result.unknown_tail_offset = bytes.size();
    return result;
}

AdapterResult read_m3d(const std::filesystem::path& path) {
    return read_records(path, 4, {".dff", ".rpc"}, "m3d", DependencyKind::render_model);
}

AdapterResult read_and(const std::filesystem::path& path) {
    return read_records(path, 2, {".anm"}, "and", DependencyKind::animation);
}

AdapterResult read_phd_candidates(const std::filesystem::path& path) {
    const auto bytes = read_file(path);
    AdapterResult result;
    const std::set<std::string> extensions{".dff", ".rpc", ".rws", ".cmo"};
    std::size_t cursor{};
    while (cursor + 4 <= bytes.size()) {
        const auto length = u32(bytes, cursor);
        if (length && *length > 0 && *length <= 4096 && cursor + 4ULL + *length <= bytes.size()) {
            std::string value(*length, '\0');
            std::memcpy(value.data(), bytes.data() + cursor + 4, *length);
            if (plausible_path(value, extensions)) {
                const auto extension = extension_lower(value);
                auto kind = extension == ".dff" || extension == ".rpc" ? DependencyKind::render_model
                                                                       : DependencyKind::physics_body;
                result.references.push_back({std::move(value), kind,
                    {path, cursor + 4, *length, std::nullopt, "phd-candidate"}});
                cursor += 4ULL + *length;
                continue;
            }
        }
        ++cursor;
    }
    result.unknown_tail_offset = 0;
    result.unknown_tail = bytes;
    result.diagnostics.push_back({MissionDiagnostic::Severity::note, "phd-partial-schema",
        "PHD references are bounded candidates; the complete record schema remains unknown",
        SourceLocation{path, 0, bytes.size(), std::nullopt, "phd-candidate"}});
    return result;
}

MissionGraph MissionGraph::load(const MissionOptions& options) {
    MissionGraph graph;
    graph.scene_path_ = choose_scene(options.input);
    if (extension_lower(graph.scene_path_.string()) != ".scn") throw std::runtime_error("Mission input is not an SCN file");
    graph.package_root_ = options.package_root ? std::filesystem::absolute(*options.package_root).lexically_normal()
                                               : infer_package_root(graph.scene_path_);
    if (options.prepared_index) {
        graph.index_ = *options.prepared_index;
    } else {
        graph.index_.add_root(graph.package_root_);
        if (options.resource_roots.empty()) {
            const auto parent = graph.package_root_.parent_path();
            if (!parent.empty() && parent != graph.package_root_) graph.index_.add_root(parent);
        } else {
            for (const auto& root : options.resource_roots) graph.index_.add_root(root);
        }
        graph.index_.build();
    }
    std::optional<std::size_t> preferred_root;
    const auto package_identity = ResourceIndex::normalize(graph.package_root_.generic_string());
    for (std::size_t index = 0; index < graph.index_.roots().size(); ++index)
        if (ResourceIndex::normalize(graph.index_.roots()[index].generic_string()) == package_identity) {
            preferred_root = index;
            break;
        }

    std::map<std::string, std::uint64_t> node_by_path;
    auto add_resolved_node = [&](const std::filesystem::path& path, const std::string& original,
                                 const DependencyKind hint) -> std::uint64_t {
        const auto absolute = std::filesystem::absolute(path).lexically_normal();
        const auto key = lower_ascii(slashes(absolute.generic_string()));
        if (const auto found = node_by_path.find(key); found != node_by_path.end()) {
            auto& existing = graph.nodes_[found->second];
            const auto refined = kind_for_extension(absolute, hint);
            if (existing.kind == ResourceKind::unknown && refined != ResourceKind::unknown) existing.kind = refined;
            return found->second;
        }
        std::error_code error;
        const auto id = static_cast<std::uint64_t>(graph.nodes_.size());
        const auto size = std::filesystem::is_regular_file(absolute, error)
                              ? static_cast<std::uint64_t>(std::filesystem::file_size(absolute, error)) : 0;
        graph.nodes_.push_back({id, kind_for_extension(absolute, hint), absolute, original,
            ResourceIndex::normalize(original), size,
            LoadState::available, {}, {}});
        auto& added = graph.nodes_.back();
        const auto extension = extension_lower(absolute.string());
        if (extension == ".rws" || extension == ".rpc" || extension == ".anm") {
            std::ifstream input(absolute, std::ios::binary);
            std::array<std::byte, 4> root_bytes{};
            input.read(reinterpret_cast<char*>(root_bytes.data()), 4);
            if (input.gcount() == 4) {
                added.root_type = std::to_integer<std::uint32_t>(root_bytes[0]) |
                                  (std::to_integer<std::uint32_t>(root_bytes[1]) << 8U) |
                                  (std::to_integer<std::uint32_t>(root_bytes[2]) << 16U) |
                                  (std::to_integer<std::uint32_t>(root_bytes[3]) << 24U);
                const bool unexpected = (extension == ".rpc" && *added.root_type != 0x10U) ||
                                        (extension == ".anm" && *added.root_type != 0x1BU) ||
                                        (added.kind == ResourceKind::visual_map && *added.root_type != 0x10U &&
                                         *added.root_type != 0x0BU);
                if (unexpected) {
                    MissionDiagnostic diagnostic{MissionDiagnostic::Severity::warning, "unexpected-root-type",
                        "Unexpected root type 0x" + [&] {
                            std::ostringstream text; text << std::hex << *added.root_type; return text.str();
                        }() + " for " + absolute.string(),
                        SourceLocation{absolute, 0, 4, std::nullopt, "root-sniff"}};
                    added.diagnostics.push_back(diagnostic);
                    graph.diagnostics_.push_back(std::move(diagnostic));
                }
            }
        }
        node_by_path[key] = id;
        return id;
    };
    const auto scene_id = add_resolved_node(graph.scene_path_,
        graph.scene_path_.lexically_relative(graph.package_root_).generic_string(), DependencyKind::package_member);
    graph.nodes_[scene_id].content_hash = content_hash(graph.scene_path_);
    if (options.detect_duplicate_scenes) {
    std::set<std::string> checked_scene_paths;
    const auto corpus_root = graph.package_root_.parent_path();
    std::vector<std::filesystem::path> scene_candidates;
    std::error_code scene_scan_error;
    std::filesystem::recursive_directory_iterator scene_iterator(
        corpus_root, std::filesystem::directory_options::skip_permission_denied, scene_scan_error);
    const std::filesystem::recursive_directory_iterator scene_end;
    for (; scene_iterator != scene_end; scene_iterator.increment(scene_scan_error)) {
        if (scene_scan_error) { scene_scan_error.clear(); continue; }
        if (!scene_iterator->is_regular_file(scene_scan_error) || scene_scan_error ||
            extension_lower(scene_iterator->path().string()) != ".scn" ||
            scene_iterator->file_size(scene_scan_error) != graph.nodes_[scene_id].size) {
            scene_scan_error.clear();
            continue;
        }
        scene_candidates.push_back(scene_iterator->path());
    }
    std::ranges::sort(scene_candidates, {}, [](const auto& path) { return slashes(path.generic_string()); });
    for (const auto& candidate : scene_candidates) {
        const auto candidate_key = lower_ascii(slashes(std::filesystem::absolute(candidate).generic_string()));
        std::error_code equivalent_error;
        const auto same_file = std::filesystem::equivalent(candidate, graph.scene_path_, equivalent_error);
        if (!checked_scene_paths.insert(candidate_key).second || (!equivalent_error && same_file)) continue;
        try {
          if (content_hash(candidate) == *graph.nodes_[scene_id].content_hash) {
            graph.diagnostics_.push_back({MissionDiagnostic::Severity::note, "duplicate-content",
                "Selected SCN has identical content at a distinct path: " + candidate.string(),
                SourceLocation{candidate, 0, graph.nodes_[scene_id].size, std::nullopt, "content-signature"}});
          }
        } catch (const std::exception& exception) {
            graph.diagnostics_.push_back({MissionDiagnostic::Severity::warning, "content-signature-failed",
                exception.what(), SourceLocation{candidate, 0, 0, std::nullopt, "content-signature"}});
        }
    }
    }

    auto add_reference = [&](const std::uint64_t source, const AdapterReference& reference) {
        const auto resolution = graph.index_.resolve(reference.path, preferred_root);
        DependencyEdge edge{source, std::nullopt, reference.kind, reference.source, resolution.status,
                            reference.path, resolution.normalized_key, {}};
        for (const auto index : resolution.candidate_indices)
            edge.candidates.push_back(graph.index_.resources()[index].path);
        if (resolution.candidate_indices.size() == 1 &&
            resolution.status != ResolutionStatus::ambiguous) {
            const auto& indexed = graph.index_.resources()[resolution.candidate_indices.front()];
            edge.target = add_resolved_node(indexed.path, reference.path, reference.kind);
        } else {
            const auto unresolved_id = static_cast<std::uint64_t>(graph.nodes_.size());
            const auto unresolved_state = resolution.status == ResolutionStatus::ambiguous ? LoadState::ambiguous :
                                          resolution.status == ResolutionStatus::outside_root ? LoadState::rejected :
                                                                                               LoadState::missing;
            graph.nodes_.push_back({unresolved_id, kind_for_extension(reference.path, reference.kind), {},
                reference.path, resolution.normalized_key, 0, unresolved_state, edge.candidates, {}});
            edge.target = unresolved_id;
            const auto severity = resolution.status == ResolutionStatus::outside_root
                                      ? MissionDiagnostic::Severity::error : MissionDiagnostic::Severity::warning;
            const auto code = resolution.status == ResolutionStatus::ambiguous ? "ambiguous-reference" :
                              resolution.status == ResolutionStatus::outside_root ? "outside-root" : "missing-reference";
            graph.diagnostics_.push_back({severity, code,
                std::string(resolution_status_name(resolution.status)) + " reference: " + reference.path,
                reference.source});
        }
        if (resolution.status == ResolutionStatus::case_mismatch ||
            resolution.status == ResolutionStatus::shared_root_case_mismatch) {
            graph.diagnostics_.push_back({MissionDiagnostic::Severity::warning, "case-mismatch",
                "Reference spelling differs from the resolved path: " + reference.path, reference.source});
        }
        graph.edges_.push_back(std::move(edge));
    };

    auto package_reference = [&](const std::filesystem::path& path, const bool required) {
        const auto relative = path.lexically_relative(graph.package_root_).generic_string();
        AdapterReference reference{relative, DependencyKind::package_member,
            {graph.scene_path_, 0, 0, std::nullopt, "mission-discovery"}};
        const auto diagnostic_before = graph.diagnostics_.size();
        add_reference(scene_id, reference);
        if (!required && graph.edges_.back().status == ResolutionStatus::missing) {
            graph.edges_.pop_back();
            graph.nodes_.pop_back();
            graph.diagnostics_.resize(diagnostic_before);
        }
    };

    const auto sibling = graph.scene_path_.parent_path() / graph.scene_path_.stem();
    for (const auto* extension : {".gsc", ".csc", ".vis"}) package_reference(sibling.string() + extension, true);
    package_reference(graph.package_root_ / "Maps" / "Secs" /
                          (graph.scene_path_.stem().string() + ".sec"), true);
    for (const auto* name : {"Anims.bdd", "Armas.bdd", "Efectos.bdd", "Materiales.bdd", "Objetos.bdd", "Sonidos.bdd"})
        package_reference(graph.package_root_ / "BDD" / name, true);
    for (const auto* extension : {".m3d", ".phd", ".and", ".txl", ".prp", ".dst", ".wad"}) {
        std::vector<std::filesystem::path> matches;
        std::error_code enumeration_error;
        std::filesystem::directory_iterator iterator(graph.scene_path_.parent_path(), enumeration_error), end;
        for (; iterator != end; iterator.increment(enumeration_error)) {
            if (enumeration_error) { enumeration_error.clear(); continue; }
            if (iterator->is_regular_file(enumeration_error) && !enumeration_error &&
                extension_lower(iterator->path().string()) == extension) matches.push_back(iterator->path());
            enumeration_error.clear();
        }
        std::ranges::sort(matches);
        for (const auto& match : matches) package_reference(match, false);
    }

    // Process only package/document/index nodes. Referenced payload nodes remain metadata-only.
    for (std::size_t node_index = 0; node_index < graph.nodes_.size(); ++node_index) {
        const auto node = graph.nodes_[node_index];
        if (node.state != LoadState::available) continue;
        AdapterResult adapter;
        bool adapted = true;
        try {
            switch (node.kind) {
            case ResourceKind::visual_index: adapter = read_vis(node.resolved_path); break;
            case ResourceKind::texture_index: adapter = read_txl(node.resolved_path); break;
            case ResourceKind::model_index: adapter = read_m3d(node.resolved_path); break;
            case ResourceKind::animation_index: adapter = read_and(node.resolved_path); break;
            case ResourceKind::physics_index: adapter = read_phd_candidates(node.resolved_path); break;
            default: adapted = false; break;
            }
        } catch (const std::exception& exception) {
            adapted = false;
            graph.diagnostics_.push_back({MissionDiagnostic::Severity::warning, "adapter-read-failed",
                exception.what(), SourceLocation{node.resolved_path, 0, 0, std::nullopt, "adapter"}});
        }
        if (adapted) {
            graph.diagnostics_.insert(graph.diagnostics_.end(), adapter.diagnostics.begin(), adapter.diagnostics.end());
            for (const auto& reference : adapter.references) add_reference(node.id, reference);
        }
        if (node.kind == ResourceKind::mission_scene || node.kind == ResourceKind::mission_script ||
            node.kind == ResourceKind::cutscene_script || node.kind == ResourceKind::database) {
          try {
            const auto document = Document::load(node.resolved_path);
            if (document.state() == ParseState::non_csffbs) {
                MissionDiagnostic diagnostic{MissionDiagnostic::Severity::error, "unexpected-type",
                    "Expected a CSFFBS document: " + node.resolved_path.string(),
                    SourceLocation{node.resolved_path, 0, std::min<std::uint64_t>(6, node.size),
                                   std::nullopt, "csffbs-sniff"}};
                graph.nodes_[node.id].diagnostics.push_back(diagnostic);
                graph.diagnostics_.push_back(std::move(diagnostic));
            }
            for (const auto& source_diagnostic : document.diagnostics()) {
                if (source_diagnostic.severity != Diagnostic::Severity::error) continue;
                MissionDiagnostic diagnostic{MissionDiagnostic::Severity::error, "csffbs-structural-error",
                    source_diagnostic.message,
                    SourceLocation{node.resolved_path, source_diagnostic.offset, 0,
                                   source_diagnostic.entry_index, "csffbs"}};
                graph.nodes_[node.id].diagnostics.push_back(diagnostic);
                graph.diagnostics_.push_back(std::move(diagnostic));
            }
            for (const auto& value : document.strings()) {
                auto text = value.display_utf8();
                static const std::set<std::string> path_extensions{
                    ".anm", ".bdd", ".cmo", ".csc", ".dds", ".dff", ".gsc", ".png", ".rpc", ".rws", ".tga", ".wad"};
                if (!plausible_path(text, path_extensions) || (text.find('/') == std::string::npos && text.find('\\') == std::string::npos)) continue;
                const auto extension = extension_lower(text);
                auto dependency = DependencyKind::document_reference;
                if (extension == ".anm") dependency = DependencyKind::animation;
                else if (extension == ".dff" || extension == ".rpc") dependency = DependencyKind::render_model;
                else if (extension == ".dds" || extension == ".png" || extension == ".tga") dependency = DependencyKind::texture;
                add_reference(node.id, {std::move(text), dependency,
                    {node.resolved_path, value.source.offset, value.source.size, value.table_index, "csffbs-string"}});
            }
          } catch (const std::exception& exception) {
            MissionDiagnostic diagnostic{MissionDiagnostic::Severity::warning, "document-read-failed",
                exception.what(), SourceLocation{node.resolved_path, 0, 0, std::nullopt, "csffbs"}};
            graph.nodes_[node.id].diagnostics.push_back(diagnostic);
            graph.diagnostics_.push_back(std::move(diagnostic));
          }
        }
    }
    std::ranges::sort(graph.edges_, [](const auto& left, const auto& right) {
        return std::tie(left.source, left.evidence.offset, left.original_reference) <
               std::tie(right.source, right.evidence.offset, right.original_reference);
    });
    return graph;
}

std::vector<const DependencyEdge*> MissionGraph::uses(const std::filesystem::path& path) const {
    std::vector<const DependencyEdge*> result;
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    for (const auto& edge : edges_) {
        if (!edge.target) continue;
        const auto& node = nodes_[*edge.target];
        if (node.state == LoadState::available &&
            ResourceIndex::normalize(node.resolved_path.lexically_normal().generic_string()) ==
                ResourceIndex::normalize(absolute.generic_string()))
            result.push_back(&edge);
    }
    return result;
}

std::string mission_graph_json(const MissionGraph& graph) {
    std::ostringstream output;
    output << "{\n  \"schema\": \"csf-mission-graph-1\",\n  \"scene\": \""
           << json_escape(graph.scene_path().generic_string()) << "\",\n  \"package_root\": \""
           << json_escape(graph.package_root().generic_string()) << "\",\n  \"roots\": [";
    for (std::size_t i = 0; i < graph.index().roots().size(); ++i) {
        if (i) output << ',';
        output << "\n    \"" << json_escape(graph.index().roots()[i].generic_string()) << '"';
    }
    if (!graph.index().roots().empty()) output << '\n';
    output << "  ],\n  \"nodes\": [";
    for (std::size_t i = 0; i < graph.nodes().size(); ++i) {
        const auto& node = graph.nodes()[i];
        if (i) output << ',';
        output << "\n    {\"id\": " << node.id << ", \"kind\": \"" << resource_kind_name(node.kind)
               << "\", \"path\": \"" << json_escape(node.resolved_path.generic_string())
               << "\", \"original_reference\": \"" << json_escape(node.original_reference)
               << "\", \"state\": \"" << load_state_name(node.state)
               << "\", \"size\": " << node.size << ", \"root_type\": ";
        if (node.root_type) output << *node.root_type; else output << "null";
        output << ", \"content_hash\": ";
        if (node.content_hash) output << '"' << std::hex << std::setw(16) << std::setfill('0')
                                      << *node.content_hash << std::dec << std::setfill(' ') << '"';
        else output << "null";
        output << ", \"candidates\": [";
        for (std::size_t candidate = 0; candidate < node.candidates.size(); ++candidate) {
            if (candidate) output << ", ";
            output << '"' << json_escape(node.candidates[candidate].generic_string()) << '"';
        }
        output << "]}";
    }
    if (!graph.nodes().empty()) output << '\n';
    output << "  ],\n  \"edges\": [";
    for (std::size_t i = 0; i < graph.edges().size(); ++i) {
        const auto& edge = graph.edges()[i];
        if (i) output << ',';
        output << "\n    {\"source\": " << edge.source << ", \"target\": ";
        if (edge.target) output << *edge.target; else output << "null";
        output << ", \"kind\": \"" << dependency_kind_name(edge.kind) << "\", \"status\": \""
               << resolution_status_name(edge.status) << "\", \"reference\": \""
               << json_escape(edge.original_reference) << "\", \"evidence\": {\"file\": \""
               << json_escape(edge.evidence.file.generic_string()) << "\", \"offset\": "
               << edge.evidence.offset << ", \"size\": " << edge.evidence.size << ", \"table_index\": ";
        if (edge.evidence.table_index) output << *edge.evidence.table_index; else output << "null";
        output << ", \"adapter\": \"" << json_escape(edge.evidence.adapter)
               << "\"}, \"candidates\": [";
        for (std::size_t candidate = 0; candidate < edge.candidates.size(); ++candidate) {
            if (candidate) output << ", ";
            output << '"' << json_escape(edge.candidates[candidate].generic_string()) << '"';
        }
        output << "]}";
    }
    if (!graph.edges().empty()) output << '\n';
    output << "  ],\n  \"diagnostics\": [";
    for (std::size_t i = 0; i < graph.diagnostics().size(); ++i) {
        const auto& diagnostic = graph.diagnostics()[i];
        if (i) output << ',';
        output << "\n    {\"severity\": \""
               << (diagnostic.severity == MissionDiagnostic::Severity::error ? "error" :
                   diagnostic.severity == MissionDiagnostic::Severity::warning ? "warning" : "note")
               << "\", \"code\": \"" << json_escape(diagnostic.code) << "\", \"message\": \""
               << json_escape(diagnostic.message) << "\", \"source\": ";
        if (diagnostic.source) {
            output << "{\"file\": \"" << json_escape(diagnostic.source->file.generic_string())
                   << "\", \"offset\": " << diagnostic.source->offset << ", \"size\": "
                   << diagnostic.source->size << ", \"table_index\": ";
            if (diagnostic.source->table_index) output << *diagnostic.source->table_index; else output << "null";
            output << ", \"adapter\": \"" << json_escape(diagnostic.source->adapter) << "\"}";
        } else output << "null";
        output << '}';
    }
    if (!graph.diagnostics().empty()) output << '\n';
    output << "  ]\n}\n";
    return output.str();
}

const char* resource_kind_name(const ResourceKind kind) noexcept {
    switch (kind) {
    case ResourceKind::mission_scene: return "mission-scene";
    case ResourceKind::mission_script: return "mission-script";
    case ResourceKind::cutscene_script: return "cutscene-script";
    case ResourceKind::security_data: return "security-data";
    case ResourceKind::database: return "database";
    case ResourceKind::visual_index: return "visual-index";
    case ResourceKind::model_index: return "model-index";
    case ResourceKind::physics_index: return "physics-index";
    case ResourceKind::animation_index: return "animation-index";
    case ResourceKind::texture_index: return "texture-index";
    case ResourceKind::visual_map: return "visual-map";
    case ResourceKind::collision_map: return "collision-map";
    case ResourceKind::render_model: return "render-model";
    case ResourceKind::physics_body: return "physics-body";
    case ResourceKind::ragdoll: return "ragdoll";
    case ResourceKind::animation: return "animation";
    case ResourceKind::collision_shape: return "collision-shape";
    case ResourceKind::texture: return "texture";
    case ResourceKind::particle_system: return "particle-system";
    case ResourceKind::audio_bank: return "audio-bank";
    case ResourceKind::spatial_data: return "spatial-data";
    case ResourceKind::unknown: return "unknown";
    }
    return "unknown";
}

const char* dependency_kind_name(const DependencyKind kind) noexcept {
    switch (kind) {
    case DependencyKind::package_member: return "package-member";
    case DependencyKind::visual_map: return "visual-map";
    case DependencyKind::collision_map: return "collision-map";
    case DependencyKind::texture_directory: return "texture-directory";
    case DependencyKind::sky_model: return "sky-model";
    case DependencyKind::render_model: return "render-model";
    case DependencyKind::physics_body: return "physics-body";
    case DependencyKind::animation: return "animation";
    case DependencyKind::texture: return "texture";
    case DependencyKind::document_reference: return "document-reference";
    }
    return "document-reference";
}

const char* resolution_status_name(const ResolutionStatus status) noexcept {
    switch (status) {
    case ResolutionStatus::explicit_path: return "explicit";
    case ResolutionStatus::exact: return "exact";
    case ResolutionStatus::case_mismatch: return "case-mismatch";
    case ResolutionStatus::mapped_dff_to_rpc: return "dff-to-rpc";
    case ResolutionStatus::shared_root: return "shared-root";
    case ResolutionStatus::shared_root_case_mismatch: return "shared-root-case-mismatch";
    case ResolutionStatus::ambiguous: return "ambiguous";
    case ResolutionStatus::missing: return "missing";
    case ResolutionStatus::outside_root: return "outside-root";
    }
    return "missing";
}

} // namespace csf
