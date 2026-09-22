#include "csf/animation_catalog.hpp"
#include "csf/cmo.hpp"
#include "csf/document.hpp"
#include "csf/export.hpp"
#include "csf/mission_edit.hpp"
#include "csf/mod_project.hpp"
#include "csf/script_signatures.hpp"
#include "csf/source_text.hpp"
#include "csf/tree.hpp"
#include "csf/mission.hpp"
#include "csf/mission_scene.hpp"
#include "csf/object_database.hpp"
#include "csf/overlay.hpp"
#include "csf/program.hpp"
#include "rws/animation.hpp"
#include "rws/decoded.hpp"
#include "rws/document.hpp"
#include "rws/obj_export.hpp"
#include "rws/physics_inspection.hpp"
#include "rws/scene_export.hpp"
#include "rws/texture_image.hpp"
#include "rws/world_recovery.hpp"
#include "rwsman/commands.hpp"
#include "rwsman/diagnostics.hpp"
#include "rwsman/discovery.hpp"
#include "rwsman/frame_pacing.hpp"
#include "rwsman/fuzzy.hpp"
#include "rwsman/history.hpp"
#include "rwsman/index_builders.hpp"
#include "rwsman/log.hpp"
#include "rwsman/search_index.hpp"
#include "rwsman/settings.hpp"
#include "rwsman/viewport_overlays.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <ranges>
#include <string>
#include <vector>

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::cerr << "CHECK failed: " #expression " at " << __FILE__ << ':' << __LINE__        \
                      << '\n';                                                                     \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

namespace {

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}

void append_header(std::vector<std::byte>& bytes, const std::uint32_t type,
                   const std::uint32_t size, const std::uint32_t stamp = 0x1C020037) {
    append_u32(bytes, type);
    append_u32(bytes, size);
    append_u32(bytes, stamp);
}

void append_f32(std::vector<std::byte>& bytes, const float value) {
    append_u32(bytes, std::bit_cast<std::uint32_t>(value));
}

void append_u16(std::vector<std::byte>& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>(value & 0xFFU));
    bytes.push_back(static_cast<std::byte>(value >> 8U));
}

void append_csf_entry(std::vector<std::byte>& bytes, const std::uint32_t next,
                      const std::uint32_t value, const std::int16_t identifier,
                      const std::uint16_t type) {
    append_u32(bytes, next);
    append_u32(bytes, value);
    append_u16(bytes, static_cast<std::uint16_t>(identifier));
    append_u16(bytes, type);
}

void append_csf_string(std::vector<std::byte>& bytes, const std::string& value,
                       const bool terminated = true) {
    append_u32(bytes, static_cast<std::uint32_t>(value.size() + (terminated ? 1U : 0U)));
    for (const auto character : value)
        bytes.push_back(static_cast<std::byte>(character));
    if (terminated) bytes.push_back(std::byte{0});
}

void append_csf_header(std::vector<std::byte>& bytes, const std::uint32_t entries,
                       const std::uint32_t identifiers, const std::uint32_t strings) {
    for (const auto character : std::string("CSFFBS"))
        bytes.push_back(static_cast<std::byte>(character));
    bytes.push_back(std::byte{0});
    bytes.push_back(std::byte{0x7F});
    append_u32(bytes, 1);
    append_u32(bytes, entries);
    append_u32(bytes, identifiers);
    append_u32(bytes, strings);
}

void append_world_sector(std::vector<std::byte>& output, const std::uint32_t format,
                         const std::int32_t material_base, const std::int32_t vertices,
                         const std::vector<std::array<std::uint16_t, 4>>& triangles) {
    std::vector<std::byte> data;
    append_u32(data, static_cast<std::uint32_t>(material_base));
    append_u32(data, static_cast<std::uint32_t>(triangles.size()));
    append_u32(data, static_cast<std::uint32_t>(vertices));
    for (const float value : {-1.0F, -2.0F, -3.0F, 4.0F, 5.0F, 6.0F})
        append_f32(data, value);
    append_u32(data, 0);
    append_u32(data, 0);
    if (vertices > 0) {
        for (std::int32_t i = 0; i < vertices; ++i) {
            append_f32(data, static_cast<float>(i));
            append_f32(data, 0);
            append_f32(data, 0);
        }
        if (format & 0x10U)
            for (std::int32_t i = 0; i < vertices; ++i)
                append_u32(data, 0x007F0000U);
        if (format & 0x08U)
            for (std::int32_t i = 0; i < vertices; ++i)
                append_u32(data, 0xFFFFFFFFU);
        auto uv_sets = (format >> 16U) & 0xFFU;
        if (uv_sets == 0) uv_sets = (format & 0x80U) ? 2U : ((format & 0x04U) ? 1U : 0U);
        for (std::uint32_t set = 0; set < uv_sets; ++set)
            for (std::int32_t i = 0; i < vertices; ++i) {
                append_f32(data, static_cast<float>(i));
                append_f32(data, static_cast<float>(set));
            }
    }
    for (const auto& triangle : triangles)
        for (const auto word : triangle)
            append_u16(data, word);
    append_header(output, 0x09, static_cast<std::uint32_t>(12U + data.size()));
    append_header(output, 0x01, static_cast<std::uint32_t>(data.size()));
    output.insert(output.end(), data.begin(), data.end());
}

