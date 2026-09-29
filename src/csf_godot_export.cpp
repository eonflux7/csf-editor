#include "csf/godot_export.hpp"

#include "csf/animation_catalog.hpp"
#include "csf/authoring_project.hpp"
#include "csf/mission_edit.hpp"
#include "csf/object_database.hpp"
#include "csf/project_pipeline.hpp"
#include "csf/tree.hpp"
#include "rws/document.hpp"
#include "rws/scene_export.hpp"
#include "rws/texture_image.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
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
// then in each Textures folder from there up to the mission package. The game
// finds more than that (a prop may use the map's textures, a weapon one kept
// under Models/Deco), so a name found neither way is taken from anywhere in
// the package, the first by path. Names are Windows-1252 (LEÑERA_Lm), the
// unpacked files UTF-8.
class TextureConverter {
public:
    TextureConverter(std::filesystem::path out, std::filesystem::path corpus, std::filesystem::path package_root)
        : out_(std::move(out)), corpus_(std::move(corpus)), package_root_(std::move(package_root)) {}

    // A resolver for a glTF written to `gltf` from the source file `source`.
    rws::SceneTextureResolver resolver(const std::filesystem::path& source, const std::filesystem::path& gltf) {
        return resolver(std::vector{source}, gltf);
    }
    // The same for a glTF made of several source files (hands holding a
    // weapon): a texture is looked up from each in turn.
    rws::SceneTextureResolver resolver(const std::vector<std::filesystem::path>& sources, const std::filesystem::path& gltf) {
        return [this, sources, gltf](const std::string_view name) -> std::optional<rws::SceneTexture> {
            std::optional<std::filesystem::path> found;
            const auto utf8 = windows_1252_to_utf8(name);
            for (const auto& source : sources)
                if (!found) found = find(source, lower(utf8));
            if (!found) found = find_anywhere(lower(utf8));
            if (!found) {
                missing_.insert(utf8);
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
    std::optional<std::filesystem::path> find_anywhere(const std::string& name) {
        if (package_files_.empty()) {
            std::vector<std::filesystem::path> files;
            std::error_code error;
            for (auto it = std::filesystem::recursive_directory_iterator(package_root_, error);
                 !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
                const auto file = lower(it->path().filename().string());
                if (it->is_regular_file() && (file.ends_with(".dds") || file.ends_with(".png"))) files.push_back(it->path());
            }
            std::ranges::sort(files);
            for (const auto& file : files) package_files_.try_emplace(lower(file.filename().string()), file);
        }
        const auto has_extension = std::filesystem::path(name).has_extension();
        for (const auto& candidate : has_extension ? std::vector{name} : std::vector{name + ".dds", name + ".png"})
            if (const auto it = package_files_.find(candidate); it != package_files_.end()) return it->second;
        return std::nullopt;
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
    std::map<std::string, std::filesystem::path> package_files_;  // lower-case name -> first by path
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
const std::map<std::int32_t, std::string_view>& humanoid_joints() {
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
    return names;
}
std::string humanoid_joint_name(const std::int32_t node_id) {
    const auto& names = humanoid_joints();
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
// Anything-stance actions from Anims/Enem: being taken out, sitting. The
// vehicles' and mounted guns' seat clips join them by their own names, from
// the classes (seat_clips).
constexpr RoleClip action_roles[]{
    {"threatened", "Amenazado"},  {"stabbed", "Acuchillado"},     {"garrotted", "Ahorcado"},
    {"strangled", "Estrangulado"}, {"strangle", "Estrangular"},   {"put_on_clothes", "EPonerseRopa"},
    {"sit_idle", "SSDKReposo"},   {"sit_idle_unarmed", "SSDKReposo_Sin_Armas"},
    {"sit_down", "SSDKSienta"},   {"sit_stand_up", "SSDKLevant"}, {"sit_die", "SSDKDead"},
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

// First-person clip roles by .ANIMACIONES_PLAYER .TIPO (our English names,
// in the style of the third-person roles). A type not listed keeps its own
// name in lower case; DUAL_<type> (two guns) becomes dual_<role>.
std::string first_person_role(const std::string& type) {
    static const std::map<std::string, std::string> roles{
        {"ESTATICO", "idle"}, {"AGACHADO", "crouch_idle"}, {"AVANZANDO", "walk"},
        {"AGACHADO_AVANZANDO", "crouch_walk"}, {"CORRIENDO", "run"}, {"DISPARO", "shoot"},
        {"DISPARAR", "shoot"}, {"DISPARO_CORTO", "shoot_short"}, {"RECARGAR", "reload"},
        {"RECARGA_PREPARAR", "reload_start"}, {"RECARGA_TERMINAR", "reload_end"}, {"SACAR", "draw"},
        {"SACAR_ARMA", "draw"}, {"GUARDAR_ARMA", "holster"}, {"METER", "put_away"}, {"OCIO", "fidget"},
        {"MUERTE", "die"}, {"MORIBUNDO", "dying"}, {"MORIBUNDO_ENTRAR", "dying_start"},
        {"APUNTAR", "aim"}, {"APUNTAR_DISPARO", "aim_shoot"}, {"LANZAR_TERMINAR", "throw"},
        {"LANZAR_TERMINAR_CORTO", "throw_short"}, {"CUERDA_PIANO", "garrotte"}, {"CURAR", "heal"},
        {"ANDAR", "walk"}, {"ANDAR_ATRAS", "walk_back"}, {"CORRER", "run"}, {"CORRER_ATRAS", "run_back"},
        {"STRAFE_DER", "strafe_right"}, {"STRAFE_IZQ", "strafe_left"}, {"REPOSO", "rest"},
    };
    auto key = type;
    std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (key.starts_with("DUAL_")) return "dual_" + first_person_role(key.substr(5));
    const auto found = roles.find(key);
    return found != roles.end() ? found->second : lower(key);
}

// One wave of a mission's audio bank (.wad): RenderWare Audio, a 0x802 chunk
// per wave holding a header (0x803: format 15, sample rate, data size, bits,
// channels; the name at byte 112) and 16-bit PCM (0x804). Every shipped bank
// is 16-bit PCM, mono or stereo, 22050 or 44100 Hz.
struct AudioWave {
    std::string name;
    std::uint32_t rate{};
    std::uint16_t channels{}, bits{};
    std::size_t offset{}, size{};
};
std::uint32_t read_u32(const std::vector<char>& bytes, const std::size_t at) {
    std::uint32_t value{};
    std::memcpy(&value, bytes.data() + at, 4);
    return value;
}
std::vector<AudioWave> read_audio_bank(const std::vector<char>& bytes) {
    std::vector<AudioWave> waves;
    for (std::size_t at = 0; at + 12 <= bytes.size();) {
        const auto type = read_u32(bytes, at), size = read_u32(bytes, at + 4);
        const auto end = at + 12 + size;
        if (type != 0x802 || end > bytes.size()) break;
        const auto header = at + 12;
        if (read_u32(bytes, header) == 0x803) {
            const auto header_size = read_u32(bytes, header + 4);
            const auto fields = header + 12;
            const auto data = fields + header_size;
            if (header_size >= 116 && data + 12 <= end && read_u32(bytes, data) == 0x804) {
                AudioWave wave;
                wave.rate = read_u32(bytes, fields + 4);
                const auto format = read_u32(bytes, fields + 16);
                wave.bits = static_cast<std::uint16_t>(format & 0xFF);
                wave.channels = static_cast<std::uint16_t>((format >> 8) & 0xFF);
                wave.offset = data + 12;
                wave.size = std::min<std::size_t>(read_u32(bytes, data + 4), end - wave.offset);
                const auto* name = bytes.data() + fields + 112;
                wave.name = std::string(name, strnlen(name, header_size - 112));
                if (wave.bits == 16 && wave.channels >= 1 && wave.rate > 0 && !wave.name.empty())
                    waves.push_back(std::move(wave));
            }
        }
        at = end;
    }
    return waves;
}
void write_wav(const std::filesystem::path& path, const AudioWave& wave, const std::vector<char>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const auto u32 = [&](const std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](const std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const auto block = static_cast<std::uint16_t>(wave.channels * wave.bits / 8);
    out.write("RIFF", 4);
    u32(static_cast<std::uint32_t>(36 + wave.size));
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(wave.channels);
    u32(wave.rate);
    u32(wave.rate * block);
    u16(block);
    u16(wave.bits);
    out.write("data", 4);
    u32(static_cast<std::uint32_t>(wave.size));
    out.write(bytes.data() + wave.offset, static_cast<std::streamsize>(wave.size));
    if (!out) throw std::runtime_error("Cannot write " + path.generic_string());
}
// A name as a file name and logical ID part: lower case, [a-z0-9_-] only.
std::string id_part(const std::string& name) {
    auto result = lower(name);
    for (auto& c : result)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
    return result;
}

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
// tag=100 (at the barrel's end on every one). `held` leaves out the root
// frame's matrix, which the holding joint replaces (first-person weapons).
struct WeaponFrames {
    std::optional<Transform> grip, muzzle;
};

// Each frame's transform in the Clump's space, in metres (none when its
// parents loop). RenderWare stores a frame's axes as rows (right, up, at):
// they are the basis columns. `held` leaves out the root frame's matrix.
std::vector<std::optional<Transform>> frame_transforms(const rws::FrameListInfo& frames, const bool held = false) {
    const auto& info = frames.frames;
    std::vector<std::optional<Transform>> world(info.size());
    const auto resolve = [&](auto&& self, const std::size_t index, const int depth) -> std::optional<Transform> {
        if (index >= info.size() || depth > 64) return std::nullopt;
        if (world[index]) return world[index];
        const auto& frame = info[index];
        if (held && frame.parent < 0) return world[index] = Transform{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
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
    for (std::size_t i = 0; i < info.size(); ++i) (void)resolve(resolve, i, 0);
    return world;
}

WeaponFrames weapon_frames(const rws::Chunk& clump, const std::span<const std::byte> bytes, const bool held = false) {
    WeaponFrames result;
    const auto* list = rws::find_child(clump, 0x0E);
    const auto frames = list ? rws::decode_frame_list(*list, bytes) : rws::DecodeResult<rws::FrameListInfo>{};
    if (!frames) return result;
    const auto world = frame_transforms(*frames.value, held);
    const auto at = [&](const std::size_t index) { return index < world.size() ? world[index] : std::nullopt; };
    for (const auto& child : clump.children)
        if (child.type == 0x14) {
            if (const auto atomic = rws::decode_atomic(child, bytes); atomic && atomic.value->frame_index >= 0)
                result.grip = at(static_cast<std::size_t>(atomic.value->frame_index));
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
                        if (text == "tag=100") result.muzzle = at(index);
        ++index;
    }
    return result;
}

// A model's named frames (the scripts' DUMMY_BICHO: an alarm's LuzAlarma, a
// truck's rastro1): each frame whose Pyro metadata is present, with its ID
// (the metadata's number), its name and its transform in the Clump's space,
// in metres, which is the space its glTF is written in.
struct NamedFrame {
    std::int32_t id{};
    std::string name;
    Transform transform{};
};
std::vector<NamedFrame> named_frames(const rws::Chunk& clump, const std::span<const std::byte> bytes) {
    std::vector<NamedFrame> result;
    const auto* list = rws::find_child(clump, 0x0E);
    const auto frames = list ? rws::decode_frame_list(*list, bytes) : rws::DecodeResult<rws::FrameListInfo>{};
    if (!frames) return result;
    const auto world = frame_transforms(*frames.value);
    std::size_t index = 0;
    for (const auto& extension : list->children) {
        if (extension.type != 0x03) continue;
        const auto* pyro = rws::find_child(extension, 0xFFFFFF00U);
        const auto metadata = pyro ? rws::decode_pyro_extension(*pyro, 0x0E, bytes) : rws::DecodeResult<rws::PyroExtensionInfo>{};
        if (metadata && metadata.value->present && metadata.value->words.size() >= 2 && index < world.size() && world[index])
            result.push_back({static_cast<std::int32_t>(metadata.value->words[1]), std::string(metadata.value->object_name()),
                              *world[index]});
        ++index;
    }
    return result;
}

// A CSFFBS value as JSON, as written: a number, a string, a list as an
// array, a record (labelled fields) as an object.
std::string csffbs_fields(const Document& document, const Node& record);
std::string csffbs_value(const Document& document, const Node& node) {
    if (!node.children.empty()) {
        if (node.children.front().identifier_index) return csffbs_fields(document, node);
        std::string out = "[";
        for (std::size_t i = 0; i < node.children.size(); ++i) out += (i ? ", " : "") + csffbs_value(document, node.children[i]);
        return out + "]";
    }
    if (const auto* value = std::get_if<std::int32_t>(&node.scalar)) return std::to_string(*value);
    if (const auto* value = std::get_if<float>(&node.scalar)) return json_number(*value);
    if (const auto* value = std::get_if<std::uint32_t>(&node.scalar)) {
        const auto* text = document.string(*value);
        return json_string(text ? text->display_utf8() : "");
    }
    return "[]";
}
// A record's fields as a JSON object: `.PART_LIFE 1` becomes "part_life": 1.
// The first of a repeated field wins (Efectos.bdd writes .AUTOSTART twice).
std::string csffbs_fields(const Document& document, const Node& record) {
    std::set<std::string> seen;
    std::string out;
    for (const auto& child : record.children) {
        const auto* label = child.identifier_index ? document.identifier(*child.identifier_index) : nullptr;
        auto key = label ? lower(label->display_utf8()) : std::string{};
        if (key.starts_with('.')) key.erase(0, 1);
        if (key.empty() || !seen.insert(key).second) continue;
        out += std::string(out.empty() ? "" : ", ") + json_string(key) + ": " + csffbs_value(document, child);
    }
    return "{" + out + "}";
}
// A record's field by name (without the dot, any case), or null.
const Node* csffbs_field(const Document& document, const Node& record, const std::string_view name) {
    for (const auto& child : record.children) {
        const auto* label = child.identifier_index ? document.identifier(*child.identifier_index) : nullptr;
        if (label && lower(label->display_utf8()) == "." + lower(std::string(name))) return &child;
    }
    return nullptr;
}
std::string csffbs_string(const Document& document, const Node* node) {
    const auto* index = node ? std::get_if<std::uint32_t>(&node->scalar) : nullptr;
    const auto* text = index ? document.string(*index) : nullptr;
    return text ? text->display_utf8() : std::string{};
}

// A file a BDD names (`Gfx\\1FU.sp`) under the package, matching each part
// of the path in any case, as Windows does.
std::optional<std::filesystem::path> find_in_package(const std::filesystem::path& root, std::string reference) {
    std::ranges::replace(reference, '\\', '/');
    auto path = root;
    for (const auto& part : std::filesystem::path(reference)) {
        if (part.empty()) continue;
        std::optional<std::filesystem::path> next;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(path, error))
            if (lower(entry.path().filename().string()) == lower(part.string())) {
                next = entry.path();
                break;
            }
        if (!next) return std::nullopt;
        path = *next;
    }
    return std::filesystem::is_regular_file(path) ? std::optional(path) : std::nullopt;
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
                               const std::map<std::int32_t, std::string>& class_weapons,
                               const std::map<std::int32_t, std::string>& effect_assets) {
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
    // The map's own effects, each at a dummy: {id, name, effect, dummy}.
    out += "\n],\n\"effects\":[";
    first = true;
    for (const auto& effect : scene.effects()) {
        if (!effect.id || !effect.class_id || !effect.dummy_id) continue;
        const auto found = effect_assets.find(*effect.class_id);
        if (found == effect_assets.end()) continue;
        out += std::string(first ? "\n" : ",\n") + "{\"id\":" + std::to_string(*effect.id) +
               ",\"name\":" + json_string(effect.name.value_or("")) + ",\"effect\":" + json_string(found->second) +
               ",\"dummy\":" + std::to_string(*effect.dummy_id) + "}";
        first = false;
    }
    return out + "\n]\n}\n";
}

GodotExportResult export_godot(const GodotExportOptions& options) {
    GodotExportResult result;
    const auto slots = mission_slots(options.corpus);
    std::vector<const MissionSlot*> chosen;
    for (const auto& mission : options.missions) {
        const auto slot = std::ranges::find_if(slots, [&](const MissionSlot& value) { return lower(value.mission) == lower(mission); });
        if (slot == slots.end()) throw std::runtime_error("No mission '" + mission + "' in " + options.corpus.generic_string());
        if (std::ranges::find(chosen, &*slot) == chosen.end()) chosen.push_back(&*slot);
    }
    if (chosen.empty()) throw std::runtime_error("No mission to export");
    check_out_folder(options.out);
    const auto corpus = std::filesystem::weakly_canonical(std::filesystem::absolute(options.corpus));
    // Provenance names files as corpus-relative paths, the same on every machine.
    const auto source = [&](const std::filesystem::path& path) {
        return std::filesystem::weakly_canonical(std::filesystem::absolute(path)).lexically_relative(corpus).generic_string();
    };
    // Shared by the missions: an asset (model, weapon, sound, first-person
    // view) comes from the first mission that has it, the same in all of them
    // but for where its textures are. Clip libraries are the union of the
    // missions' clips, each role from the first that has it (a clip of the
    // same name is the same clip in every mission).
    std::map<std::string, ManifestEntry> assets;
    std::map<std::string, std::filesystem::path> skeletons;  // skinned character ID -> model
    std::set<std::string> written_waves;
    struct LibraryRole {
        std::string role, fields;                                   // its manifest fields but for `clips`
        std::vector<std::pair<std::string, std::string>> variants;  // each clip's name and fields
    };
    std::map<std::string, std::vector<LibraryRole>> libraries;         // stance -> roles, in the order found
    std::map<std::string, std::vector<rws::CharacterClip>> library_clips;  // stance -> clips
    std::vector<std::pair<std::string, std::string>> missing_roles;    // "<stance>/<role>" -> problem
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> first_person;  // character -> weapon name, ID
    // A vehicle's seats (a mounted gun is one too): the model's seat frames
    // (its named frames 1000-1049, else 1050-1099) and, from the class with
    // the most seat clips (.ANIMACIONES <use>_PUESTO_<n>), each seat's clips
    // by use (in, idle, out, die, shoot) as Anims.bdd names.
    std::map<std::string, std::vector<std::int32_t>> seat_frames;                                   // model ID -> frame IDs
    std::map<std::string, std::map<std::int32_t, std::map<std::string, std::string>>> seat_clips;  // model ID -> seat -> use -> clip
    std::set<std::string> vehicle_models;
    const auto collected = [&](const std::string& stance, const std::string& role) {
        const auto found = libraries.find(stance);
        return found != libraries.end() && std::ranges::any_of(found->second, [&](const LibraryRole& r) { return r.role == role; });
    };

    for (const auto* slot : chosen) {
    const auto first_problem = result.problems.size();
    std::set<std::string> earlier_assets;
    for (const auto& [id, fields] : assets) earlier_assets.insert(id);
    const auto package_root = corpus / slot->mission;
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
    if (editor) {
        const auto resources = editor->resource_index(package);
        auto associations = associate_actors(editor->scene(), editor->objects(), resources);
        const auto placed = associations.size();
        // And the classes the scripts create (CREAR_BICHO: a broken alarm, a
        // burst drum), which no actor of the scene may be.
        std::set<std::int32_t> scripted;
        for (const auto kind : {MissionFileKind::mission_script, MissionFileKind::cutscene_script})
            if (const auto file = editor->file_of_kind(kind)) {
                const auto collect = [&](auto&& self, const ProgramOperand& operand) -> void {
                    if (operand.tag == "CLASSID")
                        if (const auto* number = std::get_if<std::int32_t>(&operand.value)) scripted.insert(*number);
                    for (const auto& child : operand.children) self(self, child);
                };
                const auto program = ProgramDocument::project(editor->document(*file));
                for (const auto& script : program.scripts())
                    for (const auto* list : {&script.conditions, &script.actions})
                        for (const auto& instruction : *list)
                            for (const auto& operand : instruction.operands) collect(collect, operand);
            }
        for (const auto class_id : scripted) {
            ActorAssociation association;
            association.class_id = class_id;
            for (const auto* definition : editor->objects().find_class(class_id))
                for (const auto& reference : definition->references) {
                    if (reference.kind != ObjectReference::Kind::visual_model) continue;
                    const auto resolution = resources.resolve(reference.path);
                    AssociationEvidence evidence{"Objetos.bdd " + reference.field, reference.source, reference.field, resolution,
                                                 std::nullopt};
                    if (resolution.candidate_indices.size() == 1 && resolution.status != ResolutionStatus::ambiguous)
                        evidence.resolved_path = resources.resources()[resolution.candidate_indices.front()].path;
                    association.visual_models.push_back(std::move(evidence));
                }
            associations.push_back(std::move(association));
        }
        for (std::size_t index = 0; index < associations.size(); ++index) {
            const auto& association = associations[index];
            if (!association.class_id || class_ids.contains(*association.class_id)) continue;
            const auto class_name = "class " + std::to_string(*association.class_id);
            if (association.visual_models.size() != 1 || !association.visual_models.front().resolved_path) {
                if (index < placed)
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
                std::pair<std::string, std::string> hang;  // joints or dummies, when it has them
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
                    // Where effects and links hang (DUMMY_BICHO): a skinned
                    // character's joints by HAnim ID (the others are bone_<id>),
                    // any other model's named frames.
                    if (skinned) {
                        std::string joints;
                        for (const auto& [joint, name] : humanoid_joints())
                            joints += std::string(joints.empty() ? "" : ", ") + json_string(std::to_string(joint)) + ": " +
                                      json_string(std::string(name));
                        hang = {"joints", "{" + joints + "}"};
                    } else if (clump) {
                        std::string dummies;
                        for (const auto& frame : named_frames(*clump, model.bytes()))
                            dummies += std::string(dummies.empty() ? "" : ", ") + "{\"id\": " + std::to_string(frame.id) +
                                       ", \"name\": " + json_string(windows_1252_to_utf8(frame.name)) +
                                       ", \"transform\": " + json_transform(frame.transform) + "}";
                        if (!dummies.empty()) hang = {"dummies", "[" + dummies + "]"};
                        for (const auto [low, high] : {std::pair{1000, 1049}, std::pair{1050, 1099}}) {
                            std::vector<std::int32_t> seats;
                            for (const auto& frame : named_frames(*clump, model.bytes()))
                                if (frame.id >= low && frame.id <= high) seats.push_back(frame.id);
                            std::ranges::sort(seats);
                            if (seats.empty()) continue;
                            seat_frames[id] = std::move(seats);
                            break;
                        }
                    }
                } catch (const std::exception& error) {
                    result.problems.push_back(class_name + " (" + source(model_path) + "): " + error.what());
                    continue;
                }
                assets[id] = {{"file", json_string(file.generic_string())}, {"source", json_string(source(model_path))}};
                if (!hang.second.empty()) assets[id].push_back(std::move(hang));
                result.lines.push_back(std::string(character ? "character\t" : "prop\t") + file.generic_string());
            }
            class_ids[*association.class_id] = id;
            std::map<std::int32_t, std::map<std::string, std::string>> seats;
            std::size_t clips = 0;
            for (const auto* definition : editor->objects().find_class(*association.class_id)) {
                for (const auto& field : definition->unknown_fields)
                    if (lower(field.name).find("vehicle_info") != std::string::npos) vehicle_models.insert(id);
                for (const auto& [type, animation_id] : definition->animations) {
                    static const std::map<std::string, std::string> uses{
                        {"SUBIR", "in"}, {"REPOSO", "idle"}, {"BAJAR", "out"}, {"MUERTE", "die"}, {"DISPARAR", "shoot"}};
                    const auto at = type.rfind("_PUESTO_");
                    const auto use = at == std::string::npos ? uses.end() : uses.find(type.substr(0, at));
                    if (use == uses.end()) continue;
                    const auto& records = editor->animations().records();
                    const auto record = std::ranges::find_if(records, [&](const AnimationRecord& r) { return r.id == animation_id; });
                    if (record == records.end()) continue;
                    seats[std::stoi(type.substr(at + 8))][use->second] = record->logical_name;
                    ++clips;
                }
            }
            std::size_t had = 0;
            for (const auto& [seat, by_use] : seat_clips[id]) had += by_use.size();
            if (clips > had) seat_clips[id] = std::move(seats);
            if (seat_clips[id].empty()) seat_clips.erase(id);
        }
    }

    // Sounds: the mission's bank (the .wad beside its scene) as
    // sounds/<wave name>.wav, and each Sonidos.bdd record as sound/<name>:
    // `files` (the variants one play picks from), `volume` (0-1), `loop`,
    // `positional` (.ES3D) and `radius` (metres). A record's .FILE names a wave
    // by its stem; the bank names some with a suffix (`CAMION_MOVER_01 (44)`).
    std::map<std::int32_t, std::string> sound_ids;  // Sonidos.bdd ID -> logical ID
    std::map<std::int32_t, std::string> sound_names;  // every named Sonidos.bdd record, with a wave or not
    {
        std::map<std::string, std::string> waves;  // lower-case wave name -> file
        std::vector<std::filesystem::path> banks;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator((package_root / slot->scene).parent_path(), error))
            if (lower(entry.path().extension().string()) == ".wad") banks.push_back(entry.path());
        std::ranges::sort(banks);
        std::size_t written = 0;
        for (const auto& bank : banks) {
            std::ifstream in(bank, std::ios::binary);
            const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            for (const auto& wave : read_audio_bank(bytes)) {
                const auto file = (std::filesystem::path("sounds") / (id_part(wave.name) + ".wav")).generic_string();
                if (waves.contains(lower(wave.name))) continue;
                if (written_waves.insert(file).second) {
                    write_wav(options.out / file, wave, bytes);
                    ++written;
                }
                waves[lower(wave.name)] = file;
                if (const auto paren = wave.name.find(" ("); paren != std::string::npos)
                    waves.try_emplace(lower(wave.name.substr(0, paren)), file);
            }
        }
        if (!banks.empty()) result.lines.push_back("sounds\tsounds/\t" + std::to_string(written) + " waves");
        const auto sonidos = package_root / "BDD" / "Sonidos.bdd";
        std::size_t missing = 0;
        const auto sound_database = std::filesystem::is_regular_file(sonidos) && !waves.empty()
                                        ? SoundDatabase::project(Document::load(sonidos))
                                        : SoundDatabase{};
        for (const auto& sound : sound_database.definitions()) {
                if (!sound.name.empty()) sound_names[sound.id] = "sound/" + id_part(sound.name);
                std::string files;
                for (auto reference : sound.files) {
                    std::ranges::replace(reference, '\\', '/');
                    const auto found = waves.find(lower(std::filesystem::path(reference).stem().string()));
                    if (found == waves.end()) {
                        ++missing;
                        continue;
                    }
                    files += std::string(files.empty() ? "" : ", ") + json_string(found->second);
                }
                if (files.empty() || sound.name.empty()) continue;
                const auto id = "sound/" + id_part(sound.name);
                if (assets.contains(id)) {
                    if (earlier_assets.contains(id)) sound_ids[sound.id] = id;
                    continue;
                }
                ManifestEntry entry{{"files", "[" + files + "]"},
                                    {"volume", json_number(sound.volume.value_or(100.0F) * 0.01F)},
                                    {"loop", sound.loop ? "true" : "false"},
                                    {"positional", sound.positional ? "true" : "false"}};
                if (sound.radius) entry.emplace_back("radius", json_number(*sound.radius * 0.01F));
                assets[id] = std::move(entry);
                sound_ids[sound.id] = id;
            }
        if (missing) result.problems.push_back("sounds: " + std::to_string(missing) + " waves Sonidos.bdd names are not in the bank");
        // Surface types (Materiales.bdd), as surface/<name>: the sounds of
        // stepping on it (`step`) and hitting it (`impact`). The collision
        // map's materials name theirs (`rws_surface_name`).
        const auto materiales = package_root / "BDD" / "Materiales.bdd";
        if (std::filesystem::is_regular_file(materiales))
            for (const auto& surface : project_surfaces(Document::load(materiales))) {
                if (surface.name.empty()) continue;
                ManifestEntry entry{{"id", std::to_string(surface.id)}};
                for (const auto& [key, sound] : {std::pair{"step", surface.step_sound}, std::pair{"impact", surface.impact_sound}})
                    if (sound)
                        if (const auto found = sound_ids.find(*sound); found != sound_ids.end())
                            entry.emplace_back(key, json_string(found->second));
                assets.try_emplace("surface/" + id_part(surface.name), std::move(entry));
            }
    }
    // Effects (Efectos.bdd), as effect/<name>: `kind` (.TIPO lower case:
    // particle_system, glow, light, estela), `fields`, the record's own as
    // written, lower case without the dot (the engine reads them), and
    // `texture`: a glow's or trail's .FILE, a particle system's .TEXTURE.
    // A particle system's .sp file comes as `system`, its fields likewise.
    // A light that shares its name with a glow (luz verde) is
    // effect/<name>_light. The first mission's record of a name wins.
    std::map<std::int32_t, std::string> effect_ids;  // Efectos.bdd ID -> logical ID
    if (const auto efectos = package_root / "BDD" / "Efectos.bdd"; std::filesystem::is_regular_file(efectos)) {
        const auto document = Document::load(efectos);
        std::vector<const Node*> records;
        const auto collect = [&](auto&& self, const Node& node) -> void {
            if (csffbs_field(document, node, "ID") && csffbs_field(document, node, "TIPO")) {
                records.push_back(&node);
                return;
            }
            for (const auto& child : node.children) self(self, child);
        };
        for (const auto& root : document.roots()) collect(collect, root);
        std::size_t written = 0;
        std::set<std::string> unfound;
        for (const auto* record : records) {
            const auto* id_node = csffbs_field(document, *record, "ID");
            const auto* id = id_node ? std::get_if<std::int32_t>(&id_node->scalar) : nullptr;
            const auto name = csffbs_string(document, csffbs_field(document, *record, "NOMBRE"));
            const auto kind = lower(csffbs_string(document, csffbs_field(document, *record, "TIPO")));
            if (!id || name.empty() || kind.empty()) continue;
            auto logical = "effect/" + id_part(name);
            const auto kind_of = [&](const std::string& key) -> std::string {
                const auto found = assets.find(key);
                if (found == assets.end()) return {};
                for (const auto& [field, value] : found->second)
                    if (field == "kind") return value;
                return {};
            };
            if (const auto taken = kind_of(logical); !taken.empty() && taken != json_string(kind)) logical += "_" + kind;
            effect_ids[*id] = logical;
            if (assets.contains(logical)) continue;
            ManifestEntry entry{{"kind", json_string(kind)}, {"fields", csffbs_fields(document, *record)}};
            const auto file = csffbs_string(document, csffbs_field(document, *record, "FILE"));
            std::string texture_name;
            std::filesystem::path texture_from = efectos;
            if (kind == "particle_system" && !file.empty()) {
                const auto system = find_in_package(package_root, file);
                if (!system) {
                    unfound.insert(file);
                } else {
                    const auto sp = Document::load(*system);
                    if (!sp.roots().empty()) {
                        entry.emplace_back("system", csffbs_fields(sp, sp.roots().front()));
                        texture_name = csffbs_string(sp, csffbs_field(sp, sp.roots().front(), "TEXTURE"));
                        texture_from = *system;
                    }
                }
            } else if (!file.empty()) {
                auto reference = file;
                std::ranges::replace(reference, '\\', '/');
                texture_name = std::filesystem::path(reference).filename().string();
                if (const auto found = find_in_package(package_root, file)) texture_from = *found;
            }
            if (!texture_name.empty()) {
                if (const auto texture = textures.resolver(texture_from, options.out / "manifest.json")(texture_name))
                    entry.emplace_back("texture", json_string(texture->uri));
                else
                    unfound.insert(texture_name);
            }
            assets[logical] = std::move(entry);
            ++written;
        }
        result.lines.push_back("effects\t" + std::to_string(written) + " effects");
        if (!unfound.empty()) {
            std::string names;
            for (const auto& item : unfound) names += (names.empty() ? "" : ", ") + item;
            result.problems.push_back("effects: " + std::to_string(unfound.size()) + " files not found: " + names);
        }
    }
    // A weapon's sounds as a manifest field: {"shoot": "sound/...", ...}.
    const auto weapon_sounds = [&](const WeaponDefinition& weapon, const bool player) {
        std::string fields;
        const auto add = [&](const char* key, const std::optional<std::int32_t> id) {
            if (!id) return;
            const auto found = sound_ids.find(*id);
            if (found == sound_ids.end()) return;
            fields += std::string(fields.empty() ? "" : ", ") + json_string(key) + ": " + json_string(found->second);
        };
        add("shoot", player ? weapon.player_shot_sound.has_value() ? weapon.player_shot_sound : weapon.shot_sound : weapon.shot_sound);
        add("empty", weapon.empty_sound);
        add("reload", weapon.reload_sound);
        return fields.empty() ? std::string() : "{" + fields + "}";
    };

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
                    if (const auto sounds = weapon_sounds(*weapon, false); !sounds.empty()) entry.emplace_back("sounds", sounds);
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

    // This mission's role clips that the libraries don't have yet (they are
    // written once all missions are read, below), and which roles each of its
    // animation records plays.
    std::map<std::string, std::vector<std::string>> animation_roles;  // lower-case record name -> "<library>/<role>"
    if (editor && !skeletons.empty()) {
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
        // The seats' clips, by their own names, where this mission has them.
        for (const auto& [model, seats] : seat_clips)
            for (const auto& [seat, by_use] : seats)
                for (const auto& [use, clip] : by_use) {
                    if (taken.contains(lower(clip))) continue;
                    if (std::ranges::none_of(records, [&](const AnimationRecord& r) { return lower(r.logical_name) == lower(clip); })) continue;
                    taken.insert(lower(clip));
                    roles.push_back({"actions", lower(clip), clip});
                }
        std::map<std::string, std::vector<const ClipRole*>> stances;
        for (const auto& role : roles) stances[role.stance].push_back(&role);
        for (const auto& [stance, stance_roles] : stances) {
            for (const auto* role : stance_roles) {
                const auto record = std::ranges::find_if(records, [&](const AnimationRecord& r) {
                    return lower(r.logical_name) == lower(role->clip);
                });
                if (record == records.end() || record->variants.empty()) {
                    missing_roles.emplace_back(stance + "/" + role->role, "anim " + stance + "/" + role->role + ": no clip " + role->clip);
                    continue;
                }
                if (collected(stance, role->role)) {
                    animation_roles[lower(record->logical_name)].push_back(stance + "/" + role->role);
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
                    library_clips[stance].push_back({name, rws::decode_animation(*clip_chunk, document.bytes()), loop});
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
                animation_roles[lower(record->logical_name)].push_back(stance + "/" + role->role);
                libraries[stance].push_back({role->role,
                                             "\"loop\": " + std::string(loop ? "true" : "false") +
                                                 ", \"speed\": " + json_number(record->velocity_scalar.value_or(0.0F) * 0.01F) +
                                                 ", \"clip\": " + json_string(role->clip),
                                             std::move(variants)});
            }
        }
    }

    // First person: each player class's hands (.MODELO) holding each weapon
    // with a first-person model (.FILE) and clips (.ANIMACIONES_PLAYER), as
    // fp/<hands stem>_<weapon model stem>: the hands' skin, the weapon on the
    // item joint of the hand its .MANO names (50 right, 40 left, as in third
    // person), and the clips named by first_person_role. The models are in
    // camera space: forward -Z, up Y, the eye at the origin. Which
    // weapons a player starts with is the mission script's (START_GAME), not
    // read yet: every weapon of the mission that has a first-person model is
    // exported. The player character's entry lists them under
    // `first_person`: weapon record name -> ID.
    if (editor)
        if (const auto file = editor->file_of_kind(MissionFileKind::weapons)) {
            const auto weapons = WeaponDatabase::project(editor->document(*file));
            const auto resources = editor->resource_index(package);
            const auto& records = editor->animations().records();
            const auto resolve = [&](const std::string& reference) -> std::optional<std::filesystem::path> {
                const auto resolution = resources.resolve(reference);
                if (resolution.candidate_indices.size() != 1) return std::nullopt;
                return resources.resources()[resolution.candidate_indices.front()].path;
            };
            for (const auto& [class_id, character_id] : class_ids) {
                const auto definitions = editor->objects().find_class(class_id);
                if (definitions.size() != 1 || !definitions.front()->first_person_model) continue;
                const auto hands_path = resolve(*definitions.front()->first_person_model);
                if (!hands_path) {
                    result.problems.push_back("class " + std::to_string(class_id) + ": hands " +
                                              *definitions.front()->first_person_model + " not found");
                    continue;
                }
                const auto hands = rws::Document::load(*hands_path);
                if (hands.chunks().empty() || hands.chunks().front().type != 0x10) continue;
                const auto hands_stem = lower(hands_path->stem().string());
                for (const auto& weapon : weapons.definitions()) {
                    if (!weapon.first_person_model || weapon.first_person_model->empty() || weapon.player_animations.empty())
                        continue;
                    const auto weapon_path = resolve(*weapon.first_person_model);
                    if (!weapon_path) {
                        result.problems.push_back("fp " + weapon.name.value_or("?") + ": " + *weapon.first_person_model + " not found");
                        continue;
                    }
                    const auto id = "fp/" + hands_stem + "_" + lower(weapon_path->stem().string());
                    const auto listed = lower(weapon.name.value_or(id));
                    if (!assets.contains(id)) {
                        const auto held = rws::Document::load(*weapon_path);
                        if (held.chunks().empty() || held.chunks().front().type != 0x10) {
                            result.problems.push_back(id + ": " + source(*weapon_path) + " is not a Clump");
                            continue;
                        }
                        std::vector<rws::CharacterClip> clips;
                        std::string role_fields;
                        std::set<std::string> roles_taken;
                        for (const auto& [type, animation_id] : weapon.player_animations) {
                            const auto role = first_person_role(type);
                            if (!roles_taken.insert(role).second) continue;
                            const auto record = std::ranges::find_if(records, [&](const AnimationRecord& r) { return r.id == animation_id; });
                            if (record == records.end() || record->variants.empty()) {
                                result.problems.push_back(id + "/" + role + ": no animation " + std::to_string(animation_id));
                                continue;
                            }
                            const bool loop = record->loop.value_or(false);
                            std::string variants;
                            for (const auto& variant : record->variants) {
                                const auto name = variants.empty() ? role : role + "_" + std::to_string(std::ranges::count(variants, '{') + 1);
                                const auto path = resolve(variant.reference);
                                if (!path) {
                                    result.problems.push_back(id + "/" + name + ": " + variant.reference + " not found");
                                    continue;
                                }
                                const auto document = rws::Document::load(*path);
                                const auto chunk = std::ranges::find_if(document.chunks(), [](const rws::Chunk& c) { return c.type == 0x1B; });
                                if (chunk == document.chunks().end()) continue;
                                clips.push_back({name, rws::decode_animation(*chunk, document.bytes()), loop});
                                // Sounds the clip starts: [seconds, sound ID].
                                std::string sounds;
                                for (const auto& event : variant.sounds) {
                                    std::int32_t sound_id{};
                                    const auto& text = event.logical_id;
                                    if (!event.time || std::from_chars(text.data(), text.data() + text.size(), sound_id).ec != std::errc{})
                                        continue;
                                    const auto found = sound_ids.find(sound_id);
                                    if (found == sound_ids.end()) continue;
                                    sounds += std::string(sounds.empty() ? "" : ", ") + "[" + json_number(*event.time) + ", " +
                                              json_string(found->second) + "]";
                                }
                                variants += std::string(variants.empty() ? "" : ", ") + json_string(name) +
                                            ": {\"source\": " + json_string(source(*path)) +
                                            (sounds.empty() ? "" : ", \"sounds\": [" + sounds + "]") + "}";
                            }
                            if (variants.empty()) continue;
                            role_fields += std::string(role_fields.empty() ? "" : ", ") + json_string(role) +
                                           ": {\"loop\": " + (loop ? "true" : "false") + ", \"clip\": " +
                                           json_string(record->logical_name) + ", \"type\": " + json_string(type) +
                                           ", \"clips\": {" + variants + "}}";
                        }
                        const auto fp_file = std::filesystem::path("fp") / (id.substr(3) + ".gltf");
                        try {
                            const auto stats = rws::export_character_gltf(
                                hands.chunks().front(), hands.bytes(), options.out / fp_file, std::move(clips),
                                textures.resolver(std::vector{*hands_path, *weapon_path}, options.out / fp_file), {}, {}, true,
                                {{&held.chunks().front(), held.bytes(), lower(weapon.hand.value_or("")) == "izquierda" ? 40 : 50}});
                            for (const auto& why : stats.skipped_clips) result.problems.push_back(id + "/" + why);
                            for (const auto& why : stats.skipped_parts) result.problems.push_back(id + ": " + why);
                            result.lines.push_back("fp\t" + fp_file.generic_string() + '\t' + std::to_string(stats.clips) +
                                                   " clips, " + std::to_string(stats.attached_parts) + " weapon parts");
                        } catch (const std::exception& error) {
                            result.problems.push_back(id + ": " + error.what());
                            continue;
                        }
                        ManifestEntry entry{{"file", json_string(fp_file.generic_string())},
                                            {"source", json_string(source(*hands_path))},
                                            {"weapon_source", json_string(source(*weapon_path))},
                                            {"name", json_string(weapon.name.value_or(""))},
                                            {"roles", "{" + role_fields + "}"}};
                        if (weapon.type) entry.emplace_back("type", json_string(*weapon.type));
                        if (const auto sounds = weapon_sounds(weapon, true); !sounds.empty()) entry.emplace_back("sounds", sounds);
                        // The barrel's end, from the hand's item joint (for flashes and shots).
                        const auto joint = lower(weapon.hand.value_or("")) == "izquierda" ? "bone_40" : "bone_50";
                        if (const auto frames = weapon_frames(held.chunks().front(), held.bytes(), true); frames.muzzle) {
                            entry.emplace_back("muzzle_joint", json_string(joint));
                            entry.emplace_back("muzzle", json_transform(*frames.muzzle));
                        }
                        assets[id] = std::move(entry);
                    }
                    auto& views = first_person[character_id];
                    if (std::ranges::none_of(views, [&](const auto& view) { return view.first == listed; }))
                        views.emplace_back(listed, id);
                }
            }
        }

    // Actors, routes, areas and dummies, in metres and radians.
    const auto markers = map_folder / "markers.json";
    write_file(options.out / markers, editor ? godot_markers_json(editor->scene(), editor->objects(), class_ids, class_stances, class_weapons, effect_ids)
                                             : godot_markers_json(MissionScene{}, ObjectDatabase{}, {}));
    result.lines.push_back("markers\t" + markers.generic_string());
    assets["map/" + lower(slot->mission)] = {
        {"visual", json_string((map_folder / "visual.gltf").generic_string())},
        {"collision", json_string((map_folder / "collision.gltf").generic_string())},
        {"markers", json_string(markers.generic_string())},
        {"source", json_string(source(package_root / slot->scene))},
    };
    if (sky) assets["map/" + lower(slot->mission)].emplace_back("sky", json_string(sky->generic_string()));

    // The mission's data for logic written by hand: texts, objectives, scripts
    // as readable text, and the names behind their numbers.
    if (editor) {
        const auto program = [&](const MissionFileKind kind) -> std::optional<ProgramDocument> {
            if (const auto file = editor->file_of_kind(kind)) return ProgramDocument::project(editor->document(*file));
            return std::nullopt;
        };
        const auto mission_program = program(MissionFileKind::mission_script);
        const auto cutscene_program = program(MissionFileKind::cutscene_script);
        GodotMissionSources sources;
        sources.scene = &editor->scene();
        sources.mission = mission_program ? &*mission_program : nullptr;
        sources.cutscene = cutscene_program ? &*cutscene_program : nullptr;
        const auto read_texts = [&](const std::string& name) -> std::map<std::string, std::string> {
            const auto path = corpus / "GlobalEK" / "Texts" / name;
            if (!std::filesystem::is_regular_file(path)) {
                result.problems.push_back("texts " + name + ": not found");
                return {};
            }
            std::ifstream in(path, std::ios::binary);
            const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            return fli_strings(std::as_bytes(std::span(bytes)));
        };
        sources.texts = read_texts(slot->mission + ".fli");
        sources.global_texts = read_texts("Global.fli");
        sources.sounds = sound_names;
        for (const auto& record : editor->animations().records())
            if (record.id) sources.animations.try_emplace(*record.id, record.logical_name);
        sources.animation_roles = animation_roles;
        for (const auto& definition : editor->objects().definitions())
            if (definition.class_id && definition.name) sources.classes.try_emplace(*definition.class_id, *definition.name);
        if (const auto file = editor->file_of_kind(MissionFileKind::weapons)) {
            const auto weapons = WeaponDatabase::project(editor->document(*file));
            for (const auto& weapon : weapons.definitions())
                if (weapon.id && weapon.name) sources.weapons.try_emplace(*weapon.id, *weapon.name);
        }
        sources.effects = effect_ids;
        const auto mission_file = std::filesystem::path("missions") / (slot->mission + ".json");
        std::filesystem::create_directories(options.out / "missions");
        write_file(options.out / mission_file, godot_mission_json(slot->mission, sources));
        result.lines.push_back("mission\t" + mission_file.generic_string() + '\t' +
                               std::to_string((sources.mission ? sources.mission->scripts().size() : 0) +
                                              (sources.cutscene ? sources.cutscene->scripts().size() : 0)) +
                               " scripts");
        // Each class's model, for actors the scripts create: {"204": "prop/alarmarota"}.
        std::string classes;
        for (const auto& [class_id, asset] : class_ids)
            classes += std::string(classes.empty() ? "" : ", ") + json_string(std::to_string(class_id)) + ": " + json_string(asset);
        assets["mission/" + lower(slot->mission)] = {{"file", json_string(mission_file.generic_string())},
                                                     {"map", json_string("map/" + lower(slot->mission))},
                                                     {"classes", "{" + classes + "}"}};
    }

    result.lines.push_back("textures\t" + std::to_string(textures.converted()) + " PNG");
    for (const auto& name : textures.missing()) result.problems.push_back("texture " + name + ": not found");
    for (const auto& [path, why] : textures.failed())
        result.problems.push_back("texture " + source(path) + ": " + why);
    if (chosen.size() > 1)
        for (auto i = first_problem; i < result.problems.size(); ++i)
            result.problems[i] = slot->mission + ": " + result.problems[i];
    }

    // Clip libraries: per stance, the role clips on the first skinned
    // character's skeleton (every human shares it), named by role. A role
    // whose record has several files (die, fidgets) gets one clip each: the
    // first under the role's name, the others as <role>_2, <role>_3, ...; the
    // game picks one at random (our reading). `custom` holds the one-off scene
    // clips (Anims/Costum: smoking, talking, repairing, ...) under their own
    // names, for missions to play.
    for (const auto& [key, problem] : missing_roles) {
        const auto slash = key.find('/');
        if (!collected(key.substr(0, slash), key.substr(slash + 1))) result.problems.push_back(problem);
    }
    for (auto& [stance, roles] : libraries) {
        const auto& [skeleton_id, skeleton_path] = *skeletons.begin();
        const auto file = std::filesystem::path("anims") / (stance + ".gltf");
        std::set<std::string> skipped;
        try {
            const auto model = rws::Document::load(skeleton_path);
            const auto stats = rws::export_character_gltf(model.chunks().front(), model.bytes(), options.out / file,
                                                          std::move(library_clips[stance]), {}, humanoid_joint_name,
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
        for (const auto& role : roles) {
            std::string variants;
            for (const auto& [name, fields] : role.variants)
                if (!skipped.contains(name))
                    variants += std::string(variants.empty() ? "" : ", ") + json_string(name) + ": " + fields;
            if (variants.empty()) continue;
            clip_fields += std::string(clip_fields.empty() ? "" : ", ") + json_string(role.role) + ": {" + role.fields +
                           ", \"clips\": {" + variants + "}}";
        }
        assets["anim/" + stance] = {{"file", json_string(file.generic_string())},
                                    {"skeleton", json_string(skeleton_id)},
                                    {"roles", "{" + clip_fields + "}"}};
    }
    for (const auto& [character_id, views] : first_person) {
        if (views.empty() || !assets.contains(character_id)) continue;
        std::string fields;
        for (const auto& [name, id] : views) fields += std::string(fields.empty() ? "" : ", ") + json_string(name) + ": " + json_string(id);
        assets[character_id].emplace_back("first_person", "{" + fields + "}");
    }

    for (const auto& [model, frames] : seat_frames) {
        const auto clips = seat_clips.find(model);
        if (!assets.contains(model) || (clips == seat_clips.end() && !vehicle_models.contains(model))) continue;
        auto count = static_cast<std::int32_t>(frames.size());
        if (clips != seat_clips.end() && !clips->second.empty()) count = std::max(count, clips->second.rbegin()->first + 1);
        std::string seats;
        for (std::int32_t seat = 0; seat < count; ++seat) {
            std::string fields = "\"frame\": " + std::to_string(seat < static_cast<std::int32_t>(frames.size()) ? frames[seat] : -1);
            if (clips != seat_clips.end())
                if (const auto by_use = clips->second.find(seat); by_use != clips->second.end()) {
                    std::string uses;
                    for (const auto& [use, clip] : by_use->second)
                        uses += std::string(uses.empty() ? "" : ", ") + json_string(use) + ": " + json_string("actions/" + lower(clip));
                    fields += ", \"clips\": {" + uses + "}";
                }
            seats += std::string(seats.empty() ? "" : ", ") + "{" + fields + "}";
        }
        assets[model].emplace_back("seats", "[" + seats + "]");
    }
    // A mounted gun's model has no seat frames: one seat at its origin.
    for (const auto& [model, seats] : seat_clips) {
        if (seat_frames.contains(model) || !assets.contains(model)) continue;
        std::string list;
        for (const auto& [seat, by_use] : seats) {
            std::string uses;
            for (const auto& [use, clip] : by_use)
                uses += std::string(uses.empty() ? "" : ", ") + json_string(use) + ": " + json_string("actions/" + lower(clip));
            list += std::string(list.empty() ? "" : ", ") + "{\"frame\": -1, \"clips\": {" + uses + "}}";
        }
        assets[model].emplace_back("seats", "[" + list + "]");
    }

    std::string missions;
    for (const auto* slot : chosen) missions += std::string(missions.empty() ? "" : ", ") + json_string(slot->mission);

    std::string manifest = "{\n  \"format\": " + json_string(manifest_format) +
                           ",\n  \"version\": 1,\n  \"units\": \"metres\",\n  \"up\": \"Y\",\n  \"missions\": [" +
                           missions + "],\n  \"assets\": {";
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
