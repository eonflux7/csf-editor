#include "csf/mission_edit.hpp"

#include "csf/authoring.hpp"
#include "csf/mission.hpp"
#include "csf/mod_project.hpp"
#include "rws/model_edit.hpp"
#include "csf/script_signatures.hpp"
#include "csf/source_text.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <numbers>
#include <set>
#include <sstream>

namespace csf {
namespace {

constexpr float degrees_to_radians = std::numbers::pi_v<float> / 180.0F;

std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

// Windows resolves package paths case-insensitively and accepts either separator.
std::string path_key(const std::filesystem::path& path) {
    auto key = path_utf8(path);
    std::ranges::replace(key, '\\', '/');
    std::ranges::transform(key, key.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return key;
}

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Cannot open " + path_utf8(path));
    const auto size = input.tellg();
    if (size < 0) throw std::runtime_error("Cannot size " + path_utf8(path));
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input && !bytes.empty()) throw std::runtime_error("Cannot read " + path_utf8(path));
    return bytes;
}

void write_file_atomically(const std::filesystem::path& path, const std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!output) throw std::runtime_error("Cannot write " + path_utf8(temporary));
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    if (error) throw std::runtime_error("Cannot publish " + path_utf8(path) + ": " + error.message());
}

std::string json_escape(const std::string_view value) {
    std::ostringstream out;
    for (const unsigned char c : value) {
        switch (c) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                              << static_cast<unsigned>(c) << std::dec;
            else out << static_cast<char>(c);
        }
    }
    return out.str();
}

std::string to_1252(const std::string_view utf8, const char* what) {
    auto converted = utf8_to_windows_1252(utf8);
    if (!converted)
        throw std::invalid_argument(std::string(what) + " contains a character outside Windows-1252");
    return std::move(*converted);
}

std::string display(const std::string_view windows_1252) { return windows_1252_to_utf8(windows_1252); }

bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

Vec3 add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }

float distance_squared(const Vec3& a, const Vec3& b) {
    const auto x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return x * x + y * y + z * z;
}

// ---- Tree access -----------------------------------------------------------

std::optional<float> number(const TreeNode* node) {
    if (!node) return std::nullopt;
    if (const auto value = node->as_real()) return *value;
    if (const auto value = node->as_int()) return static_cast<float>(*value);
    return std::nullopt;
}

std::optional<std::int32_t> integer(const TreeNode* node) {
    return node ? node->as_int() : std::nullopt;
}

// Keeps the stored kind: integer fields stay integers.
void assign_number(TreeNode& node, const float value) {
    if (node.kind == ValueKind::integer) node.set_int(static_cast<std::int32_t>(std::lround(value)));
    else node.set_real(value);
}

std::optional<Vec3> vec3(const TreeNode* node) {
    if (!node || node->children.size() != 3) return std::nullopt;
    const auto x = number(&node->children[0]), y = number(&node->children[1]),
               z = number(&node->children[2]);
    if (!x || !y || !z) return std::nullopt;
    return Vec3{*x, *y, *z};
}

void assign_vec3(TreeNode& node, const Vec3& value) {
    if (node.children.size() != 3) throw std::runtime_error("Position does not have three components");
    assign_number(node.children[0], value.x);
    assign_number(node.children[1], value.y);
    assign_number(node.children[2], value.z);
}

TreeNode make_vec3(const std::string_view label, const Vec3& value) {
    auto node = TreeNode::group(label);
    node.children = {TreeNode::real(std::nullopt, value.x), TreeNode::real(std::nullopt, value.y),
                     TreeNode::real(std::nullopt, value.z)};
    return node;
}

bool same_position(const Vec3& a, const Vec3& b) { return distance_squared(a, b) <= 1e-4F; }

TreeNode& root_of(Tree& tree) {
    if (tree.roots.size() != 1 || !tree.roots.front().is_container())
        throw std::runtime_error("CSFFBS file does not have a single root record");
    return tree.roots.front();
}

const TreeNode& root_of(const Tree& tree) { return root_of(const_cast<Tree&>(tree)); }

TreeNode& required(TreeNode& parent, const std::string_view label) {
    auto* found = parent.child(label);
    if (!found) throw std::runtime_error("Missing " + std::string(label));
    return *found;
}

std::optional<std::int32_t> record_id(const TreeNode& record) { return integer(record.child(".ID")); }

TreeNode* find_record(TreeNode& list, const std::int32_t id) {
    for (auto& record : list.children)
        if (record_id(record) == id) return &record;
    return nullptr;
}

std::int32_t next_id(const TreeNode& list) {
    std::int32_t maximum = 0;
    for (const auto& record : list.children)
        if (const auto id = record_id(record)) maximum = std::max(maximum, *id);
    return maximum + 1;
}

// Adds a record to an ID-keyed list. Shipped lists are almost always sorted by
// .ID, so a sorted list stays sorted; any other list gets the record appended.
void insert_record(TreeNode& list, TreeNode record) {
    const auto id = record_id(record);
    std::vector<std::int32_t> ids;
    for (const auto& value : list.children)
        if (const auto existing = record_id(value)) ids.push_back(*existing);
    if (!id || ids.size() != list.children.size() || !std::ranges::is_sorted(ids)) {
        list.children.push_back(std::move(record));
        return;
    }
    const auto position = std::ranges::lower_bound(ids, *id) - ids.begin();
    list.children.insert(list.children.begin() + position, std::move(record));
}

// Inserts `node` before the first sibling that the canonical order places after it.
void insert_ordered(TreeNode& record, TreeNode node, const std::span<const std::string_view> order) {
    const auto rank = [&](const std::string_view name) -> std::optional<std::size_t> {
        const auto found = std::ranges::find(order, name);
        if (found == order.end()) return std::nullopt;
        return static_cast<std::size_t>(found - order.begin());
    };
    const auto own = rank(node.name());
    auto position = record.children.end();
    if (own)
        for (auto it = record.children.begin(); it != record.children.end(); ++it)
            if (const auto other = rank(it->name()); other && *other > *own) {
                position = it;
                break;
            }
    record.children.insert(position, std::move(node));
}

void remove_child(TreeNode& record, const std::string_view label) {
    std::erase_if(record.children, [&](const TreeNode& child) { return child.label && child.is(label); });
}

constexpr std::array<std::string_view, 15> actor_order{
    ".NOMBRE", ".ID", ".CLASSID", ".POS", ".ANGULO", ".ANGULO_X", ".COLISION", ".FLAGS",
    ".BANDO", ".PORTRAIT", ".SEGUNDA_EXPLOSION", ".SCRIPT", ".DOOR_BOX", ".ANIMACIONES", ".CELDA"};

struct SceneView {
    TreeNode& root;
    TreeNode& actors() { return required(root, ".BICHOS"); }
    TreeNode& navigation() { return required(root, ".MALLA_NAVEGACION"); }
    TreeNode& groups() { return required(navigation(), ".GRUPOS"); }
    TreeNode& dummies() { return required(required(root, ".MALLA_DUMMIES"), ".DUMMIES"); }
    TreeNode& lights() { return required(required(root, ".MALLA_LUCES"), ".LIGHTS"); }
    TreeNode& areas() { return required(required(root, ".MALLA_AREAS"), ".AREAS"); }

    TreeNode& actor(const std::int32_t id) {
        auto* found = find_record(actors(), id);
        if (!found) throw std::invalid_argument("Actor " + std::to_string(id) + " does not exist");
        return *found;
    }
    TreeNode* group(const std::int32_t id) { return find_record(groups(), id); }
    TreeNode* point(const std::int32_t group_id, const std::int32_t point_id) {
        auto* owner = group(group_id);
        if (!owner) return nullptr;
        auto* points = owner->child(".PUNTOS");
        return points ? find_record(*points, point_id) : nullptr;
    }
};

std::optional<std::pair<std::int32_t, std::int32_t>> actor_cell(const TreeNode& actor) {
    const auto* cell = actor.child(".CELDA");
    if (!cell) return std::nullopt;
    const auto group = integer(cell->child(".GRUPO")), point = integer(cell->child(".PUNTO"));
    if (!group || !point || *group < 0 || *point < 0) return std::nullopt;
    return std::pair{*group, *point};
}

void set_actor_cell(TreeNode& actor, const std::int32_t group, const std::int32_t point) {
    if (auto* cell = actor.child(".CELDA")) {
        if (auto* value = cell->child(".GRUPO")) value->set_int(group);
        if (auto* value = cell->child(".PUNTO")) value->set_int(point);
        return;
    }
    auto cell = TreeNode::array(".CELDA");
    cell.children = {TreeNode::integer(".GRUPO", group), TreeNode::integer(".PUNTO", point)};
    insert_ordered(actor, std::move(cell), actor_order);
}

std::string actor_label(const TreeNode& actor) {
    const auto* name = actor.child(".NOMBRE");
    const auto id = record_id(actor).value_or(-1);
    if (name && name->as_string() && !name->as_string()->empty())
        return display(*name->as_string()) + " (" + std::to_string(id) + ")";
    return "actor " + std::to_string(id);
}

// Removes `id` from every folder ELEMENTOS list below `node`.
void remove_from_folders(TreeNode& node, const std::int32_t id) {
    if (node.is(".ELEMENTOS")) {
        std::erase_if(node.children, [&](const TreeNode& value) { return value.as_int() == id; });
        return;
    }
    for (auto& child : node.children) remove_from_folders(child, id);
}

// Adds `id` after `after` in the folders that list `after`; when `after` is
// absent, adds it to the first folder list.
void add_to_folders(TreeNode& node, const std::int32_t id, const std::optional<std::int32_t> after,
                    bool& placed) {
    if (node.is(".ELEMENTOS")) {
        if (after) {
            const auto found = std::ranges::find_if(node.children, [&](const TreeNode& value) {
                return value.as_int() == *after;
            });
            if (found != node.children.end()) {
                node.children.insert(found + 1, TreeNode::integer(std::nullopt, id));
                placed = true;
            }
        } else if (!placed) {
            node.children.push_back(TreeNode::integer(std::nullopt, id));
            placed = true;
        }
        return;
    }
    for (auto& child : node.children) add_to_folders(child, id, after, placed);
}

// Every operand group (TAG value...) in a program tree.
template <class Visitor>
void visit_operands(const TreeNode& node, Visitor&& visitor) {
    if (node.kind == ValueKind::group && node.children.size() >= 2)
        if (const auto tag = node.children.front().as_string()) visitor(*tag, node);
    for (const auto& child : node.children) visit_operands(child, visitor);
}

std::string file_display(const MissionFile& file) { return path_utf8(file.relative_path); }

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

MissionFileKind kind_for(const ResourceKind kind, const std::filesystem::path& path) {
    const auto name = lower(path_utf8(path.filename()));
    switch (kind) {
    case ResourceKind::mission_scene: return MissionFileKind::scene;
    case ResourceKind::mission_script: return MissionFileKind::mission_script;
    case ResourceKind::cutscene_script: return MissionFileKind::cutscene_script;
    case ResourceKind::database:
        if (name == "objetos.bdd") return MissionFileKind::objects;
        if (name == "anims.bdd") return MissionFileKind::animations;
        if (name == "armas.bdd") return MissionFileKind::weapons;
        return MissionFileKind::database;
    case ResourceKind::model_index: return MissionFileKind::model_index;
    case ResourceKind::animation_index: return MissionFileKind::animation_index;
    case ResourceKind::texture_index: return MissionFileKind::texture_index;
    case ResourceKind::physics_index: return MissionFileKind::physics_index;
    case ResourceKind::visual_index: return MissionFileKind::visual_index;
    case ResourceKind::visual_map: return MissionFileKind::visual_map;
    default: return MissionFileKind::asset;
    }
}

bool is_program(const MissionFileKind kind) {
    return kind == MissionFileKind::mission_script || kind == MissionFileKind::cutscene_script;
}

// ---- Length-prefixed package indexes (.m3d, .and) ---------------------------

struct IndexRecord {
    std::string path; // Windows-1252, without terminator
    std::vector<std::byte> suffix;
};

std::uint32_t read_u32(const std::span<const std::byte> bytes, const std::size_t at) {
    if (at + 4 > bytes.size()) throw std::runtime_error("Truncated package index");
    return std::to_integer<std::uint32_t>(bytes[at]) | (std::to_integer<std::uint32_t>(bytes[at + 1]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[at + 2]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[at + 3]) << 24U);
}

void append_u32(std::vector<std::byte>& out, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
}

// Records are u32 length, path bytes and a fixed-size suffix (four 0xFF bytes in
// .m3d, two flag bytes in .and); a zero length ends the list.
std::vector<IndexRecord> read_index(const std::span<const std::byte> bytes, const std::size_t suffix) {
    std::vector<IndexRecord> records;
    std::size_t at = 0;
    while (true) {
        const auto length = read_u32(bytes, at);
        at += 4;
        if (length == 0) break;
        if (at + length + suffix > bytes.size()) throw std::runtime_error("Truncated package index record");
        IndexRecord record;
        record.path.assign(reinterpret_cast<const char*>(bytes.data() + at), length);
        at += length;
        record.suffix.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                             bytes.begin() + static_cast<std::ptrdiff_t>(at + suffix));
        at += suffix;
        records.push_back(std::move(record));
    }
    if (at != bytes.size()) throw std::runtime_error("Package index has trailing bytes");
    return records;
}

std::vector<std::byte> write_index(const std::vector<IndexRecord>& records) {
    std::vector<std::byte> out;
    for (const auto& record : records) {
        append_u32(out, static_cast<std::uint32_t>(record.path.size()));
        const auto* data = reinterpret_cast<const std::byte*>(record.path.data());
        out.insert(out.end(), data, data + record.path.size());
        out.insert(out.end(), record.suffix.begin(), record.suffix.end());
    }
    append_u32(out, 0);
    return out;
}

// Texture names referenced by the Texture chunks (0x06) of a RenderWare stream.
void collect_texture_names(const std::span<const std::byte> bytes, std::size_t begin,
                           const std::size_t end, std::set<std::string>& names, const int depth) {
    if (depth > 64) return;
    while (begin + 12 <= end) {
        const auto type = read_u32(bytes, begin);
        const auto size = read_u32(bytes, begin + 4);
        const auto payload = begin + 12;
        if (size > end - payload) return;
        if (type == 0x06) {
            // Texture: Struct, String (name), String (mask), Extension.
            auto child = payload;
            int strings = 0;
            while (child + 12 <= payload + size) {
                const auto child_type = read_u32(bytes, child);
                const auto child_size = read_u32(bytes, child + 4);
                if (child_size > payload + size - child - 12) break;
                if (child_type == 0x02 && strings++ == 0) {
                    std::string name(reinterpret_cast<const char*>(bytes.data() + child + 12), child_size);
                    name = name.substr(0, name.find('\0'));
                    if (!name.empty()) names.insert(lower(name));
                }
                child += 12 + child_size;
            }
        } else if (type != 0x01 && type != 0x02 && size >= 12) {
            // Containers hold complete child chunks; payloads that do not parse
            // as chunks simply stop the recursive scan.
            collect_texture_names(bytes, payload, payload + size, names, depth + 1);
        }
        begin = payload + size;
    }
}

// A package path as stored (Windows-1252, backslashes) to a generic path.
std::filesystem::path windows_path_to_generic(const std::string_view windows_1252) {
    auto text = windows_1252_to_utf8(windows_1252);
    std::ranges::replace(text, '\\', '/');
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string windows_path(std::string value) {
    std::ranges::replace(value, '/', '\\');
    return value;
}

} // namespace

// ---- Files and caches -------------------------------------------------------

std::vector<std::byte> MissionFile::bytes() const { return tree ? tree->serialize() : raw; }

struct MissionEditor::Cache {
    std::uint64_t scene_revision{~0ULL};
    std::optional<Document> scene_document;
    std::optional<MissionScene> scene;
    std::uint64_t objects_revision{~0ULL};
    std::optional<ObjectDatabase> objects;
    std::uint64_t animations_revision{~0ULL};
    std::optional<AnimationCatalog> animations;
    std::map<std::size_t, std::pair<std::uint64_t, bool>> modified;
};

MissionEditor::MissionEditor() : cache_(std::make_shared<Cache>()) {}

std::size_t MissionEditor::add_file(MissionFile file) {
    const auto key = path_key(file.relative_path);
    for (std::size_t i = 0; i < files_.size(); ++i)
        if (path_key(files_[i].relative_path) == key) {
            if (files_[i].present) throw std::runtime_error("Mission file already exists: " + key);
            file.revision = files_[i].revision + 1;
            files_[i] = std::move(file);
            return i;
        }
    files_.push_back(std::move(file));
    baselines_.emplace_back();
    return files_.size() - 1;
}

