#include "csf/godot_export.hpp"

#include "csf/animation_catalog.hpp"
#include "csf/authoring_project.hpp"
#include "csf/mission_edit.hpp"
#include "csf/object_database.hpp"
#include "csf/project_pipeline.hpp"
#include "rws/document.hpp"
#include "rws/scene_export.hpp"
#include "rws/texture_image.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>

namespace csf {
namespace {

constexpr std::string_view manifest_format = "opencsf-godot-manifest";

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string json_string(const std::string_view value) {
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"' || c == '\\') out += {'\\', c};
        else if (c == '\n') out += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\u00";
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        } else out += c;
    }
    return out + '"';
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << text;
    if (!file) throw std::runtime_error("Cannot write " + path.generic_string());
}

// The out folder must be new, empty, or an earlier export: never someone's files.
void check_out_folder(const std::filesystem::path& out) {
    std::error_code error;
    if (!std::filesystem::exists(out, error) || std::filesystem::is_empty(out, error)) return;
    std::ifstream manifest(out / "manifest.json", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(manifest)), std::istreambuf_iterator<char>());
    if (text.find(json_string(manifest_format)) == std::string::npos)
        throw std::runtime_error(out.generic_string() + " is not empty and holds no earlier Godot export");
}

// How a texture's alpha is used: none, cut-out (almost every transparent pixel
// fully so, as DXT1's one-bit alpha), or blended (glass, smoke).
rws::SceneTexture::Alpha alpha_of(const std::vector<std::uint8_t>& rgba) {
    std::size_t transparent{}, partial{};
    for (std::size_t i = 3; i < rgba.size(); i += 4) {
        if (rgba[i] < 250) ++transparent;
        if (rgba[i] >= 8 && rgba[i] < 250) ++partial;
    }
    if (transparent == 0) return rws::SceneTexture::Alpha::opaque;
    return partial * 4 > transparent ? rws::SceneTexture::Alpha::blend : rws::SceneTexture::Alpha::mask;
}

// Converts the DDS/PNG textures glTF files name into PNGs under textures/,
// mirroring their corpus folders (lower case) so equal names never collide.
// A texture is looked up as the editor does: beside the file that names it,
// then in each Textures folder from there up to the mission package.
class TextureConverter {
public:
    TextureConverter(std::filesystem::path out, std::filesystem::path corpus, std::filesystem::path package_root)
        : out_(std::move(out)), corpus_(std::move(corpus)), package_root_(std::move(package_root)) {}

    // A resolver for a glTF written to `gltf` from the source file `source`.
    rws::SceneTextureResolver resolver(const std::filesystem::path& source, const std::filesystem::path& gltf) {
        return [this, source, gltf](const std::string_view name) -> std::optional<rws::SceneTexture> {
            const auto found = find(source, lower(std::string(name)));
            if (!found) {
                missing_.insert(std::string(name));
                return std::nullopt;
            }
            const auto png = convert(*found);
            if (!png) return std::nullopt;
            return rws::SceneTexture{(out_ / png->first).lexically_relative(gltf.parent_path()).generic_string(), png->second};
        };
    }
    [[nodiscard]] std::size_t converted() const noexcept { return converted_.size(); }
    [[nodiscard]] const std::set<std::string>& missing() const noexcept { return missing_; }
    [[nodiscard]] const std::map<std::string, std::string>& failed() const noexcept { return failed_; }

private:
    // Lower-case entry name (files and folders) -> path, per folder, read once.
    const std::map<std::string, std::filesystem::path>& listing(const std::filesystem::path& folder) {
        auto [it, added] = listings_.try_emplace(folder);
        if (added) {
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(folder, error))
                it->second.emplace(lower(entry.path().filename().string()), entry.path());
        }
        return it->second;
    }
    std::optional<std::filesystem::path> find(const std::filesystem::path& source, const std::string& name) {
        const auto has_extension = std::filesystem::path(name).has_extension();
        const auto in = [&](const std::filesystem::path& folder) -> std::optional<std::filesystem::path> {
            const auto& files = listing(folder);
            for (const auto& candidate : has_extension ? std::vector{name} : std::vector{name + ".dds", name + ".png"})
                if (const auto it = files.find(candidate); it != files.end()) return it->second;
            return std::nullopt;
        };
        auto folder = source.parent_path();
        if (auto found = in(folder)) return found;
        for (;;) {
            if (const auto textures = listing(folder).find("textures"); textures != listing(folder).end())
                if (auto found = in(textures->second)) return found;
            if (folder == package_root_ || !folder.has_parent_path() || folder.parent_path() == folder) return std::nullopt;
            folder = folder.parent_path();
        }
    }
    std::optional<std::pair<std::filesystem::path, rws::SceneTexture::Alpha>> convert(const std::filesystem::path& image) {
        if (const auto it = converted_.find(image); it != converted_.end()) return it->second;
        if (failed_.contains(image.generic_string())) return std::nullopt;
        int width{}, height{};
        std::vector<std::uint8_t> rgba;
        std::string error;
        if (!rws::decode_texture_image(image, width, height, rgba, error)) {
            failed_[image.generic_string()] = error;
            return std::nullopt;
        }
        auto relative = std::filesystem::path(lower(image.lexically_relative(corpus_).generic_string()));
        relative = std::filesystem::path("textures") / relative.replace_extension(".png");
        std::filesystem::create_directories((out_ / relative).parent_path());
        std::filesystem::remove(out_ / relative);
        if (!rws::write_png_rgba(out_ / relative, width, height, rgba, error)) {
            failed_[image.generic_string()] = error;
            return std::nullopt;
        }
        return converted_[image] = {relative, alpha_of(rgba)};
    }

    std::filesystem::path out_, corpus_, package_root_;
    std::map<std::filesystem::path, std::map<std::string, std::filesystem::path>> listings_;
    std::map<std::filesystem::path, std::pair<std::filesystem::path, rws::SceneTexture::Alpha>> converted_;
    std::map<std::string, std::string> failed_;  // path -> why
    std::set<std::string> missing_;              // names found nowhere
};

