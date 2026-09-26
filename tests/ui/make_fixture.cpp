// Writes the synthetic mission the UI tests open (docs/plans/editor-ux-redesign.md,
// T6): a small package with a generated World (rolling terrain, no textures),
// a player, three guards, a patrol route, cover points, a zone, a dummy and a
// light, and two scripts. It contains no game data, so the committed UI
// references can be rendered from it anywhere.
//
//   rwsman_ui_fixture <output directory>
//
// writes <output>/Fixture/Maps/M1/M1.scn (and the rest of the package).
#include "csf/source_text.hpp"
#include "csf/tree.hpp"
#include "rws/world_model.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
}

void append_header(std::vector<std::byte>& bytes, const std::uint32_t type, const std::uint32_t size) {
    append_u32(bytes, type);
    append_u32(bytes, size);
    append_u32(bytes, 0x1C020037U);
}

void append_string(std::vector<std::byte>& bytes, const std::string& value) {
    append_u32(bytes, static_cast<std::uint32_t>(value.size()));
    for (const char c : value) bytes.push_back(static_cast<std::byte>(c));
}

std::vector<std::byte> compile(const std::string_view text) {
    csf::Tree tree;
    const std::string prefix("CSFFBS\0\x7F", 8);
    std::ranges::transform(prefix, tree.prefix.begin(), [](const char c) { return static_cast<std::byte>(c); });
    tree.version = 1;
    tree.roots = csf::parse_source_text(text);
    return tree.serialize();
}

bool write(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

// Terrain height (cm) at (x, z): a gentle rise towards the north-east.
float height(const float x, const float z) {
    return 120.0F * std::sin(x / 1400.0F) * std::cos(z / 1700.0F) + 0.02F * (x + z);
}

// An untextured material with an RGBA colour (Material struct: flags, colour,
// unused, textured, ambient, specular, diffuse).
std::vector<std::byte> material(const std::uint32_t rgba) {
    std::vector<std::byte> chunk;
    append_header(chunk, 0x07, 12 + 28);
    append_header(chunk, 0x01, 28);
    append_u32(chunk, 0);
    append_u32(chunk, rgba);
    append_u32(chunk, 0);
    append_u32(chunk, 0);
    for (const float value : {1.0F, 1.0F, 1.0F}) append_u32(chunk, std::bit_cast<std::uint32_t>(value));
    return chunk;
}

std::vector<std::byte> build_world(const bool visual) {
    constexpr int tiles = 32;
    constexpr float size = 250.0F, origin = -tiles * size / 2.0F;
    std::vector<rws::WorldBuildTriangle> triangles;
    const auto vertex = [&](const float x, const float z) {
        rws::WorldBuildVertex v;
        v.position = {x, height(x, z), z};
        // Normal from the height field's slope.
        const float dx = height(x + 10.0F, z) - height(x - 10.0F, z);
        const float dz = height(x, z + 10.0F) - height(x, z - 10.0F);
        const float length = std::sqrt(dx * dx + 400.0F + dz * dz);
        v.normal = {-dx / length, 20.0F / length, -dz / length};
        // Prelight: brighter on higher ground.
        const auto shade = static_cast<std::uint32_t>(std::clamp(170.0F + v.position.y * 0.4F, 90.0F, 255.0F));
        v.prelight = 0xFF000000U | shade << 16U | shade << 8U | shade;
        v.texcoords[0] = {x / 500.0F, z / 500.0F};
        return v;
    };
    for (int i = 0; i < tiles; ++i)
        for (int j = 0; j < tiles; ++j) {
            const float x0 = origin + static_cast<float>(i) * size, z0 = origin + static_cast<float>(j) * size;
            const auto a = vertex(x0, z0), b = vertex(x0, z0 + size), c = vertex(x0 + size, z0 + size),
                       d = vertex(x0 + size, z0);
            const auto tile_material = static_cast<std::uint16_t>((i / 4 + j / 4) % 2);
            triangles.push_back({{a, b, c}, tile_material, 0});
            triangles.push_back({{a, c, d}, tile_material, 0});
        }
    rws::WorldBuildOptions options;
    const std::vector<std::vector<std::byte>> materials{material(0xFF4F7A5AU), material(0xFF5C8A62U)};
    options.material_list = rws::compose_material_list(materials, options.library_id);
    options.format = visual ? 0x400200B8U : 0x40000040U;
    options.visual_plugins = visual;
    options.max_sector_triangles = 512;
    const auto built = rws::build_world(triangles, options);
    if (!built) {
        std::fprintf(stderr, "cannot build the fixture World: %s\n", built.error.c_str());
        std::exit(1);
    }
    return rws::write_world_model(*built.value);
}

std::string pos(const float x, const float z, const float lift = 0.0F) {
    char text[96];
    std::snprintf(text, sizeof(text), "(%.1f %.1f %.1f)", x, height(x, z) + lift, z);
    return text;
}

} // namespace