MissionEditor MissionEditor::open(const std::filesystem::path& scene,
                                  const std::filesystem::path& package_root,
                                  const ModProject* project, const ResourceIndex* prepared_index) {
    MissionEditor editor;
    const auto root = std::filesystem::weakly_canonical(package_root);
    editor.package_root_ = root;
    MissionOptions options;
    options.input = std::filesystem::weakly_canonical(scene);
    options.package_root = root;
    options.prepared_index = prepared_index;
    const auto graph = MissionGraph::load(options);

    std::map<std::string, const ModFile*> authored;
    if (project)
        for (const auto& file : project->files) authored.emplace(path_key(file.relative_path), &file);

    std::set<std::string> loaded;
    const auto load = [&](const std::filesystem::path& resolved, const MissionFileKind kind) {
        const auto source = std::filesystem::weakly_canonical(resolved);
        auto relative = source.lexically_relative(root);
        if (relative.empty() || *relative.begin() == "..") return;
        const auto key = path_key(relative);
        if (!loaded.insert(key).second) return;
        MissionFile file;
        file.kind = kind;
        file.relative_path = relative;
        file.source_path = source;
        auto baseline = read_file(source);
        file.source_sha256 = sha256(baseline);
        auto content = baseline;
        if (const auto found = authored.find(key); found != authored.end()) {
            content = read_file(found->second->authored_path);
            file.source_path = found->second->authored_path;
        }
        if (Document::sniff(content)) file.tree = Tree::from_bytes(std::move(content));
        else file.raw = std::move(content);
        if (kind == MissionFileKind::scene) editor.scene_file_ = editor.files_.size();
        editor.files_.push_back(std::move(file));
        editor.baselines_.push_back(std::move(baseline));
    };

    load(graph.scene_path(), MissionFileKind::scene);
    for (const auto& node : graph.nodes()) {
        if (node.state != LoadState::available || node.resolved_path.empty()) continue;
        const auto kind = kind_for(node.kind, node.resolved_path);
        if (kind == MissionFileKind::asset || kind == MissionFileKind::scene) continue;
        load(node.resolved_path, kind);
    }
    if (editor.files_.empty() || editor.files_[editor.scene_file_].kind != MissionFileKind::scene)
        throw std::runtime_error("The mission scene is not inside the package root");
    if (!editor.files_[editor.scene_file_].tree)
        throw std::runtime_error("The mission scene is not a CSFFBS document");

    // Files that only the project has (imported models, animations, ...).
    if (project)
        for (const auto& file : project->files) {
            const auto key = path_key(file.relative_path);
            if (loaded.contains(key)) continue;
            MissionFile added;
            added.relative_path = file.relative_path;
            added.source_path = file.authored_path;
            const auto source = root / file.relative_path;
            added.added = !std::filesystem::is_regular_file(source);
            std::vector<std::byte> baseline;
            if (!added.added) {
                baseline = read_file(source);
                added.source_sha256 = sha256(baseline);
            }
            added.raw = read_file(file.authored_path);
            editor.files_.push_back(std::move(added));
            editor.baselines_.push_back(std::move(baseline));
            loaded.insert(key);
        }
    return editor;
}

std::optional<std::size_t> MissionEditor::find_file(const std::filesystem::path& relative) const {
    const auto key = path_key(relative);
    for (std::size_t i = 0; i < files_.size(); ++i)
        if (files_[i].present && path_key(files_[i].relative_path) == key) return i;
    return std::nullopt;
}

std::optional<std::size_t> MissionEditor::file_of_kind(const MissionFileKind kind) const {
    for (std::size_t i = 0; i < files_.size(); ++i)
        if (files_[i].present && files_[i].kind == kind) return i;
    return std::nullopt;
}

ResourceIndex MissionEditor::resource_index(const ResourceIndex& base) const {
    auto index = base;
    std::error_code error;
    for (const auto& file : files_) {
        if (!file.present || file.source_path.empty()) continue;
        const auto package = package_root_ / file.relative_path;
        if (std::filesystem::exists(file.source_path, error) &&
            std::filesystem::equivalent(file.source_path, package, error))
            continue;
        index.add_overlay(file.relative_path, file.source_path);
    }
    return index;
}

std::filesystem::path MissionEditor::package_path(const std::size_t file) const {
    return package_root_ / files_.at(file).relative_path;
}

Document MissionEditor::document(const std::size_t file) const {
    const auto& value = files_.at(file);
    if (!value.tree) throw std::runtime_error(file_display(value) + " is not a CSFFBS document");
    return Document::from_bytes(value.tree->serialize(), package_path(file));
}

const Document& MissionEditor::scene_document() const {
    const auto revision = files_[scene_file_].revision;
    if (cache_->scene_revision != revision || !cache_->scene_document) {
        cache_->scene_document = document(scene_file_);
        cache_->scene = MissionScene::project(*cache_->scene_document);
        cache_->scene_revision = revision;
    }
    return *cache_->scene_document;
}

const MissionScene& MissionEditor::scene() const {
    (void)scene_document();
    return *cache_->scene;
}

const ObjectDatabase& MissionEditor::objects() const {
    const auto file = file_of_kind(MissionFileKind::objects);
    const auto revision = file ? files_[*file].revision : 0;
    if (!cache_->objects || cache_->objects_revision != revision) {
        cache_->objects = file ? ObjectDatabase::project(document(*file)) : ObjectDatabase{};
        cache_->objects_revision = revision;
    }
    return *cache_->objects;
}

const AnimationCatalog& MissionEditor::animations() const {
    const auto file = file_of_kind(MissionFileKind::animations);
    const auto revision = file ? files_[*file].revision : 0;
    if (!cache_->animations || cache_->animations_revision != revision) {
        cache_->animations = file ? AnimationCatalog::project(document(*file)) : AnimationCatalog{};
        cache_->animations_revision = revision;
    }
    return *cache_->animations;
}

std::vector<std::size_t> MissionEditor::modified_files() const {
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < files_.size(); ++i) {
        const auto& file = files_[i];
        auto& cached = cache_->modified[i];
        if (cached.first != file.revision + 1) {
            cached.first = file.revision + 1;
            cached.second = file.present &&
                            (file.added || (file.tree ? file.tree->serialize() != baselines_[i]
                                                      : file.raw != baselines_[i]));
        }
        if (cached.second) result.push_back(i);
    }
    return result;
}

std::vector<std::string> MissionEditor::history_labels() const {
    std::vector<std::string> labels;
    for (const auto& entry : history_) labels.push_back(entry.label);
    return labels;
}

void MissionEditor::changed(const std::size_t file) {
    ++files_[file].revision;
    ++revision_;
}

void MissionEditor::restore(const FileSnapshot& snapshot, const bool forward) {
    auto& file = files_.at(snapshot.file);
    if (!snapshot.patches.empty()) {
        for (const auto& patch : snapshot.patches) {
            const auto& bytes = forward ? patch.after : patch.before;
            std::ranges::copy(bytes, file.raw.begin() + static_cast<std::ptrdiff_t>(patch.offset));
        }
        changed(snapshot.file);
        return;
    }
    file.present = forward ? snapshot.present_after : snapshot.present_before;
    const auto& bytes = forward ? snapshot.after : snapshot.before;
    if (file.present) {
        if (file.tree) file.tree = Tree::from_bytes(bytes);
        else file.raw = bytes;
    }
    changed(snapshot.file);
}

bool MissionEditor::undo() {
    if (!can_undo()) return false;
    --cursor_;
    // Backwards: a batch holds several snapshots of the same file in order.
    const auto& files = history_[cursor_].files;
    for (auto snapshot = files.rbegin(); snapshot != files.rend(); ++snapshot) restore(*snapshot, false);
    return true;
}

bool MissionEditor::redo() {
    if (!can_redo()) return false;
    for (const auto& snapshot : history_[cursor_].files) restore(snapshot, true);
    ++cursor_;
    return true;
}

// ---- Transactions -----------------------------------------------------------

class MissionTransaction {
public:
    MissionTransaction(MissionEditor& editor, std::string label)
        : editor_(editor), label_(std::move(label)) {}
    MissionTransaction(const MissionTransaction&) = delete;
    MissionTransaction& operator=(const MissionTransaction&) = delete;
    ~MissionTransaction() {
        if (!finished_) rollback();
    }

    Tree& tree(const std::size_t index) {
        touch(index);
        auto& file = editor_.files_.at(index);
        if (!file.tree) throw std::runtime_error(file_display(file) + " is not a CSFFBS document");
        return *file.tree;
    }
    std::vector<std::byte>& raw(const std::size_t index) {
        touch(index);
        return editor_.files_.at(index).raw;
    }
    SceneView scene() { return SceneView{root_of(tree(editor_.scene_file_))}; }

    std::size_t add_file(MissionFile file) {
        file.present = true;
        file.added = !std::filesystem::is_regular_file(editor_.package_root_ / file.relative_path);
        const auto index = editor_.add_file(std::move(file));
        snapshots_.emplace(index, MissionEditor::FileSnapshot{index, false, true, {}, {}});
        return index;
    }

    EditResult commit(std::string message, std::vector<std::string> warnings = {}) {
        // Validate every touched file before anything is published.
        for (auto& [index, snapshot] : snapshots_) {
            auto& file = editor_.files_[index];
            if (!file.present) continue;
            snapshot.after = file.bytes();
            if (!file.tree) continue;
            const auto parsed = Document::from_bytes(snapshot.after, editor_.package_path(index));
            if (parsed.has_errors() || parsed.state() != ParseState::exact)
                return reject("The edit produced an invalid CSFFBS document; nothing was changed");
            if (file.kind == MissionFileKind::scene && snapshot.present_before) {
                const auto scene = MissionScene::project(parsed);
                const auto before = MissionScene::project(Document::from_bytes(snapshot.before));
                const auto errors = [](const MissionScene& value) {
                    return std::ranges::count_if(value.diagnostics(), [](const auto& d) {
                        return d.severity == Diagnostic::Severity::error;
                    });
                };
                if (errors(scene) > errors(before))
                    warnings.push_back("The scene now has more structural diagnostics");
                if (scene.navigation_stats().invalid_connections >
                    before.navigation_stats().invalid_connections)
                    warnings.push_back("A navigation connection now points to a missing point");
            }
            if (is_program(file.kind)) {
                const auto program = ProgramDocument::project(parsed);
                for (const auto& diagnostic : program.diagnostics())
                    if (diagnostic.code == "unbalanced-program-marker" ||
                        diagnostic.code == "mismatched-program-marker" ||
                        diagnostic.code == "duplicate-script-id")
                        warnings.push_back(diagnostic.message);
            }
        }
        std::vector<MissionEditor::FileSnapshot> changed;
        for (auto& [index, snapshot] : snapshots_) {
            if (snapshot.present_before == snapshot.present_after && snapshot.before == snapshot.after)
                continue;
            // Large raw files edited in place keep only their changed runs.
            if (!editor_.files_[index].tree && snapshot.present_before && snapshot.present_after &&
                snapshot.before.size() == snapshot.after.size()) {
                for (std::size_t at = 0; at < snapshot.before.size();) {
                    if (snapshot.before[at] == snapshot.after[at]) {
                        ++at;
                        continue;
                    }
                    auto end = at;
                    while (end < snapshot.before.size() && snapshot.before[end] != snapshot.after[end]) ++end;
                    const auto first = static_cast<std::ptrdiff_t>(at), last = static_cast<std::ptrdiff_t>(end);
                    snapshot.patches.push_back({at, {snapshot.before.begin() + first, snapshot.before.begin() + last},
                                                {snapshot.after.begin() + first, snapshot.after.begin() + last}});
                    at = end;
                }
                snapshot.before.clear();
                snapshot.before.shrink_to_fit();
                snapshot.after.clear();
                snapshot.after.shrink_to_fit();
            }
            changed.push_back(std::move(snapshot));
        }
        snapshots_.clear();
        finished_ = true;
        if (changed.empty()) return {false, "No change: the values are already set", std::move(warnings)};
        if (editor_.cursor_ < editor_.history_.size()) {
            editor_.history_.erase(editor_.history_.begin() + static_cast<std::ptrdiff_t>(editor_.cursor_),
                                   editor_.history_.end());
            if (editor_.saved_position_ > editor_.cursor_) editor_.saved_position_ = ~std::size_t{};
        }
        for (const auto& snapshot : changed) editor_.changed(snapshot.file);
        editor_.history_.push_back({label_, std::move(changed)});
        ++editor_.cursor_;
        std::ranges::sort(warnings);
        warnings.erase(std::ranges::unique(warnings).begin(), warnings.end());
        return {true, std::move(message), std::move(warnings)};
    }

    EditResult reject(std::string message) {
        rollback();
        return {false, std::move(message), {}};
    }

private:
    void touch(const std::size_t index) {
        if (snapshots_.contains(index)) return;
        const auto& file = editor_.files_.at(index);
        if (!file.present) throw std::runtime_error("Mission file is not present");
        snapshots_.emplace(index, MissionEditor::FileSnapshot{index, true, true, file.bytes(), {}});
    }

    void rollback() {
        for (const auto& [index, snapshot] : snapshots_) {
            auto& file = editor_.files_[index];
            file.present = snapshot.present_before;
            if (snapshot.present_before) {
                if (file.tree) file.tree = Tree::from_bytes(snapshot.before);
                else file.raw = snapshot.before;
            }
            // Projections read during the transaction may reflect the edit.
            editor_.changed(index);
        }
        snapshots_.clear();
        finished_ = true;
    }

    MissionEditor& editor_;
    std::string label_;
    std::map<std::size_t, MissionEditor::FileSnapshot> snapshots_;
    bool finished_{};
};

namespace {

// Runs an edit; argument and lookup errors become a rejected result.
template <class Body>
EditResult run(MissionEditor& editor, std::string label, Body&& body) {
    MissionTransaction transaction(editor, std::move(label));
    try {
        return body(transaction);
    } catch (const std::invalid_argument& error) {
        return transaction.reject(error.what());
    } catch (const SourceTextError& error) {
        return transaction.reject(error.what());
    } catch (const std::runtime_error& error) {
        return transaction.reject(std::string("Edit failed: ") + error.what());
    }
}

} // namespace

// ---- References -----------------------------------------------------------

std::size_t MissionEditor::script_references(const std::string_view tag, const std::int32_t id) const {
    std::size_t count = 0;
    for (const auto& file : files_) {
        if (!file.present || !is_program(file.kind) || !file.tree) continue;
        for (const auto& root : file.tree->roots) {
            const auto* scripts = root.child(".SCRIPTS");
            if (!scripts) continue;
            visit_operands(*scripts, [&](const std::string_view operand, const TreeNode& node) {
                if (operand == tag && node.children[1].as_int() == id) ++count;
            });
        }
    }
    return count;
}

namespace {

std::size_t pathpoint_references(const std::vector<MissionFile>& files, const std::int32_t group,
                                 const std::int32_t point) {
    std::size_t count = 0;
    for (const auto& file : files) {
        if (!file.present || !is_program(file.kind) || !file.tree) continue;
        for (const auto& root : file.tree->roots)
            if (const auto* scripts = root.child(".SCRIPTS"))
                visit_operands(*scripts, [&](const std::string_view tag, const TreeNode& node) {
                    if (tag == "PATHPOINT" && node.children.size() >= 3 &&
                        node.children[1].as_int() == group && node.children[2].as_int() == point)
                        ++count;
                });
    }
    return count;
}

} // namespace

// ---- Actors ------------------------------------------------------------------

EditResult MissionEditor::set_actor_placement(const std::int32_t actor_id, const ActorPlacement& placement) {
    return run(*this, "Move actor " + std::to_string(actor_id), [&](MissionTransaction& t) {
        if (!finite(placement.position) || !std::isfinite(placement.heading_degrees) ||
            !std::isfinite(placement.pitch_degrees))
            throw std::invalid_argument("Position and angles must be finite");
        auto scene = t.scene();
        auto& actor = scene.actor(actor_id);
        auto& position = required(actor, ".POS");
        const auto old_position = vec3(&position);
        const auto old_pitch = number(actor.child(".ANGULO_X")).value_or(0);
        assign_vec3(position, placement.position);
        assign_number(required(actor, ".ANGULO"), placement.heading_degrees);
        if (auto* pitch = actor.child(".ANGULO_X")) assign_number(*pitch, placement.pitch_degrees);
        std::vector<std::string> warnings;
        if (const auto cell = actor_cell(actor)) {
            if (auto* point = scene.point(cell->first, cell->second)) {
                auto* point_position = point->child(".POS");
                const auto mirrored = point_position && old_position && vec3(point_position) &&
                                      same_position(*vec3(point_position), *old_position);
                if (mirrored) {
                    assign_vec3(*point_position, placement.position);
                    if (auto* rotation = point->child(".ROT"))
                        assign_number(*rotation, placement.heading_degrees * degrees_to_radians);
                    if (auto* pitch = point->child(".ROT_X");
                        pitch && std::abs(number(pitch).value_or(0) - old_pitch * degrees_to_radians) < 1e-4F)
                        assign_number(*pitch, placement.pitch_degrees * degrees_to_radians);
                } else {
                    warnings.push_back("Placement point " + std::to_string(cell->first) + "/" +
                                       std::to_string(cell->second) +
                                       " did not mirror the actor and was left in place");
                }
            }
        }
        return t.commit("Moved " + actor_label(actor), std::move(warnings));
    });
}