std::vector<std::byte>
make_world(const std::uint32_t format, const std::int32_t declared_sectors,
           const std::int32_t declared_triangles, const std::int32_t declared_vertices,
           const std::int32_t materials, const std::vector<std::vector<std::byte>>& sectors,
           const std::int32_t declared_planes = 0, const bool root_is_sector = true) {
    std::vector<std::byte> payload;
    append_header(payload, 0x01, 64);
    append_u32(payload, root_is_sector ? 1U : 0U);
    append_f32(payload, 0);
    append_f32(payload, 0);
    append_f32(payload, 0);
    append_u32(payload, static_cast<std::uint32_t>(declared_triangles));
    append_u32(payload, static_cast<std::uint32_t>(declared_vertices));
    append_u32(payload, static_cast<std::uint32_t>(declared_planes));
    append_u32(payload, static_cast<std::uint32_t>(declared_sectors));
    append_u32(payload, 0);
    append_u32(payload, format);
    for (const float value : {4.0F, 5.0F, 6.0F, -1.0F, -2.0F, -3.0F})
        append_f32(payload, value);
    std::vector<std::byte> material_struct;
    append_u32(material_struct, static_cast<std::uint32_t>(materials));
    for (std::int32_t i = 0; i < materials; ++i)
        append_u32(material_struct, 0xFFFFFFFFU);
    append_header(payload, 0x08, static_cast<std::uint32_t>(12U + material_struct.size()));
    append_header(payload, 0x01, static_cast<std::uint32_t>(material_struct.size()));
    payload.insert(payload.end(), material_struct.begin(), material_struct.end());
    for (const auto& sector : sectors)
        payload.insert(payload.end(), sector.begin(), sector.end());
    std::vector<std::byte> bytes;
    append_header(bytes, 0x0B, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

void append_plane(std::vector<std::byte>& output, const std::int32_t axis, const bool left_sector,
                  const bool right_sector, const float split, const float left_value,
                  const float right_value) {
    append_header(output, 0x0A, 36);
    append_header(output, 0x01, 24);
    append_u32(output, static_cast<std::uint32_t>(axis));
    append_f32(output, split);
    append_u32(output, left_sector ? 1U : 0U);
    append_u32(output, right_sector ? 1U : 0U);
    append_f32(output, left_value);
    append_f32(output, right_value);
}

void write_f32(std::vector<std::byte>& bytes, const std::size_t offset, const float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes[offset + shift / 8U] = static_cast<std::byte>((bits >> shift) & 0xFFU);
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    CHECK(output.good());
}

void append_length_string(std::vector<std::byte>& bytes, const std::string& value) {
    append_u32(bytes, static_cast<std::uint32_t>(value.size()));
    for (const auto character : value)
        bytes.push_back(static_cast<std::byte>(character));
}

struct SyntheticCsf {
    struct Item {
        std::uint32_t next{}, value{};
        std::int16_t identifier{-1};
        std::uint16_t type{};
    };
    std::vector<Item> entries;
    std::vector<std::string> identifiers;
    std::vector<std::string> strings;

    std::int16_t identifier(const std::string& name) {
        const auto found = std::ranges::find(identifiers, name);
        if (found != identifiers.end())
            return static_cast<std::int16_t>(found - identifiers.begin());
        identifiers.push_back(name);
        return static_cast<std::int16_t>(identifiers.size() - 1);
    }
    void container(const std::string& name, const std::uint32_t count,
                   const std::uint16_t type = 1) {
        entries.push_back(
            {0, count, name.empty() ? static_cast<std::int16_t>(-1) : identifier(name), type});
    }
    void integer(const std::string& name, const std::int32_t value) {
        if (!name.empty()) entries.push_back({2, 0, identifier(name), 0});
        entries.push_back({0, static_cast<std::uint32_t>(value), -1, 3});
    }
    void real(const std::string& name, const float value) {
        if (!name.empty()) entries.push_back({2, 0, identifier(name), 0});
        entries.push_back({0, std::bit_cast<std::uint32_t>(value), -1, 4});
    }
    void string(const std::string& name, const std::string& value) {
        const auto index = static_cast<std::uint32_t>(strings.size());
        strings.push_back(value);
        entries.push_back({2, 0, identifier(name), 0});
        entries.push_back({0, index, -1, 5});
    }
    void position(const float x, const float y, const float z) {
        container(".POS", 3);
        real("", x);
        real("", y);
        real("", z);
    }
    void unknown() { entries.push_back({0, 0, -1, 99}); }
    std::vector<std::byte> bytes() const {
        std::vector<std::byte> result;
        append_csf_header(result, static_cast<std::uint32_t>(entries.size()),
                          static_cast<std::uint32_t>(identifiers.size()),
                          static_cast<std::uint32_t>(strings.size()));
        for (const auto& item : entries)
            append_csf_entry(result, item.next, item.value, item.identifier, item.type);
        for (const auto& value : identifiers)
            append_csf_string(result, value);
        for (const auto& value : strings)
            append_csf_string(result, value);
        return result;
    }
};

std::vector<std::byte> make_typed_scene(const bool with_unknown = false,
                                        const float actor_heading = 0.0F) {
    SyntheticCsf csf;
    csf.container("", with_unknown ? 8U : 7U, 2);
    csf.integer(".PLAYER", 0);
    csf.container(".BICHOS", 2);
    csf.container("", 14, 2);
    csf.string(".NOMBRE", "duplicate");
    csf.integer(".ID", 10);
    csf.integer(".CLASSID", 55);
    csf.position(101, 202, 303);
    csf.real(".ANGULO", actor_heading);
    csf.integer(".FLAGS", 0);
    csf.string(".SCRIPT", "OnSpawn");
    csf.integer(".FUTURE", 99);
    csf.integer(".SEGUNDA_EXPLOSION", 1);
    csf.string(".BANDO", "ALLIES");
    csf.string(".PORTRAIT", "portrait.fbs");
    csf.container(".ANIMACIONES", 1);
    csf.container("", 2, 2);
    csf.integer(".ID", 77);
    csf.string(".TIPO", "IDLE");
    csf.container(".DOOR_BOX", 2);
    csf.container("", 3);
    csf.real("", -1);
    csf.real("", -2);
    csf.real("", -3);
    csf.container("", 3);
    csf.real("", 1);
    csf.real("", 2);
    csf.real("", 3);
    csf.container(".CELDA", 2, 2);
    csf.integer(".GRUPO", 7);
    csf.integer(".PUNTO", 8);
    csf.container("", 5, 2);
    csf.string(".NOMBRE", "duplicate");
    csf.integer(".ID", 11);
    csf.integer(".CLASSID", 56);
    csf.position(4, 5, 6);
    csf.real(".ANGULO", 1);
    csf.container(".MALLA_NAVEGACION", 2, 2);
    csf.container(".GRUPOS", 1);
    csf.container("", 5, 2);
    csf.integer(".ID", 7);
    csf.string(".NOMBRE", "route");
    csf.integer(".TIPO", 0);
    csf.container(".PUNTOS", 3);
    csf.container("", 5, 2);
    csf.integer(".ID", 8);
    csf.string(".NOMBRE", "a");
    csf.position(1, 2, 3);
    csf.real(".ROT", 0);
    csf.real(".ROT_X", 0);
    csf.container("", 5, 2);
    csf.integer(".ID", 9);
    csf.string(".NOMBRE", "b");
    csf.position(4, 5, 6);
    csf.real(".ROT", 0);
    csf.real(".ROT_X", 0);
    csf.container("", 5, 2);
    csf.integer(".ID", 9);
    csf.string(".NOMBRE", "duplicate-id");
    csf.position(7, 8, 9);
    csf.real(".ROT", 0);
    csf.real(".ROT_X", 0);
    csf.container(".CONEXIONES", 2);
    csf.container("", 2, 2);
    csf.integer(".PUNTO_ORI", 8);
    csf.integer(".PUNTO_DST", 9);
    csf.container("", 2, 2);
    csf.integer(".PUNTO_ORI", 8);
    csf.integer(".PUNTO_DST", 404);
    csf.container(".CONEXIONES_GRUPOS", 1);
    csf.container("", 4, 2);
    csf.integer(".GRUPO_ORI", 7);
    csf.integer(".PUNTO_ORI", 8);
    csf.integer(".GRUPO_DST", 7);
    csf.integer(".PUNTO_DST", 9);
    csf.container(".MALLA_LUCES", 1, 2);
    csf.container(".LIGHTS", 1);
    csf.container("", 6, 2);
    csf.integer(".ID", 1);
    csf.string(".NOMBRE", "lamp");
    csf.position(7, 8, 9);
    csf.integer(".COLOR", 0x112233);
    csf.integer(".MODULATE", 0);
    csf.real(".RADIO", 25);
    csf.container(".MALLA_DUMMIES", 2, 2);
    csf.container(".DUMMIES", 1);
    csf.container("", 5, 2);
    csf.integer(".ID", 20);
    csf.string(".NOMBRE", "camera-target");
    csf.position(2, 3, 4);
    csf.real(".ROT", 0);
    csf.real(".ROT_X", 0);
    csf.container(".CARPETAS", 1, 2);
    csf.container(".RAIZ", 1);
    csf.container("", 2, 2);
    csf.string(".NOMBRE", "");
    csf.container(".CARPETAS", 1);
    csf.container("", 2, 2);
    csf.string(".NOMBRE", "targets");
    csf.container(".ELEMENTOS", 1);
    csf.integer("", 20);
    csf.container(".EFECTOS", 1, 2);
    csf.container("", 6, 2);
    csf.integer(".ID", 40);
    csf.string(".NOMBRE", "sparks");
    csf.integer(".CLASSID", 140);
    csf.integer(".DUMMY", 20);
    csf.integer(".PRIORITY", -1);
    csf.integer(".SHARE_GROUP", -1);
    csf.container(".MALLA_AREAS", 1, 2);
    csf.container(".AREAS", 1);
    csf.container("", 8, 2);
    csf.integer(".ID", 30);
    csf.integer(".FLAGS", 1);
    csf.integer(".OCLUSION", 0);
    csf.string(".NOMBRE", "zone");
    csf.real(".HEIGHT", 100);
    csf.integer(".REVERB", 0);
    csf.integer(".LIMITREVERB", 0);
    csf.container(".PUNTOS", 3);
    csf.container("", 1, 2);
    csf.position(0, 0, 0);
    csf.container("", 1, 2);
    csf.position(10, 0, 0);
    csf.container("", 1, 2);
    csf.position(0, 0, 10);
    if (with_unknown) csf.unknown();
    return csf.bytes();
}


void test_fuzzy_matcher() {
    using rwsman::fuzzy_match;
    CHECK(fuzzy_match("", "anything") && fuzzy_match("", "anything")->score == 0);
    CHECK(!fuzzy_match("xyz", "Guard_01"));
    CHECK(!fuzzy_match("guardd", "guard"));
    const auto camel = fuzzy_match("gs", "GuardSpawn");
    CHECK(camel && camel->positions.size() == 2 && camel->positions[0] == 0 &&
          camel->positions[1] == 5);
    const auto separators = fuzzy_match("g1", "Guard_01");
    CHECK(separators && separators->positions[1] == 7);
    // Ranking: exact > prefix > word-start > scattered.
    const auto exact = fuzzy_match("guard", "Guard");
    const auto prefix = fuzzy_match("guard", "Guard_Post");
    const auto inner = fuzzy_match("guard", "Big_Guard");
    const auto scattered = fuzzy_match("guard", "Great_Umbrella_Array_Road_Dock");
    CHECK(exact && prefix && inner && scattered);
    CHECK(exact->score > prefix->score && prefix->score > inner->score &&
          inner->score > scattered->score);
    // Consecutive matches beat the same letters spread out.
    const auto tight = fuzzy_match("lgt", "lgt");
    const auto loose = fuzzy_match("lgt", "l_g_t");
    CHECK(tight && loose && tight->score > loose->score);
    // The matcher is case-insensitive and byte-position based.
    CHECK(fuzzy_match("GUARD", "guard_01") && fuzzy_match("guard", "GUARD_01"));
}

void test_command_registry() {
    using namespace rwsman;
    const auto shortcut = parse_shortcut("ctrl+shift+p");
    CHECK(shortcut && shortcut->ctrl && shortcut->shift && !shortcut->alt && shortcut->key == "P");
    CHECK(format_shortcut(*shortcut) == "Ctrl+Shift+P");
    CHECK(parse_shortcut("Alt+Left")->key == "Left" && parse_shortcut("F12")->key == "F12");
    CHECK(parse_shortcut("Ctrl++") && parse_shortcut("Ctrl++")->key == "+");
    CHECK(!parse_shortcut("") && !parse_shortcut("Ctrl+") && !parse_shortcut("Ctrl+Shift") &&
          !parse_shortcut("A+B"));

    CommandRegistry registry;
    int ran = 0;
    bool enabled = true;
    const auto add = [&](const char* id, const char* keys,
                         ShortcutScope scope = ShortcutScope::global) {
        Command command;
        command.id = id;
        command.label = id;
        command.category = "Test";
        command.shortcut = keys;
        command.scope = scope;
        command.enabled = [&] { return enabled; };
        command.run = [&] { ++ran; };
        return registry.add(std::move(command));
    };
    CHECK(add("a", "Ctrl+P"));
    CHECK(!add("a", "Ctrl+Q")); // Duplicate id.
    CHECK(add("b", "Ctrl+Shift+P"));
    CHECK(add("c", "1", ShortcutScope::viewport));
    CHECK(add("d", "1"));         // Same key, different scope: no conflict.
    CHECK(registry.conflicts().empty());
    CHECK(add("e", "ctrl+p")); // Same binding as "a" once normalized.
    CHECK(add("f", "Ctrl+"));  // Malformed binding.
    const auto conflicts = registry.conflicts();
    CHECK(conflicts.size() == 2);
    CHECK(conflicts[0].first == "a" && conflicts[0].second == "e" &&
          conflicts[0].shortcut == "Ctrl+P");
    CHECK(conflicts[1].first == "f" && conflicts[1].second.empty());

    CHECK(registry.run("b") && ran == 1);
    enabled = false;
    CHECK(!registry.run("b") && ran == 1 && !registry.is_enabled("b"));
    CHECK(!registry.run("missing") && registry.find("missing") == nullptr);
    CHECK(registry.by_category().size() == 1 && registry.by_category()[0].second.size() == 6);
}

void test_navigation_history() {
    using namespace rwsman;
    NavigationHistory history(4);
    CHECK(history.current() == nullptr && !history.can_back() && !history.back());
    const auto entry = [](int workspace, std::uint64_t offset) {
        HistoryEntry value;
        value.workspace = workspace;
        value.selection = SelectionRef::chunk(offset);
        return value;
    };
    history.push(entry(0, 1));
    history.push(entry(0, 2));
    history.push(entry(0, 3));
    CHECK(history.size() == 3 && history.current()->selection.a == 3);
    CHECK(history.back()->selection.a == 2 && history.back()->selection.a == 1 && !history.back());
    CHECK(history.forward()->selection.a == 2 && history.can_forward());
    // Same workspace and selection refreshes the camera instead of adding an entry.
    auto refreshed = entry(0, 2);
    refreshed.camera.yaw = 1.5F;
    refreshed.camera.valid = true;
    history.push(refreshed);
    CHECK(history.size() == 3 && history.current()->camera.yaw == 1.5F);
    // A new destination discards the forward stack.
    history.push(entry(1, 9));
    CHECK(history.size() == 3 && !history.can_forward() && history.current()->workspace == 1);
    // The stack is bounded; the oldest entries fall off.
    history.push(entry(1, 10));
    history.push(entry(1, 11));
    history.push(entry(1, 12));
    CHECK(history.size() == 4 && history.back() && history.back() && history.back());
    CHECK(history.current()->selection.a == 9 && !history.can_back());
    // Selections of different kinds are different destinations.
    CHECK(SelectionRef::chunk(4) != SelectionRef::scene_instance(4));
    CHECK(SelectionRef::program_script(1, 2) == SelectionRef::program_script(1, 2));
}

void test_settings_model() {
    using namespace rwsman;
    Settings settings;
    settings.resource_root = "/games/CSF unpacked/with\ttab";
    settings.game_root = "/games/Commandos Strike Force";
    settings.projects_root = "/home/user/csf projects";
    settings.ui_scale = 1.25F;
    settings.theme = "dark";
    settings.workspace = "mission";
    settings.show_bottom_dock = true;
    settings.show_hud = false;
    settings.invert_y = true;
    settings.move_speed = 2.5F;
    settings.default_view_style = 2;
    settings.idle_redraw = false;
    settings.fps_limit = 60;
    settings.background_fps_limit = 0;
    settings.show_frame_stats = true;
    settings.export_policy = ExportPolicy::confirm_overwrite;
    settings.overlays.labels = OverlayOptions::Labels::nearby;
    settings.overlays.headings = OverlayOptions::Detail::selected;
    settings.overlays.details = OverlayOptions::Detail::all;
    settings.overlays.occluded_opacity = 0.5F;
    settings.overlays.fade_distance = 0.0F;
    settings.overlays.merge_pixels = 9.0F;
    settings.overlays.icon_limit = 40;
    settings.overlays.show_minimap = true;
    settings.overlays.dim_filtered = false;
    settings.overlays.hidden_layers = {"light", "nav_link"};
    settings.overlays.presets = {{"Mine\ttab", {"actor", "area"}}, {"Empty", {}}};
    settings.add_recent_file("/maps/FR01.scn", true);
    settings.add_recent_file("/models/back\\slash\nnewline.rpc", false);
    settings.add_recent_file("/maps/FR01.scn", true); // Moves to the front, no duplicate.
    settings.add_recent_pairing("/a/main.rws", "/a/main_col.rws");
    CameraBookmark bookmark;
    bookmark.slot = 3;
    bookmark.camera = {0.5F,  -0.25F, 100.0F, 2.0F, 1.0F, 2.0F, {3.0F, 4.0F, 5.0F},
                       {6.0F, 7.0F,   8.0F},   9.0F, 1,    true};
    settings.set_bookmark("sig:1234", bookmark);
    bookmark.slot = 1;
    settings.set_bookmark("sig:1234", bookmark);
    CHECK(settings.recent_files.size() == 2 && settings.recent_files[0].path == "/maps/FR01.scn");
    CHECK(settings.bookmark("sig:1234", 3) && !settings.bookmark("sig:1234", 2) &&
          !settings.bookmark("other", 1));

    const auto text = serialize_settings(settings);
    const auto loaded = parse_settings(text);
    CHECK(loaded.warnings.empty() && loaded.existed);
    CHECK(loaded.settings == settings);
    CHECK(serialize_settings(loaded.settings) == text);

    // A settings file on disk survives a save/load cycle and never leaves a temp file.
    const auto directory = std::filesystem::temp_directory_path() / "rwsman-settings-test";
    std::filesystem::remove_all(directory);
    const auto file = directory / "nested" / "settings.ini";
    CHECK(save_settings(file, settings).empty());
    CHECK(!std::filesystem::exists(file.string() + ".tmp"));
    CHECK(load_settings(file).settings == settings);
    // Missing file: defaults, not an error.
    const auto missing = load_settings(directory / "none.ini");
    CHECK(!missing.existed && missing.warnings.empty() && missing.settings == Settings{});

    // Corrupted content falls back to defaults field by field and reports warnings.
    const auto corrupt = parse_settings(
        "version = 1\nui_scale = banana\nshow_hud = maybe\nthis line has no equals\n"
        "recent_file = mission\nbookmark = sig\t99\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\n"
        "future_key = whatever\nmove_speed = 3\n");
    CHECK(corrupt.warnings.size() == 5);
    CHECK(corrupt.settings.ui_scale == 1.0F && corrupt.settings.show_hud &&
          corrupt.settings.move_speed == 3.0F && corrupt.settings.recent_files.empty() &&
          corrupt.settings.bookmarks.empty());
    const std::string binary("\x01\x02\0garbage", 10);
    const auto garbage = parse_settings(binary);
    CHECK(garbage.settings == Settings{} && garbage.warnings.size() == 1);
    // Out-of-range values are clamped instead of trusted.
    const auto clamped = parse_settings("ui_scale = 40\nmove_speed = -2\n"
                                        "overlay_occluded_opacity = 7\noverlay_merge_pixels = -3\n"
                                        "overlay_hidden = b\ta\tb\n");
    CHECK(clamped.settings.ui_scale == 2.0F && clamped.settings.move_speed == 1.0F);
    const auto clamped_fps = parse_settings("fps_limit = 2\nbackground_fps_limit = -4\n");
    CHECK(clamped_fps.settings.fps_limit == 5 && clamped_fps.settings.background_fps_limit == 0);
    CHECK(clamped.settings.overlays.occluded_opacity == 1.0F &&
          clamped.settings.overlays.merge_pixels == 0.0F &&
          (clamped.settings.overlays.hidden_layers == std::vector<std::string>{"a", "b"}));
    const auto bad_overlay = parse_settings("overlay_labels = sometimes\noverlay_preset = \n");
    CHECK(bad_overlay.warnings.size() == 2 &&
          bad_overlay.settings.overlays.labels == OverlayOptions::Labels::hovered);
    // A newer file version still yields the values this version understands.
    const auto newer = parse_settings("version = 99\ntheme = light\n");
    CHECK(newer.settings.theme == "light" && newer.warnings.size() == 1);
    // The export policy defaults to never replacing files.
    CHECK(Settings{}.export_policy == ExportPolicy::new_files_only);
    std::filesystem::remove_all(directory);

#ifndef _WIN32
    const auto xdg = config_directory([](const char* name) -> const char* {
        return std::string_view(name) == "XDG_CONFIG_HOME" ? "/cfg" : nullptr;
    });
    CHECK(xdg == std::filesystem::path("/cfg/csf-rws-tools"));
    const auto home = config_directory([](const char* name) -> const char* {
        return std::string_view(name) == "HOME" ? "/home/u" : "";
    });
    CHECK(home == std::filesystem::path("/home/u/.config/csf-rws-tools"));
    CHECK(config_directory([](const char*) -> const char* { return nullptr; }).empty());
#endif
}

void test_log_buffer() {
    using namespace rwsman;
    LogBuffer log(3);
    const auto start = log.revision();
    log.push(LogLevel::info, "one");
    log.push(LogLevel::warn, "two");
    log.push(LogLevel::error, "three");
    log.push(LogLevel::ok, "four");
    CHECK(log.size() == 3 && log.snapshot().front().message == "two");
    CHECK(log.latest()->message == "four" && log.count(LogLevel::info) == 0 &&
          log.count(LogLevel::error) == 1);
    CHECK(log.revision() == start + 4);
    const auto text = log.to_text();
    CHECK(text.find("warn  two\n") != std::string::npos && text.find("one") == std::string::npos);
    log.clear();
    CHECK(log.size() == 0 && !log.latest() && log.revision() == start + 5);
}

void test_search_index() {
    using namespace rwsman;
    SearchIndex index;
    const auto add = [&](SymbolKind kind, const char* label, const char* detail, std::uint64_t id,
                         std::uint64_t offset, std::uint64_t end = 0) {
        SearchEntry entry;
        entry.kind = kind;
        entry.label = label;
        entry.detail = detail;
        entry.target = SelectionRef::mission_entry(static_cast<std::uint32_t>(id));
        entry.offset = offset;
        if (end) entry.end_offset = end;
        entry.id = id;
        index.add(std::move(entry));
    };
    add(SymbolKind::actor, "Guard_01", "class 55, ID 1", 17, 0x2C79D6);
    add(SymbolKind::actor, "Guard_02", "class 55, ID 2", 18, 0x2C7A00);
    add(SymbolKind::script, "GuardPatrol", "(root) · ID 4", 40, 0x3000);
    add(SymbolKind::chunk, "Clump", "0x1000", 0, 0x1000, 0x5000);
    add(SymbolKind::chunk, "Geometry", "0x1100", 0, 0x1100, 0x1800);
    CHECK(index.entries().size() == 5 && index.size() == 5);
    {
        // A rebuild to the same size still changes the generation, so cached entry
        // pointers are known to be stale.
        SearchIndex rebuilt;
        SearchEntry entry;
        entry.label = "one";
        rebuilt.add(entry);
        const auto before = rebuilt.generation();
        rebuilt.clear();
        rebuilt.add(entry);
        CHECK(rebuilt.size() == 1 && rebuilt.generation() != before);
    }

    auto results = index.query("guard", 10);
    CHECK(results.size() == 3);
    CHECK(results[0].entry->label == "Guard_01" || results[0].entry->label == "Guard_02");
    CHECK(!results[0].label_positions.empty());
    // Word-start abbreviations still find the script.
    results = index.query("gp", 10);
    CHECK(!results.empty() && results[0].entry->label == "GuardPatrol");
    // "#17" is an entry lookup and returns nothing else.
    results = index.query("#17", 10);
    CHECK(results.size() == 1 && results[0].entry->label == "Guard_01");
    // A bare number ranks the exact id first.
    results = index.query("17", 10);
    CHECK(!results.empty() && results[0].entry->label == "Guard_01");
    // Offsets: exact first, then the smallest containing range.
    results = index.query("0x1100", 10);
    CHECK(results.size() == 2 && results[0].entry->label == "Geometry" &&
          results[1].entry->label == "Clump");
    results = index.query("0x1200", 10);
    CHECK(results.size() == 2 && results[0].entry->label == "Geometry");
    CHECK(index.query("0x9999", 10).empty() && index.query("   ", 10).empty() &&
          index.query("zzzz", 10).empty());
    CHECK(index.query("guard", 1).size() == 1);
    // Explorer filters are substring matches on precomputed lowercase text.
    CHECK(index.contains(0, "guard_01") && index.contains(0, "class 55") &&
          !index.contains(0, "patrol") && index.contains(0, ""));
    CHECK(parse_hex_offset("0x2C79D6") == 0x2C79D6 && !parse_hex_offset("0x") &&
          !parse_hex_offset("2C79D6") && !parse_hex_offset("0xZZ"));
}

void test_mission_index() {
    using namespace rwsman;
    const auto document = csf::Document::from_bytes(make_typed_scene());
    const auto scene = csf::MissionScene::project(document);
    SearchIndex index;
    MissionIndexInputs inputs;
    inputs.scene = &scene;
    index_mission(index, inputs);
    CHECK(index.size() >= scene.actors().size() + scene.dummies().size() + scene.lights().size() +
                              scene.effects().size() + scene.areas().size());
    const auto results = index.query("sparks", 5);
    CHECK(!results.empty() && results[0].entry->kind == SymbolKind::effect);
    const auto* entry = results[0].entry;
    CHECK(entry->target.kind == SelectionRef::Kind::mission_entry && entry->offset &&
          entry->id && entry->target.a == *entry->id);
    // Both actors share a name; both stay reachable and distinct by entry index.
    const auto actors = index.query("duplicate", 5);
    CHECK(actors.size() >= 2 && actors[0].entry->kind == SymbolKind::actor &&
          actors[1].entry->kind == SymbolKind::actor &&
          actors[0].entry->id != actors[1].entry->id);
}


void test_mission_discovery() {
    using namespace rwsman;
    const auto root = std::filesystem::temp_directory_path() / "rwsman-discovery-test";
    std::filesystem::remove_all(root);
    const auto touch = [](const std::filesystem::path& path) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << "x";
    };
    touch(root / "Ambush" / "Maps" / "ST08" / "Ambush.scn");
    touch(root / "Convoy" / "Maps" / "FR03" / "Convoy.SCN");
    touch(root / "Convoy" / "Maps" / "FR03" / "readme.txt");
    touch(root / "Convoy" / "Maps" / "FR03" / "nested" / "Deep.scn"); // Not scanned.
    touch(root / "Other" / "Data" / "Hidden.scn");                     // Not under Maps.
    touch(root / "Maps" / "TEST" / "Direct.scn");                      // The root is a package itself.
    const auto found = discover_missions(root);
    CHECK(found.size() == 3);
    // Sorted by package, then map, then name, ignoring case.
    CHECK(found[0].package == "Ambush" && found[0].map == "ST08" && found[0].name == "Ambush");
    CHECK(found[1].package == "Convoy" && found[1].map == "FR03" && found[1].name == "Convoy");
    CHECK(found[2].package == "rwsman-discovery-test" && found[2].name == "Direct");
    CHECK(discover_missions(root, 1).size() == 1);
    CHECK(discover_missions(root / "missing").empty() && discover_missions({}).empty());
    std::filesystem::remove_all(root);
}

void test_diagnostic_table() {
    using namespace rwsman;
    std::vector<DiagnosticRow> rows;
    const auto add = [&](DiagnosticSeverity severity, const char* source, const char* file, std::optional<std::uint32_t> entry,
                         std::optional<std::uint64_t> offset, const char* message) {
        DiagnosticRow row;
        row.severity = severity;
        row.source = source;
        row.file = file;
        row.entry = entry;
        row.offset = offset;
        row.message = message;
        rows.push_back(row);
    };
    add(DiagnosticSeverity::warning, "RWS", "ST08.rws", std::nullopt, 0x3EE5C, "Chunk payload is truncated");
    add(DiagnosticSeverity::error, "CSFFBS", "Ambush.scn", 12, 0x100, "Unexpected entry type");
    add(DiagnosticSeverity::note, "Resources", "Armas.bdd", 5, 0x50, "Reference spelling differs");
    add(DiagnosticSeverity::error, "Mission", "Ambush.scn", 3, 0x40, "Actor has no .POS");
    DiagnosticFilter filter;
    // Severity order: errors first when ascending, and ties keep the input order.
    auto order = select_diagnostics(rows, filter, DiagnosticColumn::severity, true);
    CHECK((order == std::vector<std::size_t>{1, 3, 0, 2}));
    order = select_diagnostics(rows, filter, DiagnosticColumn::offset, true);
    CHECK((order == std::vector<std::size_t>{3, 2, 1, 0}));
    order = select_diagnostics(rows, filter, DiagnosticColumn::offset, false);
    CHECK((order == std::vector<std::size_t>{0, 1, 2, 3}));
    order = select_diagnostics(rows, filter, DiagnosticColumn::entry, true);
    CHECK(order.front() == 3 && order.back() == 0); // Rows without an entry sort last.
    filter.errors = false;
    CHECK(select_diagnostics(rows, filter, DiagnosticColumn::message, true).size() == 2);
    filter = {};
    filter.text = "ambush";
    CHECK(select_diagnostics(rows, filter, DiagnosticColumn::file, true).size() == 2);
    filter = {};
    filter.source = "RWS";
    CHECK(select_diagnostics(rows, filter, DiagnosticColumn::file, true).size() == 1);
    // Collecting from a projected mission reports the scene's own diagnostics with a target.
    const auto document = csf::Document::from_bytes(make_typed_scene(true));
    const auto scene = csf::MissionScene::project(document);
    DiagnosticInputs inputs;
    inputs.scene = &scene;
    inputs.scene_document = &document;
    const auto collected = collect_diagnostics(inputs);
    CHECK(!collected.empty());
    CHECK(std::ranges::any_of(collected, [](const DiagnosticRow& row) { return row.source == "CSFFBS"; }));
    CHECK(std::ranges::any_of(collected, [](const DiagnosticRow& row) {
        return row.source == "Mission" && row.target.kind == SelectionRef::Kind::mission_entry;
    }));
}

void test_search_index_grouping() {
    using namespace rwsman;
    SearchIndex index;
    SearchEntry group;
    group.kind = SymbolKind::navigation_group;
    group.label = "Patrol";
    index.add(group);
    SearchEntry point;
    point.kind = SymbolKind::navigation_point;
    point.label = "Stop 1";
    point.parent = 0;
    index.add(point);
    SearchEntry script;
    script.kind = SymbolKind::script;
    script.label = "Init";
    script.group = "Setup/Alarms";
    index.add(script);
    CHECK(index.indices_of(SymbolKind::navigation_point).size() == 1 &&
          index.entries()[index.indices_of(SymbolKind::navigation_point)[0]].parent == 0);
    CHECK(index.indices_of(SymbolKind::script).size() == 1 && index.indices_of(SymbolKind::actor).empty());
    index.clear();
    CHECK(index.size() == 0 && index.indices_of(SymbolKind::script).empty());
}

std::vector<std::byte> compile_source(const std::string_view text) {
    csf::Tree tree;
    const std::string prefix("CSFFBS\0\x7F", 8);
    std::ranges::transform(prefix, tree.prefix.begin(), [](const char c) { return static_cast<std::byte>(c); });
    tree.version = 1;
    tree.roots = csf::parse_source_text(text);
    return tree.serialize();
}

void test_csf_tree_and_source_text() {
    // The canonical writer: labelled scalars are an identifier entry (value
    // 0xFFFFFFFF) plus a value entry, containers carry their label inline, and
    // each element's first entry links to its next sibling.
    csf::Tree tree;
    std::ranges::transform(std::string("CSFFBS\0\x7F", 8), tree.prefix.begin(),
                           [](const char c) { return static_cast<std::byte>(c); });
    tree.version = 3;
    auto root = csf::TreeNode::array(std::nullopt);
    root.children.push_back(csf::TreeNode::integer(".ID", 7));
    auto position = csf::TreeNode::group(".POS");
    position.children = {csf::TreeNode::real(std::nullopt, 1.5F), csf::TreeNode::real(std::nullopt, -0.0F)};
    root.children.push_back(position);
    root.children.push_back(csf::TreeNode::string(".NOMBRE", "A"));
    root.children.push_back(csf::TreeNode::string(".OTRO", "A"));
    root.children.push_back(csf::TreeNode::string(".VACIO", ""));
    tree.roots.push_back(root);
    const auto bytes = tree.serialize();
    const auto document = csf::Document::from_bytes(bytes);
    CHECK(document.state() == csf::ParseState::exact);
    CHECK(document.header().version == 3);
    const auto& entries = document.entries();
    CHECK(entries.size() == 12);
    CHECK(entries[0].raw_value_or_size == 5 && entries[0].raw_next_entry == 0);
    CHECK(entries[1].kind() == csf::ValueKind::identifier && entries[1].raw_value_or_size == 0xFFFFFFFFU);
    CHECK(entries[1].raw_next_entry == 3 && entries[2].raw_next_entry == 0);
    CHECK(entries[3].raw_identifier_index == 1 && entries[3].raw_next_entry == 6);
    CHECK(entries[4].raw_next_entry == 5 && entries[5].raw_next_entry == 0);
    // Strings are interned in first-use order; the empty string is zero bytes.
    CHECK(document.strings().size() == 2 && document.strings()[1].bytes.empty());
    CHECK(document.identifiers().size() == 5);
    CHECK(csf::Tree::from_document(document) == tree);
    CHECK(csf::Tree::from_document(document).serialize() == bytes);

    // Source text reproduces the exact tree, including raw real bits.
    auto special = root;
    special.children.push_back(csf::TreeNode::real(".NAN", 0));
    special.children.back().raw = 0x7FC00001U;
    special.children.push_back(csf::TreeNode::string("DA\xD1O", "caf\xE9 \"x\"\n"));
    special.children.push_back(csf::TreeNode::string(".RAW", ""));
    special.children.back().text = std::string("abc", 3); // no final NUL
    special.children.push_back(csf::TreeNode::integer("weird label", -3));
    const auto printed = csf::to_source_text(special);
    CHECK(printed.find("DA\xC3\x91O:") != std::string::npos);
    CHECK(printed.find("\"abc\"~") != std::string::npos);
    CHECK(printed.find("%7fc00001") != std::string::npos);
    CHECK(printed.find("-0.0") != std::string::npos);
    CHECK(csf::parse_source_value(printed) == special);

    // Instruction blocks are indented by control flow and parse back exactly.
    const auto script = csf::parse_source_value(R"([
      .ID 22
      .ACCIONES {
        PAUSE (RANDOM (NUMERO 0.0) (NUMERO 2.0))  # comment
        WHILE (BOOL TRUE)
          PLAY_ANMBDD (THIS) (ANM_BDD 1937)
          IF (CMP (VAR 8388608)
                  (NUMERO 1.0))
            PAUSE (NUMERO 4.0)
          ELSE
          ENDIF
        WEND
      }
    ])");
    const auto* actions = script.child(".ACCIONES");
    CHECK(actions && actions->children.size() == 8);
    CHECK(actions->children[0].children.size() == 2);
    CHECK(actions->children[3].children[1].children.size() == 3);
    CHECK(actions->children[2].children[2].children[1].as_int() == 1937);
    CHECK(actions->children[0].children[1].children[1].children[1].as_real() == 0.0F);
    const auto block = csf::to_source_text(script);
    CHECK(block.find("    WHILE (BOOL TRUE)\n      PLAY_ANMBDD (THIS) (ANM_BDD 1937)") != std::string::npos);
    CHECK(block.find("\n      ELSE\n") != std::string::npos);
    CHECK(csf::parse_source_value(block) == script);
    CHECK(csf::check_script_against_signatures(script).empty() ||
          !csf::check_script_against_signatures(script).empty());
    const auto unknown = csf::check_script_against_signatures(
        csf::parse_source_value("[ .ACCIONES {\n NOT_A_REAL_OPCODE 1\n} ]"));
    CHECK(unknown.size() == 1 && unknown.front().opcode == "NOT_A_REAL_OPCODE");

    // Errors carry positions.
    try {
        (void)csf::parse_source_value("[\n  .ID 1\n  .POS (1.0 2.0\n");
        CHECK(false);
    } catch (const csf::SourceTextError& error) {
        CHECK(error.line() == 3);
    }
    try {
        (void)csf::parse_source_value("[ .ID 99999999999 ]");
        CHECK(false);
    } catch (const csf::SourceTextError&) {
    }
    CHECK(csf::utf8_to_windows_1252("\xE2\x82\xAC") == std::string("\x80"));
    CHECK(!csf::utf8_to_windows_1252("\xE4\xB8\xAD"));
    CHECK(csf::windows_1252_to_utf8("\x80\xF1") == "\xE2\x82\xAC\xC3\xB1");
    CHECK(std::ranges::is_sorted(csf::animation_slot_names()));
    CHECK(std::ranges::binary_search(csf::animation_slot_names(), std::string_view("DISTRAIDO_IDLE_ARMA1")));
}

void write_text_file(const std::filesystem::path& path, const std::string_view text) {
    std::vector<std::byte> bytes(text.size());
    std::ranges::transform(text, bytes.begin(), [](const char c) { return static_cast<std::byte>(c); });
    write_bytes(path, bytes);
}

void test_actor_look() {
    const auto root = std::filesystem::temp_directory_path() / "rws-man-actor-look-tests";
    std::filesystem::remove_all(root);
    const auto package = root / "Mission";
    const auto map = package / "Maps" / "M1";
    write_bytes(map / "M1.scn", compile_source(R"([
  .VERSION 17
  .BICHOS (
    [ .NOMBRE A .ID 1 .CLASSID 10 .POS (0.0 0.0 0.0) .ANGULO 0.0 ]
    [ .NOMBRE B .ID 5 .CLASSID 10 .POS (100.0 0.0 50.0) .ANGULO 90.0 ]
  )
])"));
    write_bytes(package / "BDD" / "Objetos.bdd", compile_source(R"([ .VERSION 10 .LISTADATOS (
  [ .ID 10 .NOMBRE Soldier .TIPO ALEMAN .HOMBRE 1 .MODELO "Models\\Char\\AlSt.dff"
    .LOD1_NOMBRE "Models\\Char\\AlStL1.dff" .LOD1_DIST 2500.0 .LOD2_DIST 0.0 .COMPOR SOLDADO
    .BBOX [ .INF (-60.0 -0.5 -19.0) .SUP (60.0 180.0 15.0) ]
    .PHYSIC [ .TIPO FILE .MODEL_FILE "Models\\ragdoll.rws" .MASS 1.0 .BOUNCE 0.1 .SLIDE 2.0 ] ]
  [ .ID 29 .NOMBRE Bidon .TIPO DECORATIVO .HOMBRE 1 .MODELO "Models\\Deco\\bidon.dff"
    .LOD1_DIST 0.0 .LOD2_DIST 0.0 .COMPOR NINGUNO
    .BBOX [ .INF (-34.0 -1.5 -34.0) .SUP (34.0 98.0 34.0) ]
    .PHYSIC [ .TIPO FILE .MODEL_FILE "Models\\Deco\\bidon.rws" .MASS 1.0 .BOUNCE 0.5 .SLIDE 0.7 ] ]
) ])"));
    std::vector<std::byte> phd;
    for (const auto b : {0xFD, 0xFC, 0xFC, 0xFC}) phd.push_back(static_cast<std::byte>(b));
    append_u32(phd, 1);
    append_u32(phd, 1);
    append_u32(phd, 3);
    append_csf_string(phd, "Models\\ragdoll.rws", false);
    append_csf_string(phd, "Models\\Char\\AlSt.dff", false);
    for (const float value : {-60.0F, -0.5F, -19.0F, 60.0F, 180.0F, 15.0F, 1.0F, 0.1F, 2.0F}) append_f32(phd, value);
    write_bytes(map / "M1.phd", phd);

    auto editor = csf::MissionEditor::open(map / "M1.scn", package);
    const auto objects_file = *editor.file_of_kind(csf::MissionFileKind::objects);
    const auto physics_file = *editor.file_of_kind(csf::MissionFileKind::physics_index);
    const auto objects_before = editor.files()[objects_file].bytes();
    const auto class_of = [&](const std::int32_t actor) {
        for (const auto& value : editor.scene().actors())
            if (value.id == actor) return value.class_id.value_or(0);
        return 0;
    };

    // The actor moves to a copy of its class with the barrel's look.
    auto result = editor.set_actor_look(5, 29);
    CHECK(result.applied && class_of(5) == 500 && class_of(1) == 10);
    CHECK(std::ranges::any_of(result.warnings, [](const std::string& w) { return w.find("skeleton") != std::string::npos; }));
    {
        const auto tree = csf::Tree::from_document(editor.document(objects_file));
        const auto& records = tree.roots[0].child(".LISTADATOS")->children;
        CHECK(records.size() == 3 && records.back().child(".ID")->as_int() == 500);
        const auto& copy = records.back();
        CHECK(copy.child(".MODELO")->as_string() == "Models\\Deco\\bidon.dff");
        CHECK(copy.child(".NOMBRE")->as_string() == "Soldier (look: bidon)");
        CHECK(!copy.child(".LOD1_NOMBRE") && copy.child(".LOD1_DIST")->as_real() == 0.0F);
        CHECK(copy.child(".COMPOR")->as_string() == "SOLDADO");
        CHECK(copy.child(".BBOX")->child(".SUP")->children[1].as_real() == 98.0F);
        CHECK(*copy.child(".PHYSIC") == *records.front().child(".PHYSIC"));
    }
    {
        // The physics descriptor gains the soldier's entry with the new model and box.
        const auto& bytes = editor.files()[physics_file].raw;
        CHECK(bytes.size() == phd.size() * 2 - 12 - 20 + 21);
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        CHECK(text.find("Models\\Deco\\bidon.dff") != std::string::npos);
        CHECK(text.rfind("Models\\ragdoll.rws") > text.find("Models\\Char\\AlSt.dff"));
    }
    const auto physics_with_copy = editor.files()[physics_file].raw;

    // The same look reuses the copy; changing back reuses the original class.
    CHECK(editor.set_actor_look(1, 29).applied && class_of(1) == 500);
    CHECK(editor.files()[physics_file].raw == physics_with_copy);
    CHECK(!editor.set_actor_look(1, 29).applied);
    CHECK(editor.set_actor_look(5, 10).applied && class_of(5) == 10);
    CHECK(editor.set_actor_look(1, 10).applied && class_of(1) == 10);
    // The unused copy and its physics entry are gone again.
    CHECK(editor.files()[objects_file].bytes() == objects_before);
    CHECK(editor.files()[physics_file].raw == phd);
    CHECK(editor.modified_files().empty());
    CHECK(!editor.set_actor_look(1, 404).applied);

    CHECK(editor.undo() && class_of(1) == 500);
    CHECK(editor.files()[physics_file].raw == physics_with_copy);
    std::filesystem::remove_all(root);
}