// Names a CSF human's joint by its HAnim node ID with Godot's
// SkeletonProfileHumanoid names, so any humanoid animation retargets onto it.
// Every shipped human has the same 44 IDs (a 3ds Max Biped); the table was
// read from their bind pose: the model faces +Z and its left is +X, as in
// glTF. Others (the Biped centre, the motion node, hand and holster dummies,
// finger and head ends) keep bone_<id>.
std::string humanoid_joint_name(const std::int32_t node_id) {
    static const std::map<std::int32_t, std::string_view> names{
        {2000, "Root"},          {1045, "Hips"},           {20, "Spine"},           {30, "Chest"},
        {1040, "Neck"},          {60, "Head"},             {1046, "Jaw"},
        {1003, "LeftShoulder"},  {1039, "LeftUpperArm"},   {2004, "LeftLowerArm"},  {1021, "LeftHand"},
        {1004, "LeftThumbProximal"},  {1005, "LeftIndexProximal"},
        {1057, "RightShoulder"}, {1093, "RightUpperArm"},  {2007, "RightLowerArm"}, {1075, "RightHand"},
        {1058, "RightThumbProximal"}, {1059, "RightIndexProximal"},
        {1023, "LeftUpperLeg"},  {1002, "LeftLowerLeg"},   {1019, "LeftFoot"},      {1024, "LeftToes"},
        {1077, "RightUpperLeg"}, {1056, "RightLowerLeg"},  {1073, "RightFoot"},     {1078, "RightToes"},
    };
    const auto it = names.find(node_id);
    return it == names.end() ? std::string{} : std::string(it->second);
}

// Where the humanoid profile hangs a joint that the Biped hangs elsewhere, so
// retargeting (which copies rotations relative to the parent) sees the same
// chain on both sides: the Hips straight under the Root (the Biped centre, ID
// 1, becomes a leaf that keeps its own motion), the clavicles under the Chest
// rather than the Neck, and the holster dummies (70-72: canteen, bag, spade)
// under the Hips, which every humanoid clip moves.
std::optional<std::int32_t> humanoid_joint_parent(const std::int32_t node_id) {
    switch (node_id) {
    case 1045: return 2000;
    case 1003:
    case 1057: return 30;
    case 70:
    case 71:
    case 72: return 1045;
    default: return std::nullopt;
    }
}