EditResult MissionEditor::set_actor_class(const std::int32_t actor_id, const std::int32_t class_id,
                                          const bool force) {
    const auto definitions = objects().find_class(class_id);
    return run(*this, "Change class of actor " + std::to_string(actor_id), [&](MissionTransaction& t) {
        if (definitions.empty() && !force)
            throw std::invalid_argument("Class " + std::to_string(class_id) +
                                        " is not in this mission's Objetos.bdd; import it first");
        auto scene = t.scene();
        auto& actor = scene.actor(actor_id);
        auto& field = required(actor, ".CLASSID");
        const auto previous = field.as_int();
        field.set_int(class_id);
        std::vector<std::string> warnings;
        // Class records share TIPO/COMPOR/HOMBRE; a different behaviour family
        // is legal data but may not suit the actor's scripts.
        if (const auto objects_file = file_of_kind(MissionFileKind::objects); objects_file && previous) {
            const auto& list = required(const_cast<TreeNode&>(root_of(*files_[*objects_file].tree)), ".LISTADATOS");
            const TreeNode* before{};
            const TreeNode* after{};
            for (const auto& record : list.children) {
                if (record_id(record) == *previous) before = &record;
                if (record_id(record) == class_id) after = &record;
            }
            for (const auto* field_name : {".TIPO", ".COMPOR", ".HOMBRE"}) {
                const auto* a = before ? before->child(field_name) : nullptr;
                const auto* b = after ? after->child(field_name) : nullptr;
                if (a && b && !(*a == *b))
                    warnings.push_back(std::string("The new class has a different ") + (field_name + 1) +
                                       "; scripts written for the old class may not fit");
            }
        }
        if (const auto uses = script_references("BICHO", actor_id))
            warnings.push_back(std::to_string(uses) + " script operand(s) reference this actor");
        return t.commit("Changed class of " + actor_label(actor) + " to " + std::to_string(class_id),
                        std::move(warnings));
    });
}

