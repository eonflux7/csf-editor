#include "csf/authoring.hpp"
#include "csf/mission_scene.hpp"
#include "csf/program.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace csf {
namespace {

void write_u32(std::vector<std::byte>& bytes, const std::size_t offset,
               const std::uint32_t value) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("CSFFBS edit offset is outside the document");
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes[offset + shift / 8U] = static_cast<std::byte>((value >> shift) & 0xFFU);
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
}

std::vector<std::byte> terminated(std::span<const std::byte> value) {
    std::vector<std::byte> result(value.begin(), value.end());
    if (result.empty() || result.back() != std::byte{0}) result.push_back(std::byte{0});
    return result;
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

std::string path_string(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string hex_bytes(const std::span<const std::byte> bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : bytes) out << std::setw(2) << std::to_integer<unsigned>(byte);
    return out.str();
}

std::string value_json(const EditValue& value) {
    return std::visit(
        [](const auto& item) -> std::string {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::vector<std::byte>>)
                return "{\"raw_hex\":\"" + hex_bytes(item) + "\"}";
            else if constexpr (std::is_same_v<T, float>) {
                std::ostringstream out;
                out << std::setprecision(std::numeric_limits<float>::max_digits10) << item;
                return out.str();
            } else return std::to_string(item);
        },
        value);
}

void write_file(const std::filesystem::path& path, const std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Cannot create output: " + path_string(path));
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) throw std::runtime_error("Cannot write complete output: " + path_string(path));
}

void write_text(const std::filesystem::path& path, const std::string_view text) {
    const auto bytes = std::as_bytes(std::span(text));
    write_file(path, bytes);
}

void publish(const std::filesystem::path& temporary, const std::filesystem::path& destination) {
    std::error_code error;
    auto backup = destination;
    backup += ".previous";
    const bool existed = std::filesystem::exists(destination, error) && !error;
    if (existed) {
        std::filesystem::remove(backup, error);
        error.clear();
        std::filesystem::rename(destination, backup, error);
        if (error) throw std::runtime_error("Cannot preserve previous output: " + error.message());
    }
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        if (existed) {
            std::error_code restore_error;
            std::filesystem::rename(backup, destination, restore_error);
        }
        throw std::runtime_error("Cannot publish validated output: " + error.message());
    }
    if (existed) std::filesystem::remove(backup, error);
}

// Small self-contained SHA-256 keeps manifests portable on Windows and Linux.
constexpr std::array<std::uint32_t, 64> sha_k{
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

} // namespace