// Which clip plays each role, per stance. The game picks clips by slot in
// code we have not joined to data yet (docs/format-reversal/anm, KB-anm-3), so
// these tables are our reading of the clip names. Soldiers share one scheme: a
// weapon prefix (SF rifle, SM submachine gun, SP pistol) and a suffix: Id idle,
// DPie standing, Aler/Al alert, Aga/Ag agachado (crouched), Ocio fidget, Cm
// caminar (walk), Cr correr (run), Ade/Atr/Izq/Der forward/back/left/right
// (Iz/De/At when walking), Cubi cubierto (bent over), Disp disparar (shoot),
// Rec/Recar recargar (reload), Imp impacto (hit), Pier pierna (leg), Dead
// (die), V volar (thrown by a blast), sospecha (suspicious), Levant stand up,
// Agacha crouch down, G giro (turn) from D de pie (standing), A alert or H
// crouched, to the right (A) or left (B): measured from the hips' height and
// yaw in the exported clips. Speeds come from each clip's .VEL.
struct ClipRole {
    std::string stance, role, clip;
};
struct RoleClip {
    std::string_view role, clip;
};
constexpr std::pair<std::string_view, std::string_view> soldier_stances[]{{"rifle", "SF"}, {"smg", "SM"}, {"pistol", "SP"}};
constexpr RoleClip soldier_roles[]{
    {"idle", "IdDPie"},           {"idle_fidget", "IdOcio"},      {"idle_alert", "IdAler"},
    {"idle_alert_fidget", "IdAlertOcio"},                         {"suspicious", "sospecha"},
    {"walk", "Cm"},               {"walk_left", "CmIz"},          {"walk_right", "CmDe"},
    {"walk_alert", "CmAler"},     {"walk_alert_back", "CmAlAt"},  {"walk_alert_left", "CmAlIz"},
    {"walk_alert_right", "CmAlDe"},
    {"run", "CrAde"},             {"run_back", "CrAtr"},          {"run_left", "CrIzq"},
    {"run_right", "CrDer"},       {"run_crouched", "CrCubi"},
    {"crouch", "Agacha"},         {"stand_up", "Levant"},         {"crouch_idle", "IdAga"},
    {"crouch_fidget", "IdAgaOcio"},
    {"crouch_walk", "CmAgac"},    {"crouch_walk_back", "CmAgAt"}, {"crouch_walk_left", "CmAgIz"},
    {"crouch_walk_right", "CmAgDe"},
    {"shoot", "Disp"},            {"crouch_shoot", "DispAg"},     {"reload", "Recar"},
    {"crouch_reload", "RecAg"},   {"throw_grenade", "GranadaA"},  {"throw_grenade_b", "GranadaB"},
    {"hit", "Impac"},             {"hit_alert", "ImpAle"},        {"hit_leg", "ImpPier"},
    {"crouch_hit", "ImpAg"},      {"gassed", "Gas"},              {"crouch_gassed", "AGas"},
    {"die", "DeadAl"},            {"crouch_die", "DeadAg"},       {"run_die", "DeadCr"},
    {"die_blast", "DeadV"},       {"die_fire", "DeadFire"},       {"die_gas", "DeadGas"},
    {"turn_right_90", "DGA90"},   {"turn_right_180", "DGA180"},   {"turn_left_90", "DGB90"},
    {"turn_left_180", "DGB180"},  {"alert_turn_right_90", "AGA90"},  {"alert_turn_right_180", "AGA180"},
    {"alert_turn_left_90", "AGB90"},   {"alert_turn_left_180", "AGB180"},
    {"crouch_turn_right_90", "HGA90"}, {"crouch_turn_right_180", "HGA180"},
    {"crouch_turn_left_90", "HGB90"},  {"crouch_turn_left_180", "HGB180"},
};
// Civilians and the unarmed (Anims/Costum/Civil*).
constexpr RoleClip unarmed_roles[]{
    {"idle", "CivilIdle"},        {"idle_alert", "CivilIdleAler"}, {"walk", "CivilAnda"},
    {"run", "CivilCr"},           {"crouch", "CivilAgachar"},      {"stand_up", "CivilLevantar"},
    {"crouch_idle", "CivilIdleAgachado"},                          {"crouch_talk", "CivilAgachadoHabla"},
    {"hit", "CivilImpac"},        {"crouch_hit", "CivilImpacAg"},
    {"crouch_turn_right_90", "CivilHGA90"}, {"crouch_turn_right_180", "CivilHGA180"},
    {"crouch_turn_left_90", "CivilHGB90"},  {"crouch_turn_left_180", "CivilHGB180"},
};
// Anything-stance actions from Anims/Enem: being taken out, sitting, the MG
// post, the Kubelwagen seats (SPKuv, driver and back).
constexpr RoleClip action_roles[]{
    {"threatened", "Amenazado"},  {"stabbed", "Acuchillado"},     {"garrotted", "Ahorcado"},
    {"strangled", "Estrangulado"}, {"strangle", "Estrangular"},   {"put_on_clothes", "EPonerseRopa"},
    {"sit_idle", "SSDKReposo"},   {"sit_idle_unarmed", "SSDKReposo_Sin_Armas"},
    {"sit_down", "SSDKSienta"},   {"sit_stand_up", "SSDKLevant"}, {"sit_die", "SSDKDead"},
    {"mg_idle", "MGId1"},         {"mg_shoot", "MGDisp"},         {"mg_die", "MGDead"},
    {"car_idle", "SPKuvIdle"},    {"car_in", "SPKuvIn"},          {"car_out", "SPKuvOut"},
    {"car_die", "SPKuvDead"},     {"car_back_idle", "SPKuvBackIdle"}, {"car_back_in", "SPKuvBackIn"},
    {"car_back_out", "SPKuvBackOut"}, {"car_back_die", "SPKuvBackDead"},
};

std::vector<ClipRole> clip_roles() {
    std::vector<ClipRole> roles;
    for (const auto& [stance, prefix] : soldier_stances)
        for (const auto& role : soldier_roles)
            roles.push_back({std::string(stance), std::string(role.role), std::string(prefix) + std::string(role.clip)});
    for (const auto& role : unarmed_roles) roles.push_back({"unarmed", std::string(role.role), std::string(role.clip)});
    for (const auto& role : action_roles) roles.push_back({"actions", std::string(role.role), std::string(role.clip)});
    return roles;
}


// One manifest entry: its logical ID and its fields, already JSON.
using ManifestEntry = std::vector<std::pair<std::string, std::string>>;

constexpr std::string_view markers_format = "opencsf-markers";
constexpr float units_per_metre = 100.0F;  // divided, so 10 units print as 0.1
constexpr float radians_per_degree = 0.01745329251994329577F;

std::string json_number(const float value) {
    std::array<char, 32> buffer{};
    const auto end = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value == 0.0F ? 0.0F : value).ptr;
    return {buffer.data(), end};
}

std::string json_metres(const Vec3& v) {
    return "[" + json_number(v.x / units_per_metre) + "," + json_number(v.y / units_per_metre) + "," +
           json_number(v.z / units_per_metre) + "]";
}

// Navigation group .TIPO (docs/format-reversal/scn, KB-scn-11).
std::string route_kind(const std::int32_t type) {
    switch (type) {
    case 0: return "path";
    case 2: return "patrol";
    case 3: return "cover";
    case 4: return "ladder";
    default: return "type_" + std::to_string(type);
    }
}

// Which side an actor is on: its own .BANDO when it has one (shipped values:
// ALEMAN, ALIADO, NEUTRO), else what its class's actor kind (.TIPO in
// Objetos.bdd) implies. Only the humanoid kinds imply one (Russians fight on
// the Allied side: our reading); vehicles, items and scenery have none.
std::string faction_of(const std::optional<std::string>& bando, const std::string_view kind) {
    if (bando) {
        const auto value = lower(*bando);
        if (value == "aleman") return "german";
        if (value == "aliado") return "allied";
        if (value == "neutro") return "neutral";
        return value;
    }
    if (kind == "player") return "player";
    if (kind == "aleman") return "german";
    if (kind == "ruso") return "allied";
    return {};
}

} // namespace