namespace {

// ---- Actor looks ---------------------------------------------------------------
//
// A look is the visual part of an Objetos.bdd class: the body model, its LOD
// and the bounding box. Changing an actor's look copies its class under a new
// ID with the look of another class, so behaviour, weapons, collision and
// physics stay as they were.

// Above every class ID of the shipped databases (the highest is 499), so a copy
// never collides with a class that another mission could import.
constexpr std::int32_t first_look_class_id = 500;
constexpr std::string_view look_marker = " (look: ";

std::string strip_look(const std::string_view name) {
    const auto at = name.rfind(look_marker);
    if (at == std::string_view::npos || !name.ends_with(')')) return std::string(name);
    return std::string(name.substr(0, at));
}

std::string record_name(const TreeNode& record) {
    const auto* name = record.child(".NOMBRE");
    return name && name->as_string() ? std::string(*name->as_string()) : std::string{};
}

bool is_look_copy(const TreeNode& record) {
    return record_id(record).value_or(0) >= first_look_class_id &&
           record_name(record).find(look_marker) != std::string::npos;
}

// Player records hold first-person hands in .MODELO and the character in
// .MODELO_TERCERA; the character is what the look is about.
TreeNode* body_model(TreeNode& record) {
    if (auto* third = record.child(".MODELO_TERCERA")) return third;
    return record.child(".MODELO");
}

std::string model_stem(const std::string_view path) {
    auto name = path.substr(path.find_last_of("\\/") == std::string_view::npos ? 0 : path.find_last_of("\\/") + 1);
    return std::string(name.substr(0, name.find_last_of('.')));
}

std::string model_folder(const std::string_view path) {
    const auto slash = path.find_last_of("\\/");
    return slash == std::string_view::npos ? std::string{} : lower(std::string(path.substr(0, slash)));
}

void apply_look(TreeNode& record, const TreeNode& look) {
    auto* body = body_model(record);
    const auto* source = body_model(const_cast<TreeNode&>(look));
    if (!body || !source) throw std::invalid_argument("Both classes need a .MODELO");
    body->text = source->text;
    remove_child(record, ".LOD1_NOMBRE");
    if (const auto* lod = look.child(".LOD1_NOMBRE")) {
        const auto distance = record.child_index(".LOD1_DIST");
        record.children.insert(record.children.begin() +
                                   static_cast<std::ptrdiff_t>(distance.value_or(record.children.size())),
                               *lod);
    }
    for (const auto* field : {".LOD1_DIST", ".LOD2_DIST", ".BBOX"})
        if (auto* target = record.child(field))
            if (const auto* value = look.child(field)) *target = *value;
}

// Two classes that differ only in ID and look suffix behave and look the same.
TreeNode comparable_class(TreeNode record) {
    remove_child(record, ".ID");
    if (auto* name = record.child(".NOMBRE")) name->set_string(strip_look(record_name(record)));
    return record;
}

// ---- Physics descriptors (.phd) ------------------------------------------------
//
// u32 FD FC FC FC magic, u32 version (1), u32 count, then per entry: u32 type,
// u32-length physics file, u32-length model (the class .MODELO), six bounding
// box floats (.BBOX .INF, .SUP) and .PHYSIC .MASS, .BOUNCE, .SLIDE. There is one
// entry per distinct combination of the classes and weapons a mission places.

struct PhysicsEntry {
    std::uint32_t type{};
    std::string file, model;
    std::array<float, 9> values{};
};

std::optional<std::vector<PhysicsEntry>> read_physics(const std::span<const std::byte> bytes) {
    try {
        if (bytes.size() < 12 || read_u32(bytes, 0) != 0xFCFCFCFDU) return std::nullopt;
        std::vector<PhysicsEntry> entries(read_u32(bytes, 8));
        std::size_t at = 12;
        const auto text = [&] {
            const auto length = read_u32(bytes, at);
            if (length > bytes.size() - at - 4) throw std::runtime_error("Truncated physics descriptor");
            std::string value(reinterpret_cast<const char*>(bytes.data() + at + 4), length);
            at += 4 + length;
            return value;
        };
        for (auto& entry : entries) {
            entry.type = read_u32(bytes, at);
            at += 4;
            entry.file = text();
            entry.model = text();
            for (auto& value : entry.values) {
                value = std::bit_cast<float>(read_u32(bytes, at));
                at += 4;
            }
        }
        if (at != bytes.size()) return std::nullopt;
        return entries;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

std::vector<std::byte> write_physics(const std::span<const std::byte> original,
                                     const std::vector<PhysicsEntry>& entries) {
    std::vector<std::byte> out(original.begin(), original.begin() + 8);
    append_u32(out, static_cast<std::uint32_t>(entries.size()));
    const auto text = [&](const std::string& value) {
        append_u32(out, static_cast<std::uint32_t>(value.size()));
        const auto* data = reinterpret_cast<const std::byte*>(value.data());
        out.insert(out.end(), data, data + value.size());
    };
    for (const auto& entry : entries) {
        append_u32(out, entry.type);
        text(entry.file);
        text(entry.model);
        for (const auto value : entry.values) append_u32(out, std::bit_cast<std::uint32_t>(value));
    }
    return out;
}

// The descriptor a class produces, without its type code (a mapping from
// .PHYSIC .TIPO that the shipped files do not make unique).
std::optional<PhysicsEntry> physics_of(const TreeNode& record) {
    const auto* physics = record.child(".PHYSIC");
    const auto* model = record.child(".MODELO");
    const auto* box = record.child(".BBOX");
    if (!physics || !model || !model->as_string() || !box) return std::nullopt;
    const auto low = vec3(box->child(".INF")), high = vec3(box->child(".SUP"));
    const auto mass = number(physics->child(".MASS")), bounce = number(physics->child(".BOUNCE")),
               slide = number(physics->child(".SLIDE"));
    if (!low || !high || !mass || !bounce || !slide) return std::nullopt;
    PhysicsEntry entry;
    if (const auto* file = physics->child(".MODEL_FILE"); file && file->as_string()) entry.file = *file->as_string();
    entry.model = *model->as_string();
    entry.values = {low->x, low->y, low->z, high->x, high->y, high->z, *mass, *bounce, *slide};
    return entry;
}

bool same_physics(const PhysicsEntry& a, const PhysicsEntry& b) {
    if (lower(a.file) != lower(b.file) || lower(a.model) != lower(b.model)) return false;
    for (std::size_t i = 0; i < a.values.size(); ++i)
        if (std::abs(a.values[i] - b.values[i]) > (i < 6 ? 1e-3F : 1e-4F)) return false;
    return true;
}

} // namespace

EditResult MissionEditor::set_actor_look(const std::int32_t actor_id, const std::int32_t look_class_id) {
    return run(*this, "Change model of actor " + std::to_string(actor_id), [&](MissionTransaction& t) {
        const auto objects_file = file_of_kind(MissionFileKind::objects);
        if (!objects_file) throw std::runtime_error("The package has no Objetos.bdd");
        auto scene = t.scene();
        auto& actor = scene.actor(actor_id);
        auto& class_field = required(actor, ".CLASSID");
        const auto class_id = class_field.as_int().value_or(0);
        auto& list = required(root_of(t.tree(*objects_file)), ".LISTADATOS");
        const auto* current = find_record(list, class_id);
        if (!current)
            throw std::invalid_argument("The actor's class " + std::to_string(class_id) +
                                        " is not in this mission's Objetos.bdd");
        const auto* look = find_record(list, look_class_id);
        if (!look)
            throw std::invalid_argument("Class " + std::to_string(look_class_id) +
                                        " is not in this mission's Objetos.bdd; import it first");
        const auto base = *current;
        auto wanted = base;
        apply_look(wanted, *look);
        const auto body = std::string(*body_model(wanted)->as_string());
        if (auto* name = wanted.child(".NOMBRE"))
            name->set_string(strip_look(record_name(base)) + std::string(look_marker) + model_stem(body) + ")");
        const auto comparable = comparable_class(wanted);
        if (comparable == comparable_class(base)) return t.reject("No change: the actor already has this model");

        std::vector<std::string> warnings;
        const auto old_body = std::string(*body_model(const_cast<TreeNode&>(base))->as_string());
        if (model_folder(old_body) != model_folder(body))
            warnings.push_back("The new model is from " + display(model_folder(body)) + ", not " +
                               display(model_folder(old_body)) +
                               "; it may lack the skeleton the actor's animations and weapons expect");
        if (const auto uses = script_references("CLASSID", class_id))
            warnings.push_back(std::to_string(uses) + " script operand(s) name class " + std::to_string(class_id) +
                               "; they no longer match this actor");

        // Reuse a class that already has this behaviour and look (for example
        // the original class when the look is changed back).
        std::int32_t target_id{};
        bool created = false;
        for (const auto& record : list.children)
            if (record_id(record) != class_id && comparable_class(record) == comparable) {
                target_id = *record_id(record);
                break;
            }
        if (!target_id) {
            target_id = std::max(first_look_class_id, next_id(list));
            required(wanted, ".ID").set_int(target_id);
            insert_record(list, wanted);
            created = true;
        }
        class_field.set_int(target_id);

        // A copy that nothing uses any more is removed again.
        std::optional<PhysicsEntry> removed_physics;
        if (is_look_copy(base) && script_references("CLASSID", class_id) == 0 &&
            std::ranges::none_of(scene.actors().children,
                                 [&](const TreeNode& other) { return integer(other.child(".CLASSID")) == class_id; })) {
            removed_physics = physics_of(base);
            std::erase_if(list.children, [&](const TreeNode& record) { return record_id(record) == class_id; });
        }

        // The mission's physics descriptors get an entry for the new class,
        // copied from the original class's entry with the new model and box.
        if (const auto physics_file = file_of_kind(MissionFileKind::physics_index)) {
            auto entries = read_physics(files_[*physics_file].raw);
            const auto needed = created ? physics_of(wanted) : std::nullopt;
            const auto original = physics_of(base);
            auto changed_entries = false;
            if (!entries) {
                if (created) warnings.push_back("The physics descriptor (.phd) could not be read and was not updated");
            } else {
                if (needed && std::ranges::none_of(*entries, [&](const PhysicsEntry& e) { return same_physics(e, *needed); })) {
                    const auto source = std::ranges::find_if(*entries, [&](const PhysicsEntry& e) {
                        return original && same_physics(e, *original);
                    });
                    if (source == entries->end()) {
                        warnings.push_back("The physics descriptor (.phd) has no entry for class " +
                                           std::to_string(class_id) + ", so none was added for the copy");
                    } else {
                        auto entry = *source;
                        entry.model = needed->model;
                        entry.values = needed->values;
                        entries->push_back(std::move(entry));
                        changed_entries = true;
                    }
                }
                if (removed_physics) {
                    const auto shipped_path = package_root_ / files_[*physics_file].relative_path;
                    const auto shipped = std::filesystem::is_regular_file(shipped_path)
                                             ? read_physics(read_file(shipped_path))
                                             : std::nullopt;
                    const auto still_used =
                        std::ranges::any_of(list.children, [&](const TreeNode& record) {
                            const auto other = physics_of(record);
                            return other && same_physics(*other, *removed_physics);
                        }) ||
                        (shipped && std::ranges::any_of(*shipped, [&](const PhysicsEntry& e) {
                             return same_physics(e, *removed_physics);
                         }));
                    if (!still_used) {
                        const auto before = entries->size();
                        std::erase_if(*entries, [&](const PhysicsEntry& e) { return same_physics(e, *removed_physics); });
                        changed_entries = changed_entries || entries->size() != before;
                    }
                }
                if (changed_entries) {
                    auto& raw = t.raw(*physics_file);
                    raw = write_physics(raw, *entries);
                }
            }
        }
        return t.commit("Changed model of " + actor_label(actor) + " to " + display(model_stem(body)) +
                            (created ? " (new class " : " (class ") + std::to_string(target_id) + ")",
                        std::move(warnings));
    });
}

EditResult MissionEditor::set_actor_name(const std::int32_t actor_id, const std::string_view utf8) {
    return run(*this, "Rename actor " + std::to_string(actor_id), [&](MissionTransaction& t) {
        const auto name = to_1252(utf8, "Name");
        auto scene = t.scene();
        auto& actor = scene.actor(actor_id);
        std::vector<std::string> warnings;
        for (const auto& other : scene.actors().children)
            if (&other != &actor && other.child(".NOMBRE") && other.child(".NOMBRE")->as_string() == name)
                warnings.push_back("Another actor already uses this name");
        required(actor, ".NOMBRE").set_string(name);
        return t.commit("Renamed actor " + std::to_string(actor_id), std::move(warnings));
    });
}

EditResult MissionEditor::set_actor_integer(const std::int32_t actor_id, const std::string_view field,
                                            const std::int32_t value) {
    return run(*this, "Set " + std::string(field) + " of actor " + std::to_string(actor_id),
               [&](MissionTransaction& t) {
                   if (field != ".COLISION" && field != ".FLAGS" && field != ".SEGUNDA_EXPLOSION")
                       throw std::invalid_argument("Field " + std::string(field) + " is not editable here");
                   auto scene = t.scene();
                   auto& actor = scene.actor(actor_id);
                   auto* node = actor.child(field);
                   if (!node || !node->as_int()) throw std::invalid_argument("Actor has no integer " + std::string(field));
                   node->set_int(value);
                   return t.commit("Set " + std::string(field) + " of " + actor_label(actor));
               });
}

EditResult MissionEditor::set_actor_faction(const std::int32_t actor_id, std::optional<std::string> utf8) {
    return run(*this, "Set faction of actor " + std::to_string(actor_id), [&](MissionTransaction& t) {
        auto scene = t.scene();
        auto& actor = scene.actor(actor_id);
        if (!utf8 || utf8->empty()) {
            remove_child(actor, ".BANDO");
        } else if (auto* existing = actor.child(".BANDO")) {
            existing->set_string(to_1252(*utf8, "Faction"));
        } else {
            insert_ordered(actor, TreeNode::string(".BANDO", to_1252(*utf8, "Faction")), actor_order);
        }
        std::vector<std::string> warnings;
        if (utf8 && *utf8 != "NEUTRO" && *utf8 != "ALEMAN" && *utf8 != "ALIADO" && !utf8->empty())
            warnings.push_back("Shipped missions only use NEUTRO, ALEMAN and ALIADO");
        return t.commit("Set faction of " + actor_label(actor), std::move(warnings));
    });
}

std::vector<std::pair<std::int32_t, std::string>> MissionEditor::actor_script_choices() const {
    std::vector<std::pair<std::int32_t, std::string>> result;
    const auto program = file_of_kind(MissionFileKind::mission_script);
    if (!program || !files_[*program].tree) return result;
    const auto* scripts = root_of(*files_[*program].tree).child(".SCRIPTS");
    if (!scripts) return result;
    for (const auto& script : scripts->children) {
        const auto id = record_id(script);
        const auto* flags = script.child(".FLAGS");
        const auto trigger = flags ? integer(flags->child(".TRIGGER")) : std::nullopt;
        if (!id || trigger.value_or(0) != 0) continue;
        const auto* name = script.child(".NOMBRE");
        result.emplace_back(*id, name && name->as_string() ? display(*name->as_string()) : std::string{});
    }
    return result;
}

EditResult MissionEditor::set_actor_scripts(const std::int32_t actor_id, std::vector<std::int32_t> script_ids,
                                            const bool force) {
    const auto choices = actor_script_choices();
    return run(*this, "Set scripts of actor " + std::to_string(actor_id), [&](MissionTransaction& t) {
        std::vector<std::string> warnings;
        for (const auto id : script_ids)
            if (std::ranges::none_of(choices, [&](const auto& choice) { return choice.first == id; })) {
                if (!force)
                    throw std::invalid_argument("Script " + std::to_string(id) +
                                                " is not a non-trigger script of the mission program");
                warnings.push_back("Script " + std::to_string(id) + " is not a known actor script");
            }
        auto scene = t.scene();
        auto& actor = scene.actor(actor_id);
        if (script_ids.empty()) {
            remove_child(actor, ".SCRIPT");
        } else {
            auto list = TreeNode::group(".SCRIPT");
            for (const auto id : script_ids) list.children.push_back(TreeNode::integer(std::nullopt, id));
            if (auto* existing = actor.child(".SCRIPT")) *existing = std::move(list);
            else insert_ordered(actor, std::move(list), actor_order);
        }
        return t.commit("Set scripts of " + actor_label(actor), std::move(warnings));
    });
}

EditResult MissionEditor::set_actor_animations(const std::int32_t actor_id,
                                               std::vector<ActorAnimationOverride> overrides,
                                               const bool force) {
    const auto& catalog = animations();
    return run(*this, "Set animations of actor " + std::to_string(actor_id), [&](MissionTransaction& t) {
        std::vector<std::string> warnings;
        const auto slots = animation_slot_names();
        std::set<std::string> used;
        for (const auto& value : overrides) {
            if (!std::ranges::binary_search(slots, std::string_view(value.slot))) {
                if (!force) throw std::invalid_argument("Unknown animation slot " + value.slot);
                warnings.push_back("Slot " + value.slot + " is not in the executable's slot table");
            }
            if (!used.insert(value.slot).second) throw std::invalid_argument("Slot " + value.slot + " is listed twice");
            if (!catalog.find_id(value.animation_id)) {
                if (!force)
                    throw std::invalid_argument("Animation " + std::to_string(value.animation_id) +
                                                " is not in this mission's Anims.bdd; import it first");
                warnings.push_back("Animation " + std::to_string(value.animation_id) + " is not in Anims.bdd");
            }
        }
        auto scene = t.scene();
        auto& actor = scene.actor(actor_id);
        if (overrides.empty()) {
            remove_child(actor, ".ANIMACIONES");
        } else {
            // Shipped override lists are ordered by animation ID.
            std::ranges::stable_sort(overrides, {}, &ActorAnimationOverride::animation_id);
            auto list = TreeNode::group(".ANIMACIONES");
            for (const auto& value : overrides) {
                auto record = TreeNode::array(std::nullopt);
                record.children = {TreeNode::integer(".ID", value.animation_id),
                                   TreeNode::string(".TIPO", to_1252(value.slot, "Slot"))};
                list.children.push_back(std::move(record));
            }
            if (auto* existing = actor.child(".ANIMACIONES")) *existing = std::move(list);
            else insert_ordered(actor, std::move(list), actor_order);
            // Shipped missions usually also preload override animations in the
            // program's resource list; listing them there is harmless.
            if (const auto program = file_of_kind(MissionFileKind::mission_script))
                if (auto* resources = root_of(t.tree(*program)).child(".RECURSOS"))
                    if (auto* preload = resources->child(".ANIMACIONES"))
                        for (const auto& value : overrides)
                            if (std::ranges::none_of(preload->children, [&](const TreeNode& id) {
                                    return id.as_int() == value.animation_id;
                                }))
                                preload->children.push_back(TreeNode::integer(std::nullopt, value.animation_id));
        }
        return t.commit("Set animations of " + actor_label(actor), std::move(warnings));
    });
}

namespace {

std::string unique_actor_name(TreeNode& actors, const std::string& base) {
    std::set<std::string> names;
    for (const auto& actor : actors.children)
        if (const auto* name = actor.child(".NOMBRE"); name && name->as_string())
            names.emplace(*name->as_string());
    if (!names.contains(base)) return base;
    for (int i = 2;; ++i) {
        auto candidate = base + "_" + std::to_string(i);
        if (!names.contains(candidate)) return candidate;
    }
}

// Adds a placement point for `position` to `group_id`, copying the layout of
// an existing point in that group when there is one.
std::int32_t add_point(SceneView& scene, const std::int32_t group_id, const Vec3& position,
                       const float rotation_radians, const float pitch_radians,
                       const TreeNode* template_point) {
    auto* group = scene.group(group_id);
    if (!group) throw std::invalid_argument("Navigation group " + std::to_string(group_id) + " does not exist");
    auto* points = group->child(".PUNTOS");
    if (!points) throw std::runtime_error("Navigation group has no point list");
    const auto id = next_id(*points);
    if (!template_point && !points->children.empty()) template_point = &points->children.front();
    TreeNode point;
    if (template_point) {
        point = *template_point;
        if (auto* name = point.child(".NOMBRE")) name->set_string("");
    } else {
        point = TreeNode::array(std::nullopt);
        point.children = {TreeNode::integer(".ID", id), TreeNode::string(".NOMBRE", ""),
                          make_vec3(".POS", position), TreeNode::real(".ROT", 0),
                          TreeNode::real(".ROT_X", 0)};
    }
    required(point, ".ID").set_int(id);
    assign_vec3(required(point, ".POS"), position);
    if (auto* rotation = point.child(".ROT")) assign_number(*rotation, rotation_radians);
    if (auto* pitch = point.child(".ROT_X")) assign_number(*pitch, pitch_radians);
    points->children.push_back(std::move(point));
    return id;
}

} // namespace

EditResult MissionEditor::duplicate_actor(const std::int32_t actor_id, const Vec3 offset,
                                          std::int32_t* new_id) {
    return run(*this, "Duplicate actor " + std::to_string(actor_id), [&](MissionTransaction& t) {
        if (!finite(offset)) throw std::invalid_argument("Offset must be finite");
        auto scene = t.scene();
        auto copy = scene.actor(actor_id);
        auto& actors = scene.actors();
        const auto id = next_id(actors);
        required(copy, ".ID").set_int(id);
        if (auto* name = copy.child(".NOMBRE"); name && name->as_string())
            name->set_string(unique_actor_name(actors, std::string(*name->as_string())));
        auto& position = required(copy, ".POS");
        const auto old_position = vec3(&position);
        if (!old_position) throw std::runtime_error("Actor position is not a vector");
        const auto moved = add(*old_position, offset);
        assign_vec3(position, moved);
        std::vector<std::string> warnings;
        if (const auto cell = actor_cell(copy)) {
            const auto* point = scene.point(cell->first, cell->second);
            if (point && vec3(point->child(".POS")) && same_position(*vec3(point->child(".POS")), *old_position)) {
                const auto heading = number(copy.child(".ANGULO")).value_or(0);
                const auto pitch = number(copy.child(".ANGULO_X")).value_or(0);
                const auto point_id = add_point(scene, cell->first, moved, heading * degrees_to_radians,
                                                pitch * degrees_to_radians, point);
                set_actor_cell(copy, cell->first, point_id);
            } else {
                set_actor_cell(copy, -1, -1);
                warnings.push_back("The source actor's cell did not mirror it; the copy has no placement point");
            }
        }
        const auto label = actor_label(copy);
        actors.children.push_back(std::move(copy));
        if (new_id) *new_id = id;
        return t.commit("Duplicated actor " + std::to_string(actor_id) + " as " + label, std::move(warnings));
    });
}

EditResult MissionEditor::add_actor(const std::int32_t class_id, const ActorPlacement& placement,
                                    const std::string_view utf8_name,
                                    const std::optional<std::int32_t> navigation_group,
                                    std::int32_t* new_id) {
    const bool known_class = !objects().find_class(class_id).empty();
    return run(*this, "Add actor of class " + std::to_string(class_id), [&](MissionTransaction& t) {
        if (!known_class)
            throw std::invalid_argument("Class " + std::to_string(class_id) +
                                        " is not in this mission's Objetos.bdd; import it first");
        if (!finite(placement.position) || !std::isfinite(placement.heading_degrees) ||
            !std::isfinite(placement.pitch_degrees))
            throw std::invalid_argument("Position and angles must be finite");
        auto scene = t.scene();
        auto& actors = scene.actors();
        const auto id = next_id(actors);
        auto name = to_1252(utf8_name, "Name");
        if (name.empty()) name = "Actor_" + std::to_string(id);
        name = unique_actor_name(actors, name);

        auto group = navigation_group;
        if (!group) {
            // Shipped actors almost always stand on a placement point of a
            // nearby TIPO 0 group; use the group of the nearest placed actor.
            float best = std::numeric_limits<float>::max();
            for (const auto& other : actors.children) {
                const auto cell = actor_cell(other);
                const auto position = vec3(other.child(".POS"));
                if (!cell || !position || !scene.group(cell->first)) continue;
                if (const auto d = distance_squared(*position, placement.position); d < best) {
                    best = d;
                    group = cell->first;
                }
            }
        }
        auto actor = TreeNode::array(std::nullopt);
        actor.children = {TreeNode::string(".NOMBRE", name),
                          TreeNode::integer(".ID", id),
                          TreeNode::integer(".CLASSID", class_id),
                          make_vec3(".POS", placement.position),
                          TreeNode::real(".ANGULO", placement.heading_degrees),
                          TreeNode::real(".ANGULO_X", placement.pitch_degrees),
                          TreeNode::integer(".COLISION", 1),
                          TreeNode::integer(".FLAGS", 0),
                          TreeNode::integer(".SEGUNDA_EXPLOSION", 0)};
        std::vector<std::string> warnings;
        if (group) {
            const auto point = add_point(scene, *group, placement.position,
                                         placement.heading_degrees * degrees_to_radians,
                                         placement.pitch_degrees * degrees_to_radians, nullptr);
            set_actor_cell(actor, *group, point);
        } else {
            set_actor_cell(actor, -1, -1);
            warnings.push_back("No navigation group was available; the actor has no placement point");
        }
        actors.children.push_back(std::move(actor));
        if (new_id) *new_id = id;
        return t.commit("Added " + display(name) + " (" + std::to_string(id) + ")", std::move(warnings));
    });
}

EditResult MissionEditor::delete_actor(const std::int32_t actor_id, const bool force) {
    const auto uses = script_references("BICHO", actor_id);
    return run(*this, "Delete actor " + std::to_string(actor_id), [&](MissionTransaction& t) {
        auto scene = t.scene();
        auto& actor = scene.actor(actor_id);
        std::vector<std::string> blockers;
        if (uses) blockers.push_back(std::to_string(uses) + " script operand(s)");
        if (integer(scene.root.child(".PLAYER")) == actor_id) blockers.push_back("the mission's starting player");
        if (const auto* multiplayer = scene.root.child(".MULTIPLAYER"))
            if (integer(multiplayer->child(".MPPOSTMEN_IDPOSTMAN")) == actor_id)
                blockers.push_back("the multiplayer postman setting");
        std::string list;
        for (const auto& blocker : blockers) list += (list.empty() ? "" : ", ") + blocker;
        if (!blockers.empty() && !force)
            throw std::invalid_argument(actor_label(actor) + " is still referenced by " + list +
                                        "; delete with force to leave those references dangling");
        const auto label = actor_label(actor);
        std::vector<std::string> warnings;
        if (!blockers.empty()) warnings.push_back("Dangling references remain: " + list);
        const auto cell = actor_cell(actor);
        const auto position = vec3(actor.child(".POS"));
        std::erase_if(scene.actors().children, [&](const TreeNode& value) { return &value == &actor; });
        // Remove the placement point too when nothing else uses it.
        if (cell && position)
            if (auto* group = scene.group(cell->first)) {
                auto* point = scene.point(cell->first, cell->second);
                const bool mirrored = point && vec3(point->child(".POS")) &&
                                      same_position(*vec3(point->child(".POS")), *position);
                bool shared = false;
                for (const auto& other : scene.actors().children) shared |= actor_cell(other) == cell;
                bool connected = false;
                if (const auto* connections = group->child(".CONEXIONES"))
                    for (const auto& c : connections->children)
                        connected |= integer(c.child(".PUNTO_ORI")) == cell->second ||
                                     integer(c.child(".PUNTO_DST")) == cell->second;
                if (const auto* connections = scene.navigation().child(".CONEXIONES"))
                    for (const auto& c : connections->children)
                        connected |= (integer(c.child(".GRUPO_ORI")) == cell->first &&
                                      integer(c.child(".PUNTO_ORI")) == cell->second) ||
                                     (integer(c.child(".GRUPO_DST")) == cell->first &&
                                      integer(c.child(".PUNTO_DST")) == cell->second);
                if (mirrored && !shared && !connected &&
                    pathpoint_references(files_, cell->first, cell->second) == 0)
                    std::erase_if(required(*group, ".PUNTOS").children,
                                  [&](const TreeNode& value) { return &value == point; });
            }
        return t.commit("Deleted " + label, std::move(warnings));
    });
}

// ---- Dummies and lights -------------------------------------------------------

EditResult MissionEditor::set_dummy_placement(const std::int32_t dummy_id, const Vec3 position,
                                              const float rotation_radians, const float pitch_radians) {
    return run(*this, "Move dummy " + std::to_string(dummy_id), [&](MissionTransaction& t) {
        if (!finite(position) || !std::isfinite(rotation_radians) || !std::isfinite(pitch_radians))
            throw std::invalid_argument("Position and angles must be finite");
        auto scene = t.scene();
        auto* dummy = find_record(scene.dummies(), dummy_id);
        if (!dummy) throw std::invalid_argument("Dummy " + std::to_string(dummy_id) + " does not exist");
        assign_vec3(required(*dummy, ".POS"), position);
        if (auto* rotation = dummy->child(".ROT")) assign_number(*rotation, rotation_radians);
        if (auto* pitch = dummy->child(".ROT_X")) assign_number(*pitch, pitch_radians);
        return t.commit("Moved dummy " + std::to_string(dummy_id));
    });
}

EditResult MissionEditor::duplicate_dummy(const std::int32_t dummy_id, const Vec3 offset, std::int32_t* new_id) {
    return run(*this, "Duplicate dummy " + std::to_string(dummy_id), [&](MissionTransaction& t) {
        auto scene = t.scene();
        auto& list = scene.dummies();
        auto* source = find_record(list, dummy_id);
        if (!source) throw std::invalid_argument("Dummy " + std::to_string(dummy_id) + " does not exist");
        auto copy = *source;
        const auto id = next_id(list);
        required(copy, ".ID").set_int(id);
        auto& position = required(copy, ".POS");
        assign_vec3(position, add(vec3(&position).value_or(Vec3{}), offset));
        list.children.push_back(std::move(copy));
        bool placed = false;
        if (auto* folders = required(scene.root, ".MALLA_DUMMIES").child(".CARPETAS"))
            add_to_folders(*folders, id, dummy_id, placed);
        if (new_id) *new_id = id;
        return t.commit("Duplicated dummy " + std::to_string(dummy_id) + " as " + std::to_string(id));
    });
}

EditResult MissionEditor::delete_dummy(const std::int32_t dummy_id, const bool force) {
    const auto uses = script_references("DUMMY", dummy_id);
    return run(*this, "Delete dummy " + std::to_string(dummy_id), [&](MissionTransaction& t) {
        auto scene = t.scene();
        auto& list = scene.dummies();
        auto* dummy = find_record(list, dummy_id);
        if (!dummy) throw std::invalid_argument("Dummy " + std::to_string(dummy_id) + " does not exist");
        std::size_t effects = 0;
        if (const auto* list_effects = scene.root.child(".EFECTOS"))
            for (const auto& effect : list_effects->children)
                effects += integer(effect.child(".DUMMY")) == dummy_id;
        if ((uses || effects) && !force)
            throw std::invalid_argument("Dummy " + std::to_string(dummy_id) + " is used by " +
                                        std::to_string(uses) + " script operand(s) and " +
                                        std::to_string(effects) + " effect(s)");
        std::erase_if(list.children, [&](const TreeNode& value) { return &value == dummy; });
        if (auto* folders = required(scene.root, ".MALLA_DUMMIES").child(".CARPETAS"))
            remove_from_folders(*folders, dummy_id);
        std::vector<std::string> warnings;
        if (uses || effects) warnings.push_back("Dangling dummy references remain");
        return t.commit("Deleted dummy " + std::to_string(dummy_id), std::move(warnings));
    });
}

EditResult MissionEditor::set_light(const std::int32_t light_id, const LightEdit& edit) {
    return run(*this, "Edit light " + std::to_string(light_id), [&](MissionTransaction& t) {
        auto scene = t.scene();
        auto* light = find_record(scene.lights(), light_id);
        if (!light) throw std::invalid_argument("Light " + std::to_string(light_id) + " does not exist");
        if (edit.position) {
            if (!finite(*edit.position)) throw std::invalid_argument("Position must be finite");
            assign_vec3(required(*light, ".POS"), *edit.position);
        }
        if (edit.color) required(*light, ".COLOR").set_int(*edit.color);
        if (edit.modulate) required(*light, ".MODULATE").set_int(*edit.modulate);
        if (edit.radius) {
            if (!std::isfinite(*edit.radius) || *edit.radius < 0)
                throw std::invalid_argument("Radius must be a finite, non-negative number");
            assign_number(required(*light, ".RADIO"), *edit.radius);
        }
        return t.commit("Edited light " + std::to_string(light_id),
                        {"Map lighting may also be baked into the visual map; runtime effect is untested"});
    });
}

EditResult MissionEditor::duplicate_light(const std::int32_t light_id, const Vec3 offset, std::int32_t* new_id) {
    return run(*this, "Duplicate light " + std::to_string(light_id), [&](MissionTransaction& t) {
        auto scene = t.scene();
        auto& list = scene.lights();
        auto* source = find_record(list, light_id);
        if (!source) throw std::invalid_argument("Light " + std::to_string(light_id) + " does not exist");
        auto copy = *source;
        const auto id = next_id(list);
        required(copy, ".ID").set_int(id);
        auto& position = required(copy, ".POS");
        assign_vec3(position, add(vec3(&position).value_or(Vec3{}), offset));
        list.children.push_back(std::move(copy));
        bool placed = false;
        if (auto* folders = required(scene.root, ".MALLA_LUCES").child(".CARPETAS"))
            add_to_folders(*folders, id, light_id, placed);
        if (new_id) *new_id = id;
        return t.commit("Duplicated light " + std::to_string(light_id) + " as " + std::to_string(id));
    });
}

EditResult MissionEditor::delete_light(const std::int32_t light_id) {
    return run(*this, "Delete light " + std::to_string(light_id), [&](MissionTransaction& t) {
        auto scene = t.scene();
        auto& list = scene.lights();
        auto* light = find_record(list, light_id);
        if (!light) throw std::invalid_argument("Light " + std::to_string(light_id) + " does not exist");
        std::erase_if(list.children, [&](const TreeNode& value) { return &value == light; });
        if (auto* folders = required(scene.root, ".MALLA_LUCES").child(".CARPETAS"))
            remove_from_folders(*folders, light_id);
        return t.commit("Deleted light " + std::to_string(light_id));
    });
}

// ---- Navigation -----------------------------------------------------------------

EditResult MissionEditor::set_navigation_point(const std::int32_t group_id, const std::int32_t point_id,
                                               const Vec3 position, const std::optional<float> rotation_radians) {
    return run(*this, "Move navigation point " + std::to_string(group_id) + "/" + std::to_string(point_id),
               [&](MissionTransaction& t) {
                   if (!finite(position) || (rotation_radians && !std::isfinite(*rotation_radians)))
                       throw std::invalid_argument("Position and rotation must be finite");
                   auto scene = t.scene();
                   auto* point = scene.point(group_id, point_id);
                   if (!point) throw std::invalid_argument("Navigation point does not exist");
                   auto& point_position = required(*point, ".POS");
                   const auto old = vec3(&point_position);
                   assign_vec3(point_position, position);
                   if (rotation_radians)
                       if (auto* rotation = point->child(".ROT")) assign_number(*rotation, *rotation_radians);
                   // The actor standing on this placement point moves with it.
                   for (auto& actor : scene.actors().children) {
                       if (actor_cell(actor) != std::pair{group_id, point_id}) continue;
                       auto* actor_position = actor.child(".POS");
                       if (!actor_position || !old || !vec3(actor_position) ||
                           !same_position(*vec3(actor_position), *old))
                           continue;
                       assign_vec3(*actor_position, position);
                       if (rotation_radians)
                           if (auto* heading = actor.child(".ANGULO"))
                               assign_number(*heading, *rotation_radians / degrees_to_radians);
                   }
                   return t.commit("Moved navigation point " + std::to_string(group_id) + "/" +
                                   std::to_string(point_id));
               });
}

EditResult MissionEditor::add_navigation_point(const std::int32_t group_id, const Vec3 position,
                                               std::int32_t* new_point_id) {
    return run(*this, "Add navigation point to group " + std::to_string(group_id), [&](MissionTransaction& t) {
        if (!finite(position)) throw std::invalid_argument("Position must be finite");
        auto scene = t.scene();
        const auto id = add_point(scene, group_id, position, 0, 0, nullptr);
        if (new_point_id) *new_point_id = id;
        return t.commit("Added navigation point " + std::to_string(group_id) + "/" + std::to_string(id));
    });
}

namespace {

bool connection_matches(const TreeNode& connection, const std::optional<std::int32_t> local_group,
                        const std::int32_t origin_group, const std::int32_t origin_point,
                        const std::int32_t destination_group, const std::int32_t destination_point) {
    const auto og = local_group ? local_group : integer(connection.child(".GRUPO_ORI"));
    const auto dg = local_group ? local_group : integer(connection.child(".GRUPO_DST"));
    return og == origin_group && integer(connection.child(".PUNTO_ORI")) == origin_point &&
           dg == destination_group && integer(connection.child(".PUNTO_DST")) == destination_point;
}

bool connection_touches(const TreeNode& connection, const std::optional<std::int32_t> local_group,
                        const std::int32_t group, const std::int32_t point) {
    const auto og = local_group ? local_group : integer(connection.child(".GRUPO_ORI"));
    const auto dg = local_group ? local_group : integer(connection.child(".GRUPO_DST"));
    return (og == group && integer(connection.child(".PUNTO_ORI")) == point) ||
           (dg == group && integer(connection.child(".PUNTO_DST")) == point);
}

} // namespace

EditResult MissionEditor::delete_navigation_point(const std::int32_t group_id, const std::int32_t point_id,
                                                  const bool force) {
    const auto script_uses = pathpoint_references(files_, group_id, point_id);
    return run(*this, "Delete navigation point " + std::to_string(group_id) + "/" + std::to_string(point_id),
               [&](MissionTransaction& t) {
                   auto scene = t.scene();
                   auto* group = scene.group(group_id);
                   auto* point = scene.point(group_id, point_id);
                   if (!group || !point) throw std::invalid_argument("Navigation point does not exist");
                   std::size_t actors = 0, links = 0;
                   for (const auto& actor : scene.actors().children)
                       actors += actor_cell(actor) == std::pair{group_id, point_id};
                   auto* local = group->child(".CONEXIONES");
                   auto* global = scene.navigation().child(".CONEXIONES");
                   if (local)
                       for (const auto& c : local->children) links += connection_touches(c, group_id, group_id, point_id);
                   if (global)
                       for (const auto& c : global->children)
                           links += connection_touches(c, std::nullopt, group_id, point_id);
                   if ((actors || script_uses) && !force)
                       throw std::invalid_argument("The point is the placement of " + std::to_string(actors) +
                                                   " actor(s) and is used by " + std::to_string(script_uses) +
                                                   " script operand(s)");
                   if (local)
                       std::erase_if(local->children, [&](const TreeNode& c) {
                           return connection_touches(c, group_id, group_id, point_id);
                       });
                   if (global)
                       std::erase_if(global->children, [&](const TreeNode& c) {
                           return connection_touches(c, std::nullopt, group_id, point_id);
                       });
                   for (auto& actor : scene.actors().children)
                       if (actor_cell(actor) == std::pair{group_id, point_id}) set_actor_cell(actor, -1, -1);
                   std::erase_if(required(*group, ".PUNTOS").children,
                                 [&](const TreeNode& value) { return &value == point; });
                   std::vector<std::string> warnings;
                   if (links) warnings.push_back("Removed " + std::to_string(links) + " connection(s)");
                   if (actors) warnings.push_back(std::to_string(actors) + " actor(s) lost their placement point");
                   if (script_uses) warnings.push_back("Script operands still name this point");
                   return t.commit("Deleted navigation point " + std::to_string(group_id) + "/" +
                                       std::to_string(point_id),
                                   std::move(warnings));
               });
}

EditResult MissionEditor::connect_navigation_points(const std::int32_t origin_group, const std::int32_t origin_point,
                                                    const std::int32_t destination_group,
                                                    const std::int32_t destination_point) {
    return run(*this, "Connect navigation points", [&](MissionTransaction& t) {
        auto scene = t.scene();
        if (!scene.point(origin_group, origin_point) || !scene.point(destination_group, destination_point))
            throw std::invalid_argument("Both navigation points must exist");
        if (origin_group == destination_group && origin_point == destination_point)
            throw std::invalid_argument("A point cannot connect to itself");
        const auto exists = [&](TreeNode* list, const std::optional<std::int32_t> local) {
            if (!list) return false;
            return std::ranges::any_of(list->children, [&](const TreeNode& c) {
                return connection_matches(c, local, origin_group, origin_point, destination_group, destination_point) ||
                       connection_matches(c, local, destination_group, destination_point, origin_group, origin_point);
            });
        };
        auto connection = TreeNode::array(std::nullopt);
        if (origin_group == destination_group) {
            auto* group = scene.group(origin_group);
            auto* list = group->child(".CONEXIONES");
            if (!list) {
                group->children.push_back(TreeNode::group(".CONEXIONES"));
                list = &group->children.back();
            }
            if (exists(list, origin_group)) throw std::invalid_argument("The points are already connected");
            connection.children = {TreeNode::integer(".PUNTO_ORI", origin_point),
                                   TreeNode::integer(".PUNTO_DST", destination_point)};
            list->children.push_back(std::move(connection));
        } else {
            auto& navigation = scene.navigation();
            auto* list = navigation.child(".CONEXIONES");
            if (!list) {
                navigation.children.push_back(TreeNode::group(".CONEXIONES"));
                list = &navigation.children.back();
            }
            if (exists(list, std::nullopt)) throw std::invalid_argument("The points are already connected");
            connection.children = {TreeNode::integer(".GRUPO_ORI", origin_group),
                                   TreeNode::integer(".PUNTO_ORI", origin_point),
                                   TreeNode::integer(".GRUPO_DST", destination_group),
                                   TreeNode::integer(".PUNTO_DST", destination_point)};
            list->children.push_back(std::move(connection));
        }
        return t.commit("Connected " + std::to_string(origin_group) + "/" + std::to_string(origin_point) +
                        " to " + std::to_string(destination_group) + "/" + std::to_string(destination_point));
    });
}

EditResult MissionEditor::disconnect_navigation_points(const std::int32_t origin_group,
                                                       const std::int32_t origin_point,
                                                       const std::int32_t destination_group,
                                                       const std::int32_t destination_point) {
    return run(*this, "Disconnect navigation points", [&](MissionTransaction& t) {
        auto scene = t.scene();
        std::size_t removed = 0;
        const auto erase = [&](TreeNode* list, const std::optional<std::int32_t> local) {
            if (!list) return;
            removed += std::erase_if(list->children, [&](const TreeNode& c) {
                return connection_matches(c, local, origin_group, origin_point, destination_group, destination_point) ||
                       connection_matches(c, local, destination_group, destination_point, origin_group, origin_point);
            });
        };
        if (origin_group == destination_group) {
            if (auto* group = scene.group(origin_group)) erase(group->child(".CONEXIONES"), origin_group);
        } else {
            erase(scene.navigation().child(".CONEXIONES"), std::nullopt);
        }
        if (!removed) throw std::invalid_argument("The points are not connected");
        return t.commit("Disconnected navigation points");
    });
}

// ---- Areas ----------------------------------------------------------------------

std::vector<std::string> area_polygon_problems(const std::vector<Vec3>& points) {
    std::vector<std::string> problems;
    const auto n = points.size();
    std::vector<std::pair<double, double>> xz;
    for (const auto& p : points) xz.emplace_back(p.x, p.z);
    auto distinct = xz;
    std::ranges::sort(distinct);
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
    if (n < 3 || distinct.size() < 3) {
        problems.emplace_back("needs at least three distinct points");
        return problems;
    }
    // A repeated vertex anywhere (a doubled loop) reaches the decomposition too.
    if (distinct.size() != n) problems.emplace_back("repeats a point (doubled or folded outline)");
    constexpr double min_edge = 0.01;       // game units (cm)
    constexpr double collinear = 1.0e-6;    // |sin| of the turn at a vertex
    const auto sub = [](auto a, auto b) { return std::pair{a.first - b.first, a.second - b.second}; };
    const auto cross = [](auto a, auto b) { return a.first * b.second - a.second * b.first; };
    const auto length = [](auto a) { return std::hypot(a.first, a.second); };
    for (std::size_t i = 0; i < n; ++i) {
        const auto e = sub(xz[(i + 1) % n], xz[i]);
        if (length(e) < min_edge) problems.push_back("zero-length edge after point " + std::to_string(i));
    }
    for (std::size_t i = 0; i < n; ++i) {
        const auto a = sub(xz[i], xz[(i + n - 1) % n]), b = sub(xz[(i + 1) % n], xz[i]);
        const auto la = length(a), lb = length(b);
        if (la >= min_edge && lb >= min_edge && std::abs(cross(a, b)) <= collinear * la * lb)
            problems.push_back("point " + std::to_string(i) + " is collinear with its neighbours");
    }
    // Proper crossings between non-adjacent edges.
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = i + 2; j < n; ++j) {
            if (i == 0 && j == n - 1) continue;
            const auto p = xz[i], p2 = xz[(i + 1) % n], q = xz[j], q2 = xz[(j + 1) % n];
            const auto d1 = cross(sub(p2, p), sub(q, p)), d2 = cross(sub(p2, p), sub(q2, p));
            const auto d3 = cross(sub(q2, q), sub(p, q)), d4 = cross(sub(q2, q), sub(p2, q));
            if (((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0)
                problems.push_back("edges " + std::to_string(i) + " and " + std::to_string(j) + " cross");
        }
    return problems;
}

namespace {

TreeNode& area_points(SceneView& scene, const std::int32_t area_id) {
    auto* area = find_record(scene.areas(), area_id);
    if (!area) throw std::invalid_argument("Area " + std::to_string(area_id) + " does not exist");
    return required(*area, ".PUNTOS");
}

} // namespace

EditResult MissionEditor::set_area_point(const std::int32_t area_id, const std::size_t index, const Vec3 position) {
    return run(*this, "Move area " + std::to_string(area_id) + " vertex", [&](MissionTransaction& t) {
        if (!finite(position)) throw std::invalid_argument("Position must be finite");
        auto scene = t.scene();
        auto& points = area_points(scene, area_id);
        if (index >= points.children.size()) throw std::invalid_argument("Area vertex does not exist");
        assign_vec3(required(points.children[index], ".POS"), position);
        return t.commit("Moved vertex " + std::to_string(index) + " of area " + std::to_string(area_id));
    });
}

EditResult MissionEditor::insert_area_point(const std::int32_t area_id, const std::size_t index, const Vec3 position) {
    return run(*this, "Insert area " + std::to_string(area_id) + " vertex", [&](MissionTransaction& t) {
        if (!finite(position)) throw std::invalid_argument("Position must be finite");
        auto scene = t.scene();
        auto& points = area_points(scene, area_id);
        if (index > points.children.size()) throw std::invalid_argument("Vertex index is out of range");
        auto vertex = points.children.empty() ? TreeNode::array(std::nullopt) : points.children.front();
        if (points.children.empty()) vertex.children = {make_vec3(".POS", position)};
        else assign_vec3(required(vertex, ".POS"), position);
        points.children.insert(points.children.begin() + static_cast<std::ptrdiff_t>(index), std::move(vertex));
        return t.commit("Inserted a vertex into area " + std::to_string(area_id));
    });
}

EditResult MissionEditor::remove_area_point(const std::int32_t area_id, const std::size_t index) {
    return run(*this, "Remove area " + std::to_string(area_id) + " vertex", [&](MissionTransaction& t) {
        auto scene = t.scene();
        auto& points = area_points(scene, area_id);
        if (index >= points.children.size()) throw std::invalid_argument("Area vertex does not exist");
        if (points.children.size() <= 3) throw std::invalid_argument("An area needs at least three vertices");
        points.children.erase(points.children.begin() + static_cast<std::ptrdiff_t>(index));
        return t.commit("Removed a vertex from area " + std::to_string(area_id));
    });
}

EditResult MissionEditor::set_area_height(const std::int32_t area_id, const float height) {
    return run(*this, "Set height of area " + std::to_string(area_id), [&](MissionTransaction& t) {
        if (!std::isfinite(height)) throw std::invalid_argument("Height must be finite");
        auto scene = t.scene();
        auto* area = find_record(scene.areas(), area_id);
        if (!area) throw std::invalid_argument("Area " + std::to_string(area_id) + " does not exist");
        assign_number(required(*area, ".HEIGHT"), height);
        return t.commit("Set height of area " + std::to_string(area_id));
    });
}

// ---- Map scene instances -----------------------------------------------------------

namespace {

bool instance_record_at(const std::span<const std::byte> bytes, const std::uint64_t offset) {
    return offset <= bytes.size() && bytes.size() - offset >= 116 && read_u32(bytes, offset) == 0x00016FC0U &&
           read_u32(bytes, offset + 36) == 0x0DU && read_u32(bytes, offset + 40) == 64U &&
           read_u32(bytes, offset + 48) == 0x01U && read_u32(bytes, offset + 52) == 52U;
}

float read_real(const std::span<const std::byte> bytes, const std::uint64_t offset) {
    return std::bit_cast<float>(read_u32(bytes, offset));
}

} // namespace

std::optional<MissionEditor::MapInstanceTransform>
MissionEditor::map_instance_transform(const std::uint64_t instance_offset) const {
    const auto file = file_of_kind(MissionFileKind::visual_map);
    if (!file || !instance_record_at(files_[*file].raw, instance_offset)) return std::nullopt;
    const auto& bytes = files_[*file].raw;
    MapInstanceTransform result;
    for (std::size_t i = 0; i < result.rotation.size(); ++i)
        result.rotation[i] = read_real(bytes, instance_offset + 60 + i * 4);
    result.position = {read_real(bytes, instance_offset + 96), read_real(bytes, instance_offset + 100),
                       read_real(bytes, instance_offset + 104)};
    return result;
}

EditResult MissionEditor::set_map_instance_transform(const std::uint64_t instance_offset,
                                                     const std::array<float, 9>& rotation,
                                                     const Vec3 position) {
    return run(*this, "Move map instance @0x" + [&] {
        std::ostringstream out;
        out << std::hex << std::uppercase << instance_offset;
        return out.str();
    }(), [&](MissionTransaction& t) {
        if (!finite(position) || !std::ranges::all_of(rotation, [](const float v) { return std::isfinite(v); }))
            throw std::invalid_argument("The transform must be finite");
        const auto file = file_of_kind(MissionFileKind::visual_map);
        if (!file) throw std::invalid_argument("The mission has no visual map stream");
        auto& bytes = t.raw(*file);
        // Same identification as the RWS reader: a 0x16FC0 record holding a
        // 64-byte Matrix Struct at fixed offsets.
        if (!instance_record_at(bytes, instance_offset))
            throw std::invalid_argument("No CSF scene instance record starts at this offset");
        const auto write_real = [&](const std::uint64_t at, const float value) {
            const auto bits = std::bit_cast<std::uint32_t>(value);
            for (unsigned i = 0; i < 4; ++i)
                bytes[static_cast<std::size_t>(at + i)] = static_cast<std::byte>((bits >> (8U * i)) & 0xFFU);
        };
        for (std::size_t i = 0; i < rotation.size(); ++i) write_real(instance_offset + 60 + i * 4, rotation[i]);
        write_real(instance_offset + 96, position.x);
        write_real(instance_offset + 100, position.y);
        write_real(instance_offset + 104, position.z);
        return t.commit("Moved map instance", {"The collision map is baked separately and does not move with "
                                               "the prop; in-game effect is untested"});
    });
}

// ---- Mission properties ------------------------------------------------------------

EditResult MissionEditor::set_player_actor(const std::int32_t actor_id) {
    return run(*this, "Set starting player", [&](MissionTransaction& t) {
        auto scene = t.scene();
        (void)scene.actor(actor_id);
        required(scene.root, ".PLAYER").set_int(actor_id);
        return t.commit("Starting player is now actor " + std::to_string(actor_id));
    });
}

EditResult MissionEditor::set_start_availability(const bool commando, const bool sniper, const bool spy) {
    return run(*this, "Set starting commandos", [&](MissionTransaction& t) {
        auto scene = t.scene();
        auto& world = required(scene.root, ".MUNDOVIS");
        required(world, ".INICIO_COMMANDO").set_int(commando ? 1 : 0);
        required(world, ".INICIO_SNIPER").set_int(sniper ? 1 : 0);
        required(world, ".INICIO_SPY").set_int(spy ? 1 : 0);
        return t.commit("Set starting commandos");
    });
}

EditResult MissionEditor::set_scores(const std::int32_t maximum, const std::int32_t minimum) {
    return run(*this, "Set mission scores", [&](MissionTransaction& t) {
        if (minimum > maximum) throw std::invalid_argument("The minimum score exceeds the maximum");
        auto scene = t.scene();
        required(scene.root, ".PUNTUACION_MAXIMA").set_int(maximum);
        required(scene.root, ".PUNTUACION_MINIMA").set_int(minimum);
        return t.commit("Set mission scores");
    });
}

namespace {

void assign_scalar(TreeNode& node, const ScalarValue& value) {
    if (const auto* i = std::get_if<std::int32_t>(&value)) {
        if (node.kind != ValueKind::integer) throw std::invalid_argument("The field is not an integer");
        node.set_int(*i);
    } else if (const auto* f = std::get_if<float>(&value)) {
        if (node.kind != ValueKind::real) throw std::invalid_argument("The field is not a real number");
        if (!std::isfinite(*f)) throw std::invalid_argument("Real values must be finite");
        node.set_real(*f);
    } else {
        if (node.kind != ValueKind::string) throw std::invalid_argument("The field is not a string");
        node.set_string(to_1252(std::get<std::string>(value), "Value"));
    }
}

// Finds the node whose value (or label) entry has `entry_index` in the canonical layout.
TreeNode* node_at_entry(std::vector<TreeNode>& nodes, const std::uint32_t entry_index, std::uint32_t& counter) {
    for (auto& node : nodes) {
        if (node.is_container()) {
            if (counter++ == entry_index) return &node;
            if (auto* found = node_at_entry(node.children, entry_index, counter)) return found;
        } else {
            const auto first = counter;
            counter += node.label ? 2 : 1;
            if (entry_index >= first && entry_index < counter) return &node;
        }
    }
    return nullptr;
}

} // namespace

EditResult MissionEditor::set_environment(const std::string_view field, const ScalarValue& value) {
    return run(*this, "Set " + std::string(field), [&](MissionTransaction& t) {
        auto scene = t.scene();
        auto* node = required(scene.root, ".MUNDOVIS").child(field);
        if (!node || node->is_container())
            throw std::invalid_argument("Environment field " + std::string(field) + " is not a scalar");
        assign_scalar(*node, value);
        return t.commit("Set environment " + std::string(field));
    });
}

EditResult MissionEditor::set_scalar(const std::size_t file, const std::uint32_t entry_index,
                                     const ScalarValue& value) {
    return run(*this, "Set entry " + std::to_string(entry_index), [&](MissionTransaction& t) {
        if (file >= files_.size() || !files_[file].present) throw std::invalid_argument("Unknown file");
        auto& tree = t.tree(file);
        std::uint32_t counter = 0;
        auto* node = node_at_entry(tree.roots, entry_index, counter);
        if (!node || node->is_container()) throw std::invalid_argument("The entry is not a scalar value");
        assign_scalar(*node, value);
        return t.commit("Set " + (node->label ? display(node->name()) : "entry " + std::to_string(entry_index)) +
                        " in " + file_display(files_[file]));
    });
}

// ---- Scripts --------------------------------------------------------------------

namespace {

TreeNode& script_list(Tree& tree) { return required(root_of(tree), ".SCRIPTS"); }

// Reports script operands whose targets do not exist in the mission.
std::vector<std::string> unresolved_references(const TreeNode& script, const MissionScene& scene,
                                               const AnimationCatalog& animations,
                                               const std::set<std::int32_t>& script_ids) {
    std::set<std::int32_t> actors, dummies, areas;
    for (const auto& actor : scene.actors())
        if (actor.id) actors.insert(*actor.id);
    for (const auto& dummy : scene.dummies())
        if (dummy.id) dummies.insert(*dummy.id);
    for (const auto& area : scene.areas())
        if (area.id) areas.insert(*area.id);
    std::set<std::string> messages;
    visit_operands(script, [&](const std::string_view tag, const TreeNode& node) {
        const auto value = node.children[1].as_int();
        if (!value || *value == 0) return; // 0 is the conventional "none"
        const auto report = [&](const char* kind) {
            messages.insert(std::string(kind) + " " + std::to_string(*value) + " (" + std::string(tag) +
                            ") does not exist in this mission");
        };
        if (tag == "BICHO" && !actors.contains(*value)) report("Actor");
        else if (tag == "DUMMY" && !dummies.contains(*value)) report("Dummy");
        else if (tag == "ZONA" && !areas.contains(*value)) report("Area");
        else if (tag == "ANM_BDD" && !animations.find_id(*value)) report("Animation");
        else if ((tag == "SCRIPT" || tag == "TRIGGER") && !script_ids.contains(*value)) report("Script");
        else if (tag == "PATHPOINT" && node.children.size() >= 3)
            if (const auto point = node.children[2].as_int(); point && !scene.navigation_point(*value, *point))
                messages.insert("Navigation point " + std::to_string(*value) + "/" + std::to_string(*point) +
                                " does not exist in this mission");
    });
    return {messages.begin(), messages.end()};
}

// Adds animation and class IDs used by scripts to the program's .RECURSOS
// preload lists, as every shipped program lists them there.
void sync_resources(TreeNode& root, const TreeNode& script) {
    auto* resources = root.child(".RECURSOS");
    if (!resources) return;
    const auto add_ids = [&](const std::string_view list_name, const std::string_view tag) {
        auto* list = resources->child(list_name);
        if (!list) return;
        std::set<std::int32_t> present;
        for (const auto& value : list->children)
            if (const auto id = value.as_int()) present.insert(*id);
        visit_operands(script, [&](const std::string_view operand, const TreeNode& node) {
            if (operand != tag) return;
            const auto id = node.children[1].as_int();
            if (id && *id > 0 && present.insert(*id).second)
                list->children.push_back(TreeNode::integer(std::nullopt, *id));
        });
    };
    add_ids(".ANIMACIONES", "ANM_BDD");
    add_ids(".CLASSID", "CLASSID");
}

std::set<std::int32_t> all_script_ids(const std::vector<MissionFile>& files) {
    std::set<std::int32_t> ids;
    for (const auto& file : files) {
        if (!file.present || !is_program(file.kind) || !file.tree) continue;
        if (const auto* scripts = root_of(*file.tree).child(".SCRIPTS"))
            for (const auto& script : scripts->children)
                if (const auto id = record_id(script)) ids.insert(*id);
    }
    return ids;
}

TreeNode parse_script(const std::string_view text) {
    auto script = parse_source_value(text);
    if (script.kind != ValueKind::array || script.label)
        throw std::invalid_argument("A script must be one unlabelled [ ... ] record");
    if (!record_id(script)) throw std::invalid_argument("A script needs an integer .ID");
    // A record without .ACCIONES is valid: shipped cutscene programs have such
    // INIT records (Convoy.csc CUT_INICIOINIT).
    return script;
}

} // namespace

std::optional<std::string> MissionEditor::script_text(const std::size_t file, const std::int32_t script_id) const {
    if (file >= files_.size() || !files_[file].present || !files_[file].tree || !is_program(files_[file].kind))
        return std::nullopt;
    auto& list = script_list(const_cast<Tree&>(*files_[file].tree));
    const auto* script = find_record(list, script_id);
    if (!script) return std::nullopt;
    return to_source_text(*script) + "\n";
}

std::int32_t MissionEditor::next_script_id(const std::size_t) const {
    const auto ids = all_script_ids(files_);
    return ids.empty() ? 1 : *ids.rbegin() + 1;
}

std::string MissionEditor::script_template(const std::int32_t id, const std::string_view utf8_name) {
    std::ostringstream out;
    TreeNode name = TreeNode::string(".NOMBRE", "");
    name.set_string(utf8_to_windows_1252(utf8_name).value_or("NEW_SCRIPT"));
    out << "[\n  .ID " << id << "\n  " << to_source_text(name)
        << "\n  .CARPETA \"\"\n  .FLAGS [\n    .TRIGGER 0\n    .ENABLED 1\n    .VALIDO 1\n  ]\n"
           "  .EVENTOS (\n    (START_GAME)\n  )\n  .ACCIONES {\n    PAUSE (NUMERO 1.0)\n  }\n]\n";
    return out.str();
}

EditResult MissionEditor::set_script_text(const std::size_t file, const std::int32_t script_id,
                                          const std::string_view text) {
    const auto& current_scene = scene();
    const auto& catalog = animations();
    auto ids = all_script_ids(files_);
    return run(*this, "Edit script " + std::to_string(script_id), [&](MissionTransaction& t) {
        if (file >= files_.size() || !is_program(files_[file].kind))
            throw std::invalid_argument("The file is not a mission or cutscene program");
        auto script = parse_script(text);
        const auto new_id = *record_id(script);
        auto& tree = t.tree(file);
        auto& list = script_list(tree);
        auto* existing = find_record(list, script_id);
        if (!existing) throw std::invalid_argument("Script " + std::to_string(script_id) + " does not exist");
        if (new_id != script_id && ids.contains(new_id))
            throw std::invalid_argument("Script ID " + std::to_string(new_id) + " is already used");
        ids.insert(new_id);
        std::vector<std::string> warnings;
        for (const auto& finding : check_script_against_signatures(script)) warnings.push_back(finding.message);
        for (auto& message : unresolved_references(script, current_scene, catalog, ids)) warnings.push_back(message);
        if (new_id != script_id) {
            const auto uses = script_references("SCRIPT", script_id) + script_references("TRIGGER", script_id);
            if (uses) warnings.push_back(std::to_string(uses) + " operand(s) still name the old script ID");
        }
        sync_resources(root_of(tree), script);
        *existing = std::move(script);
        return t.commit("Edited script " + std::to_string(new_id), std::move(warnings));
    });
}

EditResult MissionEditor::add_script(const std::size_t file, const std::string_view text, std::int32_t* new_id) {
    const auto& current_scene = scene();
    const auto& catalog = animations();
    auto ids = all_script_ids(files_);
    const auto next = next_script_id(file);
    return run(*this, "Add script", [&](MissionTransaction& t) {
        if (file >= files_.size() || !is_program(files_[file].kind))
            throw std::invalid_argument("The file is not a mission or cutscene program");
        auto script = parse_script(text);
        auto id = *record_id(script);
        std::vector<std::string> warnings;
        if (ids.contains(id)) {
            warnings.push_back("Script ID " + std::to_string(id) + " was taken; using " + std::to_string(next));
            id = next;
            required(script, ".ID").set_int(id);
        }
        ids.insert(id);
        for (const auto& finding : check_script_against_signatures(script)) warnings.push_back(finding.message);
        for (auto& message : unresolved_references(script, current_scene, catalog, ids)) warnings.push_back(message);
        auto& tree = t.tree(file);
        sync_resources(root_of(tree), script);
        script_list(tree).children.push_back(std::move(script));
        if (new_id) *new_id = id;
        return t.commit("Added script " + std::to_string(id), std::move(warnings));
    });
}

EditResult MissionEditor::delete_script(const std::size_t file, const std::int32_t script_id, const bool force) {
    const auto operand_uses = script_references("SCRIPT", script_id) + script_references("TRIGGER", script_id);
    std::size_t actor_uses = 0;
    if (file < files_.size() && files_[file].kind == MissionFileKind::mission_script)
        for (const auto& actor : scene().actors())
            actor_uses += static_cast<std::size_t>(std::ranges::count(actor.script_ids, script_id));
    return run(*this, "Delete script " + std::to_string(script_id), [&](MissionTransaction& t) {
        if (file >= files_.size() || !is_program(files_[file].kind))
            throw std::invalid_argument("The file is not a mission or cutscene program");
        if ((operand_uses || actor_uses) && !force)
            throw std::invalid_argument("Script " + std::to_string(script_id) + " is run by " +
                                        std::to_string(actor_uses) + " actor(s) and named by " +
                                        std::to_string(operand_uses) + " operand(s)");
        auto& list = script_list(t.tree(file));
        const auto removed = std::erase_if(list.children, [&](const TreeNode& value) {
            return record_id(value) == script_id;
        });
        if (!removed) throw std::invalid_argument("Script " + std::to_string(script_id) + " does not exist");
        if (actor_uses) {
            auto scene_view = t.scene();
            for (auto& actor : scene_view.actors().children)
                if (auto* scripts = actor.child(".SCRIPT")) {
                    std::erase_if(scripts->children, [&](const TreeNode& value) { return value.as_int() == script_id; });
                    if (scripts->children.empty()) remove_child(actor, ".SCRIPT");
                }
        }
        std::vector<std::string> warnings;
        if (operand_uses) warnings.push_back("Operands still name the deleted script");
        return t.commit("Deleted script " + std::to_string(script_id), std::move(warnings));
    });
}

// ---- Mission structure --------------------------------------------------------

namespace {

std::int32_t take_id(const TreeNode& list, const std::optional<std::int32_t> wanted, const char* what) {
    if (!wanted) return next_id(list);
    if (*wanted < 0) throw std::invalid_argument(std::string(what) + " IDs are not negative");
    if (find_record(const_cast<TreeNode&>(list), *wanted))
        throw std::invalid_argument(std::string(what) + " " + std::to_string(*wanted) + " already exists");
    return *wanted;
}

TreeNode nav_point_record(const std::int32_t id, const NavPointSpec& spec) {
    auto point = TreeNode::array(std::nullopt);
    point.children = {TreeNode::integer(".ID", id), TreeNode::string(".NOMBRE", ""), make_vec3(".POS", spec.position),
                      TreeNode::real(".ROT", spec.rotation_radians), TreeNode::real(".ROT_X", spec.pitch_radians)};
    return point;
}

} // namespace

EditResult MissionEditor::add_actor_record(const ActorSpec& spec, std::int32_t* new_id) {
    const bool known_class = !objects().find_class(spec.class_id).empty();
    return run(*this, "Add actor " + spec.name, [&](MissionTransaction& t) {
        if (!known_class)
            throw std::invalid_argument("Class " + std::to_string(spec.class_id) +
                                        " is not in this mission's Objetos.bdd; import it first");
        if (!finite(spec.placement.position) || !std::isfinite(spec.placement.heading_degrees) ||
            !std::isfinite(spec.placement.pitch_degrees))
            throw std::invalid_argument("Position and angles must be finite");
        auto scene = t.scene();
        auto& actors = scene.actors();
        const auto id = take_id(actors, spec.id, "Actor");
        auto name = to_1252(spec.name, "Name");
        if (name.empty()) name = "Actor_" + std::to_string(id);
        if (std::ranges::any_of(actors.children, [&](const TreeNode& a) {
                const auto* n = a.child(".NOMBRE");
                return n && n->as_string() && *n->as_string() == name;
            }))
            throw std::invalid_argument("An actor is already named " + display(name));
        if (spec.cell && !scene.point(spec.cell->first, spec.cell->second))
            throw std::invalid_argument("Placement point " + std::to_string(spec.cell->first) + "/" +
                                        std::to_string(spec.cell->second) + " does not exist");
        auto actor = TreeNode::array(std::nullopt);
        actor.children = {TreeNode::string(".NOMBRE", name),
                          TreeNode::integer(".ID", id),
                          TreeNode::integer(".CLASSID", spec.class_id),
                          make_vec3(".POS", spec.placement.position),
                          TreeNode::real(".ANGULO", spec.placement.heading_degrees),
                          TreeNode::real(".ANGULO_X", spec.placement.pitch_degrees),
                          TreeNode::integer(".COLISION", 1),
                          TreeNode::integer(".FLAGS", 0),
                          TreeNode::integer(".SEGUNDA_EXPLOSION", 0)};
        if (spec.portrait)
            insert_ordered(actor, TreeNode::string(".PORTRAIT", to_1252(*spec.portrait, "Portrait")), actor_order);
        if (!spec.scripts.empty()) {
            auto list = TreeNode::group(".SCRIPT");
            for (const auto script : spec.scripts) list.children.push_back(TreeNode::integer(std::nullopt, script));
            insert_ordered(actor, std::move(list), actor_order);
        }
        set_actor_cell(actor, spec.cell ? spec.cell->first : -1, spec.cell ? spec.cell->second : -1);
        insert_record(actors, std::move(actor));
        if (new_id) *new_id = id;
        return t.commit("Added " + display(name) + " (" + std::to_string(id) + ")");
    });
}

EditResult MissionEditor::batch(std::string label, const std::function<EditResult()>& body) {
    const auto start = cursor_;
    const auto saved = saved_position_;
    auto result = body();
    if (!result.applied) {
        while (cursor_ > start) undo();
        history_.resize(start);
        saved_position_ = saved;
        return result;
    }
    if (cursor_ - start <= 1) return result;
    // One entry whose snapshots replay in order (undo walks them backwards).
    HistoryEntry merged{std::move(label), {}};
    for (auto i = start; i < cursor_; ++i)
        for (auto& snapshot : history_[i].files) merged.files.push_back(std::move(snapshot));
    const auto end = cursor_;
    history_.resize(start);
    history_.push_back(std::move(merged));
    cursor_ = start + 1;
    if (saved_position_ > start && saved_position_ <= end) saved_position_ = ~std::size_t{};
    return result;
}

EditResult MissionEditor::add_scaled_class(const std::int32_t source_class, const float scale, std::int32_t* new_class) {
    return run(*this, "Scale class " + std::to_string(source_class), [&](MissionTransaction& t) {
        if (!(scale > 0.05F && scale < 20.0F)) throw std::invalid_argument("The scale must be between 0.05 and 20");
        const auto objects_file = file_of_kind(MissionFileKind::objects);
        if (!objects_file) throw std::runtime_error("The package has no Objetos.bdd");
        auto& list = required(root_of(t.tree(*objects_file)), ".LISTADATOS");
        const auto* source = find_record(list, source_class);
        if (!source) throw std::invalid_argument("Class " + std::to_string(source_class) + " is not in this mission's Objetos.bdd");
        auto copy = *source;
        auto* model = copy.child(".MODELO");
        if (!model || !model->as_string() || copy.child(".MODELO_TERCERA"))
            throw std::invalid_argument("Only classes with a single body model can be scaled");
        for (const auto& child : copy.children)
            if (child.label && child.name().starts_with(".LOD") && !child.name().ends_with("_DIST"))
                throw std::invalid_argument("Classes with LOD models are not scaled yet");
        if (const auto* collision = copy.child(".MODELO_COLISION"))
            if (collision->child(".CO_MODEL_EX") || collision->child(".CO_MODEL"))
                throw std::invalid_argument("Classes with a collision model (.cmo) are not scaled yet");
        // The body model: the package's .rpc for the class's .dff name.
        const auto dff = std::string(*model->as_string());
        auto rpc = std::filesystem::path(windows_path_to_generic(dff));
        rpc.replace_extension(".rpc");
        std::vector<std::byte> bytes;
        if (const auto file = find_file(rpc)) bytes = files_[*file].bytes();
        else if (std::filesystem::is_regular_file(package_root_ / rpc)) bytes = read_file(package_root_ / rpc);
        else throw std::invalid_argument("The model " + display(dff) + " is not in this mission; import the class first");
        auto scaled = rws::scale_clump(bytes, scale);
        if (!scaled) throw std::invalid_argument("The model could not be scaled: " + scaled.error);
        const auto percent = std::to_string(static_cast<int>(std::lround(scale * 100)));
        auto target = rpc;
        target.replace_filename(rpc.stem().string() + "_x" + percent + ".rpc");
        if (find_file(target)) throw std::invalid_argument("A model " + path_utf8(target) + " already exists");
        MissionFile file;
        file.relative_path = target;
        file.raw = std::move(*scaled.value);
        t.add_file(std::move(file));
        auto new_dff = dff.substr(0, dff.find_last_of("\\/") + 1) + target.stem().string() + ".dff";
        model->set_string(new_dff);
        const auto id = std::max(first_look_class_id, next_id(list));
        required(copy, ".ID").set_int(id);
        if (auto* name = copy.child(".NOMBRE")) name->set_string(record_name(*source) + " x" + std::to_string(scale).substr(0, 4));
        const auto grow = [&](TreeNode* node) {
            if (const auto value = vec3(node)) assign_vec3(*node, {value->x * scale, value->y * scale, value->z * scale});
        };
        if (auto* box = copy.child(".BBOX")) {
            grow(box->child(".INF"));
            grow(box->child(".SUP"));
        }
        if (auto* collision = copy.child(".MODELO_COLISION")) {
            grow(collision->child(".TAM"));
            grow(collision->child(".EXTERNAL_SHAPE_OFFSET"));
        }
        insert_record(list, copy);
        // The model index lists every model the mission loads.
        std::vector<std::string> warnings;
        if (const auto index = file_of_kind(MissionFileKind::model_index)) {
            auto& raw = t.raw(*index);
            auto records = read_index(raw, 4);
            records.push_back({new_dff, {std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}}});
            raw = write_index(records);
        } else {
            warnings.push_back("The package has no model index (.m3d) to list the new model in");
        }
        // The physics descriptor gets the class's entry with the new model and box.
        if (const auto physics_file = file_of_kind(MissionFileKind::physics_index)) {
            auto entries = read_physics(files_[*physics_file].raw);
            const auto original = physics_of(*source), needed = physics_of(copy);
            const auto found = entries && original ? std::ranges::find_if(*entries, [&](const PhysicsEntry& e) {
                return same_physics(e, *original);
            }) : std::vector<PhysicsEntry>::iterator{};
            if (!entries || !original || !needed || found == entries->end()) {
                warnings.push_back("The physics descriptor (.phd) has no entry for class " + std::to_string(source_class) +
                                   ", so none was added for the copy");
            } else {
                auto entry = *found;
                entry.model = needed->model;
                entry.values = needed->values;
                entries->push_back(std::move(entry));
                auto& raw = t.raw(*physics_file);
                raw = write_physics(raw, *entries);
            }
        }
        if (new_class) *new_class = id;
        return t.commit("Added class " + std::to_string(id) + " (class " + std::to_string(source_class) + " at " +
                            percent + "%, model " + display(new_dff) + ")",
                        std::move(warnings));
    });
}

EditResult MissionEditor::add_texture_list_entries(const std::vector<std::string>& entries) {
    return run(*this, "List textures", [&](MissionTransaction& t) {
        const auto file = file_of_kind(MissionFileKind::texture_index);
        if (!file) throw std::invalid_argument("The mission has no texture list (.txl)");
        auto& raw = t.raw(*file);
        std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
        const auto present = lower(text);
        std::size_t added = 0;
        for (const auto& entry : entries) {
            if (entry.empty() || present.find(lower(entry)) != std::string::npos) continue;
            if (!text.empty() && text.back() != '\n') text += "\r\n";
            text += entry + "\r\n";
            ++added;
        }
        raw.assign(reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size()));
        return t.commit("Listed " + std::to_string(added) + " texture(s)");
    });
}

EditResult MissionEditor::new_mission() {
    return run(*this, "New mission", [&](MissionTransaction& t) {
        auto scene = t.scene();
        const auto clear = [](TreeNode* list) {
            if (list) list->children.clear();
        };
        clear(&scene.actors());
        clear(scene.root.child(".EFECTOS"));
        clear(scene.root.child(".AGUAS"));
        clear(&scene.groups());
        clear(scene.navigation().child(".CONEXIONES"));
        clear(&scene.dummies());
        clear(&scene.areas());
        clear(&scene.lights());
        if (auto* objects = scene.root.child(".MALLA_SCENE_OBJS")) clear(objects->child(".SCENEOBJS"));
        clear(scene.root.child(".BRIDGES"));
        // Folders name the removed records.
        for (const auto* owner : {".MALLA_DUMMIES", ".MALLA_LUCES", ".MALLA_AREAS", ".MALLA_NAVEGACION"})
            if (auto* node = scene.root.child(owner)) remove_child(*node, ".CARPETAS");
        remove_child(scene.root, ".CARPETAS");
        for (std::size_t i = 0; i < files_.size(); ++i)
            if (files_[i].present && is_program(files_[i].kind) && files_[i].tree)
                script_list(t.tree(i)).children.clear();
        return t.commit("Emptied the scene and its scripts for a new mission");
    });
}

EditResult MissionEditor::add_dummy(const std::string_view utf8_name, const Vec3 position, const float rotation_radians,
                                    const float pitch_radians, const std::optional<std::int32_t> id,
                                    std::int32_t* new_id) {
    return run(*this, "Add dummy", [&](MissionTransaction& t) {
        if (!finite(position) || !std::isfinite(rotation_radians) || !std::isfinite(pitch_radians))
            throw std::invalid_argument("Position and angles must be finite");
        auto scene = t.scene();
        auto& list = scene.dummies();
        const auto dummy_id = take_id(list, id, "Dummy");
        auto name = to_1252(utf8_name, "Name");
        if (name.empty()) name = "DUMMY_" + std::to_string(dummy_id);
        auto dummy = TreeNode::array(std::nullopt);
        dummy.children = {TreeNode::integer(".ID", dummy_id), TreeNode::string(".NOMBRE", name),
                          make_vec3(".POS", position), TreeNode::real(".ROT", rotation_radians),
                          TreeNode::real(".ROT_X", pitch_radians)};
        insert_record(list, std::move(dummy));
        if (new_id) *new_id = dummy_id;
        return t.commit("Added dummy " + std::to_string(dummy_id));
    });
}

EditResult MissionEditor::add_navigation_group(const std::string_view utf8_name, const std::int32_t type,
                                               const std::vector<NavPointSpec>& points,
                                               const std::vector<std::pair<std::int32_t, std::int32_t>>& links,
                                               const std::optional<std::int32_t> id, std::int32_t* new_id) {
    return run(*this, "Add navigation group", [&](MissionTransaction& t) {
        auto scene = t.scene();
        auto& groups = scene.groups();
        const auto group_id = take_id(groups, id, "Navigation group");
        auto group = TreeNode::array(std::nullopt);
        auto list = TreeNode::group(".PUNTOS");
        for (std::size_t i = 0; i < points.size(); ++i) {
            if (!finite(points[i].position)) throw std::invalid_argument("Point positions must be finite");
            list.children.push_back(nav_point_record(static_cast<std::int32_t>(i + 1), points[i]));
        }
        auto connections = TreeNode::group(".CONEXIONES");
        const auto count = static_cast<std::int32_t>(points.size());
        std::set<std::pair<std::int32_t, std::int32_t>> seen;
        for (const auto& [a, b] : links) {
            if (a < 1 || b < 1 || a > count || b > count || a == b)
                throw std::invalid_argument("Link " + std::to_string(a) + "-" + std::to_string(b) + " names no two points");
            if (!seen.insert(std::minmax(a, b)).second)
                throw std::invalid_argument("Link " + std::to_string(a) + "-" + std::to_string(b) + " is repeated");
            auto connection = TreeNode::array(std::nullopt);
            connection.children = {TreeNode::integer(".PUNTO_ORI", a), TreeNode::integer(".PUNTO_DST", b)};
            connections.children.push_back(std::move(connection));
        }
        group.children = {TreeNode::integer(".ID", group_id), TreeNode::string(".NOMBRE", to_1252(utf8_name, "Name")),
                          TreeNode::integer(".TIPO", type), std::move(list), std::move(connections)};
        insert_record(groups, std::move(group));
        if (new_id) *new_id = group_id;
        return t.commit("Added navigation group " + std::to_string(group_id) + " (" + std::to_string(points.size()) +
                        " points)");
    });
}

EditResult MissionEditor::delete_navigation_group(const std::int32_t group_id, const bool force) {
    std::size_t script_uses = script_references("GRUPO_PATHPOINT", group_id);
    for (const auto& file : files_) {
        if (!file.present || !is_program(file.kind) || !file.tree) continue;
        for (const auto& root : file.tree->roots)
            if (const auto* scripts = root.child(".SCRIPTS"))
                visit_operands(*scripts, [&](const std::string_view tag, const TreeNode& node) {
                    script_uses += tag == "PATHPOINT" && node.children.size() >= 3 &&
                                   node.children[1].as_int() == group_id;
                });
    }
    return run(*this, "Delete navigation group " + std::to_string(group_id), [&](MissionTransaction& t) {
        auto scene = t.scene();
        auto& groups = scene.groups();
        auto* group = find_record(groups, group_id);
        if (!group) throw std::invalid_argument("Navigation group " + std::to_string(group_id) + " does not exist");
        std::size_t actors = 0;
        for (const auto& actor : scene.actors().children)
            if (const auto cell = actor_cell(actor); cell && cell->first == group_id) ++actors;
        if ((actors || script_uses) && !force)
            throw std::invalid_argument("Navigation group " + std::to_string(group_id) + " places " +
                                        std::to_string(actors) + " actor(s) and is named by " +
                                        std::to_string(script_uses) + " script operand(s)");
        std::erase_if(groups.children, [&](const TreeNode& value) { return &value == group; });
        if (auto* links = scene.navigation().child(".CONEXIONES"))
            std::erase_if(links->children, [&](const TreeNode& link) {
                return integer(link.child(".GRUPO_ORI")) == group_id || integer(link.child(".GRUPO_DST")) == group_id;
            });
        std::vector<std::string> warnings;
        if (actors) {
            for (auto& actor : scene.actors().children)
                if (const auto cell = actor_cell(actor); cell && cell->first == group_id) set_actor_cell(actor, -1, -1);
            warnings.push_back("Actors placed in the group have no placement point now");
        }
        if (script_uses) warnings.push_back("Scripts still name the group");
        return t.commit("Deleted navigation group " + std::to_string(group_id), std::move(warnings));
    });
}

EditResult MissionEditor::add_area(const std::string_view utf8_name, const float height, const std::vector<Vec3>& points,
                                   const std::optional<std::int32_t> id, std::int32_t* new_id) {
    return run(*this, "Add area", [&](MissionTransaction& t) {
        if (!std::isfinite(height) || height <= 0) throw std::invalid_argument("Area height must be positive");
        if (const auto problems = area_polygon_problems(points); !problems.empty())
            throw std::invalid_argument("The area " + problems.front() + " (the game crashes loading it)");
        auto scene = t.scene();
        auto& list = scene.areas();
        const auto area_id = take_id(list, id, "Area");
        auto name = to_1252(utf8_name, "Name");
        if (name.empty()) name = "ZONA_" + std::to_string(area_id);
        auto corners = TreeNode::group(".PUNTOS");
        for (const auto& point : points) {
            auto corner = TreeNode::array(std::nullopt);
            corner.children = {make_vec3(".POS", point)};
            corners.children.push_back(std::move(corner));
        }
        auto area = TreeNode::array(std::nullopt);
        area.children = {TreeNode::integer(".ID", area_id), TreeNode::integer(".FLAGS", 1),
                         TreeNode::integer(".OCLUSION", 1), TreeNode::string(".NOMBRE", name),
                         TreeNode::real(".HEIGHT", height), TreeNode::integer(".REVERB", 0),
                         TreeNode::integer(".LIMITREVERB", 0), std::move(corners)};
        insert_record(list, std::move(area));
        if (new_id) *new_id = area_id;
        return t.commit("Added area " + std::to_string(area_id));
    });
}

EditResult MissionEditor::delete_area(const std::int32_t area_id, const bool force) {
    const auto uses = script_references("ZONA", area_id);
    return run(*this, "Delete area " + std::to_string(area_id), [&](MissionTransaction& t) {
        auto scene = t.scene();
        auto& list = scene.areas();
        auto* area = find_record(list, area_id);
        if (!area) throw std::invalid_argument("Area " + std::to_string(area_id) + " does not exist");
        if (uses && !force)
            throw std::invalid_argument("Area " + std::to_string(area_id) + " is named by " + std::to_string(uses) +
                                        " script operand(s)");
        std::erase_if(list.children, [&](const TreeNode& value) { return &value == area; });
        std::vector<std::string> warnings;
        if (uses) warnings.push_back("Scripts still name the area");
        return t.commit("Deleted area " + std::to_string(area_id), std::move(warnings));
    });
}

// ---- Cross-mission import ---------------------------------------------------------

namespace {

struct Donor {
    std::filesystem::path root;
    ResourceIndex index;
    std::optional<Tree> objects, animations, weapons;