void test_mission_editor() {
    const auto root = std::filesystem::temp_directory_path() / "rws-man-mission-editor-tests";
    std::filesystem::remove_all(root);
    const auto package = root / "Mission";
    const auto donor = root / "Donor";
    const auto map = package / "Maps" / "M1";
    const std::string scene_text = R"([
  .VERSION 17
  .PLAYER 1
  .MUNDOVIS [ .RWS "Maps\\M1\\world.rws" .FOGDISTANCE 2000.0 .INICIO_COMMANDO 0 .INICIO_SNIPER 1 .INICIO_SPY 0 ]
  .BICHOS (
    [ .NOMBRE HERO .ID 1 .CLASSID 10 .POS (0.0 0.0 0.0) .ANGULO 0.0 .ANGULO_X 0.0 .COLISION 1 .FLAGS 0 .SEGUNDA_EXPLOSION 0 .CELDA [ .GRUPO 1 .PUNTO 1 ] ]
    [ .NOMBRE GUARD .ID 5 .CLASSID 10 .POS (100.0 0.0 50.0) .ANGULO 90.0 .ANGULO_X 0.0 .COLISION 1 .FLAGS 0 .SEGUNDA_EXPLOSION 0 .SCRIPT (7) .CELDA [ .GRUPO 1 .PUNTO 2 ] ]
  )
  .EFECTOS ( [ .ID 3 .NOMBRE fx .CLASSID 88 .DUMMY 2 .PRIORITY -1 .SHARE_GROUP -1 ] )
  .PUNTUACION_MAXIMA 2000
  .PUNTUACION_MINIMA 1000
  .MALLA_NAVEGACION [
    .GRUPOS (
      [ .ID 1 .NOMBRE G .TIPO 0 .PUNTOS (
          [ .ID 1 .NOMBRE "" .POS (0.0 0.0 0.0) .ROT 0.0 .ROT_X 0.0 ]
          [ .ID 2 .NOMBRE "" .POS (100.0 0.0 50.0) .ROT 1.5707964 .ROT_X 0.0 ]
          [ .ID 3 .NOMBRE "" .POS (200.0 0.0 0.0) .ROT 0.0 .ROT_X 0.0 ] )
        .CONEXIONES ( [ .PUNTO_ORI 2 .PUNTO_DST 3 ] ) ]
      [ .ID 2 .NOMBRE H .TIPO 0 .PUNTOS ( [ .ID 1 .NOMBRE "" .POS (500.0 0.0 0.0) .ROT 0.0 .ROT_X 0.0 ] )
        .CONEXIONES ( ) ]
    )
    .CONEXIONES ( )
  ]
  .MALLA_DUMMIES [ .DUMMIES ( [ .ID 2 .NOMBRE "" .POS (1.0 2.0 3.0) .ROT 0.0 .ROT_X 0.0 ] )
    .CARPETAS [ .RAIZ ( [ .NOMBRE "" .CARPETAS ( [ .NOMBRE A .ELEMENTOS (2) ] ) ] ) ] ]
  .MALLA_AREAS [ .AREAS ( [ .ID 1 .FLAGS 1 .OCLUSION 1 .NOMBRE Z .HEIGHT 200.0 .REVERB 0 .LIMITREVERB 0
    .PUNTOS ( [ .POS (0.0 0.0 0.0) ] [ .POS (10.0 0.0 0.0) ] [ .POS (10.0 0.0 10.0) ] ) ] ) ]
  .MALLA_LUCES [ .LIGHTS ( [ .ID 9 .NOMBRE "" .POS (5.0 5.0 5.0) .COLOR 255 .MODULATE 0 .RADIO 100.0 ] )
    .CARPETAS [ .RAIZ ( [ .NOMBRE "" .ELEMENTOS (9) ] ) ] ]
])";
    const std::string program_text = R"([
  .RECURSOS [ .ANIMACIONES (100) .CLASSID ( ) ]
  .VARIABLES ( )
  .SCRIPTS (
    [ .ID 7 .NOMBRE GUARD_LOOP .CARPETA "" .FLAGS [ .TRIGGER 0 .ENABLED 1 .VALIDO 1 ] .EVENTOS ( (START_GAME) )
      .ACCIONES {
        WHILE (BOOL TRUE)
          PLAY_ANMBDD (THIS) (ANM_BDD 100)
        WEND
      } ]
    [ .ID 8 .NOMBRE TRIG .CARPETA "" .FLAGS [ .TRIGGER 1 .ENABLED 1 .VALIDO 1 ] .EVENTOS ( (INIT) )
      .ACCIONES {
        SET_POSICION (BICHO 5) (DUMMY 2)
      } ]
  )
  .POOL ( )
])";
    const auto objects = [](const bool with_imported) {
        std::string text = R"([ .VERSION 10 .LISTADATOS (
  [ .ID 10 .NOMBRE Soldier .TIPO ALEMAN .COMPOR SOLDADO .HOMBRE 1 .MODELO "Models\\Char\\A.dff" ]
  [ .ID 11 .NOMBRE Other .TIPO ALEMAN .COMPOR SOLDADO .HOMBRE 1 .MODELO "Models\\Char\\A.dff" ]
  [ .ID 12 .NOMBRE Civil .TIPO NEUTRO .COMPOR CIVIL .HOMBRE 1 .MODELO "Models\\Char\\A.dff" ])";
        if (with_imported)
            text += R"(
  [ .ID 40 .NOMBRE Tank .TIPO TANQUE .COMPOR VEHICULO .HOMBRE 0 .MODELO "Models\\Vehi\\Tank.dff"
    .MODELO_COLISION [ .CO_MODEL_EX "Models\\Vehi\\Tank.cmo" ] .ARMAS (70)
    .ANIMACIONES ( [ .ID 300 .TIPO REPOSO ] ) ])";
        return text + " ) ]";
    };
    const auto animations = [](const bool donor_records) {
        std::string text = R"([ .VERSION 1 .LISTADATOS (
  [ .ID 100 .NOMBRE idle .FILE "Anims\\Comm\\idle.anm" ]
  [ .ID 101 .NOMBRE talk .FILE "Anims\\Comm\\talk.anm" ])";
        if (donor_records) text += R"(
  [ .ID 300 .NOMBRE tank_idle .FILE "Anims\\Vehi\\tank.anm" ])";
        return text + " ) ]";
    };
    for (const auto& [target, donor_side] : {std::pair{package, false}, std::pair{donor, true}}) {
        const auto maps = target / "Maps" / "M1";
        write_bytes(maps / "M1.scn", compile_source(scene_text));
        write_bytes(maps / "M1.gsc", compile_source(program_text));
        write_bytes(target / "BDD" / "Objetos.bdd", compile_source(objects(donor_side)));
        write_bytes(target / "BDD" / "Anims.bdd", compile_source(animations(donor_side)));
        write_bytes(target / "BDD" / "Armas.bdd",
                    compile_source(donor_side ? R"([ .LISTADATOS ( [ .ID 70 .NOMBRE Gun .FILE "Models\\Weap\\Gun.dff" .FILE2 "Models\\Weap\\Gun3.dff" ] ) ])"
                                              : "[ .LISTADATOS ( ) ]"));
        std::vector<std::byte> m3d;
        append_csf_string(m3d, "Models\\Char\\A.dff", false);
        for (int i = 0; i < 4; ++i) m3d.push_back(std::byte{0xFF});
        append_u32(m3d, 0);
        write_bytes(maps / "M1.m3d", m3d);
        std::vector<std::byte> and_index;
        append_csf_string(and_index, "Anims\\Comm\\idle.anm", false);
        and_index.push_back(std::byte{0});
        and_index.push_back(std::byte{1});
        append_u32(and_index, 0);
        write_bytes(maps / "M1.and", and_index);
        write_text_file(maps / "M1.txl", "Models\\Char\\Textures\\a.dds\r\n");
        write_bytes(target / "Models" / "Char" / "A.rpc", {std::byte{0x10}});
    }
    // Donor-only assets: a model whose Texture chunk names "tankskin".
    std::vector<std::byte> tank;
    std::vector<std::byte> texture;
    append_header(texture, 0x02, 12);
    for (const auto c : std::string("tankskin\0\0\0\0", 12)) texture.push_back(static_cast<std::byte>(c));
    std::vector<std::byte> texture_chunk;
    append_header(texture_chunk, 0x06, static_cast<std::uint32_t>(texture.size()));
    texture_chunk.insert(texture_chunk.end(), texture.begin(), texture.end());
    append_header(tank, 0x10, static_cast<std::uint32_t>(texture_chunk.size()));
    tank.insert(tank.end(), texture_chunk.begin(), texture_chunk.end());
    write_bytes(donor / "Models" / "Vehi" / "Tank.rpc", tank);
    write_bytes(donor / "Models" / "Vehi" / "Tank.cmo", {std::byte{1}});
    write_bytes(donor / "Models" / "Weap" / "Gun3.rpc", {std::byte{0x10}});
    write_bytes(donor / "Models" / "Vehi" / "Textures" / "tankskin.dds", {std::byte{2}});
    write_bytes(donor / "Anims" / "Vehi" / "tank.anm", {std::byte{3}});
    write_text_file(donor / "Maps" / "M1" / "M1.txl",
                    "Models\\Char\\Textures\\a.dds\r\nModels\\Vehi\\Textures\\tankskin.dds\r\n");

    const auto source_document = csf::Document::load(map / "M1.scn");
    const auto source_scene = source_document.bytes();
    auto editor = csf::MissionEditor::open(map / "M1.scn", package);
    CHECK(editor.scene().actors().size() == 2);
    CHECK(editor.file_of_kind(csf::MissionFileKind::mission_script));
    CHECK(editor.file_of_kind(csf::MissionFileKind::model_index));
    CHECK(editor.modified_files().empty() && !editor.dirty());
    CHECK(editor.find_file("maps\\m1\\M1.SCN") == editor.scene_file());

    // Moving an actor moves its mirrored placement point; undo restores bytes.
    auto result = editor.set_actor_placement(5, {{150.0F, 5.0F, 60.0F}, 180.0F, 0.0F});
    CHECK(result.applied && result.warnings.empty());
    const auto* guard = &editor.scene().actors()[1];
    CHECK(guard->position->x == 150.0F && guard->heading == 180.0F);
    const auto* point = editor.scene().navigation_point(1, 2);
    CHECK(point && point->position->x == 150.0F && std::abs(*point->heading - 3.14159265F) < 1e-5F);
    CHECK(editor.modified_files().size() == 1 && editor.dirty());
    CHECK(editor.undo());
    CHECK(editor.document(editor.scene_file()).bytes().size() == source_scene.size());
    CHECK(std::ranges::equal(editor.document(editor.scene_file()).bytes(), source_scene));
    CHECK(editor.modified_files().empty());
    CHECK(editor.redo() && editor.scene().actors()[1].position->x == 150.0F);
    // Moving the placement point carries its actor.
    CHECK(editor.set_navigation_point(1, 2, {160.0F, 5.0F, 60.0F}).applied);
    CHECK(editor.scene().actors()[1].position->x == 160.0F);

    // Class changes stay within the mission's database unless forced.
    result = editor.set_actor_class(5, 99);
    CHECK(!result.applied && result.message.find("import") != std::string::npos);
    const auto history = editor.history_position();
    result = editor.set_actor_class(5, 12);
    CHECK(result.applied && editor.scene().actors()[1].class_id == 12);
    CHECK(std::ranges::any_of(result.warnings, [](const std::string& w) { return w.find("TIPO") != std::string::npos; }));
    CHECK(editor.history_position() == history + 1);
    CHECK(!editor.set_actor_class(5, 12).applied); // no change is not a history step
    CHECK(editor.history_position() == history + 1);

    // Animation overrides are inserted in canonical field order before CELDA.
    CHECK(!editor.set_actor_animations(5, {{100, "NOT_A_SLOT"}}).applied);
    CHECK(!editor.set_actor_animations(5, {{555, "DISTRAIDO_IDLE_ARMA1"}}).applied);
    CHECK(editor.set_actor_animations(5, {{101, "DISTRAIDO_IDLE_ARMA1"}}).applied);
    CHECK(editor.scene().actors()[1].animations.size() == 1);
    CHECK(editor.scene().actors()[1].animations[0].id == 101);
    {
        const auto tree = csf::Tree::from_document(editor.scene_document());
        const auto& actor = tree.roots[0].child(".BICHOS")->children[1];
        CHECK(actor.children[actor.children.size() - 2].is(".ANIMACIONES"));
        CHECK(actor.children.back().is(".CELDA"));
    }
    CHECK(editor.set_actor_scripts(5, {}).applied);
    CHECK(editor.scene().actors()[1].script_ids.empty());
    CHECK(!editor.set_actor_scripts(5, {8}).applied); // trigger scripts are not actor scripts
    CHECK(editor.set_actor_scripts(5, {7}).applied);
    CHECK(editor.set_actor_faction(5, "ALEMAN").applied);
    CHECK(editor.scene().actors()[1].faction == "ALEMAN");
    CHECK(editor.set_actor_name(5, "Guardi\xC3\xA1n").applied);
    CHECK(editor.scene().actors()[1].name == "Guardi\xC3\xA1n");

    // Duplicates get new IDs, unique names and their own placement point.
    std::int32_t copy{};
    CHECK(editor.duplicate_actor(5, {10.0F, 0.0F, 0.0F}, &copy).applied && copy == 6);
    const auto& actors = editor.scene().actors();
    CHECK(actors.size() == 3 && actors[2].position->x == 170.0F && actors[2].group == 1 && actors[2].cell == 4);
    CHECK(actors[2].name != actors[1].name && actors[2].script_ids == std::vector<std::int32_t>{7});
    CHECK(editor.scene().navigation_point(1, 4));
    std::int32_t added{};
    CHECK(editor.add_actor(11, {{480.0F, 0.0F, 0.0F}, 45.0F, 0.0F}, "NEW", std::nullopt, &added).applied);
    CHECK(added == 7 && editor.scene().actors().back().group == 1);
    CHECK(!editor.add_actor(77, {{0, 0, 0}, 0, 0}, "X").applied);

    // Deleting refuses while scripts or the mission still name the record.
    CHECK(!editor.delete_actor(5).applied);
    CHECK(!editor.delete_actor(1).applied);
    CHECK(editor.delete_actor(6).applied);
    CHECK(!editor.scene().navigation_point(1, 4));
    CHECK(editor.delete_actor(5, true).applied);
    CHECK(editor.undo() && editor.scene().actors().size() == 3);

    // Dummies, lights, navigation and areas.
    CHECK(!editor.delete_dummy(2).applied);
    std::int32_t dummy{};
    CHECK(editor.duplicate_dummy(2, {1, 0, 0}, &dummy).applied && dummy == 3);
    CHECK(editor.set_dummy_placement(3, {9, 9, 9}, 1.0F, 0.0F).applied);
    CHECK(editor.delete_dummy(3).applied);
    CHECK(editor.set_light(9, {csf::Vec3{1, 1, 1}, 0x00FF00, std::nullopt, 50.0F}).applied);
    CHECK(editor.scene().lights()[0].radius == 50.0F);
    std::int32_t light{};
    CHECK(editor.duplicate_light(9, {1, 0, 0}, &light).applied && light == 10);
    CHECK(editor.scene().folders().back().element_ids == std::vector<std::int32_t>({9, 10}));
    CHECK(editor.delete_light(10).applied);
    CHECK(editor.connect_navigation_points(1, 3, 2, 1).applied);
    CHECK(!editor.connect_navigation_points(2, 1, 1, 3).applied);
    CHECK(editor.scene().cross_group_connections().size() == 1);
    CHECK(editor.disconnect_navigation_points(2, 1, 1, 3).applied);
    std::int32_t point_id{};
    CHECK(editor.add_navigation_point(2, {600, 0, 0}, &point_id).applied && point_id == 2);
    CHECK(editor.connect_navigation_points(2, 1, 2, 2).applied);
    CHECK(editor.delete_navigation_point(2, 2).applied);
    CHECK(editor.scene().navigation()[1].connections.empty());
    CHECK(!editor.delete_navigation_point(1, 2).applied); // placement of the guard
    CHECK(editor.insert_area_point(1, 1, {5, 0, 0}).applied);
    CHECK(editor.scene().areas()[0].points.size() == 4 && editor.scene().areas()[0].points[1].x == 5.0F);
    CHECK(editor.remove_area_point(1, 1).applied);
    CHECK(!editor.remove_area_point(1, 0).applied);
    CHECK(editor.set_area_point(1, 0, {-1, 0, 0}).applied && editor.set_area_height(1, 300).applied);

    // Mission properties and raw scalars.
    CHECK(editor.set_player_actor(5).applied && editor.scene().player().active_player == 5);
    CHECK(!editor.set_player_actor(404).applied);
    CHECK(editor.set_start_availability(true, true, false).applied);
    CHECK(editor.scene().player().commando_start == 1);
    CHECK(editor.set_scores(3000, 1500).applied);
    CHECK(!editor.set_scores(1, 2).applied);
    CHECK(editor.set_environment(".FOGDISTANCE", 1500.0F).applied);
    CHECK(!editor.set_environment(".FOGDISTANCE", 1500).applied);
    const auto fog = std::ranges::find_if(editor.scene_document().entries(), [&](const csf::Entry& entry) {
        return entry.kind() == csf::ValueKind::real &&
               std::bit_cast<float>(entry.raw_value_or_size) == 1500.0F;
    });
    CHECK(fog != editor.scene_document().entries().end());
    CHECK(editor.set_scalar(editor.scene_file(), fog->entry_index, 1600.0F).applied);

    // Scripts round-trip as text; references and signatures are checked.
    const auto program = *editor.file_of_kind(csf::MissionFileKind::mission_script);
    const auto text = editor.script_text(program, 7);
    CHECK(text && text->find("PLAY_ANMBDD (THIS) (ANM_BDD 100)") != std::string::npos);
    CHECK(!editor.set_script_text(program, 7, *text).applied); // unchanged
    auto edited = *text;
    edited.replace(edited.find("ANM_BDD 100"), 11, "ANM_BDD 101");
    result = editor.set_script_text(program, 7, edited);
    CHECK(result.applied && result.warnings.empty());
    const auto resources = csf::Tree::from_document(editor.document(program));
    CHECK(resources.roots[0].child(".RECURSOS")->child(".ANIMACIONES")->children.size() == 2);
    edited.replace(edited.find("ANM_BDD 101"), 11, "ANM_BDD 999");
    result = editor.set_script_text(program, 7, edited);
    CHECK(result.applied && std::ranges::any_of(result.warnings, [](const std::string& w) {
              return w.find("Animation 999") != std::string::npos;
          }));
    CHECK(!editor.set_script_text(program, 7, "[ .ID 7 ").applied);
    CHECK(!editor.set_script_text(program, 7, "[ .ID 8 .ACCIONES { WEND } ]").applied); // ID taken
    result = editor.set_script_text(program, 7, "[ .ID 7 .ACCIONES {\n WHILE (BOOL TRUE)\n} ]");
    CHECK(result.applied && std::ranges::any_of(result.warnings, [](const std::string& w) {
              return w.find("WEND") != std::string::npos;
          }));
    CHECK(editor.undo());
    std::int32_t script_id{};
    CHECK(editor.add_script(program, csf::MissionEditor::script_template(7, "NEW"), &script_id).applied);
    CHECK(script_id == 9);
    CHECK(editor.delete_script(program, 9).applied);
    CHECK(!editor.delete_script(program, 7).applied); // run by actor 5
    CHECK(editor.actor_script_choices().size() == 1);

    // Cross-mission import copies records, files and index entries.
    CHECK(!editor.set_actor_class(5, 40).applied);
    result = editor.import_class(donor, 40);
    CHECK(result.applied);
    CHECK(!editor.objects().find_class(40).empty());
    CHECK(editor.animations().find_id(300));
    {
        // Imported records keep the database sorted by ID.
        const auto objects_tree = csf::Tree::from_document(
            editor.document(*editor.file_of_kind(csf::MissionFileKind::objects)));
        const auto& records = objects_tree.roots[0].child(".LISTADATOS")->children;
        CHECK(records.size() == 4 && records.back().child(".ID")->as_int() == 40);
    }
    CHECK(editor.find_file("Models/Vehi/Tank.rpc") && editor.find_file("Models/Vehi/Tank.cmo"));
    CHECK(editor.find_file("Models/Weap/Gun3.rpc") && editor.find_file("Anims/Vehi/tank.anm"));
    CHECK(editor.find_file("Models/Vehi/Textures/tankskin.dds"));
    {
        const auto& m3d = editor.files()[*editor.file_of_kind(csf::MissionFileKind::model_index)].raw;
        const std::string content(reinterpret_cast<const char*>(m3d.data()), m3d.size());
        CHECK(content.find("Models\\Vehi\\Tank.dff") != std::string::npos);
        const auto& txl = editor.files()[*editor.file_of_kind(csf::MissionFileKind::texture_index)].raw;
        const std::string list(reinterpret_cast<const char*>(txl.data()), txl.size());
        CHECK(list.find("tankskin.dds") != std::string::npos);
    }
    CHECK(!editor.import_class(donor, 40).applied);
    CHECK(editor.set_actor_class(5, 40).applied);
    CHECK(editor.undo() && editor.undo());
    CHECK(!editor.find_file("Models/Vehi/Tank.rpc"));
    CHECK(editor.redo() && editor.find_file("Models/Vehi/Tank.rpc"));

    // Map scene instances are patched in place; undo keeps only changed runs.
    {
        std::vector<std::byte> map_bytes(64, std::byte{0x55});
        const auto record = map_bytes.size();
        append_header(map_bytes, 0x16FC0, 92 + 4);
        for (int i = 0; i < 6; ++i) append_u32(map_bytes, 0);
        append_header(map_bytes, 0x0D, 64);
        append_header(map_bytes, 0x01, 52);
        for (const float value : {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 10.0F, 20.0F, 30.0F})
            append_f32(map_bytes, value);
        append_u32(map_bytes, 0);
        append_u32(map_bytes, 4);
        for (const auto c : std::string("box\0")) map_bytes.push_back(static_cast<std::byte>(c));
        const auto map_file = std::ranges::find(editor.files(), csf::MissionFileKind::visual_map, &csf::MissionFile::kind);
        CHECK(map_file == editor.files().end()); // No map in the synthetic package yet.
        write_bytes(map / "world.rws", map_bytes);
        std::vector<std::byte> vis;
        append_csf_string(vis, "Maps\\M1\\world.rws", false);
        write_bytes(map / "M1.vis", vis);
        auto with_map = csf::MissionEditor::open(map / "M1.scn", package);
        const auto index = with_map.file_of_kind(csf::MissionFileKind::visual_map);
        CHECK(index.has_value());
        CHECK(!with_map.set_map_instance_transform(record + 1, {}, {0, 0, 0}).applied);
        const std::array<float, 9> turned{0, 0, -1, 0, 1, 0, 1, 0, 0};
        CHECK(with_map.set_map_instance_transform(record, turned, {11, 22, 33}).applied);
        const auto& edited = with_map.files()[*index].raw;
        CHECK(std::bit_cast<float>(static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(edited[record + 96]) |
                                                              std::to_integer<std::uint32_t>(edited[record + 97]) << 8U |
                                                              std::to_integer<std::uint32_t>(edited[record + 98]) << 16U |
                                                              std::to_integer<std::uint32_t>(edited[record + 99]) << 24U)) == 11.0F);
        CHECK(std::equal(edited.begin(), edited.begin() + static_cast<std::ptrdiff_t>(record), map_bytes.begin()));
        CHECK(with_map.undo() && with_map.files()[*index].raw == map_bytes);
        CHECK(with_map.redo() && with_map.modified_files().size() == 1);
        std::filesystem::remove(map / "world.rws");
        std::filesystem::remove(map / "M1.vis");
    }

    // Saving writes only changed files into a mod project, which reopens.
    const auto workspace = root / "Project";
    auto project = csf::ModProject::create(workspace, package, "Test");
    const auto written = editor.save(project);
    CHECK(!editor.dirty() && written.size() == editor.modified_files().size());
    CHECK(project.validate().passed);
    CHECK(std::filesystem::is_regular_file(workspace / "authored" / "Models" / "Vehi" / "Tank.rpc"));
    auto reopened = csf::MissionEditor::open(map / "M1.scn", package, &project);
    CHECK(reopened.scene().actors()[1].position->x == 160.0F);
    CHECK(!reopened.objects().find_class(40).empty());
    CHECK(reopened.find_file("Models/Vehi/Tank.rpc"));
    CHECK(reopened.modified_files().size() == written.size());
    // Undoing to the source content removes the file from the project on save.
    while (editor.undo()) {
    }
    CHECK(editor.modified_files().empty());
    CHECK(editor.save(project).empty() && project.files.empty());
    std::filesystem::remove_all(root);
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): CHECK failures intentionally unwind
void test_viewport_overlay_model() {
    using namespace rwsman;
    // Layer visibility helpers keep the list sorted and unique.
    OverlayOptions options;
    options.set_layer_hidden("nav_link", true);
    options.set_layer_hidden("actor", true);
    options.set_layer_hidden("actor", true);
    CHECK((options.hidden_layers == std::vector<std::string>{"actor", "nav_link"}));
    CHECK(options.layer_hidden("actor") && !options.layer_hidden("dummy"));
    options.set_layer_hidden("actor", false);
    CHECK(!options.layer_hidden("actor"));
    CHECK(parse_overlay_labels("all") == OverlayOptions::Labels::all && !parse_overlay_labels("x"));
    CHECK(parse_overlay_detail(overlay_detail_name(OverlayOptions::Detail::off)) ==
          OverlayOptions::Detail::off);
    const auto presets = builtin_overlay_presets();
    CHECK(!presets.empty() && presets.front().name == "All" && presets.front().hidden_layers.empty());
    for (const auto& preset : presets)
        for (const auto& layer : preset.hidden_layers)
            CHECK(std::ranges::find(overlay_layer_keys, layer) != overlay_layer_keys.end());

    // Markers within the radius merge into the first one's cluster; pinned ones never do.
    const std::array markers{ScreenMarker{10, 10}, ScreenMarker{13, 11}, ScreenMarker{40, 10},
                             ScreenMarker{11, 10, true}, ScreenMarker{41, 12}, ScreenMarker{12, 9}};
    const auto clusters = cluster_markers(markers, 6.0F);
    CHECK(clusters.size() == 3);
    CHECK((clusters[0].members == std::vector<std::uint32_t>{0, 1, 5}));
    CHECK((clusters[1].members == std::vector<std::uint32_t>{2, 4}));
    CHECK((clusters[2].members == std::vector<std::uint32_t>{3}) && clusters[2].x == 11.0F);
    CHECK(cluster_markers(markers, 0.0F).size() == markers.size());
    // Chains do not grow past the anchor's radius.
    const std::array chain{ScreenMarker{0, 0}, ScreenMarker{5, 0}, ScreenMarker{10, 0}};
    CHECK(cluster_markers(chain, 6.0F).size() == 2);

    // Distance dimming starts at the scene's near side, or at the camera inside it.
    const auto outside = marker_fade_range(500.0F, 100.0F, 3.0F);
    CHECK(outside.near == 400.0F && outside.far == 700.0F);
    CHECK(marker_distance_fade(outside, 390.0F) == 1.0F);
    CHECK(marker_distance_fade(outside, 700.0F) == marker_fade_floor);
    CHECK(marker_distance_fade(outside, 5000.0F) == marker_fade_floor);
    CHECK(marker_distance_fade(outside, 450.0F) > marker_distance_fade(outside, 600.0F));
    const auto inside = marker_fade_range(20.0F, 100.0F, 3.0F);
    CHECK(inside.near == 0.0F && inside.far == 300.0F);
    CHECK(marker_distance_fade(marker_fade_range(500.0F, 100.0F, 0.0F), 5000.0F) == 1.0F);
    // Walls: none leaves a marker alone, each further one dims toward the limit.
    CHECK(marker_wall_fade(0, 0.25F) == 1.0F);
    CHECK(std::abs(marker_wall_fade(1, 0.25F) - 0.5F) < 1e-5F);
    CHECK(marker_wall_fade(2, 0.25F) < marker_wall_fade(1, 0.25F));
    CHECK(marker_wall_fade(3, 0.25F) > 0.25F);
    CHECK(marker_wall_fade(1, 0.0F) == 0.0F && marker_wall_fade(4, 1.0F) == 1.0F);

    // Labels avoid each other, obstacles, and the viewport edge; priority wins ties.
    const ScreenRect bounds{0, 0, 200, 100};
    const std::array requests{LabelRequest{50, 50, 40, 10, 1.0F}, LabelRequest{50, 50, 40, 10, 5.0F},
                              LabelRequest{195, 50, 40, 10, 0.0F}, LabelRequest{50, 50, 40, 10, 0.5F},
                              LabelRequest{50, 50, 40, 10, 0.2F}, LabelRequest{50, 50, 40, 10, 0.1F}};
    const auto placed = place_labels(requests, bounds, 4.0F);
    CHECK(placed[1] && placed[1]->x0 > 50.0F && placed[1]->y1 <= 50.0F); // Highest: right-above.
    CHECK(placed[0] && placed[0]->x0 > 50.0F && placed[0]->y0 >= 50.0F);  // Next: right-below.
    CHECK(placed[2] && placed[2]->x1 < 195.0F);                           // Flipped left at the edge.
    CHECK(placed[3] && placed[4] && !placed[5]);                          // Four slots, then none.
    for (std::size_t i = 0; i < placed.size(); ++i)
        for (std::size_t j = i + 1; j < placed.size(); ++j)
            if (placed[i] && placed[j]) CHECK(!placed[i]->overlaps(*placed[j]));
    const std::array obstacle{ScreenRect{0, 0, 200, 100}};
    CHECK(!place_labels(std::span(requests).first(1), bounds, 4.0F, obstacle)[0]);

    // Off-screen indicators sit on the inset edge in the target's direction.
    const auto right = edge_indicator(100, 0, {0, 0, 200, 100}, 10);
    CHECK(right && right->x == 190.0F && right->y == 50.0F && right->angle == 0.0F);
    const auto corner = edge_indicator(-1, -1, {0, 0, 200, 100}, 10);
    CHECK(corner && corner->y == 10.0F && corner->x == 60.0F);
    CHECK(!edge_indicator(0, 0, {0, 0, 200, 100}, 10));

    // Filter terms: entry, kind, text; all must match.
    const OverlayFilterSubject guard{"actor", "Actors", "Faction: Nazi", "Guard_Tower_01", 42};
    CHECK(overlay_filter_matches("", guard) && overlay_filter_matches("   ", guard));
    CHECK(overlay_filter_matches("#42", guard) && !overlay_filter_matches("#41", guard));
    CHECK(overlay_filter_matches("kind:act tower", guard) && !overlay_filter_matches("kind:nav", guard));
    CHECK(overlay_filter_matches("GUARD nazi", guard) && !overlay_filter_matches("guard sniper", guard));
    CHECK(overlay_filter_matches("gtw", guard) == false);   // Too scattered for a fuzzy hit.
    CHECK(overlay_filter_matches("grdtwr", guard));         // Compact abbreviations match.
    CHECK(overlay_filter_matches("tow01", guard));
    CHECK(!overlay_filter_matches("#abc", guard));

    // Polygons: containment and ear clipping in both windings, concave included.
    const std::array<Point2, 6> l_shape{{{0, 0}, {4, 0}, {4, 2}, {2, 2}, {2, 4}, {0, 4}}};
    CHECK(point_in_polygon(l_shape, 1, 3) && !point_in_polygon(l_shape, 3, 3));
    for (const bool reversed : {false, true}) {
        auto polygon = l_shape;
        if (reversed) std::ranges::reverse(polygon);
        const auto triangles = triangulate_polygon(polygon);
        CHECK(triangles.size() == 4);
        float area = 0.0F;
        for (const auto& t : triangles) {
            const auto &a = polygon[t[0]], &b = polygon[t[1]], &c = polygon[t[2]];
            area += std::abs((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])) * 0.5F;
        }
        CHECK(std::abs(area - 12.0F) < 1e-4F);
    }
    const std::array<Point2, 3> flat{{{0, 0}, {1, 1}, {2, 2}}};
    CHECK(triangulate_polygon(flat).empty() && triangulate_polygon(std::span(flat).first(2)).empty());
}