namespace {

// The stance a weapon's .TIPO puts its holder in (see godot_stance), or empty.
std::string weapon_stance(const WeaponDefinition& weapon) {
    if (!weapon.type) return {};
    const auto type = lower(*weapon.type);
    if (type == "rifle" || type == "rifle_precision" || type == "escopeta" || type == "mg") return "rifle";
    if (type == "smg") return "smg";
    if (type == "pistola" || type == "pistola_silenciador") return "pistol";
    if (type == "desarmado" || type == "mano") return "unarmed";
    return {};
}

// The weapon that gives a class its stance: its first that has one.
const WeaponDefinition* held_weapon(const ObjectDefinition& definition, const WeaponDatabase& weapons) {
    for (const auto id : definition.weapon_ids)
        if (const auto* weapon = weapons.find_id(id); weapon && !weapon_stance(*weapon).empty()) return weapon;
    return nullptr;
}

using Transform = std::array<float, 12>;  // basis columns x, y, z, then origin

// A weapon model's attachment frames in the space its glTF is written in
// (the Clump's, in metres): `grip`, the frame its mesh hangs on, whose origin
// is where the hand closes (on every shipped third-person weapon: X along the
// barrel, Z up), and `muzzle`, the frame whose 3ds Max user property is
// tag=100 (at the barrel's end on every one).
struct WeaponFrames {
    std::optional<Transform> grip, muzzle;
};
WeaponFrames weapon_frames(const rws::Chunk& clump, const std::span<const std::byte> bytes) {
    WeaponFrames result;
    const auto* list = rws::find_child(clump, 0x0E);
    const auto frames = list ? rws::decode_frame_list(*list, bytes) : rws::DecodeResult<rws::FrameListInfo>{};
    if (!frames) return result;
    const auto& info = frames.value->frames;
    // RenderWare stores a frame's axes as rows (right, up, at): they are the basis columns.
    std::vector<std::optional<Transform>> world(info.size());
    const auto resolve = [&](auto&& self, const std::size_t index, const int depth) -> std::optional<Transform> {
        if (index >= info.size() || depth > 64) return std::nullopt;
        if (world[index]) return world[index];
        const auto& frame = info[index];
        Transform local{frame.rotation[0], frame.rotation[1], frame.rotation[2], frame.rotation[3], frame.rotation[4],
                        frame.rotation[5], frame.rotation[6], frame.rotation[7], frame.rotation[8],
                        frame.position.x * 0.01F, frame.position.y * 0.01F, frame.position.z * 0.01F};
        if (frame.parent >= 0) {
            const auto parent = self(self, static_cast<std::size_t>(frame.parent), depth + 1);
            if (!parent) return std::nullopt;
            Transform composed{};
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 3; ++row) {
                    float value = column == 3 ? (*parent)[9 + row] : 0.0F;
                    for (int k = 0; k < 3; ++k) value += (*parent)[k * 3 + row] * local[column * 3 + k];
                    composed[column * 3 + row] = value;
                }
            local = composed;
        }
        return world[index] = local;
    };
    for (const auto& child : clump.children)
        if (child.type == 0x14) {
            if (const auto atomic = rws::decode_atomic(child, bytes); atomic && atomic.value->frame_index >= 0)
                result.grip = resolve(resolve, static_cast<std::size_t>(atomic.value->frame_index), 0);
            break;
        }
    // Frame extensions follow the list's Struct, one per frame, in order.
    std::size_t index = 0;
    for (const auto& extension : list->children) {
        if (extension.type != 0x03) continue;
        if (const auto* user = rws::find_child(extension, 0x11F))
            if (const auto data = rws::decode_user_data(*user, bytes))
                for (const auto& array : data.value->arrays)
                    for (const auto& text : array.strings)
                        if (text == "tag=100") result.muzzle = resolve(resolve, index, 0);
        ++index;
    }
    return result;
}

std::string json_transform(const Transform& transform) {
    std::string out = "[";
    for (std::size_t i = 0; i < transform.size(); ++i) out += (i ? "," : "") + json_number(transform[i]);
    return out + "]";
}

} // namespace

std::string godot_stance(const ObjectDefinition& definition, const WeaponDatabase& weapons) {
    const auto* weapon = held_weapon(definition, weapons);
    return weapon ? weapon_stance(*weapon) : std::string{};
}