    explicit Donor(const std::filesystem::path& package_root)
        : root(std::filesystem::weakly_canonical(package_root)) {
        index.add_root(root);
        index.build();
        const auto load = [&](const char* relative) -> std::optional<Tree> {
            const auto found = index.resolve(relative);
            if (found.candidate_indices.size() != 1) return std::nullopt;
            return Tree::from_bytes(read_file(index.resources()[found.candidate_indices.front()].path));
        };
        objects = load("BDD/Objetos.bdd");
        animations = load("BDD/Anims.bdd");
        weapons = load("BDD/Armas.bdd");
    }

    std::optional<std::filesystem::path> resolve(const std::string_view windows_1252) const {
        auto reference = windows_1252_to_utf8(windows_1252);
        auto found = index.resolve(reference);
        if (found.candidate_indices.empty() && lower(reference).ends_with(".dff")) {
            // Object databases name models as .dff; packages ship them as .rpc.
            reference = reference.substr(0, reference.size() - 4) + ".rpc";
            found = index.resolve(reference);
        }
        if (found.candidate_indices.empty()) return std::nullopt;
        return std::filesystem::weakly_canonical(index.resources()[found.candidate_indices.front()].path);
    }
};

const TreeNode* donor_record(const std::optional<Tree>& tree, const std::string_view list, const std::int32_t id) {
    if (!tree) return nullptr;
    const auto* records = root_of(*tree).child(list);
    if (!records) return nullptr;
    for (const auto& record : records->children)
        if (record_id(record) == id) return &record;
    return nullptr;
}

// Every string in a record that names a package file.
void collect_paths(const TreeNode& node, std::vector<std::string>& output) {
    if (const auto value = node.as_string()) {
        const auto key = lower(std::string(*value));
        for (const auto* extension : {".dff", ".rpc", ".rws", ".cmo", ".anm", ".dds", ".png", ".fbs", ".sp"})
            if (key.ends_with(extension)) {
                output.emplace_back(*value);
                break;
            }
    }
    for (const auto& child : node.children) collect_paths(child, output);
}

void collect_animation_ids(const TreeNode& node, std::set<std::int32_t>& output) {
    if (node.is(".ANIMACIONES"))
        for (const auto& slot : node.children)
            if (const auto id = integer(slot.child(".ID"))) output.insert(*id);
    for (const auto& child : node.children) collect_animation_ids(child, output);
}

} // namespace

namespace {

struct ImportContext {
    MissionEditor& editor;
    MissionTransaction& transaction;
    const Donor& donor;
    std::vector<std::string> notes;
    std::size_t copied_files{};