void test_frame_pacing() {
    using namespace rwsman;
    const FramePacingSettings defaults;
    FramePacingInput input;
    input.now = 10.0;
    input.last_input = 9.9;
    // Recent input keeps frames coming; so does anything animating.
    CHECK(!decide_frame_pacing(defaults, input).wait_for_events);
    input.last_input = 1.0;
    const auto idle = decide_frame_pacing(defaults, input);
    CHECK(idle.wait_for_events && idle.wait_timeout == 0.5 && idle.min_frame_time == 0.0);
    input.text_input = true;
    CHECK(decide_frame_pacing(defaults, input).wait_timeout == 0.3);
    input.background_work = true;
    CHECK(decide_frame_pacing(defaults, input).wait_timeout == 0.1);
    input.animating = true;
    CHECK(!decide_frame_pacing(defaults, input).wait_for_events);
    FramePacingSettings always{false, 0, 0};
    input.animating = false;
    CHECK(!decide_frame_pacing(always, input).wait_for_events);
    // Caps: the slower of the foreground and (while unfocused) background limits.
    FramePacingSettings capped{true, 120, 20};
    input.animating = true;
    CHECK(std::abs(decide_frame_pacing(capped, input).min_frame_time - 1.0 / 120) < 1e-12);
    input.focused = false;
    CHECK(std::abs(decide_frame_pacing(capped, input).min_frame_time - 1.0 / 20) < 1e-12);
    input.iconified = true;
    const auto hidden = decide_frame_pacing(capped, input);
    CHECK(hidden.wait_for_events && hidden.wait_timeout == 0.25);

    FrameRateCounter counter;
    CHECK(counter.fps() == 0.0);
    for (int i = 0; i < 40; ++i) counter.add(1.0 / 50);
    counter.add(-1.0);
    CHECK(std::abs(counter.fps() - 50.0) < 1e-6);
    counter.reset();
    counter.add(0.1);
    CHECK(std::abs(counter.fps() - 10.0) < 1e-9);

    ViewVolume perspective;
    perspective.near_plane = 1.0F;
    perspective.far_plane = 100.0F;
    CHECK(sphere_in_view(perspective, 0, 0, -10, 1));
    CHECK(!sphere_in_view(perspective, 0, 0, 10, 1));     // Behind the eye.
    CHECK(!sphere_in_view(perspective, 0, 0, -200, 1));   // Past the far plane.
    CHECK(sphere_in_view(perspective, 0, 0, -0.5F, 1));   // Straddles the near plane.
    CHECK(!sphere_in_view(perspective, 20, 0, -10, 1));   // Far to the right.
    CHECK(sphere_in_view(perspective, 5.3F, 0, -10, 1));  // Just touches the right plane.
    CHECK(!sphere_in_view(perspective, 0, -20, -10, 1));  // Below.
    CHECK(sphere_in_view(perspective, 0, 0, std::numeric_limits<float>::quiet_NaN(), 1));
    ViewVolume ortho;
    ortho.orthographic = true;
    ortho.aspect = 2.0F;
    ortho.orthographic_scale = 10.0F;
    ortho.near_plane = -50.0F;
    CHECK(sphere_in_view(ortho, 19, 0, 0, 0.5F) && !sphere_in_view(ortho, 21, 0, 0, 0.5F));
    CHECK(sphere_in_view(ortho, 0, 10.4F, 0, 0.5F) && !sphere_in_view(ortho, 0, 11, 0, 0.5F));
}