std::string sha256(const std::span<const std::byte> input) {
    std::vector<std::byte> data(input.begin(), input.end());
    const auto bit_size = static_cast<std::uint64_t>(data.size()) * 8U;
    data.push_back(std::byte{0x80});
    while (data.size() % 64 != 56) data.push_back(std::byte{0});
    for (int shift = 56; shift >= 0; shift -= 8)
        data.push_back(static_cast<std::byte>((bit_size >> shift) & 0xffU));
    std::array<std::uint32_t, 8> h{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    for (std::size_t block = 0; block < data.size(); block += 64) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto at = block + i * 4;
            w[i] = (std::to_integer<std::uint32_t>(data[at]) << 24U) |
                   (std::to_integer<std::uint32_t>(data[at + 1]) << 16U) |
                   (std::to_integer<std::uint32_t>(data[at + 2]) << 8U) |
                   std::to_integer<std::uint32_t>(data[at + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const auto s0 = std::rotr(w[i-15],7) ^ std::rotr(w[i-15],18) ^ (w[i-15] >> 3U);
            const auto s1 = std::rotr(w[i-2],17) ^ std::rotr(w[i-2],19) ^ (w[i-2] >> 10U);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        auto [a,b,c,d,e,f,g,hh] = h;
        for (std::size_t i = 0; i < 64; ++i) {
            const auto s1 = std::rotr(e,6) ^ std::rotr(e,11) ^ std::rotr(e,25);
            const auto ch = (e & f) ^ (~e & g);
            const auto t1 = hh + s1 + ch + sha_k[i] + w[i];
            const auto s0 = std::rotr(a,2) ^ std::rotr(a,13) ^ std::rotr(a,22);
            const auto maj = (a & b) ^ (a & c) ^ (b & c);
            const auto t2 = s0 + maj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto word : h) out << std::setw(8) << word;
    return out.str();
}

const char* edit_target_kind_name(const EditTargetKind kind) noexcept {
    switch (kind) {
    case EditTargetKind::integer: return "integer";
    case EditTargetKind::real: return "real";
    case EditTargetKind::string_reference: return "string-reference-copy-on-write";
    case EditTargetKind::string_value: return "string-value-global";
    }
    return "unknown";
}

const char* authoring_category_name(const AuthoringCategory category) noexcept {
    switch (category) {
    case AuthoringCategory::database_scalar: return "database-scalar";
    case AuthoringCategory::reference: return "reference";
    case AuthoringCategory::spatial: return "spatial";
    case AuthoringCategory::player_property: return "player-property";
    case AuthoringCategory::script_flag: return "script-flag";
    }
    return "unknown";
}

std::vector<EditableField> authorable_fields(const Document& document) {
    std::vector<EditableField> result;
    const auto label = [&](const Node& node) {
        if (!node.identifier_index) return std::string{};
        const auto* value = document.identifier(*node.identifier_index);
        return value ? value->display_utf8() : std::string{};
    };
    const auto accepted = [](const std::string& name, const ValueKind kind,
                             const std::string& parent)
        -> std::optional<std::pair<AuthoringCategory, std::string>> {
        const std::set<std::string> player{".PLAYER", ".INICIO_COMMANDO", ".INICIO_SNIPER",
                                           ".INICIO_SPY", ".PUNTUACION_MAXIMA", ".PUNTUACION_MINIMA"};
        const std::set<std::string> flags{".ENABLED", ".VALIDO", ".TRIGGER"};
        const std::set<std::string> spatial{".ANGULO", ".ANGULO_X", ".ROT", ".ROT_X"};
        const std::set<std::string> references{
            ".MAPA_SECTORES", ".BANDO", ".PORTRAIT", "FILE", "FILE2", ".FILE", ".FILE2",
            "MODELO", "MODELO_TERCERA", "MODEL", "THIRDPERSONMODEL", "MODELFILE",
            "MODELO_COLISION", "COLLISIONMODEL", "RAGDOLL", "ANIMACION", "ANIMATION"};
        const std::set<std::string> database{
            "DAMAGE", "DANO", "DAÑO", "FORCE", "FUERZA", "DISPERSION", "CADENCE", "CADENCIA",
            "AMMO", "MUNICION", "COUNT", "CANTIDAD", "HEALTH", "SALUD", "MASS", "MASA",
            "DAMPING", "AMORTIGUACION", "COLOR", "DURATION", "DURACION", "TIME", "TIEMPO",
            ".COLISION", ".FLAGS", ".SEGUNDA_EXPLOSION", ".PRIORITY", ".SHARE_GROUP",
            ".RADIUS", ".RADIO"};
        if (player.contains(name) && kind == ValueKind::integer)
            return {{AuthoringCategory::player_property, "Supported mission/player scalar"}};
        if (flags.contains(name) && kind == ValueKind::integer)
            return {{AuthoringCategory::script_flag, "Boolean script flag (0 or 1)"}};
        if ((spatial.contains(name) || (name.empty() && parent == ".POS")) && kind == ValueKind::real)
            return {{AuthoringCategory::spatial, "Finite mission transform component"}};
        if (references.contains(name) && kind == ValueKind::string)
            return {{AuthoringCategory::reference, "Supported path/reference value"}};
        if (database.contains(name) && (kind == ValueKind::integer || kind == ValueKind::real))
            return {{AuthoringCategory::database_scalar, "Reviewed database scalar"}};
        return std::nullopt;
    };
    const auto walk = [&](const auto& self, const Node& node, const std::string& parent) -> void {
        const auto name = label(node);
        const auto kind = document.entries().at(node.entry_index).kind();
        if (kind)
            if (const auto policy = accepted(name, *kind, parent))
                result.push_back({node.entry_index,
                                  *kind == ValueKind::integer ? EditTargetKind::integer
                                  : *kind == ValueKind::real ? EditTargetKind::real
                                                            : EditTargetKind::string_reference,
                                  policy->first, name.empty() ? parent + " component" : name,
                                  policy->second});
        for (const auto& child : node.children) self(self, child, name.empty() ? parent : name);
    };
    for (const auto& root : document.roots()) walk(walk, root, {});
    return result;
}

EditSession::EditSession(Document document)
    : original_(document), document_(std::move(document)), bytes_(document_.bytes().begin(),
                                                               document_.bytes().end()) {
    if (document_.has_errors()) throw std::runtime_error("Cannot edit a structurally invalid CSFFBS document");
}

EditSession EditSession::load(const std::filesystem::path& path) {
    return EditSession(Document::load(path));
}

EditSession EditSession::from_document(Document document) { return EditSession(std::move(document)); }

void EditSession::reparse() {
    document_ = Document::from_bytes(bytes_);
    if (document_.has_errors()) throw std::runtime_error("An edit produced an invalid CSFFBS document");
}

void EditSession::commit(EditCommand command) {
    if (cursor_ < history_.size()) history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_), history_.end());
    apply(command, true);
    history_.push_back(std::move(command));
    ++cursor_;
}

EditValidation EditSession::set_integer(const std::uint32_t index, const std::int32_t value,
                                        std::string meaning) {
    if (index >= document_.entries().size() ||
        document_.entries()[index].kind() != ValueKind::integer)
        return {false, true, "Target is not a known integer entry"};
    const auto before = static_cast<std::int32_t>(document_.entries()[index].raw_value_or_size);
    if (before == value) return {true, false, "Value is unchanged"};
    EditValidation validation{true, false, {}};
    commit({{EditTargetKind::integer,index,0}, before, value, validation, std::move(meaning)});
    return validation;
}

EditValidation EditSession::set_real(const std::uint32_t index, const float value,
                                     std::string meaning) {
    if (!std::isfinite(value)) return {false, true, "Real values must be finite"};
    if (index >= document_.entries().size() || document_.entries()[index].kind() != ValueKind::real)
        return {false, true, "Target is not a known real entry"};
    const auto before = std::bit_cast<float>(document_.entries()[index].raw_value_or_size);
    if (std::bit_cast<std::uint32_t>(before) == std::bit_cast<std::uint32_t>(value))
        return {true, false, "Value is unchanged"};
    EditValidation validation{true, false, {}};
    commit({{EditTargetKind::real,index,0}, before, value, validation, std::move(meaning)});
    return validation;
}

EditValidation EditSession::set_string(const std::uint32_t index,
                                       const std::span<const std::byte> value,
                                       const bool global_replace, std::string meaning) {
    if (index >= document_.entries().size() || document_.entries()[index].kind() != ValueKind::string)
        return {false, true, "Target is not a known string entry"};
    const auto table = document_.entries()[index].raw_value_or_size;
    const auto* old = document_.string(table);
    if (!old || !old->complete) return {false, true, "String reference is invalid or truncated"};
    auto next = terminated(value);
    if (old->bytes == next) return {true, false, "Value is unchanged"};
    EditValidation validation{true, false, {}};
    const auto kind = global_replace ? EditTargetKind::string_value : EditTargetKind::string_reference;
    EditCommand command{{kind,index,table}, old->bytes, next, validation, std::move(meaning)};
    if (!global_replace) command.before = table;
    commit(std::move(command));
    return validation;
}

EditValidation EditSession::set_string(const std::uint32_t index, const std::string_view value,
                                       const bool global_replace, std::string meaning) {
    return set_string(index, std::as_bytes(std::span(value)), global_replace, std::move(meaning));
}

void EditSession::apply(const EditCommand& command, const bool forward) {
    const auto index = command.target.entry_index;
    if (index >= document_.entries().size()) throw std::runtime_error("Edit target no longer exists");
    const auto entry_offset = static_cast<std::size_t>(document_.entries()[index].source.offset);
    if (command.target.kind == EditTargetKind::integer) {
        const auto value = std::get<std::int32_t>(forward ? command.after : command.before);
        write_u32(bytes_, entry_offset + 4, static_cast<std::uint32_t>(value));
    } else if (command.target.kind == EditTargetKind::real) {
        const auto value = std::get<float>(forward ? command.after : command.before);
        write_u32(bytes_, entry_offset + 4, std::bit_cast<std::uint32_t>(value));
    } else if (command.target.kind == EditTargetKind::string_value) {
        const auto* string = document_.string(command.target.table_index);
        if (!string || !string->complete) throw std::runtime_error("Edited string table record is unavailable");
        const auto& payload = std::get<std::vector<std::byte>>(forward ? command.after : command.before);
        std::vector<std::byte> record;
        append_u32(record, static_cast<std::uint32_t>(payload.size()));
        record.insert(record.end(), payload.begin(), payload.end());
        const auto begin = bytes_.begin() + static_cast<std::ptrdiff_t>(string->source.offset);
        bytes_.erase(begin, begin + static_cast<std::ptrdiff_t>(string->source.size));
        bytes_.insert(bytes_.begin() + static_cast<std::ptrdiff_t>(string->source.offset),
                      record.begin(), record.end());
    } else if (forward) {
        const auto& payload = std::get<std::vector<std::byte>>(command.after);
        const auto insert_offset = document_.trailing_bytes().empty()
                                       ? bytes_.size()
                                       : static_cast<std::size_t>(document_.trailing_bytes().data() - document_.bytes().data());
        std::vector<std::byte> record;
        append_u32(record, static_cast<std::uint32_t>(payload.size()));
        record.insert(record.end(), payload.begin(), payload.end());
        bytes_.insert(bytes_.begin() + static_cast<std::ptrdiff_t>(insert_offset), record.begin(), record.end());
        write_u32(bytes_, 20, document_.header().string_count + 1);
        write_u32(bytes_, entry_offset + 4, document_.header().string_count);
    } else {
        const auto last = document_.strings().empty() ? nullptr : &document_.strings().back();
        if (!last || last->table_index != document_.header().string_count - 1)
            throw std::runtime_error("Copy-on-write string history is inconsistent");
        const auto begin = bytes_.begin() + static_cast<std::ptrdiff_t>(last->source.offset);
        bytes_.erase(begin, begin + static_cast<std::ptrdiff_t>(last->source.size));
        write_u32(bytes_, 20, document_.header().string_count - 1);
        write_u32(bytes_, entry_offset + 4, std::get<std::uint32_t>(command.before));
    }
    reparse();
}

bool EditSession::undo() {
    if (!can_undo()) return false;
    apply(history_[cursor_ - 1], false);
    --cursor_;
    return true;
}

bool EditSession::redo() {
    if (!can_redo()) return false;
    apply(history_[cursor_], true);
    ++cursor_;
    return true;
}

void EditSession::discard() {
    bytes_.assign(original_.bytes().begin(), original_.bytes().end());
    reparse();
    history_.clear();
    cursor_ = 0;
}

std::vector<std::byte> EditSession::serialize() const { return bytes_; }

std::string EditSession::change_manifest_json(const std::filesystem::path& output,
                                              const SaveValidation& validation) const {
    std::ostringstream out;
    out << "{\n  \"schema\":\"csf-change-manifest-1\",\n  \"source\":\""
        << json_escape(path_string(original_.source_path().filename())) << "\",\n  \"output\":\""
        << json_escape(path_string(output.filename())) << "\",\n  \"source_sha256\":\""
        << sha256(original_.bytes()) << "\",\n  \"output_sha256\":\"" << sha256(bytes_)
        << "\",\n  \"validation\":\"" << (validation.passed ? "passed" : "failed")
        << "\",\n  \"changes\":[";
    for (std::size_t i = 0; i < cursor_; ++i) {
        const auto& change = history_[i];
        if (i) out << ',';
        out << "\n    {\"entry\":" << change.target.entry_index << ",\"kind\":\""
            << edit_target_kind_name(change.target.kind) << "\",\"before\":"
            << value_json(change.before) << ",\"after\":" << value_json(change.after)
            << ",\"meaning\":\"" << json_escape(change.meaning) << "\"}";
    }
    if (cursor_) out << '\n';
    out << "  ],\n  \"diagnostics\":[";
    for (std::size_t i = 0; i < validation.diagnostics.size(); ++i) {
        if (i) out << ',';
        out << "\"" << json_escape(validation.diagnostics[i]) << "\"";
    }
    out << "]\n}\n";
    return out.str();
}

SaveResult EditSession::save_copy(const std::filesystem::path& output,
                                  std::optional<std::filesystem::path> manifest) const {
    if (output.empty()) throw std::runtime_error("Output path is empty");
    std::error_code error;
    if (!original_.source_path().empty() &&
        std::filesystem::weakly_canonical(output, error) ==
            std::filesystem::weakly_canonical(original_.source_path(), error))
        throw std::runtime_error("Refusing to overwrite the source CSFFBS file");
    if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path(), error);
    if (error) throw std::runtime_error("Cannot create output directory: " + error.message());
    auto temporary = output;
    temporary += ".tmp-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    try {
        write_file(temporary, bytes_);
        const auto reopened = Document::load(temporary);
        SaveValidation validation;
        validation.passed = !reopened.has_errors() && reopened.bytes().size() == bytes_.size() &&
                            std::equal(reopened.bytes().begin(), reopened.bytes().end(), bytes_.begin());
        for (const auto& diagnostic : reopened.diagnostics())
            validation.diagnostics.push_back(diagnostic.message);
        auto extension = original_.source_path().extension().string();
        std::ranges::transform(extension, extension.begin(), [](const unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        if (extension == ".scn") {
            const auto scene = MissionScene::project(reopened);
            for (const auto& diagnostic : scene.diagnostics()) {
                validation.diagnostics.push_back(diagnostic.code + ": " + diagnostic.message);
                validation.passed &= diagnostic.severity != Diagnostic::Severity::error;
            }
            const auto& nav = scene.navigation_stats();
            if (nav.invalid_connections || nav.duplicate_group_ids || nav.duplicate_point_ids)
                validation.diagnostics.push_back(
                    "navigation: invalid=" + std::to_string(nav.invalid_connections) +
                    ", duplicate-groups=" + std::to_string(nav.duplicate_group_ids) +
                    ", duplicate-points=" + std::to_string(nav.duplicate_point_ids));
        } else if (extension == ".gsc" || extension == ".csc") {
            const auto program = ProgramDocument::project(reopened);
            for (const auto& diagnostic : program.diagnostics()) {
                validation.diagnostics.push_back(diagnostic.code + ": " + diagnostic.message);
                validation.passed &= diagnostic.severity != Diagnostic::Severity::error;
            }
        }
        if (!validation.passed) throw std::runtime_error("Reopened output failed structural or byte validation");
        publish(temporary, output);
        const auto manifest_path = manifest.value_or(output.string() + ".changes.json");
        auto manifest_temp = manifest_path;
        manifest_temp += ".tmp";
        if (!manifest_path.parent_path().empty())
            std::filesystem::create_directories(manifest_path.parent_path(), error);
        write_text(manifest_temp, change_manifest_json(output, validation));
        publish(manifest_temp, manifest_path);
        return {output, manifest_path, sha256(original_.bytes()), sha256(bytes_), validation};
    } catch (...) {
        std::filesystem::remove(temporary, error);
        throw;
    }
}

} // namespace csf