    // Copies a donor file into the package unless an equivalent path exists.
    std::optional<std::filesystem::path> copy_file(const std::string_view reference) {
        const auto source = donor.resolve(reference);
        if (!source) {
            notes.push_back("Donor file " + display(reference) + " was not found");
            return std::nullopt;
        }
        const auto relative = source->lexically_relative(donor.root);
        if (relative.empty() || *relative.begin() == "..")
            throw std::runtime_error("Donor file is outside the donor package: " + path_utf8(*source));
        if (!editor.find_file(relative) &&
            !std::filesystem::is_regular_file(editor.package_root() / relative)) {
            MissionFile file;
            file.kind = MissionFileKind::asset;
            file.relative_path = relative;
            file.source_path = *source;
            file.raw = read_file(*source);
            transaction.add_file(std::move(file));
            ++copied_files;
        }
        return relative;
    }

    void add_index_entry(const MissionFileKind kind, const std::filesystem::path& relative,
                         const std::vector<std::byte>& suffix) {
        const auto file = editor.file_of_kind(kind);
        if (!file) {
            notes.push_back(std::string("The package has no ") + mission_file_kind_name(kind) + " to update");
            return;
        }
        auto& bytes = transaction.raw(*file);
        auto records = read_index(bytes, suffix.size());
        auto path = windows_path(to_1252(path_utf8(relative), "Path"));
        if (lower(path).ends_with(".rpc")) path = path.substr(0, path.size() - 4) + ".dff";
        const auto key = lower(path);
        if (std::ranges::any_of(records, [&](const IndexRecord& r) { return lower(r.path) == key; })) return;
        records.push_back({path, suffix});
        bytes = write_index(records);
    }

