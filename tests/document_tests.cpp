#include "csf/animation_catalog.hpp"
#include "csf/cmo.hpp"
#include "csf/document.hpp"
#include "csf/export.hpp"
#include "csf/mission.hpp"
#include "csf/mission_scene.hpp"
#include "csf/object_database.hpp"
#include "csf/overlay.hpp"
#include "rws/animation.hpp"
#include "rws/decoded.hpp"
#include "rws/document.hpp"
#include "rws/obj_export.hpp"
#include "rws/physics_inspection.hpp"
#include "rws/scene_export.hpp"
#include "rws/world_recovery.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
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
    csf.container("", with_unknown ? 7U : 6U, 2);
    csf.integer(".PLAYER", 0);
    csf.container(".BICHOS", 2);
    csf.container("", 9, 2);
    csf.string(".NOMBRE", "duplicate");
    csf.integer(".ID", 10);
    csf.integer(".CLASSID", 55);
    csf.position(1, 2, 3);
    csf.real(".ANGULO", actor_heading);
    csf.integer(".FLAGS", 0);
    csf.string(".SCRIPT", "OnSpawn");
    csf.integer(".FUTURE", 99);
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

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): CHECK failures intentionally unwind
int main() {
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
              scene.areas()[0].height == 100);
        csf::MissionSymbolIndex symbols;
        symbols.add_scene(scene);
        CHECK(symbols.exact("duplicate").size() == 2);
        CHECK(symbols.exact("OnSpawn").size() == 1 &&
              symbols.exact("OnSpawn")[0]->role == csf::SymbolRole::typed_reference);
        const auto json = csf::mission_scene_json(scene);
        CHECK(json == csf::mission_scene_json(scene));
        CHECK(json.find("csf-mission-scene-1") != std::string::npos);
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
        CHECK(scene.actors()[0].unknown_fields.size() == 3);
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
        SyntheticCsf animations;
        animations.container("", 1, 2);
        animations.container(".ANIMACIONES", 1);
        animations.container("", 6, 2);
        animations.integer(".ID", 1378);
        animations.string(".NOMBRE", "walk");
        animations.string(".FICHERO_ANIM", "Anims/Walk.anm");
        animations.integer(".LOOP", 1);
        animations.real(".BLEND_IN", 0.2F);
        animations.string(".MODELO", "Guard.rpc");
        const auto catalog_document = csf::Document::from_bytes(animations.bytes());
        const auto catalog = csf::AnimationCatalog::project(catalog_document);
        CHECK(catalog.records().size() == 1 && catalog.records()[0].id == 1378 &&
              catalog.records()[0].logical_name == "walk" &&
              catalog.records()[0].variants.size() == 1 && catalog.records()[0].loop == true);
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
    return 0;
}