// The mission's placements for a game engine, in its own units: metres on the
// glTF axes (the map's), angles in radians. `yaw` turns about +Y so that +Z,
// where every model faces, turns to (sin yaw, 0, cos yaw); `pitch` turns about
// X after it (Godot's default YXZ Euler order: rotation = (pitch, yaw, 0)).
std::string godot_markers_json(const MissionScene& scene, const ObjectDatabase& objects,
                               const std::map<std::int32_t, std::string>& class_assets,
                               const std::map<std::int32_t, std::string>& class_stances,
                               const std::map<std::int32_t, std::string>& class_weapons) {
    std::string out = "{\n\"format\":" + json_string(markers_format) +
                      ",\n\"version\":1,\n\"units\":\"metres\",\n\"up\":\"Y\",\n\"actors\":[";
    bool first = true;
    for (const auto& actor : scene.actors()) {
        const auto position = scene.actor_spawn_position(actor);
        if (!actor.id || !position) continue;
        std::string kind, asset, stance, weapon;
        if (actor.class_id) {
            if (const auto definitions = objects.find_class(*actor.class_id); definitions.size() == 1 && definitions.front()->type)
                kind = lower(*definitions.front()->type);
            if (const auto found = class_assets.find(*actor.class_id); found != class_assets.end()) asset = found->second;
            if (const auto found = class_stances.find(*actor.class_id); found != class_stances.end()) stance = found->second;
            if (const auto found = class_weapons.find(*actor.class_id); found != class_weapons.end()) weapon = found->second;
        }
        const auto faction = faction_of(actor.faction, kind);
        out += std::string(first ? "\n" : ",\n") + "{\"id\":" + std::to_string(*actor.id) +
               ",\"name\":" + json_string(actor.name.value_or("")) + ",\"class\":" + std::to_string(actor.class_id.value_or(-1));
        if (!kind.empty()) out += ",\"kind\":" + json_string(kind);
        if (!faction.empty()) out += ",\"faction\":" + json_string(faction);
        if (!asset.empty()) out += ",\"asset\":" + json_string(asset);
        if (!stance.empty()) out += ",\"stance\":" + json_string(stance);
        if (!weapon.empty()) out += ",\"weapon\":" + json_string(weapon);
        out += ",\"position\":" + json_metres(*position) +
               ",\"yaw\":" + json_number(actor.heading.value_or(0) * radians_per_degree);
        if (actor.pitch.value_or(0) != 0) out += ",\"pitch\":" + json_number(*actor.pitch * radians_per_degree);
        // The navigation point it stands on (.CELDA), which names its route or post.
        if (actor.group && actor.cell && *actor.group >= 0 && *actor.cell >= 0)
            out += ",\"point\":[" + std::to_string(*actor.group) + "," + std::to_string(*actor.cell) + "]";
        out += "}";
        first = false;
    }
    out += "\n],\n\"routes\":[";
    first = true;
    for (const auto& group : scene.navigation()) {
        if (!group.id) continue;
        out += std::string(first ? "\n" : ",\n") + "{\"id\":" + std::to_string(*group.id) +
               ",\"name\":" + json_string(group.name.value_or("")) +
               ",\"kind\":" + json_string(route_kind(group.type.value_or(0))) + ",\"points\":[";
        bool first_point = true;
        for (const auto& point : group.points) {
            if (!point.id || !point.position) continue;
            out += std::string(first_point ? "" : ",") + "{\"id\":" + std::to_string(*point.id);
            if (point.name && !point.name->empty()) out += ",\"name\":" + json_string(*point.name);
            out += ",\"position\":" + json_metres(*point.position);
            if (point.heading) out += ",\"yaw\":" + json_number(*point.heading);  // .ROT is already radians
            out += "}";
            first_point = false;
        }
        out += "],\"links\":[";
        first_point = true;
        for (const auto& link : group.connections) {
            if (!link.origin_point || !link.destination_point) continue;
            out += std::string(first_point ? "" : ",") + "[" + std::to_string(*link.origin_point) + "," +
                   std::to_string(*link.destination_point) + "]";
            first_point = false;
        }
        out += "]}";
        first = false;
    }
    // Links between points of different routes: [group, point, group, point].
    out += "\n],\n\"route_links\":[";
    first = true;
    for (const auto& link : scene.cross_group_connections()) {
        if (!link.origin_group || !link.origin_point || !link.destination_group || !link.destination_point) continue;
        out += std::string(first ? "\n" : ",\n") + "[" + std::to_string(*link.origin_group) + "," +
               std::to_string(*link.origin_point) + "," + std::to_string(*link.destination_group) + "," +
               std::to_string(*link.destination_point) + "]";
        first = false;
    }
    // Areas are floor polygons extruded `height` metres up.
    out += "\n],\n\"areas\":[";
    first = true;
    for (const auto& area : scene.areas()) {
        if (!area.id) continue;
        out += std::string(first ? "\n" : ",\n") + "{\"id\":" + std::to_string(*area.id) +
               ",\"name\":" + json_string(area.name.value_or("")) +
               ",\"height\":" + json_number(area.height.value_or(0) / units_per_metre) + ",\"points\":[";
        for (std::size_t i = 0; i < area.points.size(); ++i) out += (i ? "," : "") + json_metres(area.points[i]);
        out += "]}";
        first = false;
    }
    out += "\n],\n\"dummies\":[";
    first = true;
    for (const auto& dummy : scene.dummies()) {
        if (!dummy.id || !dummy.position) continue;
        out += std::string(first ? "\n" : ",\n") + "{\"id\":" + std::to_string(*dummy.id) +
               ",\"name\":" + json_string(dummy.name.value_or("")) + ",\"position\":" + json_metres(*dummy.position) +
               ",\"yaw\":" + json_number(dummy.heading.value_or(0));
        if (dummy.pitch.value_or(0) != 0) out += ",\"pitch\":" + json_number(*dummy.pitch);
        out += "}";
        first = false;
    }
    return out + "\n]\n}\n";
}