    void add_texture_entries(const std::filesystem::path& model) {
        const auto file = editor.file_of_kind(MissionFileKind::texture_index);
        const auto source = donor.root / model;
        if (!file || !std::filesystem::is_regular_file(source)) return;
        const auto bytes = read_file(source);
        std::set<std::string> names;
        collect_texture_names(bytes, 0, bytes.size(), names, 0);
        if (names.empty()) return;
        // Find each texture in the donor's catalog and copy what is missing.
        const auto donor_txl = [&]() -> std::vector<std::string> {
            for (const auto& resource : donor.index.resources())
                if (lower(path_utf8(resource.path.extension())) == ".txl") {
                    const auto text = read_file(resource.path);
                    std::vector<std::string> lines;
                    std::string line;
                    for (const auto b : text) {
                        const auto c = static_cast<char>(b);
                        if (c == '\n' || c == '\r') {
                            if (!line.empty()) lines.push_back(line);
                            line.clear();
                        } else {
                            line.push_back(c);
                        }
                    }
                    if (!line.empty()) lines.push_back(line);
                    return lines;
                }
            return {};
        }();
        std::string text;
        {
            const auto& txl = transaction.raw(*file);
            text.assign(reinterpret_cast<const char*>(txl.data()), txl.size());
        }
        const auto present = lower(text);
        std::vector<std::string> additions;
        for (const auto& name : names) {
            for (const auto& entry : donor_txl) {
                const auto lowered = lower(entry);
                const auto slash = lowered.find_last_of("\\/");
                const auto stem = lowered.substr(slash == std::string::npos ? 0 : slash + 1);
                if (stem.substr(0, stem.find_last_of('.')) != name) continue;
                if (present.find(lowered) == std::string::npos &&
                    std::ranges::find(additions, entry) == additions.end())
                    additions.push_back(entry);
            }
        }
        // Copying files may grow the file list, so the list is rewritten after.
        for (const auto& entry : additions) copy_file(entry);
        for (const auto& entry : additions) {
            if (!text.empty() && text.back() != '\n') text += "\r\n";
            text += entry + "\r\n";
        }
        auto& txl = transaction.raw(*file);
        txl.assign(reinterpret_cast<const std::byte*>(text.data()),
                   reinterpret_cast<const std::byte*>(text.data() + text.size()));
    }