int main() {
    test_fuzzy_matcher();
    test_viewport_overlay_model();
    test_command_registry();
    test_navigation_history();
    test_settings_model();
    test_frame_pacing();
    test_log_buffer();
    test_search_index();
    test_mission_index();
    test_mission_discovery();
    test_diagnostic_table();
    test_search_index_grouping();
    test_csf_tree_and_source_text();
    test_mission_editor();
    test_actor_look();
    {
        const std::array primitives{csf::ScreenOverlayPrimitive{1, csf::OverlayPrimitiveKind::point,
                                                                10, 10, 10, 10, 0, true, false},
                                    csf::ScreenOverlayPrimitive{2,
                                                                csf::OverlayPrimitiveKind::segment,
                                                                0, 10, 20, 10, 1, true, false},
                                    csf::ScreenOverlayPrimitive{3, csf::OverlayPrimitiveKind::point,
                                                                10, 10, 10, 10, 0, false, false},
                                    csf::ScreenOverlayPrimitive{4, csf::OverlayPrimitiveKind::point,
                                                                10, 10, 10, 10, 0, true, true}};
        const auto overlap = csf::pick_overlay(primitives, 10, 10, 8);
        CHECK(overlap && overlap->source_entry == 1 && overlap->distance_pixels == 0);
        const auto segment = csf::pick_overlay(std::span(primitives).subspan(1, 1), 5, 13, 4);
        CHECK(segment && segment->source_entry == 2 && segment->distance_pixels == 3);
        CHECK(!csf::pick_overlay(std::span(primitives).subspan(2), 10, 10, 8));
        CHECK(!csf::pick_overlay(primitives, 100, 100, 8));
    }
    {
        const auto document = csf::Document::from_bytes(make_typed_scene());
        CHECK(document.state() == csf::ParseState::exact);
        const auto scene = csf::MissionScene::project(document);
        CHECK(scene.player().active_player == 0);
        CHECK(scene.actors().size() == 2 && scene.actors()[0].name == "duplicate" &&
              scene.actors()[1].name == "duplicate");
        CHECK(scene.actors()[0].source.entry_index != scene.actors()[1].source.entry_index);
        CHECK(scene.actors()[0].flags == 0 && !scene.actors()[1].flags);
        CHECK(scene.actors()[0].unknown_fields.size() == 1 &&
              scene.actors()[0].unknown_fields[0].name == ".FUTURE");
        CHECK(scene.actors()[0].secondary_explosion == 1 &&
              scene.actors()[0].faction == "ALLIES" &&
              scene.actors()[0].portrait == "portrait.fbs");
        CHECK(scene.actors()[0].animations.size() == 1 &&
              scene.actors()[0].animations[0].id == 77 &&
              scene.actors()[0].animations[0].type == "IDLE");
        CHECK(scene.actors()[0].door_box && (*scene.actors()[0].door_box)[1].z == 3);
        const auto spawn = scene.actor_spawn_position(scene.actors()[0]);
        CHECK(scene.actors()[0].position && scene.actors()[0].position->x == 101 && spawn &&
              spawn->x == 1 && spawn->y == 2 && spawn->z == 3);
        const auto fallback = scene.actor_spawn_position(scene.actors()[1]);
        CHECK(fallback && fallback->x == 4 && fallback->y == 5 && fallback->z == 6);
        CHECK(std::abs(csf::mission_actor_angle_radians(180) - 3.14159265F) < 1.0e-6F);
        CHECK(scene.navigation_stats().groups == 1 && scene.navigation_stats().points == 3 &&
              scene.navigation_stats().connections == 3 &&
              scene.navigation_stats().connected_components == 1 &&
              scene.navigation_stats().orphan_points == 1 &&
              scene.navigation_stats().invalid_connections == 3 &&
              scene.navigation_stats().duplicate_point_ids == 1);
        CHECK(scene.lights().size() == 1 && scene.lights()[0].radius == 25);
        CHECK(scene.dummies().size() == 1 && scene.folders().size() == 1 &&
              scene.folders()[0].element_ids[0] == 20);
        CHECK(scene.areas().size() == 1 && scene.areas()[0].points.size() == 3 &&
              scene.areas()[0].height == 100 && scene.areas()[0].reverb == 0 &&
              scene.areas()[0].limit_reverb == 0 && scene.areas()[0].unknown_fields.empty());
        CHECK(scene.effects().size() == 1 && scene.effects()[0].name == "sparks" &&
              scene.effects()[0].dummy_id == 20);
        csf::MissionSymbolIndex symbols;
        symbols.add_scene(scene);
        CHECK(symbols.exact("duplicate").size() == 2);
        CHECK(symbols.exact("OnSpawn").size() == 1 &&
              symbols.exact("OnSpawn")[0]->role == csf::SymbolRole::typed_reference);
        CHECK(symbols.exact("sparks").size() == 1 && symbols.exact("dummy:20").size() == 1);
        const auto json = csf::mission_scene_json(scene);
        CHECK(json == csf::mission_scene_json(scene));
        CHECK(json.find("csf-mission-scene-1") != std::string::npos);
        CHECK(json.find("\"effects\":[") != std::string::npos &&
              json.find("\"secondary_explosion\":1") != std::string::npos);
    }
    {
        const auto document = csf::Document::from_bytes(make_typed_scene(true));
        CHECK(document.has_errors());
        const auto scene = csf::MissionScene::project(document);
        CHECK(scene.actors().size() == 2);
        CHECK(std::ranges::any_of(scene.diagnostics(), [](const auto& diagnostic) {
            return diagnostic.code == "document-diagnostic";
        }));
    }
    {
        const auto document = csf::Document::from_bytes(
            make_typed_scene(false, std::numeric_limits<float>::infinity()));
        const auto scene = csf::MissionScene::project(document);
        const auto json = csf::mission_scene_json(scene);
        CHECK(json.find("\"heading\":null") != std::string::npos);
        CHECK(json.find("nan") == std::string::npos && json.find("inf") == std::string::npos);
        CHECK(std::ranges::any_of(scene.diagnostics(), [](const auto& diagnostic) {
            return diagnostic.code == "non-finite-number";
        }));
    }
    {
        SyntheticCsf csf;
        csf.container("", 1, 2);
        csf.container(".BICHOS", 1);
        csf.container("", 3, 2);
        csf.string(".ID", "wrong");
        csf.integer(".ID", 7);
        csf.integer(".SEGUNDA_EXPLOSION", 1);
        const auto scene = csf::MissionScene::project(csf::Document::from_bytes(csf.bytes()));
        CHECK(scene.actors().size() == 1 && !scene.actors()[0].id);
        CHECK(scene.actors()[0].unknown_fields.size() == 2 &&
              scene.actors()[0].secondary_explosion == 1);
        CHECK(std::ranges::any_of(scene.diagnostics(), [](const auto& diagnostic) {
            return diagnostic.code == "duplicate-typed-field";
        }));
        CHECK(std::ranges::any_of(scene.diagnostics(), [](const auto& diagnostic) {
            return diagnostic.code == "wrong-typed-field";
        }));
    }
    const auto mission_test_root =
        std::filesystem::temp_directory_path() /
        ("rws-man-phase2-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(mission_test_root);
    {
        SyntheticCsf database;
        database.container("", 1, 2);
        database.container("", 2, 2);
        database.integer(".ID", 55);
        database.string(".NOMBRE", "Espia");
        const auto path = mission_test_root / "Objetos.bdd";
        write_bytes(path, database.bytes());
        const auto document = csf::Document::load(path);
        csf::MissionSymbolIndex index;
        index.add_document(document);
        const auto class_sites = index.exact("class:55");
        CHECK(class_sites.size() == 1 && class_sites.front()->role == csf::SymbolRole::definition);
    }
    {
        const auto package = mission_test_root / "Package";
        write_bytes(package / "Models" / "Hero.rpc", {std::byte{1}});
        write_bytes(package / "Textures" / "MixedCase.DDS", {std::byte{2}});
        write_bytes(package / std::filesystem::path(L"Models/caf\u00e9.rpc"), {std::byte{3}});
        write_bytes(package / std::filesystem::path(L"gfx/ca\u00c3\u00b1onazoHU2.sp"),
                    {std::byte{4}});
        csf::ResourceIndex index;
        index.add_root(package);
        index.build();
        const auto exact = index.resolve("Models/Hero.rpc", 0);
        CHECK(exact.status == csf::ResolutionStatus::exact && exact.candidate_indices.size() == 1);
        const auto slash = index.resolve("Models\\Hero.rpc", 0);
        CHECK(slash.status == csf::ResolutionStatus::exact && slash.candidate_indices.size() == 1);
        const auto case_mismatch = index.resolve("textures/mixedcase.dds", 0);
        CHECK(case_mismatch.status == csf::ResolutionStatus::case_mismatch);
        CHECK(index.resolve("Textures/", 0).status == csf::ResolutionStatus::exact);
        const auto mapped = index.resolve("Models/Hero.dff", 0);
        CHECK(mapped.status == csf::ResolutionStatus::mapped_dff_to_rpc);
        CHECK(index.resolve(std::string("Models/caf\xE9.rpc"), 0).status ==
              csf::ResolutionStatus::exact);
        CHECK(index.resolve(std::string("Models/caf\xC3\xA9.rpc"), 0).status ==
              csf::ResolutionStatus::exact);
        CHECK(index.resolve(std::string("gfx/ca\xC3\x83\xC2\xB1onazoHU2.sp"), 0).status ==
              csf::ResolutionStatus::exact);
        CHECK(index.resolve("../secret.dds", 0).status == csf::ResolutionStatus::outside_root);
        CHECK(index.resolve("missing.dds", 0).status == csf::ResolutionStatus::missing);

        const auto shared = mission_test_root / "Shared";
        write_bytes(shared / "A" / "Models" / "Duplicate.rpc", {std::byte{3}});
        write_bytes(shared / "B" / "Models" / "Duplicate.rpc", {std::byte{4}});
        csf::ResourceIndex ambiguous_index;
        ambiguous_index.add_root(shared);
        ambiguous_index.build();
        const auto ambiguous = ambiguous_index.resolve("Models/Duplicate.rpc");
        CHECK(ambiguous.status == csf::ResolutionStatus::ambiguous);
        CHECK(ambiguous.candidate_indices.size() == 2);

        csf::ResourceIndex precedence_index;
        precedence_index.add_root(shared / "A");
        precedence_index.add_root(shared / "B");
        precedence_index.build();
        const auto preferred = precedence_index.resolve("Models/Duplicate.rpc", 0);
        CHECK(preferred.status == csf::ResolutionStatus::exact);
        CHECK(precedence_index.resources()[preferred.candidate_indices.front()].root_index == 0);

        const auto overlap_package = mission_test_root / "Overlap" / "Game";
        write_bytes(overlap_package / "Nested" / "Models" / "Only.rpc", {std::byte{5}});
        csf::ResourceIndex overlap_index;
        overlap_index.add_root(overlap_package);
        overlap_index.add_root(overlap_package.parent_path());
        overlap_index.build();
        const auto overlap = overlap_index.resolve("Models/Only.rpc");
        CHECK(overlap.status == csf::ResolutionStatus::shared_root);
        CHECK(overlap.candidate_indices.size() == 1);
    }
    {
        const auto adapters = mission_test_root / "adapters";
        std::vector<std::byte> vis;
        append_length_string(vis, "Maps/Test.rws");
        append_length_string(vis, "Maps/Test_col.rws");
        append_length_string(vis, "Maps/Textures/");
        append_length_string(vis, "Models/Sky.dff");
        vis.push_back(std::byte{0xAA});
        write_bytes(adapters / "test.vis", vis);
        const auto parsed_vis = csf::read_vis(adapters / "test.vis");
        CHECK(parsed_vis.references.size() == 4);
        CHECK(parsed_vis.references[1].kind == csf::DependencyKind::collision_map);
        CHECK(parsed_vis.unknown_tail == std::vector<std::byte>{std::byte{0xAA}});

        std::vector<std::byte> m3d;
        append_length_string(m3d, "Models/Hero.dff");
        append_u32(m3d, 0xFFFFFFFFU);
        write_bytes(adapters / "test.m3d", m3d);
        const auto parsed_m3d = csf::read_m3d(adapters / "test.m3d");
        CHECK(parsed_m3d.references.size() == 1 && parsed_m3d.unknown_tail.empty());

        std::vector<std::byte> and_bytes;
        append_length_string(and_bytes, "Anims/Idle.anm");
        append_u16(and_bytes, 0x0100);
        write_bytes(adapters / "test.and", and_bytes);
        const auto parsed_and = csf::read_and(adapters / "test.and");
        CHECK(parsed_and.references.size() == 1 && parsed_and.unknown_tail.empty());

        const std::string lines = "Textures/A.dds\r\n\r\nTextures/A.dds\nTextures/B.png";
        std::vector<std::byte> txl;
        for (const auto character : lines)
            txl.push_back(static_cast<std::byte>(character));
        write_bytes(adapters / "test.txl", txl);
        const auto parsed_txl = csf::read_txl(adapters / "test.txl");
        CHECK(parsed_txl.references.size() == 3);
        CHECK(parsed_txl.references[0].source.table_index == 0);
        CHECK(parsed_txl.references[1].source.table_index == 2);

        const auto texture_package = adapters / "texture-package";
        write_bytes(texture_package / "Models" / "Vehi" / "Textures" / "shared.dds",
                    {std::byte{1}});
        write_bytes(texture_package / "Models" / "Weap" / "Textures" / "shared.dds",
                    {std::byte{2}});
        write_bytes(texture_package / "Models" / "Char" / "Textures" / "Uniform.dds",
                    {std::byte{3}});
        write_bytes(texture_package / "Models" / "Char" / "Textures" /
                        "Uniform_Alt001.png",
                    {std::byte{4}});
        write_bytes(texture_package / "Models" / "Char" / "Textures" /
                        "Uniform_Alt002.dds",
                    {std::byte{5}});
        const std::string catalog_lines =
            "Models\\Weap\\Textures/shared.dds\r\n"
            "Models\\Char\\Textures/Uniform.dds\r\n"
            "Models\\Char\\Textures\\Uniform_Alt001.png\r\n"
            "Models\\Char\\Textures/Uniform_Alt002.dds\r\n";
        std::vector<std::byte> catalog_bytes;
        for (const auto character : catalog_lines)
            catalog_bytes.push_back(static_cast<std::byte>(character));
        const auto catalog_txl = texture_package / "Maps" / "Test" / "Test.txl";
        write_bytes(catalog_txl, catalog_bytes);
        csf::ResourceIndex texture_resources;
        texture_resources.add_root(texture_package);
        texture_resources.build();
        csf::TextureCatalog catalog;
        catalog.add(csf::read_txl(catalog_txl), texture_resources, 0);
        CHECK(catalog.entries().size() == 4);
        CHECK(catalog.resolve("shared") ==
              texture_package / "Models" / "Weap" / "Textures" / "shared.dds");
        CHECK(catalog.resolve("Uniform") ==
              texture_package / "Models" / "Char" / "Textures" / "Uniform.dds");
        CHECK(catalog.resolve("uniform.dds", 1) ==
              texture_package / "Models" / "Char" / "Textures" / "Uniform_Alt001.png");
        CHECK(catalog.resolve("Uniform", 2) ==
              texture_package / "Models" / "Char" / "Textures" / "Uniform_Alt002.dds");
        CHECK(catalog.resolve("Uniform", 3) ==
              texture_package / "Models" / "Char" / "Textures" / "Uniform.dds");
        CHECK(catalog.variants("Uniform") == std::vector<std::uint32_t>({0, 1, 2}));
        CHECK(catalog.maximum_variant() == 2);
    }
    {
        const auto package = mission_test_root / "Mission";
        std::vector<std::byte> empty_csf;
        append_csf_header(empty_csf, 0, 0, 0);
        const auto map = package / "Maps" / "M1";
        for (const auto* extension : {".scn", ".gsc", ".csc"})
            write_bytes(map / (std::string("Mission") + extension), empty_csf);
        std::vector<std::byte> referenced_scene;
        append_csf_header(referenced_scene, 0, 0, 1);
        append_csf_string(referenced_scene, "Models/caf\xE9.rpc");
        write_bytes(map / "Mission.scn", referenced_scene);
        write_bytes(package / "Maps" / "Secs" / "Mission.sec", {std::byte{0}});
        for (const auto* name : {"Anims.bdd", "Armas.bdd", "Efectos.bdd", "Materiales.bdd",
                                 "Objetos.bdd", "Sonidos.bdd"})
            write_bytes(package / "BDD" / name, empty_csf);
        write_bytes(package / "Maps" / "M1" / "world.rws", {std::byte{0}});
        write_bytes(package / "Maps" / "M1" / "world_col.rws", {std::byte{0}});
        write_bytes(package / std::filesystem::path(L"Models/caf\u00e9.rpc"), {std::byte{0x10}});
        write_bytes(package / "Models" / "Weap" / "Textures" / "mission.dds",
                    {std::byte{1}});
        const std::string mission_txl_line = "Models\\Weap\\Textures/mission.dds\r\n";
        std::vector<std::byte> mission_txl;
        for (const auto character : mission_txl_line)
            mission_txl.push_back(static_cast<std::byte>(character));
        write_bytes(map / "Mission.txl", mission_txl);
        std::vector<std::byte> vis;
        append_length_string(vis, "Maps/M1/world.rws");
        append_length_string(vis, "Maps/M1/world_col.rws");
        append_length_string(vis, "Maps/M1");
        append_length_string(vis, "Models/Missing.dff");
        write_bytes(map / "Mission.vis", vis);
        const auto graph = csf::MissionGraph::load({map / "Mission.scn", package, {}});
        CHECK(graph.nodes().size() >= 12);
        CHECK(std::ranges::any_of(graph.edges(), [](const auto& edge) {
            return edge.kind == csf::DependencyKind::visual_map &&
                   edge.status == csf::ResolutionStatus::exact;
        }));
        CHECK(std::ranges::any_of(graph.edges(), [](const auto& edge) {
            return edge.kind == csf::DependencyKind::sky_model &&
                   edge.status == csf::ResolutionStatus::missing;
        }));
        const auto first_json = csf::mission_graph_json(graph);
        const auto second_json = csf::mission_graph_json(graph);
        CHECK(first_json == second_json &&
              first_json.find("csf-mission-graph-1") != std::string::npos);
        CHECK(first_json.find("caf\xC3\xA9.rpc") != std::string::npos);
        CHECK(first_json.find("\\u00c3") == std::string::npos);
        CHECK(graph.uses(map / "world.rws").size() == 1);
        CHECK(graph.uses(map / "WORLD.RWS").size() == 1);
        CHECK(graph.textures().resolve("mission") ==
              package / "Models" / "Weap" / "Textures" / "mission.dds");
    }
    {
        // Regressions found by opening the CSF_unpacks corpus: unset VIS fields, security
        // files named by the scene, and texture lists that live in another package.
        const auto regression = mission_test_root / "Regression";
        std::vector<std::byte> empty_csf;
        append_csf_header(empty_csf, 0, 0, 0);
        const auto write_package = [&](const std::filesystem::path& package, const std::string& map_name,
                                       const std::string& scene, const std::vector<std::string>& strings,
                                       const std::vector<std::string>& vis_fields) {
            const auto map = package / "Maps" / map_name;
            std::vector<std::byte> scene_bytes;
            append_csf_header(scene_bytes, 0, 0, static_cast<std::uint32_t>(strings.size()));
            for (const auto& value : strings) append_csf_string(scene_bytes, value);
            write_bytes(map / (scene + ".scn"), scene_bytes);
            write_bytes(map / (scene + ".gsc"), empty_csf);
            write_bytes(map / (scene + ".csc"), empty_csf);
            std::vector<std::byte> vis;
            for (const auto& field : vis_fields) append_length_string(vis, field);
            write_bytes(map / (scene + ".vis"), vis);
            for (const auto* name : {"Anims.bdd", "Armas.bdd", "Efectos.bdd", "Materiales.bdd",
                                     "Objetos.bdd", "Sonidos.bdd"})
                write_bytes(package / "BDD" / name, empty_csf);
            return map / (scene + ".scn");
        };
        const auto has_edge = [](const csf::MissionGraph& graph, const std::string& reference,
                                 const csf::ResolutionStatus status) {
            return std::ranges::any_of(graph.edges(), [&](const auto& edge) {
                return edge.status == status &&
                       edge.original_reference.find(reference) != std::string::npos;
            });
        };

        // An empty VIS field is an unset reference, not a missing one.
        std::vector<std::byte> vis_with_unset_sky;
        append_length_string(vis_with_unset_sky, "Maps/M1/world.rws");
        append_length_string(vis_with_unset_sky, "Maps/M1/world_col.rws");
        append_length_string(vis_with_unset_sky, "Maps/M1/Textures/");
        append_length_string(vis_with_unset_sky, "");
        write_bytes(regression / "unset.vis", vis_with_unset_sky);
        const auto unset = csf::read_vis(regression / "unset.vis");
        CHECK(unset.references.size() == 3);
        CHECK(std::ranges::none_of(unset.references, [](const auto& reference) {
            return reference.kind == csf::DependencyKind::sky_model;
        }));

        // The scene names its security file; a shared one must resolve, not be guessed.
        const auto home_scene = write_package(regression / "Home", "M1", "Level",
                                              {"Maps\\Secs\\Shared.sec"},
                                              {"Maps/M1/world.rws", "Maps/M1/world_col.rws",
                                               "Maps/M1/Textures/", ""});
        write_bytes(regression / "Home" / "Maps" / "Secs" / "Shared.sec", {std::byte{0}});
        // The visual map and its texture list only exist in a sibling package.
        write_bytes(regression / "Other" / "Maps" / "M1" / "world.rws", {std::byte{0}});
        write_bytes(regression / "Other" / "Maps" / "M1" / "world_col.rws", {std::byte{0}});
        const std::string other_txl_text = "Maps\\M1\\Textures/lightmap.dds\r\n";
        std::vector<std::byte> other_txl;
        for (const auto character : other_txl_text) other_txl.push_back(static_cast<std::byte>(character));
        write_bytes(regression / "Other" / "Maps" / "M1" / "world.txl", other_txl);
        write_bytes(regression / "Other" / "Maps" / "M1" / "Textures" / "lightmap.dds",
                    {std::byte{2}});
        const auto home = csf::MissionGraph::load({home_scene, regression / "Home", {}});
        CHECK(std::ranges::none_of(home.edges(), [](const auto& edge) {
            return edge.status == csf::ResolutionStatus::missing &&
                   (edge.kind == csf::DependencyKind::sky_model ||
                    edge.original_reference.empty() ||
                    edge.original_reference.find(".sec") != std::string::npos);
        }));
        CHECK(has_edge(home, "Shared.sec", csf::ResolutionStatus::exact));
        CHECK(!has_edge(home, "Level.sec", csf::ResolutionStatus::missing));
        CHECK(home.uses(regression / "Home" / "Maps" / "Secs" / "Shared.sec").size() == 1);
        CHECK(std::ranges::none_of(home.diagnostics(), [](const auto& diagnostic) {
            return diagnostic.message.find(".sec") != std::string::npos;
        }));

        // The texture list is inferred from the visual map in the other package.
        CHECK(home.uses(regression / "Other" / "Maps" / "M1" / "world.txl").size() == 1);
        CHECK(home.textures().resolve("lightmap") ==
              regression / "Other" / "Maps" / "M1" / "Textures" / "lightmap.dds");

        // A texture list already found next to the scene is not added twice.
        const auto local_scene = write_package(regression / "Local", "M2", "Level2", {},
                                               {"Maps/M2/world2.rws", "", "", ""});
        write_bytes(regression / "Local" / "Maps" / "M2" / "world2.rws", {std::byte{0}});
        write_bytes(regression / "Local" / "Maps" / "M2" / "world2.txl", other_txl);
        const auto local = csf::MissionGraph::load({local_scene, regression / "Local", {}});
        CHECK(local.uses(regression / "Local" / "Maps" / "M2" / "world2.txl").size() == 1);

        // A guessed "<scene>.sec" is dropped, but a security file the scene names and that
        // does not exist is still reported.
        const auto absent_scene = write_package(regression / "Absent", "M3", "Level3",
                                                {"Maps\\Secs\\Absent.sec"}, {"", "", "", ""});
        const auto absent = csf::MissionGraph::load({absent_scene, regression / "Absent", {}});
        CHECK(has_edge(absent, "Absent.sec", csf::ResolutionStatus::missing));
        CHECK(!has_edge(absent, "Level3.sec", csf::ResolutionStatus::missing));

        // A scene linked by path (a bridge target) is an edge, but it is not read, so the
        // linked mission's own references do not join this mission's graph.
        const auto bridge_scene = write_package(regression / "Bridge", "M4", "Start",
                                                {"Maps/M5/Next.scn"}, {"", "", "", ""});
        write_package(regression / "Bridge", "M5", "Next", {"Models/NextOnly.rpc"},
                      {"", "", "", ""});
        const auto bridge = csf::MissionGraph::load({bridge_scene, regression / "Bridge", {}});
        CHECK(has_edge(bridge, "Next.scn", csf::ResolutionStatus::exact));
        CHECK(std::ranges::none_of(bridge.edges(), [](const auto& edge) {
            return edge.original_reference.find("NextOnly") != std::string::npos;
        }));
    }
    std::filesystem::remove_all(mission_test_root);
    {
        std::vector<std::byte> bytes;
        append_csf_header(bytes, 0, 0, 0);
        const auto document = csf::Document::from_bytes(std::move(bytes));
        CHECK(document.state() == csf::ParseState::exact);
        CHECK(document.entries().empty() && document.roots().empty());
        CHECK(document.diagnostics().empty());
    }
    {
        std::vector<std::byte> bytes;
        append_csf_header(bytes, 5, 1, 1);
        append_csf_entry(bytes, 0, 3, -1, 1); // group containing three values
        append_csf_entry(bytes, 2, 0, 0, 0);  // named integer label
        append_csf_entry(bytes, 0, 42, -1, 3);
        append_csf_entry(bytes, 0, std::bit_cast<std::uint32_t>(1.5F), -1, 4);
        append_csf_entry(bytes, 0, 0, -1, 5);
        append_csf_string(bytes, "answer");
        append_csf_string(bytes, "caf\xE9");
        const auto original = bytes;
        const auto document = csf::Document::from_bytes(std::move(bytes));
        CHECK(document.state() == csf::ParseState::exact);
        CHECK(document.roots().size() == 1);
        CHECK(document.roots()[0].children.size() == 3);
        CHECK(document.roots()[0].children[0].identifier_index == 0);
        CHECK(std::get<std::int32_t>(document.roots()[0].children[0].scalar) == 42);
        CHECK(std::get<float>(document.roots()[0].children[1].scalar) == 1.5F);
        CHECK(document.strings()[0].display_utf8() == "caf\xC3\xA9");
        CHECK(document.bytes().size() == original.size());
        CHECK(std::equal(document.bytes().begin(), document.bytes().end(), original.begin()));
    }
    {
        std::vector<std::byte> bytes;
        append_csf_header(bytes, 1, 0, 0);
        append_csf_entry(bytes, 0, 9, -1, 5);
        bytes.push_back(std::byte{0xAA});
        const auto document = csf::Document::from_bytes(std::move(bytes));
        CHECK(document.state() == csf::ParseState::partial);
        CHECK(document.has_errors());
        CHECK(document.trailing_bytes().size() == 1);
    }
    {
        std::vector<std::byte> bytes;
        append_csf_header(bytes, 1, 0, 0);
        append_csf_entry(bytes, 0, 0, -1, 99);
        const auto document = csf::Document::from_bytes(std::move(bytes));
        CHECK(document.state() == csf::ParseState::unsupported);
        CHECK(document.entries().size() == 1 && document.roots().size() == 1);
    }
    {
        std::vector<std::byte> bytes;
        append_csf_header(bytes, 3, 1, 0);
        append_csf_entry(bytes, 0, 0, 0, 0);
        append_csf_entry(bytes, 0, 0, -1, 99);
        append_csf_entry(bytes, 0, 42, -1, 3);
        append_csf_string(bytes, "label");
        const auto document = csf::Document::from_bytes(std::move(bytes));
        CHECK(document.roots().size() == 2);
        CHECK(!document.roots()[1].identifier_index);
        CHECK(std::ranges::any_of(document.diagnostics(), [](const auto& diagnostic) {
            return diagnostic.message.find("unknown entry") != std::string::npos;
        }));
    }
    {
        std::vector<std::byte> bytes;
        for (const auto character : std::string("CSFFBS"))
            bytes.push_back(static_cast<std::byte>(character));
        const auto document = csf::Document::from_bytes(std::move(bytes));
        CHECK(document.state() == csf::ParseState::partial);
        CHECK(document.has_errors());
    }
    {
        std::vector<std::byte> bytes;
        append_csf_header(bytes, 0xFFFFFFFFU, 0, 0);
        const auto document = csf::Document::from_bytes(std::move(bytes));
        CHECK(document.state() == csf::ParseState::partial);
        CHECK(document.entries().empty());
    }
    {
        std::vector<std::byte> bytes;
        append_csf_header(bytes, 0, 0, 1);
        append_u32(bytes, 0); // zero-length empty string
        const auto document = csf::Document::from_bytes(std::move(bytes));
        CHECK(document.state() == csf::ParseState::exact);
        CHECK(document.strings().size() == 1 && document.strings()[0].bytes.empty());
        CHECK(document.strings()[0].display_utf8().empty());
    }
    {
        std::vector<std::byte> bytes;
        append_csf_header(bytes, 0, 2, 2);
        append_csf_string(bytes, "duplicate");
        append_csf_string(bytes, "duplicate");
        append_csf_string(bytes, "same");
        append_csf_string(bytes, "same");
        const auto document = csf::Document::from_bytes(std::move(bytes));
        CHECK(document.state() == csf::ParseState::exact);
        CHECK(document.identifiers().size() == 2 && document.strings().size() == 2);
        CHECK(document.identifiers()[0].bytes == document.identifiers()[1].bytes);
        CHECK(document.strings()[0].bytes == document.strings()[1].bytes);
    }
    {
        std::vector<std::byte> bytes;
        append_csf_header(bytes, 0, 0, 1);
        append_csf_string(bytes, "unterminated", false);
        const auto document = csf::Document::from_bytes(std::move(bytes));
        CHECK(document.state() == csf::ParseState::exact);
        CHECK(!document.strings()[0].has_final_null);
        CHECK(document.diagnostics().size() == 1);
        CHECK(document.diagnostics()[0].severity == csf::Diagnostic::Severity::warning);
    }
    {
        std::vector<std::byte> bytes;
        append_csf_header(bytes, 1, 0, 0);
        append_u32(bytes, 0); // only one third of an entry
        const auto document = csf::Document::from_bytes(std::move(bytes));
        CHECK(document.state() == csf::ParseState::partial);
        CHECK(document.entries().empty() && document.has_errors());
    }
    {
        std::vector<std::byte> bytes;
        append_csf_header(bytes, 0, 1, 0);
        append_u16(bytes, 4); // truncated length field
        const auto document = csf::Document::from_bytes(std::move(bytes));
        CHECK(document.state() == csf::ParseState::partial);
        CHECK(document.identifiers().empty() && document.has_errors());
    }
    {
        std::vector<std::byte> bytes;
        append_csf_header(bytes, 0, 0, 1);
        append_u32(bytes, 10);
        bytes.push_back(std::byte{'x'});
        const auto document = csf::Document::from_bytes(std::move(bytes));
        CHECK(document.state() == csf::ParseState::partial);
        CHECK(document.strings().size() == 1 && !document.strings()[0].complete);
    }
    {
        std::vector<std::byte> bytes;
        append_csf_header(bytes, 2, 0, 0);
        append_csf_entry(bytes, 0, 2, -1, 2); // array claims two values
        append_csf_entry(bytes, 0, 7, -1, 3); // but has only one
        const auto document = csf::Document::from_bytes(std::move(bytes));
        CHECK(document.state() == csf::ParseState::partial);
        CHECK(document.roots().size() == 1 && document.roots()[0].children.size() == 1);
    }
    {
        std::vector<std::byte> bytes;
        append_csf_header(bytes, 4, 1, 0);
        append_csf_entry(bytes, 0, 1, 0, 2);  // named array
        append_csf_entry(bytes, 0, 1, -1, 1); // anonymous nested group
        append_csf_entry(bytes, 0, 9, -1, 3);
        append_csf_entry(bytes, 0, 0, 7, 1); // invalid identifier on empty group root
        append_csf_string(bytes, "named");
        const auto document = csf::Document::from_bytes(std::move(bytes));
        CHECK(document.state() == csf::ParseState::partial);
        CHECK(document.roots().size() == 2);
        CHECK(document.roots()[0].identifier_index == 0);
        CHECK(document.roots()[0].children[0].children.size() == 1);
        CHECK(document.has_errors());
    }
    {
        constexpr std::uint32_t depth = 258;
        std::vector<std::byte> bytes;
        append_csf_header(bytes, depth + 1, 0, 0);
        for (std::uint32_t index = 0; index < depth; ++index)
            append_csf_entry(bytes, 0, 1, -1, 1);
        append_csf_entry(bytes, 0, 1, -1, 3);
        const auto document = csf::Document::from_bytes(std::move(bytes));
        CHECK(document.state() == csf::ParseState::partial);
        CHECK(document.has_errors());
    }
    {
        std::vector<std::byte> bytes;
        append_csf_header(bytes, 3, 1, 1);
        append_csf_entry(bytes, 0, 0, 0, 0);
        append_csf_entry(bytes, 0, 5, -1, 3);
        append_csf_entry(bytes, 0, 0, -1, 5);
        append_csf_string(bytes, "value");
        append_csf_string(bytes, "caf\xE9");
        const auto document = csf::Document::from_bytes(std::move(bytes));
        const auto text1 = csf::export_text(document);
        const auto text2 = csf::export_text(document);
        const auto json1 = csf::export_json(document);
        const auto json2 = csf::export_json(document);
        CHECK(text1 == text2 && json1 == json2);
        CHECK(text1.find("caf\\xe9") != std::string::npos);
        CHECK(json1.find("\"raw_hex\":\"636166e900\"") != std::string::npos);
        CHECK(json1.find("\"entry_index\":1") != std::string::npos);
    }
    {
        const auto version = rws::decode_library_id(0x1C020037);
        CHECK(version.encoded_version == 0x37002);
        CHECK(version.major == 3 && version.minor == 7 && version.revision == 0 &&
              version.binary == 2);
        CHECK(version.build == 55);
    }
    {
        std::vector<std::byte> bytes;
        append_header(bytes, 0x10, 16);
        append_header(bytes, 0x01, 4);
        append_u32(bytes, 7);
        const auto document = rws::Document::from_bytes(std::move(bytes));
        CHECK(document.chunks().size() == 1);
        CHECK(document.chunks()[0].children.size() == 1);
        CHECK(document.chunks()[0].children[0].type == 0x01);
        CHECK(document.diagnostics().empty());
        const auto clump = rws::decode_clump(document.chunks()[0], document.bytes());
        CHECK(clump && clump.value->atomics == 7);
    }
    {
        std::vector<std::byte> bytes;
        append_header(bytes, 0x0B, 100);
        append_u32(bytes, 0);
        const auto document = rws::Document::from_bytes(std::move(bytes));
        CHECK(document.chunks().size() == 1);
        CHECK(document.chunks()[0].truncated);
        CHECK(!document.diagnostics().empty());
    }
    {
        std::vector<std::byte> bytes;
        append_header(bytes, 0x01, 0); // ordinary stream prefix establishes the stamp
        for (int i = 0; i < 5; ++i)
            bytes.push_back(std::byte{0x55}); // game-specific records
        const auto world_offset = bytes.size();
        append_header(bytes, 0x0B, 44); // one Struct child, declared 32 bytes beyond EOF
        append_header(bytes, 0x01, 0);
        const auto document = rws::Document::from_bytes(std::move(bytes));
        CHECK(document.chunks().size() == 2);
        CHECK(document.chunks()[1].offset == world_offset);
        CHECK(document.chunks()[1].type == 0x0B && document.chunks()[1].truncated);
        CHECK(document.chunks()[1].children.size() == 1);
    }
    {
        std::vector<std::byte> bytes;
        append_header(bytes, 0x01, 0);
        append_header(bytes, 0x16FC0, 99);
        append_u32(bytes, 1001);
        append_u32(bytes, 42);
        append_f32(bytes, 5000.0F);
        append_u32(bytes, 0);
        append_u32(bytes, 0);
        append_u32(bytes, 0x641);
        append_header(bytes, 0x0D, 64);
        append_header(bytes, 0x01, 52);
        for (int i = 0; i < 9; ++i)
            append_f32(bytes, i % 4 == 0 ? 1.0F : 0.0F);
        append_f32(bytes, 10.0F);
        append_f32(bytes, 20.0F);
        append_f32(bytes, 30.0F);
        append_u32(bytes, 3);
        append_u32(bytes, 7);
        for (const char character : std::string("ARBOL_3"))
            bytes.push_back(static_cast<std::byte>(character));
        const auto world_offset = bytes.size();
        append_header(bytes, 0x0B, 44);
        append_header(bytes, 0x01, 0);
        const auto document = rws::Document::from_bytes(std::move(bytes));
        CHECK(document.scene_instances().size() == 1);
        const auto& instance = document.scene_instances().front();
        CHECK(instance.prototype_id == 1001 && instance.instance_id == 42);
        CHECK(instance.prototype_name == "ARBOL_3" && instance.physical_size == 123);
        CHECK(instance.maximum_visibility_distance == 5000.0F);
        CHECK(instance.minimum_visibility_distance == 0.0F);
        CHECK(instance.visibility_fade_range == 0.0F);
        CHECK(rws::has_scene_instance_flag(instance.flags, rws::SceneInstanceFlag::enabled));
        CHECK(rws::has_scene_instance_flag(instance.flags, rws::SceneInstanceFlag::animated));
        CHECK(rws::scene_instance_flag_names(instance.flags) ==
              "enabled, mipmapped, animated, scene-registered");
        CHECK(instance.position.x == 10.0F && instance.position.y == 20.0F &&
              instance.position.z == 30.0F);
        CHECK(document.chunks().size() == 2 && document.chunks()[1].offset == world_offset);
    }
    {
        constexpr std::uint32_t struct_size = 120;
        std::vector<std::byte> bytes;
        append_header(bytes, 0x0F, 12 + struct_size);
        append_header(bytes, 0x01, struct_size);
        append_u32(bytes, 0x12); // positions + normals
        append_u32(bytes, 1);    // triangles
        append_u32(bytes, 3);    // vertices
        append_u32(bytes, 1);    // morph targets
        for (int i = 0; i < 2; ++i)
            append_u32(bytes, 0); // one eight-byte triangle
        append_f32(bytes, 0);
        append_f32(bytes, 0);
        append_f32(bytes, 0);
        append_f32(bytes, 1);
        append_u32(bytes, 1);
        append_u32(bytes, 1);
        for (int i = 0; i < 18; ++i)
            append_f32(bytes, 0); // positions + normals
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto geometry = rws::decode_geometry(document.chunks()[0], document.bytes());
        CHECK(geometry);
        CHECK(geometry.value->vertex_count == 3);
        CHECK(geometry.value->triangle_count == 1);
        CHECK(geometry.value->computed_size == struct_size);
        CHECK(geometry.value->morph_targets[0].has_vertices);
        CHECK(geometry.value->morph_targets[0].has_normals);
    }
    {
        constexpr std::uint32_t struct_size = 68;
        std::vector<std::byte> bytes;
        append_header(bytes, 0x0F, 12 + struct_size);
        append_header(bytes, 0x01, struct_size);
        append_u32(bytes, 0x00020002); // positions + two explicit UV sets
        append_u32(bytes, 0);          // triangles
        append_u32(bytes, 1);          // vertices
        append_u32(bytes, 1);          // morph targets
        append_f32(bytes, 0.1F);
        append_f32(bytes, 0.2F); // UV1
        append_f32(bytes, 0.3F);
        append_f32(bytes, 0.4F); // UV2
        append_f32(bytes, 0);
        append_f32(bytes, 0);
        append_f32(bytes, 0);
        append_f32(bytes, 1);
        append_u32(bytes, 1);
        append_u32(bytes, 0);
        append_f32(bytes, 0);
        append_f32(bytes, 0);
        append_f32(bytes, 0);
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto geometry = rws::decode_geometry(document.chunks()[0], document.bytes());
        CHECK(geometry && geometry.value->texcoord_sets == 2);
        CHECK(geometry.value->texcoord_offsets.size() == 2);
        CHECK(geometry.value->texcoord_offsets[1] - geometry.value->texcoord_offsets[0] == 8);
    }
    {
        std::vector<std::byte> bytes;
        append_header(bytes, 0x050E, 32);
        append_u32(bytes, 0); // flags: triangle list
        append_u32(bytes, 1); // mesh count
        append_u32(bytes, 3); // total indices
        append_u32(bytes, 3); // entry indices
        append_u32(bytes, 2); // material
        append_u32(bytes, 0);
        append_u32(bytes, 1);
        append_u32(bytes, 2);
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto mesh = rws::decode_bin_mesh(document.chunks()[0], document.bytes());
        CHECK(mesh && mesh.value->meshes.size() == 1);
        CHECK(mesh.value->total_indices == 3);
        CHECK(mesh.value->meshes[0].material_index == 2);
    }
    {
        constexpr std::uint32_t struct_size = 58;
        constexpr std::uint32_t tree_size = 12 + struct_size;
        std::vector<std::byte> bytes;
        append_header(bytes, 0x011D, 4 + 12 + tree_size);
        append_u32(bytes, 0x37002);
        append_header(bytes, 0x2C, tree_size);
        append_header(bytes, 0x01, struct_size);
        append_u32(bytes, 1);
        append_f32(bytes, -1);
        append_f32(bytes, -2);
        append_f32(bytes, -3);
        append_f32(bytes, 4);
        append_f32(bytes, 5);
        append_f32(bytes, 6);
        append_u32(bytes, 3);
        append_u32(bytes, 1);
        bytes.push_back(std::byte{1});
        bytes.push_back(std::byte{0xFF});
        bytes.push_back(std::byte{2});
        bytes.push_back(std::byte{0});
        append_f32(bytes, 10);
        bytes.push_back(std::byte{0});
        bytes.push_back(std::byte{0xFF});
        bytes.push_back(std::byte{7});
        bytes.push_back(std::byte{0});
        append_f32(bytes, 20);
        bytes.push_back(std::byte{2});
        bytes.push_back(std::byte{0});
        bytes.push_back(std::byte{1});
        bytes.push_back(std::byte{0});
        bytes.push_back(std::byte{0});
        bytes.push_back(std::byte{0});
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto collision = rws::decode_collision_tree(document.chunks()[0], document.bytes());
        CHECK(collision && collision.value->version == 0x37002);
        CHECK(collision.value->triangle_count == 3 && collision.value->split_count == 1);
        CHECK(collision.value->bounding_box_inf.y == -2.0F &&
              collision.value->bounding_box_sup.z == 6.0F);
        CHECK(collision.value->splits[0].left.type == 1 &&
              collision.value->splits[0].left.index == 2);
        CHECK(collision.value->triangle_map.size() == 3 && collision.value->triangle_map[0] == 2);
    }
    {
        std::vector<std::byte> bytes;
        append_header(bytes, 0x24, 60);
        append_u32(bytes, 2);
        append_u32(bytes, 0x10);
        append_u32(bytes, 0x11223344);
        append_u32(bytes, 72);
        for (std::uint8_t i = 0; i < 16; ++i)
            bytes.push_back(static_cast<std::byte>(i));
        append_u32(bytes, 0x0B);
        append_u32(bytes, 0x55667788);
        append_u32(bytes, 84);
        for (std::uint8_t i = 16; i < 32; ++i)
            bytes.push_back(static_cast<std::byte>(i));
        append_header(bytes, 0x10, 0);
        append_header(bytes, 0x0B, 0);
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto contents = rws::decode_table_of_contents(document.chunks()[0], document.bytes());
        CHECK(contents && contents.value->entries.size() == 2);
        CHECK(contents.value->entries[0].chunk_type == 0x10);
        CHECK(contents.value->entries[0].object_id == 0x11223344);
        CHECK(contents.value->entries[0].offset == 72 && contents.value->entries[0].guid[15] == 15);
        CHECK(contents.value->entries[1].chunk_type == 0x0B &&
              contents.value->entries[1].offset == 84);
    }
    {
        constexpr std::uint32_t texture_payload_size = 64;
        constexpr std::uint32_t effects_payload_size = 100;
        std::vector<std::byte> bytes;
        append_header(bytes, 0x120, effects_payload_size);
        append_u32(bytes, 4); // dual-pass effect
        append_u32(bytes, 4); // dual-pass slot
        append_u32(bytes, 1); // source: zero
        append_u32(bytes, 3); // destination: source color
        append_u32(bytes, 1); // embedded texture present
        append_header(bytes, 0x06, texture_payload_size);
        append_header(bytes, 0x01, 4);
        append_u32(bytes, 0x00011106);
        append_header(bytes, 0x02, 8);
        for (const char character : std::string("TEST_Lm\0", 8))
            bytes.push_back(static_cast<std::byte>(character));
        append_header(bytes, 0x02, 4);
        append_u32(bytes, 0);
        append_header(bytes, 0x03, 0);
        append_u32(bytes, 0); // unused second effect slot
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto effects =
            rws::decode_material_effects(document.chunks()[0], 0x07, document.bytes());
        CHECK(effects && effects.value->effect_type == 4 && effects.value->slot_type == 4);
        CHECK(effects.value->has_dual_texture && effects.value->dual_texture.name == "TEST_Lm");
        CHECK(effects.value->source_blend == 1 && effects.value->destination_blend == 3);
    }
    {
        std::vector<std::byte> bytes;
        append_header(bytes, 0x011E, 32);
        append_u32(bytes, 0x100);
        append_u32(bytes, 42);
        append_u32(bytes, 1);
        append_u32(bytes, 3);
        append_u32(bytes, 36);
        append_u32(bytes, 7);
        append_u32(bytes, 0);
        append_u32(bytes, 3);
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto hierarchy = rws::decode_hanim(document.chunks()[0], document.bytes());
        CHECK(hierarchy && hierarchy.value->nodes.size() == 1);
        CHECK(hierarchy.value->hierarchy_id == 42);
        CHECK(hierarchy.value->nodes[0].node_id == 7);
    }
    {
        std::vector<std::byte> bytes;
        append_header(bytes, 0x0116, 186);
        bytes.push_back(std::byte{2}); // bones
        bytes.push_back(std::byte{2}); // used bones
        bytes.push_back(std::byte{2}); // max weights
        bytes.push_back(std::byte{0});
        bytes.push_back(std::byte{0});
        bytes.push_back(std::byte{1});
        for (int i = 0; i < 2; ++i)
            append_u32(bytes, 0x00000100U); // four packed indices per vertex
        for (int i = 0; i < 8; ++i)
            append_f32(bytes, i % 4 == 0 ? 1.0F : 0.0F);
        for (int i = 0; i < 32; ++i) {
            const auto component = i % 16;
            append_f32(bytes, component == 0 || component == 5 || component == 10 ? 1.0F : 0.0F);
        }
        append_u32(bytes, 1);
        append_u32(bytes, 0);
        append_u32(bytes, 0);
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto skin = rws::decode_skin(document.chunks()[0], 2, document.bytes());
        CHECK(skin && skin.value->bone_count == 2 && skin.value->used_bones.size() == 2);
        CHECK(skin.value->vertex_count == 2 && skin.value->trailing_split_bytes == 0);
        const auto inverse_bind = rws::decode_inverse_bind_matrices(*skin.value, document.bytes());
        CHECK(inverse_bind.size() == 2 && inverse_bind[0][3] == 0 && inverse_bind[0][7] == 0 &&
              inverse_bind[0][11] == 0 && inverse_bind[0][15] == 1);
    }
    {
        std::vector<std::byte> bytes;
        append_header(bytes, 0x011F, 51);
        append_u32(bytes, 2);
        append_u32(bytes, 4);
        bytes.push_back(std::byte{'i'});
        bytes.push_back(std::byte{'d'});
        bytes.push_back(std::byte{'\0'});
        bytes.push_back(std::byte{'\0'});
        append_u32(bytes, 1);
        append_u32(bytes, 1);
        append_u32(bytes, 42);
        append_u32(bytes, 6);
        bytes.push_back(std::byte{'l'});
        bytes.push_back(std::byte{'a'});
        bytes.push_back(std::byte{'b'});
        bytes.push_back(std::byte{'e'});
        bytes.push_back(std::byte{'l'});
        bytes.push_back(std::byte{'\0'});
        append_u32(bytes, 3);
        append_u32(bytes, 1);
        append_u32(bytes, 5);
        bytes.push_back(std::byte{'t'});
        bytes.push_back(std::byte{'e'});
        bytes.push_back(std::byte{'s'});
        bytes.push_back(std::byte{'t'});
        bytes.push_back(std::byte{'\0'});
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto data = rws::decode_user_data(document.chunks()[0], document.bytes());
        CHECK(data && data.value->arrays.size() == 2);
        CHECK(data.value->arrays[0].integers[0] == 42);
        CHECK(data.value->arrays[1].strings[0] == "test");
    }
    {
        rws::PyroExtensionInfo metadata;
        metadata.owner_type = 0x14;
        metadata.words = {0x002A0009U};
        CHECK(metadata.atomic_object_index() == 9);
        metadata.words[0] = 0x002AFFFFU;
        CHECK(!metadata.atomic_object_index());
    }
    {
        std::vector<std::byte> bytes;
        append_header(bytes, 0xFFFFFF00U, 15);
        append_u32(bytes, 1); // World Sector schema version
        append_u32(bytes, 1); // per-vertex byte array present
        bytes.push_back(std::byte{0x10});
        bytes.push_back(std::byte{0x20});
        bytes.push_back(std::byte{0x30});
        append_u32(bytes, 0x09); // first four bytes swallowed from the next header
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto metadata =
            rws::decode_pyro_extension(document.chunks()[0], 0x09, document.bytes());
        CHECK(metadata && metadata.value->present);
        CHECK(metadata.value->world_sector_vertex_bytes.size() == 3);
        CHECK(metadata.value->world_sector_vertex_bytes[2] == 0x30);
    }
    {
        std::vector<std::byte> payload;
        append_u32(payload, 2); // material schema version
        append_u32(payload, 1); // optional metadata record present
        for (const auto value : {1U, 0U, 4U, 7U, 2U})
            append_u32(payload, value);
        append_u32(payload, 7);
        for (const char character : std::string("Cemento"))
            payload.push_back(static_cast<std::byte>(character));
        std::vector<std::byte> bytes;
        append_header(bytes, 0xFFFFFF00U, static_cast<std::uint32_t>(payload.size()));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto metadata =
            rws::decode_pyro_extension(document.chunks()[0], 0x07, document.bytes());
        CHECK(metadata && metadata.value->version == 2 && metadata.value->present);
        CHECK(metadata.value->words.size() == 6 && metadata.value->strings[0] == "Cemento");
    }
    {
        std::vector<std::byte> payload;
        append_u32(payload, 0x18);
        append_u32(payload, 0x00010017);
        append_u32(payload, 0x0B);
        append_u32(payload, 3);
        append_u32(payload, 0x11);
        append_u32(payload, 0x11);
        append_u32(payload, 5);
        append_f32(payload, 3.0F);
        append_u32(payload, 5);
        append_f32(payload, 5.75F);
        append_u32(payload, 8);
        for (int i = 0; i < 12; ++i)
            append_f32(payload, i % 5 == 0 ? 1.0F : 0.0F);
        for (float value : {0.0F, 0.2F, 0.2F}) {
            append_u32(payload, 5);
            append_f32(payload, value);
        }
        append_u32(payload, 1);
        payload.push_back(std::byte{0x12});
        payload.push_back(std::byte{0});
        append_u32(payload, 1);
        payload.push_back(std::byte{0x34});
        payload.push_back(std::byte{0});
        append_u32(payload, 5);
        append_f32(payload, 3.0F);
        append_u32(payload, 9);
        append_u32(payload, 6);
        append_f32(payload, 1);
        append_f32(payload, 2);
        append_f32(payload, 3);
        append_u32(payload, 7);
        append_f32(payload, 0);
        append_f32(payload, 0);
        append_f32(payload, 0);
        append_f32(payload, 1);
        for (float value : {1.0F, 0.25F, 0.5F}) {
            append_u32(payload, 5);
            append_f32(payload, value);
        }
        append_u32(payload, 6);
        append_f32(payload, 1);
        append_f32(payload, 0);
        append_f32(payload, 0);
        append_u32(payload, 3);
        append_u32(payload, 3);
        append_u32(payload, 6);
        append_f32(payload, -1);
        append_f32(payload, 0.25F);
        append_f32(payload, 0.5F);
        std::vector<std::byte> bytes;
        append_header(bytes, 0x907, static_cast<std::uint32_t>(12 + payload.size()), 0x1C020018);
        append_header(bytes, 1, static_cast<std::uint32_t>(payload.size()), 0x1C020018);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto body = rws::decode_physics_body_def(document.chunks()[0], document.bytes());
        CHECK(body && body.value->volume.kind == 0x11 && body.value->mass == 3.0F);
        CHECK(body.value->volume.cylinder_radius == 3.0F);
        CHECK(body.value->volume.cylinder_half_height == 5.75F);
        CHECK(body.value->volume.fatness == 0.0F);
        CHECK(body.value->volume.friction == 0.2F && body.value->volume.restitution == 0.2F);
        CHECK(body.value->volume.flags == 0x12 && body.value->volume.collision_group == 0x34);
        CHECK(body.value->principal_inertia.x == 1.0F && body.value->flags == 3);
        CHECK(body.value->linear_damping == 0.25F && body.value->angular_damping == 0.5F);
        CHECK(body.value->finite_rotation_axis.x == 1.0F);
        CHECK(rws::has_physics_body_flag(body.value->flags,
                                         rws::PhysicsBodyFlag::finite_rotation_axis));
        CHECK(rws::physics_body_flag_names(body.value->flags) ==
              "finite-rotation axis, oriented inertia");
        CHECK(body.value->center_of_mass.x == -1.0F);
    }
    {
        std::vector<std::byte> payload;
        append_u32(payload, 0x18);
        append_u32(payload, 0x00010017);
        append_u32(payload, 0x0B);
        append_u32(payload, 3);
        append_u32(payload, 0x13);
        append_u32(payload, 0x13);
        append_u32(payload, 0x00010015);
        append_u32(payload, 3);
        append_u32(payload, 1);
        append_u32(payload, 0x00010017);
        append_u32(payload, 0x0B);
        append_u32(payload, 3);
        append_u32(payload, 0x0E);
        append_u32(payload, 0x0E);
        append_u32(payload, 8);
        for (int i = 0; i < 12; ++i)
            append_f32(payload, i % 5 == 0 ? 1.0F : 0.0F);
        for (float value : {2.0F, 0.3F, 0.1F}) {
            append_u32(payload, 5);
            append_f32(payload, value);
        }
        append_u32(payload, 1);
        append_u16(payload, 0);
        append_u32(payload, 1);
        append_u16(payload, 0);
        append_u32(payload, 5);
        append_f32(payload, 12.0F);
        append_u32(payload, 6);
        append_f32(payload, 1.0F);
        append_f32(payload, 2.0F);
        append_f32(payload, 3.0F);
        append_u32(payload, 9);
        append_u32(payload, 6);
        append_f32(payload, 4.0F);
        append_f32(payload, 5.0F);
        append_f32(payload, 6.0F);
        append_u32(payload, 7);
        append_f32(payload, 0.0F);
        append_f32(payload, 0.0F);
        append_f32(payload, 0.0F);
        append_f32(payload, 1.0F);
        append_u32(payload, 8);
        for (int i = 0; i < 12; ++i)
            append_f32(payload, i % 5 == 0 ? 1.0F : 0.0F);
        for (float value : {0.0F, 0.4F, 0.2F}) {
            append_u32(payload, 5);
            append_f32(payload, value);
        }
        append_u32(payload, 1);
        append_u16(payload, 0);
        append_u32(payload, 1);
        append_u16(payload, 0);
        append_u32(payload, 5);
        append_f32(payload, 12.0F);
        append_u32(payload, 9);
        append_u32(payload, 6);
        append_f32(payload, 4.0F);
        append_f32(payload, 5.0F);
        append_f32(payload, 6.0F);
        append_u32(payload, 7);
        append_f32(payload, 0.0F);
        append_f32(payload, 0.0F);
        append_f32(payload, 0.0F);
        append_f32(payload, 1.0F);
        for (float value : {1.0F, 0.0F, 0.0F}) {
            append_u32(payload, 5);
            append_f32(payload, value);
        }
        append_u32(payload, 6);
        append_f32(payload, 0.0F);
        append_f32(payload, 0.0F);
        append_f32(payload, 0.0F);
        append_u32(payload, 3);
        append_u32(payload, 0);
        append_u32(payload, 6);
        append_f32(payload, 0.0F);
        append_f32(payload, 0.0F);
        append_f32(payload, 0.0F);
        std::vector<std::byte> bytes;
        append_header(bytes, 0x907, static_cast<std::uint32_t>(12 + payload.size()), 0x1C020018);
        append_header(bytes, 1, static_cast<std::uint32_t>(payload.size()), 0x1C020018);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto body = rws::decode_physics_body_def(document.chunks()[0], document.bytes());
        CHECK(body && body.value->volume.kind == 0x13 && body.value->volume.children.size() == 1);
        CHECK(body.value->volume.trilist_mass == 12.0F);
        CHECK(body.value->volume.trilist_center_of_mass->y == 2.0F);
        CHECK(body.value->volume.trilist_principal_inertia->z == 6.0F);
        CHECK((*body.value->volume.trilist_inertia_orientation)[3] == 1.0F);
    }
    {
        std::vector<std::byte> first, second;
        append_world_sector(first, 0, 0, 3, {{{0, 1, 2, 0}}});
        append_world_sector(second, 0, 1, 3, {{{2, 1, 0, 0}}});
        auto bytes = make_world(0, 2, 2, 6, 2, {first, second});
        const auto original = bytes;
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto worlds = rws::recover_worlds(document.chunks(), document.bytes());
        CHECK(worlds.size() == 1);
        const auto& world = worlds.front();
        CHECK(world.status == rws::WorldRecoveryStatus::complete);
        CHECK(world.sectors.size() == 2 && world.recovered_triangles == 2 &&
              world.recovered_vertices == 6 && world.material_count == 2);
        CHECK(world.sectors[1].material_window_base == 1);
        CHECK(std::equal(original.begin(), original.end(), document.bytes().begin()));
    }
    {
        constexpr std::uint32_t format = 0x00020018U; // normals, prelight, two UV sets
        std::vector<std::byte> sector;
        append_world_sector(sector, format, 0, 3, {{{0, 1, 2, 0}}});
        const auto document = rws::Document::from_bytes(make_world(format, 1, 1, 3, 1, {sector}));
        const auto world = rws::recover_world(document.chunks()[0], document.bytes());
        CHECK(world.status == rws::WorldRecoveryStatus::complete && world.sectors.size() == 1);
        const auto& recovered = world.sectors.front();
        CHECK(recovered.normals_offset != 0 && recovered.prelight_offset != 0);
        CHECK(recovered.texcoord_offsets.size() == 2);
        CHECK(recovered.prelight_offset - recovered.normals_offset == 12);
        CHECK(recovered.texcoord_offsets[1] - recovered.texcoord_offsets[0] == 24);
        CHECK(recovered.triangles_offset - recovered.texcoord_offsets[1] == 24);
    }
    {
        std::vector<std::byte> sector;
        append_world_sector(sector, 0, 2, 3, {{{0, 1, 9, 0}}});
        const auto document = rws::Document::from_bytes(make_world(0, 2, 2, 6, 1, {sector}));
        const auto world = rws::recover_world(document.chunks()[0], document.bytes());
        CHECK(world.status == rws::WorldRecoveryStatus::partial);
        CHECK(world.sectors.size() == 1 && world.invalid_triangles == 1);
        CHECK(world.invalid_material_references == 0); // invalid vertices take precedence
        CHECK(!world.diagnostics.empty());
    }
    {
        std::vector<std::byte> malformed;
        append_world_sector(malformed, 0, 0, -1, {});
        const auto document = rws::Document::from_bytes(make_world(0, 1, 0, 0, 1, {malformed}));
        const auto world = rws::recover_world(document.chunks()[0], document.bytes());
        CHECK(world.status == rws::WorldRecoveryStatus::failed);
        CHECK(world.sectors.empty() && world.invalid_candidates == 1);
    }
    {
        std::vector<std::byte> sector;
        append_world_sector(sector, 0, 1, 3, {{{0, 1, 2, 0}}});
        const auto document = rws::Document::from_bytes(make_world(0, 1, 1, 3, 1, {sector}));
        const auto world = rws::recover_world(document.chunks()[0], document.bytes());
        CHECK(world.status == rws::WorldRecoveryStatus::partial);
        CHECK(world.invalid_material_references == 1 && world.invalid_triangles == 0);
    }
    {
        std::vector<std::byte> sector;
        append_world_sector(sector, 0, 0, 3, {{{0, 1, 2, 0}}});
        // A complete header signature inside vertex bytes must not be scanned after
        // the enclosing sector's exact Struct range has been accepted.
        const auto write_u32 = [&](const std::size_t offset, const std::uint32_t value) {
            for (unsigned shift = 0; shift < 32; shift += 8)
                sector[offset + shift / 8U] = static_cast<std::byte>((value >> shift) & 0xFFU);
        };
        write_u32(68, 0x09);
        write_u32(76, 0x1C020037);
        write_u32(80, 0x01);
        write_u32(88, 0x1C020037);
        const auto document = rws::Document::from_bytes(make_world(0, 1, 1, 3, 1, {sector}));
        const auto world = rws::recover_world(document.chunks()[0], document.bytes());
        CHECK(world.status == rws::WorldRecoveryStatus::complete);
        CHECK(world.sectors.size() == 1 && world.invalid_candidates == 0);
    }
    {
        std::vector<std::byte> plane, left, right;
        append_plane(plane, 0, true, true, 1.0F, 4.0F, -1.0F);
        append_world_sector(left, 0, 0, 3, {{{0, 1, 2, 0}}});
        append_world_sector(right, 0, 0, 3, {{{0, 1, 2, 0}}});
        const auto bytes = make_world(0, 2, 2, 6, 1, {plane, left, right}, 1, false);
        const auto& original = bytes;
        const auto document = rws::Document::from_bytes(bytes);
        const auto world = rws::recover_world(document.chunks()[0], document.bytes());
        CHECK(world.topology_status == rws::WorldTopologyStatus::complete);
        CHECK(world.planes.size() == 1 && world.topology_nodes.size() == 3);
        CHECK(world.topology_root == 0 && world.planes[0].left_node == 1 &&
              world.planes[0].right_node == 2);
        CHECK(world.topology_nodes[1].parent == 0 && world.topology_nodes[1].is_left_child == true);
        CHECK(world.topology_nodes[2].parent == 0 &&
              world.topology_nodes[2].is_left_child == false);
        CHECK(world.topology_stats.maximum_depth == 1 && world.topology_stats.linked_sectors == 2);
        CHECK(std::equal(original.begin(), original.end(), document.bytes().begin()));
    }
    {
        std::vector<std::byte> root, branch, a, b, c;
        append_plane(root, 0, false, true, 1.0F, 4.0F, -1.0F);
        append_plane(branch, 8, true, true, 0.0F, 6.0F, -3.0F);
        append_world_sector(a, 0, 0, 3, {{{0, 1, 2, 0}}});
        append_world_sector(b, 0, 0, 3, {{{0, 1, 2, 0}}});
        append_world_sector(c, 0, 0, 3, {{{0, 1, 2, 0}}});
        const auto document =
            rws::Document::from_bytes(make_world(0, 3, 3, 9, 1, {root, branch, a, b, c}, 2, false));
        const auto world = rws::recover_world(document.chunks()[0], document.bytes());
        CHECK(world.topology_status == rws::WorldTopologyStatus::complete);
        CHECK(world.topology_nodes.size() == 5 && world.topology_stats.maximum_depth == 2);
        CHECK(world.topology_nodes[4].parent == 0 &&
              world.topology_nodes[4].is_left_child == false);
    }
    {
        std::vector<std::byte> invalid, sector;
        append_plane(invalid, 3, true, true, std::numeric_limits<float>::infinity(), 4.0F, -1.0F);
        append_world_sector(sector, 0, 0, 3, {{{0, 1, 2, 0}}});
        const auto document =
            rws::Document::from_bytes(make_world(0, 1, 1, 3, 1, {invalid, sector}, 1, false));
        const auto world = rws::recover_world(document.chunks()[0], document.bytes());
        CHECK(world.status == rws::WorldRecoveryStatus::complete && world.sectors.size() == 1);
        CHECK(world.topology_status == rws::WorldTopologyStatus::failed);
        CHECK(world.topology_stats.invalid_candidates == 1 ||
              world.topology_stats.ambiguous_candidates == 1);
    }
    {
        std::vector<std::byte> sector;
        append_world_sector(sector, 0, 0, 3, {{{0, 1, 2, 0}}});
        // Positions begin 68 bytes into this synthetic Atomic Section.
        for (const auto [offset, value] :
             std::array<std::pair<std::size_t, float>, 9>{{{68, 0.0F},
                                                           {72, 0.0F},
                                                           {76, 0.0F},
                                                           {80, 1.0F},
                                                           {84, 0.0F},
                                                           {88, 0.0F},
                                                           {92, 0.0F},
                                                           {96, 1.0F},
                                                           {100, 0.0F}}})
            write_f32(sector, offset, value);
        const auto bytes = make_world(0, 1, 1, 3, 1, {sector});
        const auto& original = bytes;
        const auto document = rws::Document::from_bytes(bytes);
        const auto worlds = rws::recover_worlds(document.chunks(), document.bytes());
        const auto vertex =
            rws::decode_recovered_world_vertex(worlds[0].sectors[0], 2, document.bytes());
        CHECK(vertex && vertex.value->x == 0 && vertex.value->y == 1);
        CHECK(!rws::decode_recovered_world_vertex(worlds[0].sectors[0], -1, document.bytes()));
        CHECK(!rws::decode_recovered_world_vertex(worlds[0].sectors[0], 3, document.bytes()));
        const auto triangle =
            rws::decode_recovered_world_triangle_resolved(worlds[0], 0, 0, document.bytes());
        CHECK(triangle && triangle.value->source_offset == worlds[0].sectors[0].triangles_offset);
        CHECK(!rws::decode_recovered_world_triangle_resolved(worlds[0], 1, 0, document.bytes()));
        const rws::CollisionRay ray{{0.25F, 0.25F, 1.0F}, {0, 0, -1}};
        const auto hit = rws::pick_collision_worlds(worlds, document.bytes(), ray);
        CHECK(hit && hit->sector_index == 0 && hit->triangle_index == 0 && hit->material_slot == 0);
        CHECK(std::abs(hit->position.z) < 1.0e-6F && hit->geometric_normal.z > 0.99F);
        CHECK(std::abs(hit->barycentric[0] - 0.5F) < 1.0e-5F);
        const std::array clips{rws::CollisionClipPlane{true, 0, true, 0.5F}};
        CHECK(!rws::pick_collision_worlds(worlds, document.bytes(), ray, clips));
        // One surface between the ray origin and the target counts as one wall.
        CHECK(rws::count_collision_walls(worlds, document.bytes(), ray, 2.0F, 0.01F) == 1);
        CHECK(rws::count_collision_walls(worlds, document.bytes(), ray, 0.5F, 0.01F) == 0);
        CHECK(rws::count_collision_walls(worlds, document.bytes(), ray, 2.0F, 0.01F, clips) == 0);
        const auto measurement = rws::measure_points({0, 0, 0}, {3, 4, 12});
        CHECK(measurement.distance == 13 && measurement.absolute_delta.y == 4);
        CHECK(std::equal(original.begin(), original.end(), document.bytes().begin()));

        const auto directory = std::filesystem::temp_directory_path() / "rws-man-s02-tests";
        std::filesystem::create_directories(directory);
        const auto gltf = directory / "collision.gltf";
        const auto obj = directory / "collision.obj";
        const auto gltf_stats =
            rws::export_collision_gltf(document.chunks(), document.bytes(), gltf);
        const auto obj_stats = rws::export_collision_obj(document.chunks(), document.bytes(), obj);
        CHECK(gltf_stats.triangles == 1 && obj_stats.triangles == 1 &&
              obj_stats.skipped_triangles == 0);
        const auto read_text = [](const std::filesystem::path& path) {
            std::ifstream input(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), {});
        };
        const auto gltf_text = read_text(gltf), obj_text = read_text(obj);
        CHECK(gltf_text.find("rws_source_offset") != std::string::npos);
        CHECK(gltf_text.find("rws_sector_index") != std::string::npos);
        CHECK(gltf_text.find("NORMAL") != std::string::npos);
        CHECK(obj_text.find("world_0_sector_0") != std::string::npos);
        CHECK(obj_text.find("usemtl world_0_collision_material_0") != std::string::npos);
        CHECK(std::equal(original.begin(), original.end(), document.bytes().begin()));
        std::filesystem::remove_all(directory);
    }
    {
        const std::string source =
            "// retained comment\n"
            "ExternalShape {\n"
            "  Shape = Box\n"
            "  Center = 1 2 3\n"
            "  Dimensions = 4 -5 6\n"
            "  BoneIndex = 7\n"
            "  Label = \"torso\"\n"
            "  FutureField = keep_me\n"
            "}\n"
            "Internal { Shape = Sphere\n Radius = 2\n HotPoint = 8 9 10\n }\n";
        const auto cmo = csf::CmoDocument::parse(source, "synthetic.cmo");
        CHECK(cmo.source() == source);
        CHECK(std::ranges::any_of(cmo.tokens(), [](const auto& token) {
            return token.kind == csf::CmoTokenKind::comment;
        }));
        CHECK(cmo.shapes().size() == 2);
        CHECK(cmo.shapes()[0].kind == csf::CmoShapeKind::box && cmo.shapes()[0].external);
        CHECK(cmo.shapes()[0].center && cmo.shapes()[0].center->z == 3);
        CHECK(cmo.shapes()[0].bone_index == 7 && cmo.shapes()[0].label == "torso");
        CHECK(std::ranges::any_of(cmo.diagnostics(), [](const auto& value) {
            return value.code == "negative-shape-size";
        }));
        CHECK(cmo.shapes()[1].kind == csf::CmoShapeKind::sphere &&
              cmo.shapes()[1].hot_points.size() == 1);
        CHECK(cmo.validate_bones(7).size() == 1);
        CHECK(cmo.text(cmo.shapes()[0].range).find("ExternalShape") != std::string_view::npos);
    }
    {
        rws::PhysicsVolumeInfo root;
        root.kind = 0x13;
        rws::PhysicsVolumeInfo box;
        box.kind = 0x10;
        box.box_half_extents = rws::Vec3{1, 2, 3};
        box.fatness = 0.5F;
        box.matrix = {1, 0, 0, 0, 1, 0, 0, 0, 1, 10, 0, 0};
        root.children.push_back(box);
        const auto bounds = rws::physics_local_bounds(root);
        CHECK(bounds.valid && bounds.minimum.x == 8.5F && bounds.maximum.x == 11.5F);
        const auto flattened = rws::flatten_physics_volumes(root);
        CHECK(flattened.size() == 2 && flattened[1].path == "0/0");
        const auto comparison =
            rws::compare_bounds({{-1, -1, -1}, {1, 1, 1}, true}, {{0, -2, -1}, {2, 2, 1}, true});
        CHECK(comparison.center_delta.x == 1 && comparison.extent_delta.y == 2 &&
              comparison.gross_volume_ratio == 2);
        rws::FrameListInfo frames;
        frames.frames.resize(2);
        frames.frames[0].rotation = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        frames.frames[0].parent = -1;
        frames.frames[0].position = {1, 0, 0};
        frames.frames[1].rotation = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        frames.frames[1].parent = 0;
        frames.frames[1].position = {0, 2, 0};
        const auto transforms = rws::skeleton_rest_transforms(frames);
        CHECK(transforms.size() == 2 && transforms[1].values[9] == 1 &&
              transforms[1].values[10] == 2);
    }
    {
        SyntheticCsf source;
        source.container("", 1, 2);
        source.container(".OBJETOS", 2);
        source.container("", 4, 2);
        source.integer(".ID", 55);
        source.string(".NOMBRE", "guard");
        source.string(".MODELO", "Models/Guard.dff");
        source.string(".MODELO_COLISION", "Models/Guard.cmo");
        source.container("", 3, 2);
        source.integer(".CLASSID", 55);
        source.integer(".ID", 101);
        source.string(".MODEL_FILE", "Models/Guard.rws");
        const auto database =
            csf::ObjectDatabase::project(csf::Document::from_bytes(source.bytes()));
        CHECK(database.definitions().size() == 2 && database.find_class(55).size() == 2);
        CHECK(std::ranges::any_of(database.diagnostics(), [](const auto& value) {
            return value.code == "duplicate-object-class";
        }));
        const auto directory =
            std::filesystem::temp_directory_path() / "rws-man-p04-association-tests";
        write_bytes(directory / "Models" / "Guard.rpc", {std::byte{1}});
        write_bytes(directory / "Models" / "Guard.cmo", {std::byte{2}});
        write_bytes(directory / "Models" / "Guard.rws", {std::byte{3}});
        csf::ResourceIndex resources;
        resources.add_root(directory);
        resources.build();
        const auto scene =
            csf::MissionScene::project(csf::Document::from_bytes(make_typed_scene()));
        const auto associations = csf::associate_actors(scene, database, resources);
        CHECK(associations.size() == 2 && associations[0].definitions.size() == 2);
        CHECK(associations[0].visual_models.size() == 1 &&
              associations[0].visual_models[0].resolution.status ==
                  csf::ResolutionStatus::mapped_dff_to_rpc);
        CHECK(associations[0].collision_models.size() == 1 &&
              associations[0].physics_models.size() == 1);
        std::filesystem::remove_all(directory);
    }
    {
        // Standard HAnim interpolation type 1: two interleaved tracks with two keys each.
        std::vector<std::byte> payload;
        append_u32(payload, 0x100);
        append_u32(payload, 1);
        append_u32(payload, 4);
        append_u32(payload, 0);
        append_f32(payload, 1.0F);
        const auto key = [&](float time, float x, float qz, float qw, std::uint32_t previous) {
            append_f32(payload, time);
            append_f32(payload, 0);
            append_f32(payload, 0);
            append_f32(payload, qz);
            append_f32(payload, qw);
            append_f32(payload, x);
            append_f32(payload, 0);
            append_f32(payload, 0);
            append_u32(payload, previous);
        };
        key(0, 0, 0, 1, 0xFF30C9D8U);
        key(0, 0, 0, 1, 0xFF30C9D8U);
        key(1, 10, 0, -1, 0);
        key(1, 0, 0, 1, 36);
        std::vector<std::byte> bytes;
        append_header(bytes, 0x1B, static_cast<std::uint32_t>(payload.size()));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        const auto document = rws::Document::from_bytes(bytes);
        auto clip = rws::decode_animation(document.chunks()[0], document.bytes());
        CHECK(clip.valid() && clip.layout == rws::AnimationLayout::hanim_uncompressed_36);
        CHECK(clip.tracks.size() == 2 && clip.tracks[0].keyframes.size() == 2 &&
              clip.keyframes[2].previous_keyframe == 0);
        rws::FrameListInfo frames;
        frames.frames.resize(2);
        for (auto& f : frames.frames)
            f.rotation = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        frames.frames[0].parent = -1;
        frames.frames[1].parent = 0;
        frames.frames[1].position = {0, 2, 0};
        rws::HAnimInfo hierarchy;
        hierarchy.nodes = {{100, 0, 0}, {200, 1, 0}};
        const auto compatibility = rws::map_animation_tracks(clip, hierarchy, frames.frames.size());
        CHECK(compatibility.compatible && clip.tracks[1].node_id == 200);
        // Frame List order is not HAnim matrix order. Frame 0 has no HAnim
        // node, matrix 0 maps to frame 1, and matrix 1 maps to frame 3.
        std::vector<std::byte> frame_payload;
        std::vector<std::byte> frame_struct;
        append_u32(frame_struct, 4);
        for (int i = 0; i < 4; ++i) {
            for (const float value : {1.F, 0.F, 0.F, 0.F, 1.F, 0.F, 0.F, 0.F, 1.F})
                append_f32(frame_struct, value);
            append_f32(frame_struct, 0);
            append_f32(frame_struct, 0);
            append_f32(frame_struct, 0);
            append_u32(frame_struct, i == 0 ? 0xFFFFFFFFU : static_cast<std::uint32_t>(i - 1));
            append_u32(frame_struct, 0);
        }
        append_header(frame_payload, 0x01, static_cast<std::uint32_t>(frame_struct.size()));
        frame_payload.insert(frame_payload.end(), frame_struct.begin(), frame_struct.end());
        const auto append_frame_extension = [&](const std::optional<std::int32_t> id,
                                                const bool root = false) {
            std::vector<std::byte> extension;
            if (id) {
                std::vector<std::byte> hanim;
                append_u32(hanim, 0x100);
                append_u32(hanim, static_cast<std::uint32_t>(*id));
                append_u32(hanim, root ? 2U : 0U);
                if (root) {
                    append_u32(hanim, 0);
                    append_u32(hanim, 36);
                    append_u32(hanim, 100);
                    append_u32(hanim, 0);
                    append_u32(hanim, 0);
                    append_u32(hanim, 200);
                    append_u32(hanim, 1);
                    append_u32(hanim, 0);
                }
                append_header(extension, 0x11E, static_cast<std::uint32_t>(hanim.size()));
                extension.insert(extension.end(), hanim.begin(), hanim.end());
            }
            append_header(frame_payload, 0x03, static_cast<std::uint32_t>(extension.size()));
            frame_payload.insert(frame_payload.end(), extension.begin(), extension.end());
        };
        append_frame_extension(std::nullopt);
        append_frame_extension(100, true);
        append_frame_extension(300);
        append_frame_extension(200);
        std::vector<std::byte> frame_bytes;
        append_header(frame_bytes, 0x0E, static_cast<std::uint32_t>(frame_payload.size()));
        frame_bytes.insert(frame_bytes.end(), frame_payload.begin(), frame_payload.end());
        const auto frame_document = rws::Document::from_bytes(frame_bytes);
        const auto binding =
            rws::decode_hanim_binding(frame_document.chunks()[0], frame_document.bytes());
        CHECK(binding && binding.value->complete() &&
              binding.value->matrix_to_frame == std::vector<std::int32_t>({1, 3}));
        auto rebound_clip = clip;
        const auto rebound = rws::map_animation_tracks(rebound_clip, *binding.value, 4);
        CHECK(rebound.compatible && rebound_clip.tracks[0].frame_index == 1 &&
              rebound_clip.tracks[1].frame_index == 3);
        const auto rebound_frames =
            rws::decode_frame_list(frame_document.chunks()[0], frame_document.bytes());
        const auto rebound_pose =
            rws::evaluate_pose(rebound_clip, *rebound_frames.value, .5F, false);
        CHECK(std::abs(rebound_pose.local[1].translation.x - 5) < 1e-5F &&
              std::abs(rebound_pose.local[0].translation.x) < 1e-5F);
        auto incompatible_clip = clip;
        const auto incompatible = rws::map_animation_tracks(incompatible_clip, hierarchy, 1);
        CHECK(!incompatible.compatible && !incompatible.diagnostics.empty());
        const auto middle = rws::evaluate_pose(clip, frames, 0.5F, false);
        CHECK(std::abs(middle.local[0].translation.x - 5) < 1e-5F &&
              std::abs(middle.local[0].rotation.w - 1) < 1e-5F);
        CHECK(std::abs(middle.world[1][12] - 5) < 1e-5F && std::abs(middle.world[1][13]) < 1e-5F);
        CHECK(rws::evaluate_pose(clip, frames, -1, false).sampled_time == 0);
        CHECK(std::abs(rws::evaluate_pose(clip, frames, 1.25F, true).sampled_time - 0.25F) < 1e-5F);
        const auto motion = rws::extract_root_motion(clip, 3);
        CHECK(motion.size() == 3 && std::abs(motion[1].x - 5) < 1e-5F);
        const std::vector identity{
            std::array<float, 16>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
        auto translated = identity;
        translated[0][12] = 3;
        rws::SkinVertex vertex;
        vertex.position = {1, 2, 3};
        vertex.normal = {0, 1, 0};
        vertex.weights[0] = 1;
        const auto skinned = rws::cpu_skin(std::span(&vertex, 1), identity, translated);
        CHECK(skinned.size() == 1 && skinned[0].position.x == 4 && skinned[0].normal.y == 1);
        // RenderWare inverse binds include the atomic transform.  Recovering the
        // bind bone must make bind-time skinning exactly reproduce the static
        // atomic path, including a non-identity rotation.
        rws::FrameListInfo bind_frames;
        bind_frames.frames.resize(1);
        bind_frames.frames[0].rotation = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        bind_frames.frames[0].parent = -1;
        rws::HAnimBinding bind_mapping;
        bind_mapping.matrix_to_frame = {0};
        const std::array<float, 16> atomic_bind{0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 10, 20, 0, 1};
        const std::vector<std::array<float, 16>> baked_inverse_bind{
            {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 7, 16, 0, 1}};
        const auto recovered =
            rws::recover_skin_bind_pose(bind_frames, bind_mapping, baked_inverse_bind, atomic_bind);
        CHECK(recovered && std::abs(recovered.value->world[0][12] - 3) < 1e-5F &&
              std::abs(recovered.value->world[0][13] - 4) < 1e-5F);
        rws::AnimationClip empty_clip;
        const auto bind_pose =
            rws::evaluate_pose(empty_clip, bind_frames, recovered.value->local, 0, false);
        rws::SkinVertex bind_vertex;
        bind_vertex.position = {2, 0, 0};
        bind_vertex.normal = {1, 0, 0};
        bind_vertex.weights[0] = 1;
        const auto bind_skinned =
            rws::cpu_skin(std::span(&bind_vertex, 1), baked_inverse_bind, bind_pose.world);
        CHECK(bind_skinned.size() == 1 && std::abs(bind_skinned[0].position.x - 10) < 1e-5F &&
              std::abs(bind_skinned[0].position.y - 22) < 1e-5F);

        // A partial clip drives the parent while an untracked child retains its
        // recovered bind-local offset and follows the animated hierarchy.
        rws::FrameListInfo partial_frames;
        partial_frames.frames.resize(2);
        for (auto& f : partial_frames.frames)
            f.rotation = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        partial_frames.frames[0].parent = -1;
        partial_frames.frames[1].parent = 0;
        rws::HAnimBinding partial_mapping;
        partial_mapping.matrix_to_frame = {0, 1};
        const std::vector<std::array<float, 16>> partial_inverse_bind{
            {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -1, 0, 0, 1},
            {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -1, -2, 0, 1}};
        const std::array<float, 16> identity_matrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const auto partial_bind = rws::recover_skin_bind_pose(
            partial_frames, partial_mapping, partial_inverse_bind, identity_matrix);
        CHECK(partial_bind);
        rws::AnimationClip partial_clip;
        partial_clip.keyframes.push_back(
            {0, 0, {0, 0, 0, 1}, {5, 0, 0}, 0, -1, 0, std::nullopt, {}});
        partial_clip.tracks.push_back({0, std::nullopt, 0, {0}});
        const auto partial_pose =
            rws::evaluate_pose(partial_clip, partial_frames, partial_bind.value->local, 0, false);
        rws::SkinVertex child_vertex;
        child_vertex.weights[0] = 1;
        child_vertex.bones[0] = 1;
        const auto partial_skinned =
            rws::cpu_skin(std::span(&child_vertex, 1), partial_inverse_bind,
                          std::array{partial_pose.world[0], partial_pose.world[1]});
        CHECK(partial_skinned.size() == 1 && std::abs(partial_skinned[0].position.x - 4) < 1e-5F &&
              std::abs(partial_skinned[0].position.y) < 1e-5F);
        const auto directory =
            std::filesystem::temp_directory_path() / "rws-man-p05-animation-tests";
        std::filesystem::create_directories(directory);
        const auto output = directory / "clip.gltf";
        rws::export_animation_gltf(clip, frames, hierarchy, {}, output);
        CHECK(std::filesystem::exists(output) && std::filesystem::exists(directory / "clip.bin") &&
              std::filesystem::exists(directory / "clip.manifest.json"));
        std::ifstream gltf(output);
        const std::string text(std::istreambuf_iterator<char>(gltf), {});
        CHECK(text.find("\"animations\"") != std::string::npos &&
              text.find("inverseBindMatrices") != std::string::npos);
        gltf.close();
        std::filesystem::remove_all(directory);
    }
    {
        // Invalid forward previous links and non-finite transforms are hard validation errors.
        std::vector<std::byte> payload;
        append_u32(payload, 0x100);
        append_u32(payload, 1);
        append_u32(payload, 1);
        append_u32(payload, 0);
        append_f32(payload, 1);
        append_f32(payload, 0);
        append_f32(payload, std::numeric_limits<float>::quiet_NaN());
        append_f32(payload, 0);
        append_f32(payload, 0);
        append_f32(payload, 1);
        append_f32(payload, 0);
        append_f32(payload, 0);
        append_f32(payload, 0);
        append_u32(payload, 36);
        std::vector<std::byte> bytes;
        append_header(bytes, 0x1B, static_cast<std::uint32_t>(payload.size()));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        const auto document = rws::Document::from_bytes(bytes);
        const auto clip = rws::decode_animation(document.chunks()[0], document.bytes());
        CHECK(!clip.valid());
        CHECK(std::ranges::any_of(
            clip.diagnostics, [](const auto& value) { return value.code == "invalid-previous"; }));
        CHECK(std::ranges::any_of(clip.diagnostics, [](const auto& value) {
            return value.code == "non-finite-transform";
        }));
    }
    {
        // Compressed type 2 uses Criterion's 1/4/11 scalar and a translation trailer.
        std::vector<std::byte> payload;
        append_u32(payload, 0x100);
        append_u32(payload, 2);
        append_u32(payload, 1);
        append_u32(payload, 0);
        append_f32(payload, 0);
        append_f32(payload, 0);
        append_u16(payload, 0);
        append_u16(payload, 0);
        append_u16(payload, 0);
        append_u16(payload, 0x7800);
        append_u16(payload, 0);
        append_u16(payload, 0);
        append_u16(payload, 0);
        append_u32(payload, 0xFF30C9D8U);
        for (float v : {1, 2, 3, 4, 5, 6})
            append_f32(payload, v);
        std::vector<std::byte> bytes;
        append_header(bytes, 0x1B, static_cast<std::uint32_t>(payload.size()));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        const auto document = rws::Document::from_bytes(bytes);
        const auto clip = rws::decode_animation(document.chunks()[0], document.bytes());
        CHECK(clip.valid() && clip.keyframes.size() == 1 && clip.keyframes[0].rotation.w == 1);
        CHECK(clip.keyframes[0].translation.x == 1 && clip.translation_scale.z == 6);
    }
    {
        std::vector<std::byte> payload;
        append_u32(payload, 0x100);
        append_u32(payload, 99);
        append_u32(payload, 1);
        append_u32(payload, 0);
        append_f32(payload, 1);
        append_u32(payload, 0x12345678);
        std::vector<std::byte> bytes;
        append_header(bytes, 0x1B, static_cast<std::uint32_t>(payload.size()));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        const auto document = rws::Document::from_bytes(bytes);
        const auto clip = rws::decode_animation(document.chunks()[0], document.bytes());
        CHECK(!clip.supported() && clip.keyframes.empty() && !clip.trailing_bytes.empty());
    }
    {
        SyntheticCsf data;
        data.container("", 7, 2);
        data.container(".MUNDOVIS", 1, 2);
        data.container(".MTACTICO_ORIGEN", 3);
        data.real("", 1);
        data.real("", 2);
        data.real("", 3);
        data.string(".MAPA_SECTORES", "Maps/Test.png");
        data.integer(".PUNTUACION_MAXIMA", 100);
        data.integer(".PUNTUACION_MINIMA", 10);
        data.container(".MALLA_SCENE_OBJS", 1, 2);
        data.container(".SCENEOBJS", 1);
        data.container("", 4, 2);
        data.string(".ID", "WINDMILL");
        data.integer(".ANIMACION", 1378);
        data.integer(".TIPO_OFFSET", 2);
        data.real(".OFFSET", 0.25F);
        data.container(".BRIDGES", 1);
        data.container("", 3, 2);
        data.string(".VISUALRWS", "Maps/Test.rws");
        data.string(".PHYSICRWS", "Maps/Test_col.rws");
        data.container(".CONTROL_POINTS", 1);
        data.container("", 5, 2);
        data.integer(".TYPE", 1);
        data.container(".P1", 3);
        data.real("", 1); data.real("", 2); data.real("", 3);
        data.container(".P2", 3);
        data.real("", 4); data.real("", 5); data.real("", 6);
        data.real(".HEIGHT", 50);
        data.string(".SCN", "Maps/Other.scn");
        data.container(".AGUAS", 1);
        data.container("", 2, 2);
        data.string(".NOMBRE", "water");
        data.string(".AGUA_TEXNM", "Gfx/normal.png");
        const auto scene = csf::MissionScene::project(csf::Document::from_bytes(data.bytes()));
        CHECK(scene.environment().size() == 1 &&
              std::get<csf::Vec3>(scene.environment()[0].value).z == 3);
        CHECK(scene.metadata().sector_map == "Maps/Test.png" &&
              scene.metadata().maximum_score == 100 && scene.metadata().minimum_score == 10);
        CHECK(scene.scene_objects().size() == 1 &&
              scene.scene_objects()[0].animation_id == 1378 &&
              scene.scene_objects()[0].offset_type == 2);
        CHECK(scene.bridges().size() == 1 && scene.bridges()[0].control_points.size() == 1 &&
              scene.bridges()[0].control_points[0].target_scene == "Maps/Other.scn");
        CHECK(scene.waters().size() == 1 && scene.waters()[0].fields.size() == 2);
        const auto json = csf::mission_scene_json(scene);
        CHECK(json.find("scene_objects") != std::string::npos &&
              json.find("Maps/Other.scn") != std::string::npos);
    }
    {
        // Programs retain every ordered and nested operand and use lexical variable scope.
        SyntheticCsf data;
        data.container("", 2, 2);
        data.container(".VARIABLES", 1);
        data.container("", 4, 2);
        data.integer(".ID", 7);
        data.string(".TYPE", "NUMERO");
        data.string(".NOMBRE", "global");
        data.real(".VALOR", 1.5F);
        data.container(".SCRIPTS", 1);
        data.container("", 8, 2);
        data.integer(".ID", 99);
        data.string(".NOMBRE", "sample");
        data.string(".CARPETA", "Folder/Case");
        data.container(".FLAGS", 3, 2);
        data.integer(".TRIGGER", 1);
        data.integer(".ENABLED", 0);
        data.integer(".VALIDO", 1);
        data.container(".VARIABLES", 1);
        data.container("", 5, 2);
        data.integer(".ID", 7);
        data.integer(".ARRAY", 1);
        data.string(".TYPE", "BICHO");
        data.string(".NOMBRE", "local");
        data.integer(".VALOR", 10);
        data.container(".EVENTOS", 1);
        data.container("", 1);
        data.string("", "START_GAME");
        data.container(".CONDICIONES", 1);
        data.container("", 2);
        data.string("", "IF");
        data.container("", 2);
        data.string("", "VAR");
        data.integer("", 7);
        data.container(".ACCIONES", 3);
        data.container("", 4);
        data.string("", "PLAY_SONIDOID");
        data.container("", 2);
        data.string("", "SONIDO_BDD");
        data.integer("", 123);
        data.container("", 2);
        data.string("", "NUMERO");
        data.real("", 0.5F);
        data.container("", 2);
        data.string("", "NUMERO");
        data.real("", 0.75F);
        data.container("", 1);
        data.string("", "ENDIF");
        data.container("", 2);
        data.string("", "CAMARA_EN_DUMMY");
        data.container("", 2);
        data.string("", "DUMMY");
        data.integer("", 20);
        const auto program = csf::ProgramDocument::project(csf::Document::from_bytes(data.bytes()));
        CHECK(program.global_variables().size() == 1 && program.scripts().size() == 1);
        const auto& script = program.scripts()[0];
        CHECK(script.id == 99 && script.folder == "Folder/Case" && script.flags.trigger == true &&
              script.flags.enabled == false && script.local_variables.size() == 1 &&
              script.events[0].name == "START_GAME");
        CHECK(script.actions.size() == 3 && script.actions[0].operands.size() == 3 &&
              script.actions[0].operands[1].tag == "NUMERO" &&
              script.actions[0].operands[2].tag == "NUMERO");
        const auto json = csf::program_json(program);
        CHECK(json.find("Folder/Case") != std::string::npos &&
              json.find("PLAY_SONIDOID") != std::string::npos);
        csf::ProgramReferenceIndex references;
        references.add_program(program);
        CHECK(std::ranges::any_of(references.references(), [](const auto& value) {
            return value.kind == csf::ProgramReferenceKind::variable &&
                   value.status == csf::ProgramReferenceStatus::resolved &&
                   value.targets.size() == 1;
        }));
    }
    {
        SyntheticCsf animations;
        animations.container("", 1, 2);
        animations.container(".ANIMACIONES", 1);
        animations.container("", 7, 2);
        animations.integer(".ID", 1378);
        animations.string(".NOMBRE", "walk");
        animations.string(".FICHERO_ANIM", "Anims/Walk.anm");
        animations.integer(".LOOP", 1);
        animations.real(".BLEND_IN", 0.2F);
        animations.string(".MODELO", "Guard.rpc");
        animations.integer(".SNDID", 7); // Outside FILES: kept on the record only.
        const auto catalog_document = csf::Document::from_bytes(animations.bytes());
        const auto catalog = csf::AnimationCatalog::project(catalog_document);
        CHECK(catalog.records().size() == 1 && catalog.records()[0].id == 1378 &&
              catalog.records()[0].logical_name == "walk" &&
              catalog.records()[0].variants.size() == 1 && catalog.records()[0].loop == true);
        CHECK(catalog.records()[0].sounds.size() == 1 &&
              catalog.records()[0].sounds[0].logical_id == "7" &&
              catalog.records()[0].variants[0].sounds.empty());
        CHECK(catalog.find_id(1378) == &catalog.records()[0]);
        CHECK(catalog.compatible("Models/Guard.rpc").size() == 1);
        CHECK(catalog.compatible("Models/Other.rpc").empty());
        SyntheticCsf gsc;
        gsc.container("", 1, 2);
        gsc.container(".SCRIPTS", 1);
        gsc.container("", 3, 2);
        gsc.integer(".ID", 99);
        gsc.string(".NOMBRE", "actor-script");
        gsc.container(".ACCIONES", 1);
        gsc.container("", 3, 2);
        gsc.string("", "PLAY_ANMBDD");
        gsc.container("", 1, 2);
        gsc.string("", "THIS");
        gsc.container("", 2, 2);
        gsc.string("", "ANM_BDD");
        gsc.integer("", 1378);
        csf::ScriptAnimationIndex script_animations;
        script_animations.add_document(csf::Document::from_bytes(gsc.bytes()));
        const std::array<std::int32_t, 1> script_ids{99};
        const auto assigned = script_animations.for_actor(script_ids, 10);
        CHECK(assigned.size() == 1 && assigned[0]->animation_id == 1378 &&
              assigned[0]->targets_this && assigned[0]->script_name == "actor-script");
        SyntheticCsf cutscene;
        cutscene.container("", 1, 2);
        cutscene.container(".SCRIPTS", 2);
        cutscene.container("", 2, 2);
        cutscene.string(".NOMBRE", "intro");
        cutscene.container(".ACCIONES", 5);
        cutscene.container("", 2, 2);
        cutscene.string("", "CAMERA_DUMMY");
        cutscene.real(".FOV", 70);
        cutscene.container("", 1, 2);
        cutscene.string("", "WAIT_CONDITION");
        cutscene.container("", 2, 2);
        cutscene.string("", "ANIMATION");
        cutscene.real(".TIME", 1.5F);
        cutscene.container("", 1, 2);
        cutscene.string("", "CONTINUE");
        cutscene.container("", 2, 2);
        cutscene.string("", "PLAY_SOUND");
        cutscene.integer(".ID", 999);
        cutscene.container("", 2, 2);
        cutscene.string(".NOMBRE", "intro");
        cutscene.container(".ACCIONES", 1);
        cutscene.container("", 1, 2);
        cutscene.string("", "END");
        const auto cutscene_document = csf::Document::from_bytes(cutscene.bytes());
        const auto timeline = csf::CutsceneTimeline::project(cutscene_document);
        CHECK(timeline.scripts().size() == 2);
        const auto& script = timeline.scripts().front();
        CHECK(std::ranges::any_of(script.actions, [](const auto& a) {
            return a.kind == csf::CutsceneActionKind::camera;
        }));
        CHECK(std::ranges::any_of(script.blocks, [](const auto& b) { return b.runtime_wait; }));
        CHECK(std::ranges::any_of(script.blocks, [](const auto& b) { return b.conditional; }));
        CHECK(script.actions[0].numeric_value == 70 && !script.actions[0].explicit_time);
        CHECK(script.actions[2].explicit_time == 1.5F);
        CHECK(std::ranges::any_of(timeline.diagnostics(), [](const auto& value) {
            return value.code == "duplicate-cutscene-script";
        }));
        CHECK(timeline.scripts()[0].source.entry_index != timeline.scripts()[1].source.entry_index);
    }
    {
        SyntheticCsf scene;
        scene.container("", 1, 2);
        scene.container(".BICHOS", 1);
        scene.container("", 6, 2);
        scene.string(".NOMBRE", "scripted");
        scene.integer(".ID", 10);
        scene.integer(".CLASSID", 55);
        scene.position(0, 0, 0);
        scene.container(".SCRIPT", 2);
        scene.integer("", 99);
        scene.integer("", 100);
        scene.container(".CELDA", 2, 2);
        scene.integer(".GRUPO", 1);
        scene.integer(".PUNTO", 2);
        const auto projected = csf::MissionScene::project(csf::Document::from_bytes(scene.bytes()));
        CHECK(projected.actors().size() == 1 && projected.actors()[0].script_ids.size() == 2);
        CHECK(projected.actors()[0].script_ids[0] == 99 &&
              projected.actors()[0].script_ids[1] == 100);
    }
    {
        // The PNG decoder is portable and shared with the GUI preview.
        const std::array<std::uint8_t, 77> png_bytes{
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
            0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
            0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xB6, 0x0D, 0x24, 0x00, 0x00, 0x00,
            0x14, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
            0x1F, 0x0C, 0x81, 0x34, 0x10, 0x30, 0x34, 0x00, 0x00, 0x47, 0x4B, 0x08,
            0x79, 0xC3, 0x25, 0x87, 0xEB, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,
            0x44, 0xAE, 0x42, 0x60, 0x82};
        std::vector<std::byte> png(png_bytes.size());
        std::ranges::transform(png_bytes, png.begin(), [](const std::uint8_t value) {
            return static_cast<std::byte>(value);
        });
        int width = 0, height = 0;
        std::vector<std::uint8_t> rgba;
        std::string error;
        CHECK(rws::decode_png(png, width, height, rgba, error));
        CHECK(width == 2 && height == 2 && rgba.size() == 16);
        CHECK(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255);
        CHECK(rgba[4] == 0 && rgba[5] == 255 && rgba[6] == 0 && rgba[7] == 255);
        CHECK(rgba[8] == 0 && rgba[9] == 0 && rgba[10] == 255 && rgba[11] == 255);
        CHECK(rgba[12] == 255 && rgba[13] == 255 && rgba[14] == 0 && rgba[15] == 128);
        // Truncated data must fail cleanly and report an error.
        CHECK(!rws::decode_png(std::span(png).first(20), width, height, rgba, error));
        CHECK(!error.empty());
    }
    {
        // A byte-oriented uncompressed 32-bit DDS exercises the moved decoder.
        std::vector<std::byte> dds;
        const auto put_u32 = [&dds](const std::uint32_t value) { append_u32(dds, value); };
        put_u32(0x20534444);  // "DDS " magic
        put_u32(124);         // header size
        put_u32(0);           // flags
        put_u32(2);           // height
        put_u32(2);           // width
        put_u32(0);           // pitch/linear size
        put_u32(0);           // depth
        put_u32(0);           // mipmap count
        for (int i = 0; i < 11; ++i) put_u32(0);  // reserved
        put_u32(32);          // pixel format size
        put_u32(0x41);        // DDPF_RGB | DDPF_ALPHAPIXELS
        put_u32(0);           // fourcc (empty for uncompressed)
        put_u32(32);          // rgb bit count
        put_u32(0x00FF0000);  // red mask
        put_u32(0x0000FF00);  // green mask
        put_u32(0x000000FF);  // blue mask
        put_u32(0xFF000000);  // alpha mask
        put_u32(0x1000);      // caps
        for (int i = 0; i < 4; ++i) put_u32(0);  // caps2..4 and reserved2
        put_u32(0xFFFF0000);  // red
        put_u32(0xFF00FF00);  // green
        put_u32(0xFF0000FF);  // blue
        put_u32(0x80FFFF00);  // translucent yellow
        const auto path =
            std::filesystem::temp_directory_path() / "rws-man-texture-tests" / "sample.dds";
        write_bytes(path, dds);
        int width = 0, height = 0;
        std::vector<std::uint8_t> rgba;
        std::string error;
        CHECK(rws::decode_texture_image(path, width, height, rgba, error));
        CHECK(width == 2 && height == 2 && rgba.size() == 16);
        CHECK(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255);
        CHECK(rgba[4] == 0 && rgba[5] == 255 && rgba[6] == 0 && rgba[7] == 255);
        CHECK(rgba[8] == 0 && rgba[9] == 0 && rgba[10] == 255 && rgba[11] == 255);
        CHECK(rgba[12] == 255 && rgba[13] == 255 && rgba[14] == 0 && rgba[15] == 128);
        std::filesystem::remove(path);
    }
    return 0;
}