GodotExportResult export_godot(const GodotExportOptions& options) {
    GodotExportResult result;
    const auto slots = mission_slots(options.corpus);
    const auto slot = std::ranges::find_if(slots, [&](const MissionSlot& value) { return lower(value.mission) == lower(options.mission); });
    if (slot == slots.end()) throw std::runtime_error("No mission '" + options.mission + "' in " + options.corpus.generic_string());
    check_out_folder(options.out);
    const auto corpus = std::filesystem::weakly_canonical(std::filesystem::absolute(options.corpus));
    const auto package_root = corpus / slot->mission;
    // Provenance names files as corpus-relative paths, the same on every machine.
    const auto source = [&](const std::filesystem::path& path) {
        return std::filesystem::weakly_canonical(std::filesystem::absolute(path)).lexically_relative(corpus).generic_string();
    };
    std::map<std::string, ManifestEntry> assets;
    TextureConverter textures(options.out, corpus, package_root);

    // The map: visual and collision glTF, beside each other.
    const auto map_folder = std::filesystem::path("maps") / slot->mission;
    std::filesystem::create_directories(options.out / map_folder);
    const auto visual = rws::Document::load(package_root / slot->visual_map);
    const auto visual_stats =
        rws::export_scene_gltf(visual.chunks(), visual.scene_instances(), visual.bytes(), options.out / map_folder / "visual.gltf",
                               textures.resolver(package_root / slot->visual_map, options.out / map_folder / "visual.gltf"));
    result.lines.push_back("visual\t" + (map_folder / "visual.gltf").generic_string() + '\t' +
                           std::to_string(visual_stats.triangles) + " triangles");
    const auto collision = rws::Document::load(package_root / slot->collision_map);
    const auto collision_stats =
        rws::export_collision_gltf(collision.chunks(), collision.bytes(), options.out / map_folder / "collision.gltf");
    result.lines.push_back("collision\t" + (map_folder / "collision.gltf").generic_string() + '\t' +
                           std::to_string(collision_stats.triangles) + " triangles");

    ResourceIndex package;
    package.add_root(package_root);
    package.build();

    // The sky dome the mission's .vis names, when it has one.
    std::optional<std::filesystem::path> sky;
    auto vis = package_root / slot->scene;
    vis.replace_extension(".vis");
    if (std::filesystem::is_regular_file(vis))
        for (const auto& reference : read_vis(vis).references) {
            if (reference.kind != DependencyKind::sky_model) continue;
            const auto resolution = package.resolve(reference.path);
            if (resolution.candidate_indices.size() != 1) {
                result.problems.push_back("sky " + reference.path + ": not found");
                continue;
            }
            const auto& model_path = package.resources()[resolution.candidate_indices.front()].path;
            try {
                const auto model = rws::Document::load(model_path);
                (void)rws::export_scene_gltf(model.chunks(), model.scene_instances(), model.bytes(),
                                             options.out / map_folder / "sky.gltf",
                                             textures.resolver(model_path, options.out / map_folder / "sky.gltf"));
                sky = map_folder / "sky.gltf";
                result.lines.push_back("sky\t" + sky->generic_string());
            } catch (const std::exception& error) {
                result.problems.push_back("sky " + source(model_path) + ": " + error.what());
            }
        }

    // One model per actor class: characters from Models/Char, props from the rest.
    // A scene that cannot be opened still leaves the map usable.
    std::optional<MissionEditor> editor;
    try {
        editor.emplace(MissionEditor::open(package_root / slot->scene, package_root));
    } catch (const std::exception& error) {
        result.problems.push_back("scene " + source(package_root / slot->scene) + ": " + error.what());
    }
    std::map<std::int32_t, std::string> class_ids;  // class -> logical ID
    std::map<std::string, std::filesystem::path> skeletons;  // skinned character ID -> model
    if (editor) {
        const auto resources = editor->resource_index(package);
        for (const auto& association : associate_actors(editor->scene(), editor->objects(), resources)) {
            if (!association.class_id || class_ids.contains(*association.class_id)) continue;
            const auto class_name = "class " + std::to_string(*association.class_id);
            if (association.visual_models.size() != 1 || !association.visual_models.front().resolved_path) {
                result.problems.push_back(class_name + ": no single resolved visual model (" +
                                          std::to_string(association.visual_models.size()) + " named)");
                continue;
            }
            const auto& model_path = *association.visual_models.front().resolved_path;
            const auto relative = model_path.lexically_relative(package_root);
            const bool character = std::ranges::any_of(relative, [](const auto& part) { return lower(part.string()) == "char"; });
            const auto stem = lower(model_path.stem().string());
            const auto id = std::string(character ? "character/" : "prop/") + stem;
            const auto file = std::filesystem::path(character ? "characters" : "props") / (stem + ".gltf");
            if (!assets.contains(id)) {
                try {
                    const auto model = rws::Document::load(model_path);
                    // A character with a skin keeps its skeleton; anything else is a static model.
                    const auto* clump = model.chunks().empty() || model.chunks().front().type != 0x10 ? nullptr : &model.chunks().front();
                    bool skinned = false;
                    if (character && clump) {
                        try {
                            (void)rws::export_character_gltf(*clump, model.bytes(), options.out / file, {},
                                                             textures.resolver(model_path, options.out / file), humanoid_joint_name,
                                                             humanoid_joint_parent);
                            skinned = true;
                            skeletons[id] = model_path;
                        } catch (const std::exception& error) {
                            result.problems.push_back(class_name + " (" + source(model_path) + "): exported without its skeleton: " + error.what());
                        }
                    }
                    if (!skinned)
                        (void)rws::export_scene_gltf(model.chunks(), model.scene_instances(), model.bytes(), options.out / file,
                                                     textures.resolver(model_path, options.out / file));
                } catch (const std::exception& error) {
                    result.problems.push_back(class_name + " (" + source(model_path) + "): " + error.what());
                    continue;
                }
                assets[id] = {{"file", json_string(file.generic_string())}, {"source", json_string(source(model_path))}};
                result.lines.push_back(std::string(character ? "character\t" : "prop\t") + file.generic_string());
            }
            class_ids[*association.class_id] = id;
        }
    }

    // Each character class's stance, from its weapons (Armas.bdd), and the
    // third-person model (.FILE2) of the weapon that gives it, as
    // weapon/<model stem>: its mesh, and its grip and muzzle frames in the
    // manifest.
    std::map<std::int32_t, std::string> class_stances, class_weapons;
    if (editor)
        if (const auto file = editor->file_of_kind(MissionFileKind::weapons)) {
            const auto weapons = WeaponDatabase::project(editor->document(*file));
            const auto resources = editor->resource_index(package);
            for (const auto& [class_id, id] : class_ids) {
                if (!id.starts_with("character/")) continue;
                const auto definitions = editor->objects().find_class(class_id);
                if (definitions.size() != 1) continue;
                const auto* weapon = held_weapon(*definitions.front(), weapons);
                if (!weapon) continue;
                class_stances[class_id] = weapon_stance(*weapon);
                if (!weapon->third_person_model || weapon->third_person_model->empty()) continue;
                const auto resolution = resources.resolve(*weapon->third_person_model);
                if (resolution.candidate_indices.size() != 1) {
                    result.problems.push_back("weapon " + weapon->name.value_or("?") + ": " + *weapon->third_person_model +
                                              " not found");
                    continue;
                }
                const auto& model_path = resources.resources()[resolution.candidate_indices.front()].path;
                const auto stem = lower(model_path.stem().string());
                const auto weapon_id = "weapon/" + stem;
                class_weapons[class_id] = weapon_id;
                if (assets.contains(weapon_id)) continue;
                const auto weapon_file = std::filesystem::path("weapons") / (stem + ".gltf");
                try {
                    const auto model = rws::Document::load(model_path);
                    (void)rws::export_scene_gltf(model.chunks(), model.scene_instances(), model.bytes(), options.out / weapon_file,
                                                 textures.resolver(model_path, options.out / weapon_file));
                    ManifestEntry entry{{"file", json_string(weapon_file.generic_string())},
                                        {"source", json_string(source(model_path))},
                                        {"stance", json_string(weapon_stance(*weapon))}};
                    if (!model.chunks().empty() && model.chunks().front().type == 0x10) {
                        const auto frames = weapon_frames(model.chunks().front(), model.bytes());
                        if (frames.grip) entry.emplace_back("grip", json_transform(*frames.grip));
                        if (frames.muzzle) entry.emplace_back("muzzle", json_transform(*frames.muzzle));
                        if (!frames.grip) result.problems.push_back(weapon_id + ": no grip frame");
                    }
                    assets[weapon_id] = std::move(entry);
                    result.lines.push_back("weapon\t" + weapon_file.generic_string());
                } catch (const std::exception& error) {
                    result.problems.push_back(weapon_id + " (" + source(model_path) + "): " + error.what());
                    class_weapons.erase(class_id);
                }
            }
        }

    // Clip libraries: per stance, the role clips on the first skinned
    // character's skeleton (every human shares it), named by role. A role
    // whose record has several files (die, fidgets) gets one clip each: the
    // first under the role's name, the others as <role>_2, <role>_3, ...; the
    // game picks one at random (our reading). `custom` holds the one-off scene
    // clips (Anims/Costum: smoking, talking, repairing, ...) under their own
    // names, for missions to play.
    if (editor && !skeletons.empty()) {
        const auto& [skeleton_id, skeleton_path] = *skeletons.begin();
        const auto resources = editor->resource_index(package);
        const auto& records = editor->animations().records();
        auto roles = clip_roles();
        std::set<std::string> taken;
        for (const auto& role : roles) taken.insert(lower(role.clip));
        for (const auto& record : records) {
            if (record.variants.empty() || taken.contains(lower(record.logical_name))) continue;
            if (lower(record.variants.front().reference).find("costum\\") == std::string::npos) continue;
            taken.insert(lower(record.logical_name));
            roles.push_back({"custom", lower(record.logical_name), record.logical_name});
        }
        std::map<std::string, std::vector<const ClipRole*>> stances;
        for (const auto& role : roles) stances[role.stance].push_back(&role);
        for (const auto& [stance, stance_roles] : stances) {
            std::vector<rws::CharacterClip> clips;
            // Per role: its manifest fields but for `clips`, then each clip's name and fields.
            std::vector<std::pair<std::string, std::string>> role_fields;
            std::vector<std::vector<std::pair<std::string, std::string>>> role_clips;
            for (const auto* role : stance_roles) {
                const auto record = std::ranges::find_if(records, [&](const AnimationRecord& r) {
                    return lower(r.logical_name) == lower(role->clip);
                });
                if (record == records.end() || record->variants.empty()) {
                    result.problems.push_back("anim " + stance + "/" + role->role + ": no clip " + role->clip);
                    continue;
                }
                const bool loop = record->loop.value_or(false);
                std::vector<std::pair<std::string, std::string>> variants;
                for (const auto& variant : record->variants) {
                    const auto name = variants.empty() ? role->role : role->role + "_" + std::to_string(variants.size() + 1);
                    const auto resolution = resources.resolve(variant.reference);
                    if (resolution.candidate_indices.size() != 1) {
                        result.problems.push_back("anim " + stance + "/" + name + ": " + variant.reference + " not found");
                        continue;
                    }
                    const auto& path = resources.resources()[resolution.candidate_indices.front()].path;
                    const auto document = rws::Document::load(path);
                    const auto clip_chunk = std::ranges::find_if(document.chunks(), [](const rws::Chunk& c) { return c.type == 0x1B; });
                    if (clip_chunk == document.chunks().end()) {
                        result.problems.push_back("anim " + source(path) + ": no Animation chunk");
                        continue;
                    }
                    clips.push_back({name, rws::decode_animation(*clip_chunk, document.bytes()), loop});
                    // Sounds the clip starts at a time: [seconds, sound ID]. What -2 is, is
                    // unknown: its times don't line up with the walk's foot plants.
                    std::string sounds;
                    for (const auto& sound : variant.sounds) {
                        if (!sound.time) continue;
                        sounds += std::string(sounds.empty() ? "" : ",") + "[" + json_number(*sound.time) + "," +
                                  json_string(sound.logical_id) + "]";
                    }
                    variants.emplace_back(name, "{\"source\": " + json_string(source(path)) +
                                                    (sounds.empty() ? "" : ", \"sounds\": [" + sounds + "]") + "}");
                }
                if (variants.empty()) continue;
                // The engine moves the actor at .VEL game units a second; the clips stay in place.
                role_fields.emplace_back(role->role, "\"loop\": " + std::string(loop ? "true" : "false") +
                                                         ", \"speed\": " + json_number(record->velocity_scalar.value_or(0.0F) * 0.01F) +
                                                         ", \"clip\": " + json_string(role->clip));
                role_clips.push_back(std::move(variants));
            }
            if (clips.empty()) continue;
            const auto file = std::filesystem::path("anims") / (stance + ".gltf");
            std::set<std::string> skipped;
            try {
                const auto model = rws::Document::load(skeleton_path);
                const auto stats = rws::export_character_gltf(model.chunks().front(), model.bytes(), options.out / file,
                                                              std::move(clips), {}, humanoid_joint_name,
                                                              humanoid_joint_parent, false);
                for (const auto& why : stats.skipped_clips) {
                    result.problems.push_back("anim " + stance + "/" + why);
                    skipped.insert(why.substr(0, why.find(':')));
                }
                result.lines.push_back("anims\t" + file.generic_string() + '\t' + std::to_string(stats.clips) + " clips");
            } catch (const std::exception& error) {
                result.problems.push_back("anim " + stance + ": " + error.what());
                continue;
            }
            std::string clip_fields;
            for (std::size_t i = 0; i < role_fields.size(); ++i) {
                std::string variants;
                for (const auto& [name, fields] : role_clips[i])
                    if (!skipped.contains(name))
                        variants += std::string(variants.empty() ? "" : ", ") + json_string(name) + ": " + fields;
                if (variants.empty()) continue;
                clip_fields += std::string(clip_fields.empty() ? "" : ", ") + json_string(role_fields[i].first) + ": {" +
                               role_fields[i].second + ", \"clips\": {" + variants + "}}";
            }
            assets["anim/" + stance] = {{"file", json_string(file.generic_string())},
                                        {"skeleton", json_string(skeleton_id)},
                                        {"roles", "{" + clip_fields + "}"}};
        }
    }

    // Actors, routes, areas and dummies, in metres and radians.
    const auto markers = map_folder / "markers.json";
    write_file(options.out / markers, editor ? godot_markers_json(editor->scene(), editor->objects(), class_ids, class_stances, class_weapons)
                                             : godot_markers_json(MissionScene{}, ObjectDatabase{}, {}));
    result.lines.push_back("markers\t" + markers.generic_string());
    assets["map/" + lower(slot->mission)] = {
        {"visual", json_string((map_folder / "visual.gltf").generic_string())},
        {"collision", json_string((map_folder / "collision.gltf").generic_string())},
        {"markers", json_string(markers.generic_string())},
        {"source", json_string(source(package_root / slot->scene))},
    };
    if (sky) assets["map/" + lower(slot->mission)].emplace_back("sky", json_string(sky->generic_string()));

    result.lines.push_back("textures\t" + std::to_string(textures.converted()) + " PNG");
    for (const auto& name : textures.missing()) result.problems.push_back("texture " + name + ": not found");
    for (const auto& [path, why] : textures.failed())
        result.problems.push_back("texture " + source(path) + ": " + why);

    std::string manifest = "{\n  \"format\": " + json_string(manifest_format) +
                           ",\n  \"version\": 1,\n  \"units\": \"metres\",\n  \"up\": \"Y\",\n  \"missions\": [" +
                           json_string(slot->mission) + "],\n  \"assets\": {";
    bool first = true;
    for (const auto& [id, fields] : assets) {
        manifest += std::string(first ? "\n" : ",\n") + "    " + json_string(id) + ": {";
        for (std::size_t i = 0; i < fields.size(); ++i)
            manifest += (i ? ", " : "") + json_string(fields[i].first) + ": " + fields[i].second;
        manifest += "}";
        first = false;
    }
    write_file(options.out / "manifest.json", manifest + "\n  }\n}\n");
    result.lines.push_back("manifest\tmanifest.json\t" + std::to_string(assets.size()) + " assets");
    return result;
}

} // namespace csf