int main(const int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: rwsman_ui_fixture <output directory>\n");
        return 2;
    }
    const auto package = std::filesystem::path(argv[1]) / "Fixture";
    std::error_code error;
    std::filesystem::remove_all(package, error);
    const auto maps = package / "Maps" / "M1";

    const std::string scene = R"([
  .VERSION 17
  .PLAYER 1
  .MUNDOVIS [ .RWS "Maps\\M1\\world.rws" .FOGDISTANCE 20000.0 .INICIO_COMMANDO 1 .INICIO_SNIPER 0 .INICIO_SPY 0 ]
  .BICHOS (
    [ .NOMBRE COMMANDO .ID 1 .CLASSID 12 .POS )" + pos(-2400, -2400) + R"( .ANGULO 45.0 .ANGULO_X 0.0 .COLISION 1 .FLAGS 0 .SEGUNDA_EXPLOSION 0 ]
    [ .NOMBRE GUARD_GATE .ID 5 .CLASSID 10 .POS )" + pos(600, 300) + R"( .ANGULO 90.0 .ANGULO_X 0.0 .COLISION 1 .FLAGS 0 .SEGUNDA_EXPLOSION 0 .SCRIPT (7) ]
    [ .NOMBRE GUARD_PATROL .ID 6 .CLASSID 10 .POS )" + pos(-800, 1200) + R"( .ANGULO 0.0 .ANGULO_X 0.0 .COLISION 1 .FLAGS 0 .SEGUNDA_EXPLOSION 0 ]
    [ .NOMBRE OFFICER .ID 7 .CLASSID 11 .POS )" + pos(1500, 1600) + R"( .ANGULO 200.0 .ANGULO_X 0.0 .COLISION 1 .FLAGS 0 .SEGUNDA_EXPLOSION 0 ]
  )
  .EFECTOS ( )
  .PUNTUACION_MAXIMA 2000
  .PUNTUACION_MINIMA 1000
  .MALLA_NAVEGACION [
    .GRUPOS (
      [ .ID 1 .NOMBRE ROUTE .TIPO 0 .PUNTOS (
          [ .ID 1 .NOMBRE "" .POS )" + pos(-800, 1200) + R"( .ROT 0.0 .ROT_X 0.0 ]
          [ .ID 2 .NOMBRE "" .POS )" + pos(800, 1200) + R"( .ROT 1.5707964 .ROT_X 0.0 ]
          [ .ID 3 .NOMBRE "" .POS )" + pos(800, -600) + R"( .ROT 3.1415927 .ROT_X 0.0 ]
          [ .ID 4 .NOMBRE "" .POS )" + pos(-800, -600) + R"( .ROT -1.5707964 .ROT_X 0.0 ] )
        .CONEXIONES ( [ .PUNTO_ORI 1 .PUNTO_DST 2 ] [ .PUNTO_ORI 2 .PUNTO_DST 3 ] [ .PUNTO_ORI 3 .PUNTO_DST 4 ]
                      [ .PUNTO_ORI 4 .PUNTO_DST 1 ] ) ]
      [ .ID 2 .NOMBRE COVER .TIPO 3 .PUNTOS (
          [ .ID 1 .NOMBRE "" .POS )" + pos(1200, 500) + R"( .ROT 1.5707964 .ROT_X 0.0 ]
          [ .ID 2 .NOMBRE "" .POS )" + pos(1200, 900) + R"( .ROT 1.5707964 .ROT_X 0.0 ] )
        .CONEXIONES ( ) ]
    )
    .CONEXIONES ( )
  ]
  .MALLA_DUMMIES [ .DUMMIES ( [ .ID 2 .NOMBRE "" .POS )" + pos(-2000, -1800, 180) + R"( .ROT 0.7 .ROT_X 0.0 ] )
    .CARPETAS [ .RAIZ ( [ .NOMBRE "" .ELEMENTOS (2) ] ) ] ]
  .MALLA_AREAS [ .AREAS ( [ .ID 1 .FLAGS 1 .OCLUSION 1 .NOMBRE YARD .HEIGHT 300.0 .REVERB 0 .LIMITREVERB 0
    .PUNTOS ( [ .POS )" + pos(1000, 1000) + R"( ] [ .POS )" + pos(2200, 1000) + R"( ] [ .POS )" + pos(2200, 2200) + R"( ]
              [ .POS )" + pos(1000, 2200) + R"( ] ) ] ) ]
  .MALLA_LUCES [ .LIGHTS ( [ .ID 9 .NOMBRE "" .POS )" + pos(1600, 1600, 250) + R"( .COLOR 16777215 .MODULATE 0 .RADIO 800.0 ] )
    .CARPETAS [ .RAIZ ( [ .NOMBRE "" .ELEMENTOS (9) ] ) ] ]
])";
    const std::string program = R"([
  .RECURSOS [ .ANIMACIONES (100) .CLASSID ( ) ]
  .VARIABLES ( )
  .SCRIPTS (
    [ .ID 7 .NOMBRE GUARD_IDLE .CARPETA "" .FLAGS [ .TRIGGER 0 .ENABLED 1 .VALIDO 1 ] .EVENTOS ( (INIT) )
      .ACCIONES {
        WHILE (BOOL TRUE)
          PLAY_ANMBDD (THIS) (ANM_BDD 100)
        WEND
      } ]
    [ .ID 8 .NOMBRE ENTER_YARD .CARPETA "" .FLAGS [ .TRIGGER 1 .ENABLED 1 .VALIDO 1 ] .EVENTOS ( (START_GAME) )
      .ACCIONES {
        SEND_EVENT (EVENT INIT)
      } ]
  )
  .POOL ( )
])";
    const std::string objects = R"([ .VERSION 10 .LISTADATOS (
  [ .ID 10 .NOMBRE Soldier .TIPO ALEMAN .COMPOR SOLDADO .HOMBRE 1 .MODELO "Models\\Char\\Soldier.dff" ]
  [ .ID 11 .NOMBRE Officer .TIPO ALEMAN .COMPOR SOLDADO .HOMBRE 1 .MODELO "Models\\Char\\Officer.dff" ]
  [ .ID 12 .NOMBRE Commando .TIPO ALIADO .COMPOR COMANDO .HOMBRE 1 .MODELO "Models\\Char\\Commando.dff" ]
  [ .ID 20 .NOMBRE Crate .TIPO OBJETO .COMPOR OBJETO .HOMBRE 0 .MODELO "Models\\Props\\Crate.dff" ]
) ])";
    const std::string animations = R"([ .VERSION 1 .LISTADATOS (
  [ .ID 100 .NOMBRE idle .FILE "Anims\\Comm\\idle.anm" ]
) ])";

    std::vector<std::byte> vis;
    append_string(vis, "Maps\\M1\\world.rws");
    append_string(vis, "Maps\\M1\\world_col.rws");
    append_string(vis, "Maps\\M1\\Textures");
    append_string(vis, "");

    const bool ok = write(maps / "M1.scn", compile(scene)) && write(maps / "M1.gsc", compile(program)) &&
                    write(maps / "M1.vis", vis) && write(maps / "world.rws", build_world(true)) &&
                    write(maps / "world_col.rws", build_world(false)) &&
                    write(package / "BDD" / "Objetos.bdd", compile(objects)) &&
                    write(package / "BDD" / "Anims.bdd", compile(animations)) &&
                    write(package / "BDD" / "Armas.bdd", compile("[ .LISTADATOS ( ) ]"));
    if (!ok) {
        std::fprintf(stderr, "cannot write the fixture under %s\n", package.string().c_str());
        return 1;
    }
    std::printf("%s\n", (maps / "M1.scn").string().c_str());
    return 0;
}