    void import_animation_record(const std::int32_t id) {
        if (editor.animations().find_id(id)) return;
        const auto* record = donor_record(donor.animations, ".LISTADATOS", id);
        if (!record) {
            notes.push_back("Animation " + std::to_string(id) + " is not in the donor's Anims.bdd");
            return;
        }
        const auto file = editor.file_of_kind(MissionFileKind::animations);
        if (!file) throw std::runtime_error("The package has no Anims.bdd");
        auto& list = required(root_of(transaction.tree(*file)), ".LISTADATOS");
        if (find_record(list, id)) return;
        insert_record(list, *record);
        std::vector<std::string> paths;
        collect_paths(*record, paths);
        for (const auto& reference : paths)
            if (const auto relative = copy_file(reference))
                if (lower(path_utf8(*relative)).ends_with(".anm"))
                    add_index_entry(MissionFileKind::animation_index, *relative, animation_flags(*relative));
    }

    // The two .and flag bytes are not understood, so an imported animation keeps
    // the donor's; 00 01 is the most common value in shipped indexes.
    std::vector<std::byte> animation_flags(const std::filesystem::path& relative) const {
        const auto wanted = lower(windows_path(to_1252(path_utf8(relative), "Path")));
        for (const auto& resource : donor.index.resources()) {
            if (lower(path_utf8(resource.path.extension())) != ".and") continue;
            try {
                for (const auto& record : read_index(read_file(resource.path), 2))
                    if (lower(windows_path(record.path)) == wanted) return record.suffix;
            } catch (const std::runtime_error&) {
            }
        }
        return {std::byte{0x00}, std::byte{0x01}};
    }
};

} // namespace

EditResult MissionEditor::import_animation(const std::filesystem::path& donor_package_root,
                                           const std::int32_t animation_id) {
    return run(*this, "Import animation " + std::to_string(animation_id), [&](MissionTransaction& t) {
        if (animations().find_id(animation_id))
            throw std::invalid_argument("Animation " + std::to_string(animation_id) + " is already in Anims.bdd");
        const Donor donor(donor_package_root);
        if (!donor_record(donor.animations, ".LISTADATOS", animation_id))
            throw std::invalid_argument("The donor has no animation " + std::to_string(animation_id));
        ImportContext context{*this, t, donor, {}, 0};
        context.import_animation_record(animation_id);
        context.notes.push_back("Copied " + std::to_string(context.copied_files) + " file(s)");
        return t.commit("Imported animation " + std::to_string(animation_id), std::move(context.notes));
    });
}

EditResult MissionEditor::import_class(const std::filesystem::path& donor_package_root, const std::int32_t class_id) {
    return run(*this, "Import class " + std::to_string(class_id), [&](MissionTransaction& t) {
        if (!objects().find_class(class_id).empty())
            throw std::invalid_argument("Class " + std::to_string(class_id) + " is already in Objetos.bdd");
        const Donor donor(donor_package_root);
        const auto* record = donor_record(donor.objects, ".LISTADATOS", class_id);
        if (!record) throw std::invalid_argument("The donor has no class " + std::to_string(class_id));
        const auto objects_file = file_of_kind(MissionFileKind::objects);
        if (!objects_file) throw std::runtime_error("The package has no Objetos.bdd");
        ImportContext context{*this, t, donor, {}, 0};

        insert_record(required(root_of(t.tree(*objects_file)), ".LISTADATOS"), *record);

        std::vector<std::string> paths;
        collect_paths(*record, paths);
        // Weapons the class carries come with their own models.
        if (const auto* weapons = record->child(".ARMAS"))
            for (const auto& weapon : weapons->children)
                if (const auto weapon_id = weapon.as_int()) {
                    const auto weapons_file = file_of_kind(MissionFileKind::weapons);
                    if (!weapons_file) break;
                    auto& list = required(root_of(t.tree(*weapons_file)), ".LISTADATOS");
                    if (find_record(list, *weapon_id)) continue;
                    if (const auto* weapon_record = donor_record(donor.weapons, ".LISTADATOS", *weapon_id)) {
                        insert_record(list, *weapon_record);
                        collect_paths(*weapon_record, paths);
                    } else {
                        context.notes.push_back("Weapon " + std::to_string(*weapon_id) + " is not in the donor");
                    }
                }
        for (const auto& reference : paths) {
            const auto relative = context.copy_file(reference);
            if (!relative) continue;
            const auto extension = lower(path_utf8(relative->extension()));
            if (extension == ".rpc") {
                context.add_index_entry(MissionFileKind::model_index, *relative,
                                        {std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}});
                context.add_texture_entries(*relative);
            }
        }
        std::set<std::int32_t> animation_ids;
        collect_animation_ids(*record, animation_ids);
        for (const auto id : animation_ids) context.import_animation_record(id);
        context.notes.push_back("Copied " + std::to_string(context.copied_files) + " file(s)");
        if (record->child(".PHYSIC"))
            context.notes.push_back("The physics descriptor (.phd) was not updated; its schema is unknown");
        return t.commit("Imported class " + std::to_string(class_id), std::move(context.notes));
    });
}

// ---- Saving ----------------------------------------------------------------------

std::vector<std::filesystem::path> MissionEditor::save(ModProject& project) {
    std::vector<std::filesystem::path> written;
    const auto modified = modified_files();
    // A degenerate area polygon crashes the level load (KB-scn-9).
    if (std::ranges::find(modified, scene_file_) != modified.end())
        for (const auto& area : scene().areas())
            if (const auto problems = area_polygon_problems(area.points); !problems.empty())
                throw std::runtime_error("Refusing to save: area " + std::to_string(area.id.value_or(-1)) + " " +
                                         problems.front() + " (the game crashes loading it)");
    std::set<std::string> keep;
    const auto authored_root = std::filesystem::weakly_canonical(project.workspace_root / "authored");
    for (const auto index : modified) {
        const auto& file = files_[index];
        const auto bytes = file.bytes();
        // A file the project takes from elsewhere (an authoring project's
        // build/, `csf-mod add`) stays there unless an edit here changed it.
        const auto external = std::ranges::find_if(project.files, [&](const ModFile& value) {
            if (path_key(value.relative_path) != path_key(file.relative_path)) return false;
            const auto relative = std::filesystem::weakly_canonical(value.authored_path).lexically_relative(authored_root);
            return relative.empty() || *relative.begin() == "..";
        });
        if (external != project.files.end() && (file.revision == 0 || sha256(bytes) == external->output_sha256)) {
            keep.insert(path_key(file.relative_path));
            continue;
        }
        if (file.tree) {
            const auto check = Document::from_bytes(bytes);
            if (check.has_errors() || check.state() != ParseState::exact)
                throw std::runtime_error("Refusing to save invalid CSFFBS output " + file_display(file));
        }
        const auto output = project.workspace_root / "authored" / file.relative_path;
        write_file_atomically(output, bytes);
        auto manifest = output;
        manifest += ".changes.json";
        std::ostringstream json;
        json << "{\n  \"schema\":\"csf-mission-edit-1\",\n  \"relative_path\":\""
             << json_escape(path_utf8(file.relative_path)) << "\",\n  \"source_sha256\":\""
             << file.source_sha256 << "\",\n  \"output_sha256\":\"" << sha256(bytes)
             << "\",\n  \"added\":" << (file.added ? "true" : "false") << ",\n  \"operations\":[";
        bool first = true;
        for (std::size_t i = 0; i < cursor_; ++i)
            if (std::ranges::any_of(history_[i].files, [&](const FileSnapshot& s) { return s.file == index; })) {
                json << (first ? "\n" : ",\n") << "    \"" << json_escape(history_[i].label) << '"';
                first = false;
            }
        json << (first ? "" : "\n  ") << "]\n}\n";
        const auto text = json.str();
        write_file_atomically(manifest, std::as_bytes(std::span(text)));
        ModFile entry;
        entry.relative_path = file.relative_path;
        entry.authored_path = output;
        entry.change_manifest = manifest;
        entry.source_sha256 = file.source_sha256;
        entry.output_sha256 = sha256(bytes);
        project.add_file(std::move(entry));
        keep.insert(path_key(file.relative_path));
        written.push_back(output);
    }
    // Files that are back to their source content leave the project.
    std::erase_if(project.files, [&](const ModFile& value) {
        const auto key = path_key(value.relative_path);
        if (keep.contains(key)) return false;
        const auto owned = std::ranges::any_of(files_, [&](const MissionFile& file) {
            return path_key(file.relative_path) == key;
        });
        if (!owned) return false;
        std::error_code ignored;
        std::filesystem::remove(value.authored_path, ignored);
        std::filesystem::remove(value.change_manifest, ignored);
        return true;
    });
    project.save();
    saved_position_ = cursor_;
    return written;
}

std::optional<MissionProjectInfo> read_mission_project_info(const std::filesystem::path& workspace) {
    std::ifstream input(workspace / ".csf-mission");
    std::string magic, scene, archive;
    std::getline(input, magic);
    if (!input || magic != "csf-mission-1") return std::nullopt;
    input >> std::quoted(scene) >> std::quoted(archive);
    if (!input) return std::nullopt;
    const auto to_path = [](const std::string& value) {
        return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
    };
    return MissionProjectInfo{to_path(scene), to_path(archive)};
}

void write_mission_project_info(const std::filesystem::path& workspace, const MissionProjectInfo& info) {
    std::ostringstream out;
    out << "csf-mission-1\n" << std::quoted(path_utf8(info.scene)) << ' '
        << std::quoted(path_utf8(info.original_archive)) << '\n';
    const auto text = out.str();
    write_file_atomically(workspace / ".csf-mission", std::as_bytes(std::span(text)));
}

const char* mission_file_kind_name(const MissionFileKind kind) noexcept {
    switch (kind) {
    case MissionFileKind::scene: return "scene";
    case MissionFileKind::mission_script: return "mission script";
    case MissionFileKind::cutscene_script: return "cutscene script";
    case MissionFileKind::objects: return "object database";
    case MissionFileKind::animations: return "animation database";
    case MissionFileKind::weapons: return "weapon database";
    case MissionFileKind::database: return "database";
    case MissionFileKind::model_index: return "model index (.m3d)";
    case MissionFileKind::animation_index: return "animation index (.and)";
    case MissionFileKind::texture_index: return "texture list (.txl)";
    case MissionFileKind::physics_index: return "physics descriptor (.phd)";
    case MissionFileKind::visual_index: return "visual index (.vis)";
    case MissionFileKind::visual_map: return "visual map (.rws)";
    case MissionFileKind::asset: return "asset";
    }
    return "file";
}

} // namespace csf
