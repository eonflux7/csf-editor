#include "geometry_preview.hpp"
#include "rws/world_recovery.hpp"

#include <imgui.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_map>

namespace rwsman {
namespace {

using GlSizePtr = std::ptrdiff_t;
using CreateShaderProc = GLuint (APIENTRY*)(GLenum);
using ShaderSourceProc = void (APIENTRY*)(GLuint, GLsizei, const char* const*, const GLint*);
using CompileShaderProc = void (APIENTRY*)(GLuint);
using GetShaderIvProc = void (APIENTRY*)(GLuint, GLenum, GLint*);
using GetShaderInfoLogProc = void (APIENTRY*)(GLuint, GLsizei, GLsizei*, char*);
using DeleteShaderProc = void (APIENTRY*)(GLuint);
using CreateProgramProc = GLuint (APIENTRY*)();
using AttachShaderProc = void (APIENTRY*)(GLuint, GLuint);
using LinkProgramProc = void (APIENTRY*)(GLuint);
using GetProgramIvProc = void (APIENTRY*)(GLuint, GLenum, GLint*);
using GetProgramInfoLogProc = void (APIENTRY*)(GLuint, GLsizei, GLsizei*, char*);
using DeleteProgramProc = void (APIENTRY*)(GLuint);
using UseProgramProc = void (APIENTRY*)(GLuint);
using GenVertexArraysProc = void (APIENTRY*)(GLsizei, GLuint*);
using BindVertexArrayProc = void (APIENTRY*)(GLuint);
using DeleteVertexArraysProc = void (APIENTRY*)(GLsizei, const GLuint*);
using GenBuffersProc = void (APIENTRY*)(GLsizei, GLuint*);
using BindBufferProc = void (APIENTRY*)(GLenum, GLuint);
using BufferDataProc = void (APIENTRY*)(GLenum, GlSizePtr, const void*, GLenum);
using DeleteBuffersProc = void (APIENTRY*)(GLsizei, const GLuint*);
using EnableVertexAttribArrayProc = void (APIENTRY*)(GLuint);
using VertexAttribPointerProc = void (APIENTRY*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using GetUniformLocationProc = GLint (APIENTRY*)(GLuint, const char*);
using Uniform1iProc = void (APIENTRY*)(GLint, GLint);
using Uniform1fProc = void (APIENTRY*)(GLint, GLfloat);
using Uniform2fProc = void (APIENTRY*)(GLint, GLfloat, GLfloat);
using Uniform3fProc = void (APIENTRY*)(GLint, GLfloat, GLfloat, GLfloat);
using Uniform4fProc = void (APIENTRY*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
using ActiveTextureProc = void (APIENTRY*)(GLenum);
using GenerateMipmapProc = void (APIENTRY*)(GLenum);

constexpr GLenum gl_vertex_shader = 0x8B31;
constexpr GLenum gl_fragment_shader = 0x8B30;
constexpr GLenum gl_compile_status = 0x8B81;
constexpr GLenum gl_link_status = 0x8B82;
constexpr GLenum gl_array_buffer = 0x8892;
constexpr GLenum gl_static_draw = 0x88E4;
constexpr GLenum gl_texture0 = 0x84C0;
constexpr GLenum gl_texture1 = gl_texture0 + 1;
constexpr GLenum gl_texture_max_anisotropy = 0x84FE;
constexpr GLenum gl_max_texture_max_anisotropy = 0x84FF;

struct GlApi {
    CreateShaderProc create_shader{};
    ShaderSourceProc shader_source{};
    CompileShaderProc compile_shader{};
    GetShaderIvProc get_shader_iv{};
    GetShaderInfoLogProc get_shader_log{};
    DeleteShaderProc delete_shader{};
    CreateProgramProc create_program{};
    AttachShaderProc attach_shader{};
    LinkProgramProc link_program{};
    GetProgramIvProc get_program_iv{};
    GetProgramInfoLogProc get_program_log{};
    DeleteProgramProc delete_program{};
    UseProgramProc use_program{};
    GenVertexArraysProc gen_vertex_arrays{};
    BindVertexArrayProc bind_vertex_array{};
    DeleteVertexArraysProc delete_vertex_arrays{};
    GenBuffersProc gen_buffers{};
    BindBufferProc bind_buffer{};
    BufferDataProc buffer_data{};
    DeleteBuffersProc delete_buffers{};
    EnableVertexAttribArrayProc enable_vertex_attrib_array{};
    VertexAttribPointerProc vertex_attrib_pointer{};
    GetUniformLocationProc get_uniform_location{};
    Uniform1iProc uniform_1i{};
    Uniform1fProc uniform_1f{};
    Uniform2fProc uniform_2f{};
    Uniform3fProc uniform_3f{};
    Uniform4fProc uniform_4f{};
    ActiveTextureProc active_texture{};
    GenerateMipmapProc generate_mipmap{};

    bool load() {
#define LOAD_GL(member, name) member = reinterpret_cast<decltype(member)>(glfwGetProcAddress(name)); if (!member) return false
        LOAD_GL(create_shader, "glCreateShader");
        LOAD_GL(shader_source, "glShaderSource");
        LOAD_GL(compile_shader, "glCompileShader");
        LOAD_GL(get_shader_iv, "glGetShaderiv");
        LOAD_GL(get_shader_log, "glGetShaderInfoLog");
        LOAD_GL(delete_shader, "glDeleteShader");
        LOAD_GL(create_program, "glCreateProgram");
        LOAD_GL(attach_shader, "glAttachShader");
        LOAD_GL(link_program, "glLinkProgram");
        LOAD_GL(get_program_iv, "glGetProgramiv");
        LOAD_GL(get_program_log, "glGetProgramInfoLog");
        LOAD_GL(delete_program, "glDeleteProgram");
        LOAD_GL(use_program, "glUseProgram");
        LOAD_GL(gen_vertex_arrays, "glGenVertexArrays");
        LOAD_GL(bind_vertex_array, "glBindVertexArray");
        LOAD_GL(delete_vertex_arrays, "glDeleteVertexArrays");
        LOAD_GL(gen_buffers, "glGenBuffers");
        LOAD_GL(bind_buffer, "glBindBuffer");
        LOAD_GL(buffer_data, "glBufferData");
        LOAD_GL(delete_buffers, "glDeleteBuffers");
        LOAD_GL(enable_vertex_attrib_array, "glEnableVertexAttribArray");
        LOAD_GL(vertex_attrib_pointer, "glVertexAttribPointer");
        LOAD_GL(get_uniform_location, "glGetUniformLocation");
        LOAD_GL(uniform_1i, "glUniform1i");
        LOAD_GL(uniform_1f, "glUniform1f");
        LOAD_GL(uniform_2f, "glUniform2f");
        LOAD_GL(uniform_3f, "glUniform3f");
        LOAD_GL(uniform_4f, "glUniform4f");
        LOAD_GL(active_texture, "glActiveTexture");
        LOAD_GL(generate_mipmap, "glGenerateMipmap");
#undef LOAD_GL
        return true;
    }
};

struct AffineTransform {
    std::array<float, 9> rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
    rws::Vec3 position{};
};

AffineTransform frame_transform(const rws::FrameInfo& frame) {
    // RwMatrix stores right/up/at vectors; convert those basis columns to a
    // conventional row-major matrix for point multiplication below.
    return {{{frame.rotation[0], frame.rotation[3], frame.rotation[6],
              frame.rotation[1], frame.rotation[4], frame.rotation[7],
              frame.rotation[2], frame.rotation[5], frame.rotation[8]}}, frame.position};
}

rws::Vec3 rotate(const AffineTransform& transform, const rws::Vec3 value) {
    const auto& m = transform.rotation;
    return {m[0] * value.x + m[1] * value.y + m[2] * value.z,
            m[3] * value.x + m[4] * value.y + m[5] * value.z,
            m[6] * value.x + m[7] * value.y + m[8] * value.z};
}

rws::Vec3 transform_point(const AffineTransform& transform, const rws::Vec3 value) {
    const auto rotated = rotate(transform, value);
    return {rotated.x + transform.position.x, rotated.y + transform.position.y,
            rotated.z + transform.position.z};
}

rws::Vec3 inverse_transform_point(const AffineTransform& transform, const rws::Vec3 value) {
    const auto& m = transform.rotation;
    const rws::Vec3 translated{value.x - transform.position.x, value.y - transform.position.y,
                               value.z - transform.position.z};
    const float c00 = m[4] * m[8] - m[5] * m[7];
    const float c01 = m[2] * m[7] - m[1] * m[8];
    const float c02 = m[1] * m[5] - m[2] * m[4];
    const float determinant = m[0] * c00 + m[1] * (m[5] * m[6] - m[3] * m[8]) +
                              m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (std::abs(determinant) < 1.0e-8F) return translated;
    const float inverse = 1.0F / determinant;
    return {(c00 * translated.x + c01 * translated.y + c02 * translated.z) * inverse,
            ((m[5] * m[6] - m[3] * m[8]) * translated.x +
             (m[0] * m[8] - m[2] * m[6]) * translated.y +
             (m[2] * m[3] - m[0] * m[5]) * translated.z) * inverse,
            ((m[3] * m[7] - m[4] * m[6]) * translated.x +
             (m[1] * m[6] - m[0] * m[7]) * translated.y +
             (m[0] * m[4] - m[1] * m[3]) * translated.z) * inverse};
}

AffineTransform compose(const AffineTransform& parent, const AffineTransform& local) {
    AffineTransform result;
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 3; ++column) {
            result.rotation[row * 3 + column] = 0.0F;
            for (unsigned k = 0; k < 3; ++k)
                result.rotation[row * 3 + column] +=
                    parent.rotation[row * 3 + k] * local.rotation[k * 3 + column];
        }
    result.position = transform_point(parent, local.position);
    return result;
}

GlApi& gl_api() {
    static GlApi api;
    static const bool loaded = api.load();
    (void)loaded;
    return api;
}

float read_f32(const std::span<const std::byte> bytes, const std::uint64_t offset) {
    std::uint32_t raw{};
    for (unsigned i = 0; i < 4; ++i)
        raw |= std::to_integer<std::uint32_t>(bytes[static_cast<std::size_t>(offset + i)]) << (i * 8U);
    return std::bit_cast<float>(raw);
}

std::uint32_t read_span_u32(const std::span<const std::byte> bytes, const std::uint64_t offset) {
    std::uint32_t result{};
    for (unsigned i = 0; i < 4; ++i)
        result |= std::to_integer<std::uint32_t>(bytes[static_cast<std::size_t>(offset + i)]) << (i * 8U);
    return result;
}

std::uint16_t read_span_u16(const std::span<const std::byte> bytes, const std::uint64_t offset) {
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes[static_cast<std::size_t>(offset)]) |
        (std::to_integer<std::uint16_t>(bytes[static_cast<std::size_t>(offset + 1)]) << 8U));
}

ImU32 material_color(const std::uint16_t material, const float shade) {
    constexpr std::array<std::array<float, 3>, 12> colors{{
        {0.36F, 0.67F, 0.91F}, {0.91F, 0.48F, 0.35F}, {0.48F, 0.82F, 0.49F},
        {0.84F, 0.67F, 0.31F}, {0.68F, 0.49F, 0.86F}, {0.32F, 0.78F, 0.76F},
        {0.91F, 0.43F, 0.67F}, {0.62F, 0.70F, 0.35F}, {0.42F, 0.55F, 0.83F},
        {0.86F, 0.58F, 0.39F}, {0.48F, 0.76F, 0.65F}, {0.74F, 0.50F, 0.64F}
    }};
    const auto& color = colors[material % colors.size()];
    return IM_COL32(static_cast<int>(255.0F * color[0] * shade),
                    static_cast<int>(255.0F * color[1] * shade),
                    static_cast<int>(255.0F * color[2] * shade), 255);
}

std::array<std::uint8_t, 4> collision_surface_color(std::string name,
                                                    const std::uint16_t material) {
    std::transform(name.begin(), name.end(), name.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    const auto contains = [&](const std::string_view value) { return name.find(value) != std::string::npos; };
    if (contains("metal")) return {75, 135, 210, 255};
    if (contains("madera") || contains("wood")) return {205, 137, 72, 255};
    if (contains("veget") || contains("grass")) return {78, 155, 80, 255};
    if (contains("barro") || contains("tierra") || contains("mud") || contains("earth"))
        return {145, 100, 62, 255};
    if (contains("cristal") || contains("glass")) return {75, 205, 220, 255};
    if (contains("escal") || contains("stair")) return {235, 205, 65, 255};
    if (contains("piedra") || contains("cement") || contains("concrete") ||
        contains("baldosa") || contains("stone") || contains("tile"))
        return {145, 150, 155, 255};
    const auto packed = material_color(material, 1.0F);
    return {static_cast<std::uint8_t>((packed >> IM_COL32_R_SHIFT) & 0xFFU),
            static_cast<std::uint8_t>((packed >> IM_COL32_G_SHIFT) & 0xFFU),
            static_cast<std::uint8_t>((packed >> IM_COL32_B_SHIFT) & 0xFFU), 255};
}

std::uint16_t read_u16(const std::vector<std::byte>& bytes, const std::size_t offset) {
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[offset]) |
        (std::to_integer<std::uint16_t>(bytes[offset + 1]) << 8U));
}

std::uint32_t read_u32(const std::vector<std::byte>& bytes, const std::size_t offset) {
    return std::to_integer<std::uint32_t>(bytes[offset]) |
        (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U) |
        (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U) |
        (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::array<std::uint8_t, 4> color_565(const std::uint16_t value) {
    const auto r = static_cast<std::uint8_t>((value >> 11U) & 31U);
    const auto g = static_cast<std::uint8_t>((value >> 5U) & 63U);
    const auto b = static_cast<std::uint8_t>(value & 31U);
    return {static_cast<std::uint8_t>((r << 3U) | (r >> 2U)),
            static_cast<std::uint8_t>((g << 2U) | (g >> 4U)),
            static_cast<std::uint8_t>((b << 3U) | (b >> 2U)), 255};
}

std::uint8_t masked_channel(const std::uint32_t value, const std::uint32_t mask,
                            const std::uint8_t fallback) {
    if (mask == 0) return fallback;
    const auto shift = std::countr_zero(mask);
    const auto maximum = mask >> shift;
    if (maximum == 0) return fallback;
    return static_cast<std::uint8_t>(((value & mask) >> shift) * 255U / maximum);
}

bool decode_dds(const std::filesystem::path& path, int& width, int& height,
                std::vector<std::uint8_t>& rgba, std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { error = "Cannot open " + path.string(); return false; }
    stream.seekg(0, std::ios::end);
    const auto file_size = stream.tellg();
    if (file_size < 0) { error = "Cannot determine DDS size for " + path.string(); return false; }
    std::vector<std::byte> bytes(static_cast<std::size_t>(file_size));
    stream.seekg(0, std::ios::beg);
    if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()), file_size)) {
        error = "Cannot read " + path.string(); return false;
    }
    if (bytes.size() < 128 || std::to_integer<char>(bytes[0]) != 'D' ||
        std::to_integer<char>(bytes[1]) != 'D' || std::to_integer<char>(bytes[2]) != 'S' ||
        std::to_integer<char>(bytes[3]) != ' ') {
        error = "Invalid DDS header in " + path.string(); return false;
    }
    width = static_cast<int>(read_u32(bytes, 16));
    height = static_cast<int>(read_u32(bytes, 12));
    const std::string fourcc{std::to_integer<char>(bytes[84]), std::to_integer<char>(bytes[85]),
                             std::to_integer<char>(bytes[86]), std::to_integer<char>(bytes[87])};
    constexpr std::uint32_t ddpf_alpha_pixels = 0x01U;
    constexpr std::uint32_t ddpf_fourcc = 0x04U;
    constexpr std::uint32_t ddpf_rgb = 0x40U;
    const auto pixel_flags = read_u32(bytes, 80);
    const auto rgb_bits = read_u32(bytes, 88);
    const auto red_mask = read_u32(bytes, 92), green_mask = read_u32(bytes, 96);
    const auto blue_mask = read_u32(bytes, 100), alpha_mask = read_u32(bytes, 104);
    const bool dxt1 = (pixel_flags & ddpf_fourcc) && fourcc == "DXT1";
    const bool dxt3 = (pixel_flags & ddpf_fourcc) && fourcc == "DXT3";
    const bool dxt5 = (pixel_flags & ddpf_fourcc) && fourcc == "DXT5";
    const bool uncompressed = (pixel_flags & ddpf_rgb) && (rgb_bits == 16U || rgb_bits == 32U) &&
                              red_mask && green_mask && blue_mask;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
        (!dxt1 && !dxt3 && !dxt5 && !uncompressed)) {
        error = "Unsupported DDS format/dimensions in " + path.string(); return false;
    }
    if (static_cast<std::size_t>(width) > std::numeric_limits<std::size_t>::max() /
            static_cast<std::size_t>(height) / 4U) {
        error = "DDS dimensions overflow in " + path.string(); return false;
    }
    if (uncompressed) {
        const auto bytes_per_pixel = static_cast<std::size_t>(rgb_bits / 8U);
        const auto row_bytes = static_cast<std::size_t>(width) * bytes_per_pixel;
        constexpr std::uint32_t ddsd_pitch = 0x08U;
        const auto declared_pitch = static_cast<std::size_t>(read_u32(bytes, 20));
        const auto pitch = (read_u32(bytes, 8) & ddsd_pitch) && declared_pitch >= row_bytes ?
                           declared_pitch : row_bytes;
        if (pitch > (bytes.size() - 128U) / static_cast<std::size_t>(height)) {
            error = "Truncated DDS image in " + path.string(); return false;
        }
        rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 0);
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const auto source = 128U + static_cast<std::size_t>(y) * pitch +
                                static_cast<std::size_t>(x) * bytes_per_pixel;
            const auto packed = bytes_per_pixel == 2U ? read_u16(bytes, source) : read_u32(bytes, source);
            const auto output = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                 static_cast<std::size_t>(x)) * 4U;
            rgba[output] = masked_channel(packed, red_mask, 0);
            rgba[output + 1] = masked_channel(packed, green_mask, 0);
            rgba[output + 2] = masked_channel(packed, blue_mask, 0);
            rgba[output + 3] = masked_channel(
                packed, alpha_mask, (pixel_flags & ddpf_alpha_pixels) ? 0 : 255);
        }
        return true;
    }

    const std::size_t block_size = dxt1 ? 8U : 16U;
    const auto blocks_x = static_cast<std::size_t>((width + 3) / 4);
    const auto blocks_y = static_cast<std::size_t>((height + 3) / 4);
    if (blocks_x > std::numeric_limits<std::size_t>::max() / blocks_y ||
        blocks_x * blocks_y > (bytes.size() - 128) / block_size) {
        error = "Truncated DDS image in " + path.string(); return false;
    }
    rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 0);
    std::size_t cursor = 128;
    for (std::size_t by = 0; by < blocks_y; ++by) for (std::size_t bx = 0; bx < blocks_x; ++bx) {
        std::uint64_t alpha = ~std::uint64_t{};
        if (dxt3) {
            alpha = static_cast<std::uint64_t>(read_u32(bytes, cursor)) |
                    (static_cast<std::uint64_t>(read_u32(bytes, cursor + 4)) << 32U);
            cursor += 8;
        } else if (dxt5) {
            const auto alpha0 = std::to_integer<std::uint8_t>(bytes[cursor]);
            const auto alpha1 = std::to_integer<std::uint8_t>(bytes[cursor + 1]);
            std::array<std::uint8_t, 8> palette{alpha0, alpha1};
            if (alpha0 > alpha1) {
                for (unsigned i = 1; i <= 6; ++i)
                    palette[i + 1] = static_cast<std::uint8_t>(
                        ((7U - i) * alpha0 + i * alpha1) / 7U);
            } else {
                for (unsigned i = 1; i <= 4; ++i)
                    palette[i + 1] = static_cast<std::uint8_t>(
                        ((5U - i) * alpha0 + i * alpha1) / 5U);
                palette[6] = 0; palette[7] = 255;
            }
            std::uint64_t alpha_indices{};
            for (unsigned i = 0; i < 6; ++i)
                alpha_indices |= static_cast<std::uint64_t>(
                    std::to_integer<std::uint8_t>(bytes[cursor + 2 + i])) << (i * 8U);
            alpha = alpha_indices;
            cursor += 8;
            // Store the decoded palette below; DXT5's alpha indices are three bits.
            for (unsigned py = 0; py < 4; ++py) for (unsigned px = 0; px < 4; ++px) {
                const auto pixel = py * 4U + px;
                const auto x = bx * 4U + px, y = by * 4U + py;
                if (x >= static_cast<std::size_t>(width) || y >= static_cast<std::size_t>(height)) continue;
                const auto output = (y * static_cast<std::size_t>(width) + x) * 4U;
                rgba[output + 3] = palette[(alpha_indices >> (pixel * 3U)) & 7U];
            }
        }
        const auto c0_raw = read_u16(bytes, cursor), c1_raw = read_u16(bytes, cursor + 2);
        std::array<std::array<std::uint8_t, 4>, 4> colors{};
        colors[0] = color_565(c0_raw); colors[1] = color_565(c1_raw);
        if (dxt3 || dxt5 || c0_raw > c1_raw) {
            for (unsigned channel = 0; channel < 3; ++channel) {
                colors[2][channel] = static_cast<std::uint8_t>((2U * colors[0][channel] + colors[1][channel]) / 3U);
                colors[3][channel] = static_cast<std::uint8_t>((colors[0][channel] + 2U * colors[1][channel]) / 3U);
            }
            colors[2][3] = colors[3][3] = 255;
        } else {
            for (unsigned channel = 0; channel < 3; ++channel)
                colors[2][channel] = static_cast<std::uint8_t>((colors[0][channel] + colors[1][channel]) / 2U);
            colors[2][3] = 255; colors[3] = {0, 0, 0, 0};
        }
        const auto indices = read_u32(bytes, cursor + 4);
        cursor += 8;
        for (unsigned py = 0; py < 4; ++py) for (unsigned px = 0; px < 4; ++px) {
            const auto x = bx * 4U + px, y = by * 4U + py;
            if (x >= static_cast<std::size_t>(width) || y >= static_cast<std::size_t>(height)) continue;
            const auto pixel = py * 4U + px;
            auto color = colors[(indices >> (pixel * 2U)) & 3U];
            if (dxt3) color[3] = static_cast<std::uint8_t>(((alpha >> (pixel * 4U)) & 15U) * 17U);
            if (dxt5) {
                const auto output = (y * static_cast<std::size_t>(width) + x) * 4U;
                color[3] = rgba[output + 3];
            }
            const auto output = (y * static_cast<std::size_t>(width) + x) * 4U;
            std::copy(color.begin(), color.end(), rgba.begin() + static_cast<std::ptrdiff_t>(output));
        }
    }
    return true;
}

unsigned int upload_texture(const int width, const int height, const std::uint8_t* rgba) {
    GLuint texture{};
    auto& gl = gl_api();
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    gl.generate_mipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    GLfloat maximum_anisotropy = 1.0F;
    glGetFloatv(gl_max_texture_max_anisotropy, &maximum_anisotropy);
    if (maximum_anisotropy > 1.0F)
        glTexParameterf(GL_TEXTURE_2D, gl_texture_max_anisotropy,
                        std::min(maximum_anisotropy, 8.0F));
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    if (gl.generate_mipmap) gl.generate_mipmap(GL_TEXTURE_2D);
    return texture;
}

std::filesystem::path find_texture(const std::filesystem::path& source_path, const std::string& name) {
    if (name.empty()) return {};
    const std::array candidates{
        source_path.parent_path() / "Textures" / (name + ".dds"),
        source_path.parent_path() / (name + ".dds"),
        source_path.parent_path().parent_path() / "Textures" / (name + ".dds")
    };
    std::error_code error;
    for (const auto& candidate : candidates)
        if (std::filesystem::is_regular_file(candidate, error)) return candidate;

    return {};
}

} // namespace

void GeometryPreview::clear() {
    destroy_gpu_resources();
    if (!owned_texture_ids_.empty())
        glDeleteTextures(static_cast<GLsizei>(owned_texture_ids_.size()), owned_texture_ids_.data());
    if (checker_texture_ != 0) glDeleteTextures(1, &checker_texture_);
    chunk_offset_ = ~std::uint64_t{};
    scene_mode_ = false;
    scene_clump_count_ = scene_instance_count_ = scene_world_sector_count_ = scene_skipped_count_ = 0;
    scene_world_triangle_count_ = 0;
    scene_custom_instance_count_ = scene_unresolved_instance_count_ = 0;
    collision_sector_count_ = collision_triangle_count_ = collision_material_count_ = 0;
    collision_declared_sector_count_ = 0;
    collision_recovery_status_ = rws::WorldRecoveryStatus::failed;
    collision_diagnostics_.clear();
    collision_surface_labels_.clear();
    collision_source_path_.clear();
    vertices_.clear();
    uv_sets_.clear();
    faces_.clear();
    gpu_vertices_.clear();
    draw_batches_.clear();
    material_colors_.clear();
    material_textures_.clear();
    material_lightmap_textures_.clear();
    material_texture_names_.clear();
    material_lightmap_texture_names_.clear();
    owned_texture_ids_.clear();
    checker_texture_ = 0;
    visual_material_slot_count_ = 0;
    loaded_texture_count_ = missing_texture_count_ = 0;
    texture_status_.clear();
    selected_uv_set_ = 0;
    error_.clear();
}

void GeometryPreview::select_uv_set(const std::size_t index) {
    if (index >= uv_sets_.size()) return;
    selected_uv_set_ = index;
    const auto& selected = uv_sets_[selected_uv_set_];
    for (auto& vertex : gpu_vertices_) {
        const auto uv = vertex.source_index < selected.size() ? selected[vertex.source_index] : Uv{};
        vertex.debug_u = uv.u;
        vertex.debug_v = uv.v;
    }
    if (vertex_buffer_ != 0) {
        auto& gl = gl_api();
        gl.bind_buffer(gl_array_buffer, vertex_buffer_);
        gl.buffer_data(gl_array_buffer, static_cast<GlSizePtr>(gpu_vertices_.size() * sizeof(GpuVertex)),
                       gpu_vertices_.data(), gl_static_draw);
    }
}

void GeometryPreview::reset_view() {
    yaw_ = -0.65F;
    pitch_ = scene_mode_ ? 0.35F : -0.35F;
    target_yaw_ = yaw_;
    target_pitch_ = pitch_;
    distance_ = std::max(radius_ * 3.0F, 0.01F);
    pan_x_ = pan_y_ = 0.0F;
    navigation_offset_ = {};
    target_navigation_offset_ = {};
    preserve_camera_position_ = false;
}

void GeometryPreview::frame_bounds(const rws::Vec3 center, const float radius) {
    center_ = center;
    radius_ = std::max(radius, 0.001F);
    reset_view();
}

void GeometryPreview::pan_camera(const float delta_x, const float delta_y) {
    const float focal = std::max(std::min(canvas_width_, canvas_height_) * 0.78F, 1.0F);
    pan_x_ += delta_x / focal;
    pan_y_ += delta_y / focal;
}

rws::Vec3 GeometryPreview::camera_offset(const float yaw, const float pitch) const {
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const std::array<rws::Vec3, 3> rows = scene_mode_ ?
        std::array<rws::Vec3, 3>{{{cy, 0, -sy}, {-sp * sy, cp, -sp * cy}, {cp * sy, sp, cp * cy}}} :
        std::array<rws::Vec3, 3>{{{cy, -sy, 0}, {-sp * sy, -sp * cy, cp}, {cp * sy, cp * cy, sp}}};
    const rws::Vec3 view_offset{-pan_x_ * distance_,
                                pan_y_ * distance_,
                                distance_};
    return {rows[0].x * view_offset.x + rows[1].x * view_offset.y + rows[2].x * view_offset.z,
            rows[0].y * view_offset.x + rows[1].y * view_offset.y + rows[2].y * view_offset.z,
            rows[0].z * view_offset.x + rows[1].z * view_offset.y + rows[2].z * view_offset.z};
}

std::optional<std::uint64_t> GeometryPreview::pick_scene(const float mouse_x,
                                                         const float mouse_y) const {
    if (!scene_mode_ || canvas_width_ <= 0.0F || canvas_height_ <= 0.0F) return std::nullopt;
    const float ndc_x = 2.0F * (mouse_x - canvas_x_) / canvas_width_ - 1.0F;
    const float ndc_y = 1.0F - 2.0F * (mouse_y - canvas_y_) / canvas_height_;
    const float aspect = canvas_width_ / canvas_height_;
    constexpr float tan_half_fov = 0.46630766F; // tan(25 degrees)
    rws::Vec3 view_direction{ndc_x * aspect * tan_half_fov, ndc_y * tan_half_fov, -1.0F};
    const float view_length = std::sqrt(view_direction.x * view_direction.x +
        view_direction.y * view_direction.y + 1.0F);
    view_direction.x /= view_length; view_direction.y /= view_length;
    view_direction.z /= view_length;

    const float cy = std::cos(yaw_), sy = std::sin(yaw_);
    const float cp = std::cos(pitch_), sp = std::sin(pitch_);
    const std::array<rws::Vec3, 3> rows = scene_mode_ ?
        std::array<rws::Vec3, 3>{{{cy, 0, -sy}, {-sp * sy, cp, -sp * cy}, {cp * sy, sp, cp * cy}}} :
        std::array<rws::Vec3, 3>{{{cy, -sy, 0}, {-sp * sy, -sp * cy, cp}, {cp * sy, cp * cy, sp}}};
    auto inverse_rotate = [&](const rws::Vec3 value) {
        return rws::Vec3{rows[0].x * value.x + rows[1].x * value.y + rows[2].x * value.z,
                         rows[0].y * value.x + rows[1].y * value.y + rows[2].y * value.z,
                         rows[0].z * value.x + rows[1].z * value.y + rows[2].z * value.z};
    };
    const auto camera_from_target = camera_offset(yaw_, pitch_);
    const rws::Vec3 origin{center_.x + navigation_offset_.x + camera_from_target.x,
                           center_.y + navigation_offset_.y + camera_from_target.y,
                           center_.z + navigation_offset_.z + camera_from_target.z};
    const auto direction = inverse_rotate(view_direction);

    float closest = std::numeric_limits<float>::max();
    std::optional<std::uint64_t> result;
    for (const auto& batch : draw_batches_) {
        if (batch.layer == PreviewLayer::collision_world || !show_visual_) continue;
        const auto end = static_cast<std::size_t>(batch.first) + batch.count;
        for (std::size_t i = batch.first; i + 2 < end; i += 3) {
            const auto& a = gpu_vertices_[i];
            const auto& b = gpu_vertices_[i + 1];
            const auto& c = gpu_vertices_[i + 2];
            const rws::Vec3 edge1{b.x - a.x, b.y - a.y, b.z - a.z};
            const rws::Vec3 edge2{c.x - a.x, c.y - a.y, c.z - a.z};
            const rws::Vec3 p{direction.y * edge2.z - direction.z * edge2.y,
                              direction.z * edge2.x - direction.x * edge2.z,
                              direction.x * edge2.y - direction.y * edge2.x};
            const float determinant = edge1.x * p.x + edge1.y * p.y + edge1.z * p.z;
            if (std::abs(determinant) < 0.000001F) continue;
            const float inverse_determinant = 1.0F / determinant;
            const rws::Vec3 offset{origin.x - a.x, origin.y - a.y, origin.z - a.z};
            const float u = (offset.x * p.x + offset.y * p.y + offset.z * p.z) * inverse_determinant;
            if (u < 0.0F || u > 1.0F) continue;
            const rws::Vec3 q{offset.y * edge1.z - offset.z * edge1.y,
                              offset.z * edge1.x - offset.x * edge1.z,
                              offset.x * edge1.y - offset.y * edge1.x};
            const float v = (direction.x * q.x + direction.y * q.y + direction.z * q.z) *
                inverse_determinant;
            if (v < 0.0F || u + v > 1.0F) continue;
            const float distance = (edge2.x * q.x + edge2.y * q.y + edge2.z * q.z) *
                inverse_determinant;
            if (distance > 0.0F && distance < closest) {
                closest = distance;
                result = batch.owner_offset;
            }
        }
    }
    return result;
}

void GeometryPreview::update_keyboard_navigation() {
    const auto& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered();
    const bool fast = hovered &&
        (ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift));
    // Distance-scaled movement gives level overview speed while zoomed out and
    // precise building/interior movement up close. Keep only a tiny world-scale
    // floor so a near-zero camera distance can never stall navigation entirely.
    const float speed_floor = scene_mode_ ? radius_ * 0.00005F : radius_ * 0.002F;
    const float speed = std::max(distance_ * 0.35F, std::max(speed_floor, 0.01F)) *
        navigation_speed_ * (fast ? 4.0F : 1.0F) * io.DeltaTime;
    const float cy = std::cos(yaw_), sy = std::sin(yaw_);
    const rws::Vec3 forward = scene_mode_ ? rws::Vec3{-sy, 0.0F, -cy} :
                                                   rws::Vec3{-sy, -cy, 0.0F};
    const rws::Vec3 right = scene_mode_ ? rws::Vec3{cy, 0.0F, -sy} :
                                                 rws::Vec3{cy, -sy, 0.0F};
    auto move = [&](const rws::Vec3 direction, const float amount) {
        target_navigation_offset_.x += direction.x * amount;
        target_navigation_offset_.y += direction.y * amount;
        target_navigation_offset_.z += direction.z * amount;
    };
    if (hovered && ImGui::IsKeyDown(ImGuiKey_W)) move(forward, speed);
    if (hovered && ImGui::IsKeyDown(ImGuiKey_S)) move(forward, -speed);
    if (hovered && ImGui::IsKeyDown(ImGuiKey_D)) move(right, speed);
    if (hovered && ImGui::IsKeyDown(ImGuiKey_A)) move(right, -speed);
    const rws::Vec3 up = scene_mode_ ? rws::Vec3{0, 1, 0} : rws::Vec3{0, 0, 1};
    if (hovered && ImGui::IsKeyDown(ImGuiKey_E)) move(up, speed);
    if (hovered && ImGui::IsKeyDown(ImGuiKey_Q)) move(up, -speed);

    const float rotation_alpha = 1.0F - std::exp(-18.0F * std::min(io.DeltaTime, 0.1F));
    const auto old_camera_offset = camera_offset(yaw_, pitch_);
    yaw_ += (target_yaw_ - yaw_) * rotation_alpha;
    pitch_ += (target_pitch_ - pitch_) * rotation_alpha;
    if (preserve_camera_position_) {
        const auto new_camera_offset = camera_offset(yaw_, pitch_);
        const rws::Vec3 correction{old_camera_offset.x - new_camera_offset.x,
                                  old_camera_offset.y - new_camera_offset.y,
                                  old_camera_offset.z - new_camera_offset.z};
        navigation_offset_.x += correction.x;
        navigation_offset_.y += correction.y;
        navigation_offset_.z += correction.z;
        target_navigation_offset_.x += correction.x;
        target_navigation_offset_.y += correction.y;
        target_navigation_offset_.z += correction.z;
        if (std::abs(target_yaw_ - yaw_) < 0.0001F &&
            std::abs(target_pitch_ - pitch_) < 0.0001F)
            preserve_camera_position_ = false;
    }
    const float movement_alpha = 1.0F - std::exp(-12.0F * std::min(io.DeltaTime, 0.1F));
    navigation_offset_.x += (target_navigation_offset_.x - navigation_offset_.x) * movement_alpha;
    navigation_offset_.y += (target_navigation_offset_.y - navigation_offset_.y) * movement_alpha;
    navigation_offset_.z += (target_navigation_offset_.z - navigation_offset_.z) * movement_alpha;
}

bool GeometryPreview::load(const rws::Chunk& geometry_chunk,
                           const std::span<const std::byte> bytes,
                           const std::filesystem::path& source_path) {
    clear();
    scene_mode_ = false;
    chunk_offset_ = geometry_chunk.offset;
    const auto geometry = rws::decode_geometry(geometry_chunk, bytes);
    if (!geometry) { error_ = geometry.error; return false; }
    if (geometry.value->triangle_layout == rws::TriangleLayout::unknown) {
        error_ = "Triangle word order is ambiguous";
        return false;
    }
    const auto morph = std::find_if(geometry.value->morph_targets.begin(),
        geometry.value->morph_targets.end(), [](const rws::MorphTargetInfo& value) {
            return value.has_vertices;
        });
    if (morph == geometry.value->morph_targets.end()) {
        error_ = "Geometry has no non-native vertex array";
        return false;
    }
    uv_sets_.reserve(geometry.value->texcoord_offsets.size());
    for (const auto uv_offset : geometry.value->texcoord_offsets) {
        const auto uv_bytes = static_cast<std::uint64_t>(geometry.value->vertex_count) * 8U;
        if (uv_offset <= bytes.size() && uv_bytes <= bytes.size() - uv_offset) {
            std::vector<Uv> uv_set;
            uv_set.reserve(static_cast<std::size_t>(geometry.value->vertex_count));
            for (std::int32_t i = 0; i < geometry.value->vertex_count; ++i) {
                const auto offset = uv_offset + static_cast<std::uint64_t>(i) * 8U;
                // RenderWare's PC UVs and DDS images both use a top-left origin. DDS rows are
                // uploaded unchanged, so retaining V preserves the original pairing. Flipping
                // only the coordinates mirrors atlas islands into unrelated lightmap regions.
                uv_set.push_back({read_f32(bytes, offset), read_f32(bytes, offset + 4)});
            }
            uv_sets_.push_back(std::move(uv_set));
        }
    }
    if (view_style_ >= 4 && view_style_ <= 6 && uv_sets_.size() > 1) selected_uv_set_ = 1;

    if (const auto* material_list_chunk = rws::find_child(geometry_chunk, 0x08)) {
        const auto material_list = rws::decode_material_list(*material_list_chunk, bytes);
        if (material_list) {
            std::vector<const rws::Chunk*> material_chunks;
            for (const auto& child : material_list_chunk->children)
                if (child.type == 0x07) material_chunks.push_back(&child);
            material_colors_.resize(static_cast<std::size_t>(material_list.value->material_count), {190, 190, 190, 255});
            material_textures_.resize(material_colors_.size());
            material_lightmap_textures_.resize(material_colors_.size());
            material_texture_names_.resize(material_colors_.size());
            material_lightmap_texture_names_.resize(material_colors_.size());
            std::unordered_map<std::string, unsigned int> texture_cache;
            auto load_texture = [&](const std::string& name) -> unsigned int {
                if (name.empty()) return 0;
                if (const auto cached = texture_cache.find(name); cached != texture_cache.end())
                    return cached->second;
                const auto path = find_texture(source_path, name);
                if (path.empty()) {
                    ++missing_texture_count_;
                    if (texture_status_.empty())
                        texture_status_ = "Could not locate " + name + ".dds from " +
                            source_path.parent_path().string();
                    return 0;
                }
                int width{}, height{};
                std::vector<std::uint8_t> rgba;
                std::string texture_error;
                if (!decode_dds(path, width, height, rgba, texture_error)) {
                    ++missing_texture_count_;
                    if (texture_status_.empty()) texture_status_ = std::move(texture_error);
                    return 0;
                }
                const auto id = upload_texture(width, height, rgba.data());
                owned_texture_ids_.push_back(id);
                texture_cache.emplace(name, id);
                ++loaded_texture_count_;
                return id;
            };
            std::size_t next_material{};
            for (std::size_t i = 0; i < material_colors_.size(); ++i) {
                const auto remap = i < material_list.value->remap.size() ? material_list.value->remap[i] : -1;
                if (remap >= 0 && static_cast<std::size_t>(remap) < i) {
                    material_colors_[i] = material_colors_[static_cast<std::size_t>(remap)];
                    material_textures_[i] = material_textures_[static_cast<std::size_t>(remap)];
                    material_lightmap_textures_[i] = material_lightmap_textures_[static_cast<std::size_t>(remap)];
                    material_texture_names_[i] = material_texture_names_[static_cast<std::size_t>(remap)];
                    material_lightmap_texture_names_[i] =
                        material_lightmap_texture_names_[static_cast<std::size_t>(remap)];
                    continue;
                }
                if (next_material >= material_chunks.size()) continue;
                const auto& material_chunk = *material_chunks[next_material++];
                const auto material = rws::decode_material(material_chunk, bytes);
                if (material) material_colors_[i] = material.value->color;
                if (const auto* texture_chunk = rws::find_child(material_chunk, 0x06)) {
                    const auto texture = rws::decode_texture(*texture_chunk, bytes);
                    if (texture && !texture.value->name.empty()) {
                        material_texture_names_[i] = texture.value->name;
                        material_textures_[i] = load_texture(texture.value->name);
                    }
                }
                const auto* extension = rws::find_child(material_chunk, 0x03);
                const auto* effects_chunk = extension ? rws::find_child(*extension, 0x120) : nullptr;
                if (!effects_chunk) continue;
                const auto effects = rws::decode_material_effects(*effects_chunk, 0x07, bytes);
                if (!effects || !effects.value->has_dual_texture || effects.value->dual_texture.name.empty())
                    continue;
                material_lightmap_texture_names_[i] = effects.value->dual_texture.name;
                material_lightmap_textures_[i] = load_texture(effects.value->dual_texture.name);
            }
        }
    }
    std::array<std::uint8_t, 64 * 64 * 4> checker{};
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
        const bool bright = ((x / 8) ^ (y / 8)) & 1;
        const auto offset = static_cast<std::size_t>(y * 64 + x) * 4U;
        checker[offset] = checker[offset + 1] = checker[offset + 2] = bright ? 210 : 65;
        checker[offset + 3] = 255;
    }
    checker_texture_ = upload_texture(64, 64, checker.data());
    const auto vertex_bytes = static_cast<std::uint64_t>(geometry.value->vertex_count) * 12U;
    if (morph->vertices_offset > bytes.size() || vertex_bytes > bytes.size() - morph->vertices_offset) {
        error_ = "Geometry vertex array is outside the file";
        return false;
    }
    vertices_.reserve(static_cast<std::size_t>(geometry.value->vertex_count));
    rws::Vec3 minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max()};
    rws::Vec3 maximum{-minimum.x, -minimum.y, -minimum.z};
    for (std::int32_t i = 0; i < geometry.value->vertex_count; ++i) {
        const auto offset = morph->vertices_offset + static_cast<std::uint64_t>(i) * 12U;
        rws::Vec3 vertex{read_f32(bytes, offset), read_f32(bytes, offset + 4), read_f32(bytes, offset + 8)};
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z)) {
            vertices_.clear(); faces_.clear();
            error_ = "Geometry contains a non-finite vertex";
            return false;
        }
        vertices_.push_back(vertex);
        minimum.x = std::min(minimum.x, vertex.x); minimum.y = std::min(minimum.y, vertex.y);
        minimum.z = std::min(minimum.z, vertex.z); maximum.x = std::max(maximum.x, vertex.x);
        maximum.y = std::max(maximum.y, vertex.y); maximum.z = std::max(maximum.z, vertex.z);
    }
    center_ = {(minimum.x + maximum.x) * 0.5F, (minimum.y + maximum.y) * 0.5F,
               (minimum.z + maximum.z) * 0.5F};
    radius_ = 0.0F;
    for (const auto& vertex : vertices_) {
        const auto x = vertex.x - center_.x, y = vertex.y - center_.y, z = vertex.z - center_.z;
        radius_ = std::max(radius_, std::sqrt(x * x + y * y + z * z));
    }
    radius_ = std::max(radius_, 0.001F);
    all_center_ = center_;
    all_radius_ = radius_;
    faces_.reserve(static_cast<std::size_t>(geometry.value->triangle_count));
    for (std::int32_t i = 0; i < geometry.value->triangle_count; ++i) {
        const auto triangle = rws::decode_triangle(*geometry.value, i, bytes);
        if (!triangle) { error_ = triangle.error; faces_.clear(); return false; }
        if (triangle.value->vertices[0] >= vertices_.size() ||
            triangle.value->vertices[1] >= vertices_.size() ||
            triangle.value->vertices[2] >= vertices_.size()) {
            error_ = "Triangle references a vertex outside the geometry";
            faces_.clear(); return false;
        }
        faces_.push_back({triangle.value->vertices[0], triangle.value->vertices[1],
                          triangle.value->vertices[2], triangle.value->material});
    }
    std::vector<std::size_t> face_order(faces_.size());
    for (std::size_t i = 0; i < face_order.size(); ++i) face_order[i] = i;
    std::stable_sort(face_order.begin(), face_order.end(), [&](const std::size_t a, const std::size_t b) {
        return faces_[a].material < faces_[b].material;
    });
    gpu_vertices_.reserve(faces_.size() * 3U);
    for (const auto face_index : face_order) {
        const auto& face = faces_[face_index];
        if (draw_batches_.empty() || draw_batches_.back().material != face.material)
            draw_batches_.push_back({face.material, static_cast<std::uint32_t>(gpu_vertices_.size()),
                                     0, geometry_chunk.offset});
        const auto& a = vertices_[face.a];
        const auto& b = vertices_[face.b];
        const auto& c = vertices_[face.c];
        const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
        const float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const float normal_length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (normal_length > 0.0F) { nx /= normal_length; ny /= normal_length; nz /= normal_length; }
        const std::array indices{face.a, face.b, face.c};
        for (const auto index : indices) {
            const auto base_uv = !uv_sets_.empty() && index < uv_sets_.front().size() ?
                uv_sets_.front()[index] : Uv{};
            const auto lightmap_uv = uv_sets_.size() > 1 && index < uv_sets_[1].size() ?
                uv_sets_[1][index] : Uv{};
            const auto debug_uv = selected_uv_set_ < uv_sets_.size() &&
                index < uv_sets_[selected_uv_set_].size() ? uv_sets_[selected_uv_set_][index] : Uv{};
            const auto& vertex = vertices_[index];
            gpu_vertices_.push_back({vertex.x, vertex.y, vertex.z,
                base_uv.u, base_uv.v, lightmap_uv.u, lightmap_uv.v,
                debug_uv.u, debug_uv.v, nx, ny, nz, index});
        }
        draw_batches_.back().count += 3;
    }
    if (!create_gpu_resources()) return false;
    reset_view();
    return true;
}

bool GeometryPreview::load_scene(const std::vector<rws::Chunk>& chunks,
                                 const std::span<const std::byte> bytes,
                                 const std::span<const rws::SceneInstance> instances,
                                 const std::filesystem::path& source_path,
                                 const rws::Document* collision_document,
                                 const bool main_is_collision) {
    clear();
    scene_mode_ = true;
    view_style_ = 1;
    wireframe_ = false;

    rws::Vec3 minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max()};
    rws::Vec3 maximum{-minimum.x, -minimum.y, -minimum.z};
    std::unordered_map<std::string, unsigned int> texture_cache;
    auto load_texture = [&](const std::string& name) -> unsigned int {
        if (name.empty()) return 0;
        if (const auto cached = texture_cache.find(name); cached != texture_cache.end())
            return cached->second;
        const auto path = find_texture(source_path, name);
        if (path.empty()) {
            ++missing_texture_count_;
            if (texture_status_.empty()) texture_status_ = "Could not locate " + name + ".dds";
            return 0;
        }
        int width{}, height{};
        std::vector<std::uint8_t> rgba;
        std::string texture_error;
        if (!decode_dds(path, width, height, rgba, texture_error)) {
            ++missing_texture_count_;
            if (texture_status_.empty()) texture_status_ = std::move(texture_error);
            return 0;
        }
        const auto id = upload_texture(width, height, rgba.data());
        owned_texture_ids_.push_back(id);
        texture_cache.emplace(name, id);
        ++loaded_texture_count_;
        return id;
    };

    struct PrototypeRange {
        std::size_t batch_begin{}, batch_end{};
        AffineTransform original_root;
    };
    std::unordered_map<std::uint32_t, PrototypeRange> prototypes;
    for (const auto& clump_chunk : chunks) {
        if (clump_chunk.type != 0x10) continue;
        ++scene_clump_count_;
        const auto prototype_batch_begin = draw_batches_.size();
        std::optional<std::uint32_t> prototype_id;
        const auto* frame_list_chunk = rws::find_child(clump_chunk, 0x0E);
        const auto* geometry_list_chunk = rws::find_child(clump_chunk, 0x1A);
        if (!frame_list_chunk || !geometry_list_chunk) {
            ++scene_skipped_count_;
            continue;
        }
        const auto frames = rws::decode_frame_list(*frame_list_chunk, bytes);
        if (!frames) {
            ++scene_skipped_count_;
            continue;
        }
        std::vector<const rws::Chunk*> geometries;
        for (const auto& child : geometry_list_chunk->children)
            if (child.type == 0x0F) geometries.push_back(&child);

        std::vector<AffineTransform> world_frames(frames.value->frames.size());
        std::vector<std::uint8_t> frame_state(frames.value->frames.size());
        auto resolve_frame = [&](auto&& self, const std::size_t index) -> bool {
            if (index >= world_frames.size()) return false;
            if (frame_state[index] == 2) return true;
            if (frame_state[index] == 1) return false;
            frame_state[index] = 1;
            const auto& frame = frames.value->frames[index];
            const auto local = frame_transform(frame);
            if (frame.parent >= 0) {
                const auto parent = static_cast<std::size_t>(frame.parent);
                if (!self(self, parent)) return false;
                world_frames[index] = compose(world_frames[parent], local);
            } else {
                world_frames[index] = local;
            }
            frame_state[index] = 2;
            return true;
        };

        for (const auto& atomic_chunk : clump_chunk.children) {
            if (atomic_chunk.type != 0x14) continue;
            if (!prototype_id) {
                const auto* extension = rws::find_child(atomic_chunk, 0x03);
                const auto* pyro = extension ? rws::find_child(*extension, 0xFFFFFF00U) : nullptr;
                const auto metadata = pyro ? rws::decode_pyro_extension(*pyro, 0x14, bytes) :
                                             rws::DecodeResult<rws::PyroExtensionInfo>{};
                if (metadata) {
                    if (const auto index = metadata.value->atomic_object_index())
                        prototype_id = 1000U + *index;
                }
            }
            const auto atomic = rws::decode_atomic(atomic_chunk, bytes);
            if (!atomic || atomic.value->frame_index < 0 || atomic.value->geometry_index < 0 ||
                static_cast<std::size_t>(atomic.value->geometry_index) >= geometries.size() ||
                !resolve_frame(resolve_frame, static_cast<std::size_t>(atomic.value->frame_index))) {
                ++scene_skipped_count_;
                continue;
            }
            const auto geometry = rws::decode_geometry(
                *geometries[static_cast<std::size_t>(atomic.value->geometry_index)], bytes);
            if (!geometry || geometry.value->triangle_layout == rws::TriangleLayout::unknown) {
                ++scene_skipped_count_;
                continue;
            }
            const auto& geometry_chunk = *geometries[static_cast<std::size_t>(atomic.value->geometry_index)];
            const auto* material_list_chunk = rws::find_child(geometry_chunk, 0x08);
            const auto material_list = material_list_chunk ?
                rws::decode_material_list(*material_list_chunk, bytes) : rws::DecodeResult<rws::MaterialListInfo>{};
            const std::size_t material_base = material_colors_.size();
            const auto material_count = material_list && material_list.value->material_count > 0 ?
                static_cast<std::size_t>(material_list.value->material_count) : 1U;
            material_colors_.resize(material_base + material_count, {190, 190, 190, 255});
            material_textures_.resize(material_base + material_count);
            material_lightmap_textures_.resize(material_base + material_count);
            material_texture_names_.resize(material_base + material_count);
            material_lightmap_texture_names_.resize(material_base + material_count);
            if (material_list_chunk && material_list) {
                std::vector<const rws::Chunk*> material_chunks;
                for (const auto& child : material_list_chunk->children)
                    if (child.type == 0x07) material_chunks.push_back(&child);
                std::size_t next_material{};
                for (std::size_t slot = 0; slot < material_count; ++slot) {
                    const auto destination = material_base + slot;
                    const auto remap = slot < material_list.value->remap.size() ?
                        material_list.value->remap[slot] : -1;
                    if (remap >= 0 && static_cast<std::size_t>(remap) < slot) {
                        const auto source = material_base + static_cast<std::size_t>(remap);
                        material_colors_[destination] = material_colors_[source];
                        material_textures_[destination] = material_textures_[source];
                        material_lightmap_textures_[destination] = material_lightmap_textures_[source];
                        material_texture_names_[destination] = material_texture_names_[source];
                        material_lightmap_texture_names_[destination] = material_lightmap_texture_names_[source];
                        continue;
                    }
                    if (next_material >= material_chunks.size()) continue;
                    const auto& material_chunk = *material_chunks[next_material++];
                    const auto material = rws::decode_material(material_chunk, bytes);
                    if (material) material_colors_[destination] = material.value->color;
                    if (const auto* texture_chunk = rws::find_child(material_chunk, 0x06)) {
                        const auto texture = rws::decode_texture(*texture_chunk, bytes);
                        if (texture && !texture.value->name.empty()) {
                            material_texture_names_[destination] = texture.value->name;
                            material_textures_[destination] = load_texture(texture.value->name);
                        }
                    }
                    const auto* extension = rws::find_child(material_chunk, 0x03);
                    const auto* effects_chunk = extension ? rws::find_child(*extension, 0x120) : nullptr;
                    if (effects_chunk) {
                        const auto effects = rws::decode_material_effects(*effects_chunk, 0x07, bytes);
                        if (effects && effects.value->has_dual_texture &&
                            !effects.value->dual_texture.name.empty()) {
                            material_lightmap_texture_names_[destination] = effects.value->dual_texture.name;
                            material_lightmap_textures_[destination] = load_texture(effects.value->dual_texture.name);
                        }
                    }
                }
            }
            const auto morph = std::find_if(geometry.value->morph_targets.begin(),
                geometry.value->morph_targets.end(), [](const rws::MorphTargetInfo& value) {
                    return value.has_vertices;
                });
            if (morph == geometry.value->morph_targets.end()) {
                ++scene_skipped_count_;
                continue;
            }
            const auto& transform = world_frames[static_cast<std::size_t>(atomic.value->frame_index)];
            std::vector<Uv> base_uvs, lightmap_uvs;
            if (!geometry.value->texcoord_offsets.empty()) {
                base_uvs.reserve(static_cast<std::size_t>(geometry.value->vertex_count));
                const auto offset = geometry.value->texcoord_offsets[0];
                for (std::int32_t i = 0; i < geometry.value->vertex_count; ++i)
                    base_uvs.push_back({read_f32(bytes, offset + static_cast<std::uint64_t>(i) * 8U),
                        read_f32(bytes, offset + static_cast<std::uint64_t>(i) * 8U + 4)});
            }
            if (geometry.value->texcoord_offsets.size() > 1) {
                lightmap_uvs.reserve(static_cast<std::size_t>(geometry.value->vertex_count));
                const auto offset = geometry.value->texcoord_offsets[1];
                for (std::int32_t i = 0; i < geometry.value->vertex_count; ++i)
                    lightmap_uvs.push_back({read_f32(bytes, offset + static_cast<std::uint64_t>(i) * 8U),
                        read_f32(bytes, offset + static_cast<std::uint64_t>(i) * 8U + 4)});
            }
            std::vector<rws::Vec3> instance_vertices;
            instance_vertices.reserve(static_cast<std::size_t>(geometry.value->vertex_count));
            for (std::int32_t i = 0; i < geometry.value->vertex_count; ++i) {
                const auto offset = morph->vertices_offset + static_cast<std::uint64_t>(i) * 12U;
                const auto vertex = transform_point(transform,
                    {read_f32(bytes, offset), read_f32(bytes, offset + 4), read_f32(bytes, offset + 8)});
                instance_vertices.push_back(vertex);
                vertices_.push_back(vertex);
                minimum.x = std::min(minimum.x, vertex.x); minimum.y = std::min(minimum.y, vertex.y);
                minimum.z = std::min(minimum.z, vertex.z); maximum.x = std::max(maximum.x, vertex.x);
                maximum.y = std::max(maximum.y, vertex.y); maximum.z = std::max(maximum.z, vertex.z);
            }
            std::vector<rws::TriangleInfo> triangles;
            triangles.reserve(static_cast<std::size_t>(geometry.value->triangle_count));
            for (std::int32_t i = 0; i < geometry.value->triangle_count; ++i) {
                const auto decoded_triangle = rws::decode_triangle(*geometry.value, i, bytes);
                if (decoded_triangle) triangles.push_back(*decoded_triangle.value);
            }
            std::stable_sort(triangles.begin(), triangles.end(), [](const auto& left, const auto& right) {
                return left.material < right.material;
            });
            for (const auto& triangle : triangles) {
                if (triangle.vertices[0] >= instance_vertices.size() ||
                    triangle.vertices[1] >= instance_vertices.size() ||
                    triangle.vertices[2] >= instance_vertices.size()) continue;
                const auto local_material = std::min<std::size_t>(triangle.material, material_count - 1);
                const auto global_material = material_base + local_material;
                if (draw_batches_.empty() || draw_batches_.back().material != global_material ||
                    draw_batches_.back().owner_offset != clump_chunk.offset)
                    draw_batches_.push_back({static_cast<std::uint16_t>(global_material),
                        static_cast<std::uint32_t>(gpu_vertices_.size()), 0, clump_chunk.offset});
                const auto& a = instance_vertices[triangle.vertices[0]];
                const auto& b = instance_vertices[triangle.vertices[1]];
                const auto& c = instance_vertices[triangle.vertices[2]];
                float nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
                float ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
                float nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (length > 0.0F) { nx /= length; ny /= length; nz /= length; }
                for (const auto index : triangle.vertices) {
                    const auto* vertex = &instance_vertices[index];
                    const auto base_uv = index < base_uvs.size() ? base_uvs[index] : Uv{};
                    const auto lightmap_uv = index < lightmap_uvs.size() ? lightmap_uvs[index] : Uv{};
                    gpu_vertices_.push_back({vertex->x, vertex->y, vertex->z,
                        base_uv.u, base_uv.v, lightmap_uv.u, lightmap_uv.v,
                        lightmap_uv.u, lightmap_uv.v, nx, ny, nz, index});
                }
                faces_.push_back({});
                draw_batches_.back().count += 3;
            }
            if (!triangles.empty()) {
                ++scene_instance_count_;
            }
        }
        if (prototype_id && !prototypes.contains(*prototype_id) &&
            draw_batches_.size() > prototype_batch_begin) {
            AffineTransform original_root;
            const auto root = std::find_if(frames.value->frames.begin(), frames.value->frames.end(),
                [](const rws::FrameInfo& frame) { return frame.parent < 0; });
            if (root != frames.value->frames.end()) {
                const auto index = static_cast<std::size_t>(root - frames.value->frames.begin());
                if (resolve_frame(resolve_frame, index)) original_root = world_frames[index];
            }
            prototypes[*prototype_id] = {prototype_batch_begin, draw_batches_.size(), original_root};
        }
    }

    for (const auto& instance : instances) {
        const auto found = prototypes.find(instance.prototype_id);
        if (found == prototypes.end()) {
            ++scene_unresolved_instance_count_;
            continue;
        }
        const AffineTransform transform{{{
            instance.rotation[0], instance.rotation[3], instance.rotation[6],
            instance.rotation[1], instance.rotation[4], instance.rotation[7],
            instance.rotation[2], instance.rotation[5], instance.rotation[8]}}, instance.position};
        for (auto batch_index = found->second.batch_begin; batch_index < found->second.batch_end; ++batch_index) {
            const auto source_batch = draw_batches_[batch_index];
            DrawBatch output_batch{source_batch.material, static_cast<std::uint32_t>(gpu_vertices_.size()),
                                   source_batch.count, instance.offset};
            for (std::uint32_t i = 0; i < source_batch.count; ++i) {
                auto vertex = gpu_vertices_[static_cast<std::size_t>(source_batch.first) + i];
                const auto local = inverse_transform_point(found->second.original_root,
                    {vertex.x, vertex.y, vertex.z});
                const auto position = transform_point(transform, local);
                vertex.x = position.x; vertex.y = position.y; vertex.z = position.z;
                const auto local_normal = inverse_transform_point(found->second.original_root,
                    {vertex.nx + found->second.original_root.position.x,
                     vertex.ny + found->second.original_root.position.y,
                     vertex.nz + found->second.original_root.position.z});
                const auto& m = transform.rotation;
                const rws::Vec3 normal{m[0] * local_normal.x + m[1] * local_normal.y + m[2] * local_normal.z,
                                       m[3] * local_normal.x + m[4] * local_normal.y + m[5] * local_normal.z,
                                       m[6] * local_normal.x + m[7] * local_normal.y + m[8] * local_normal.z};
                const auto length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
                if (length > 0.0F) {
                    vertex.nx = normal.x / length; vertex.ny = normal.y / length; vertex.nz = normal.z / length;
                }
                gpu_vertices_.push_back(vertex);
                vertices_.push_back(position);
                minimum.x = std::min(minimum.x, position.x); minimum.y = std::min(minimum.y, position.y);
                minimum.z = std::min(minimum.z, position.z); maximum.x = std::max(maximum.x, position.x);
                maximum.y = std::max(maximum.y, position.y); maximum.z = std::max(maximum.z, position.z);
            }
            for (std::uint32_t i = 0; i < source_batch.count / 3U; ++i) faces_.push_back({});
            draw_batches_.push_back(output_batch);
        }
        ++scene_custom_instance_count_;
    }

    // CSF stores the terrain as a RenderWare World after its custom instance
    // region. Document recovery exposes the World itself, while its
    // historically short sector/plugin payloads make declared-size BSP walking
    // unreliable. Use the shared core recovery result rather than the parsed tree.
    const auto world_chunk = std::find_if(chunks.begin(), chunks.end(), [](const rws::Chunk& chunk) {
        return chunk.type == 0x0B;
    });
    if (!main_is_collision && world_chunk != chunks.end()) {
        const auto world = rws::decode_world(*world_chunk, bytes);
        if (world) {
            const auto* material_list_chunk = rws::find_child(*world_chunk, 0x08);
            const auto material_list = material_list_chunk ?
                rws::decode_material_list(*material_list_chunk, bytes) : rws::DecodeResult<rws::MaterialListInfo>{};
            const std::size_t material_base = material_colors_.size();
            const auto material_count = material_list && material_list.value->material_count > 0 ?
                static_cast<std::size_t>(material_list.value->material_count) : 1U;
            material_colors_.resize(material_base + material_count, {155, 158, 150, 255});
            material_textures_.resize(material_base + material_count);
            material_lightmap_textures_.resize(material_base + material_count);
            material_texture_names_.resize(material_base + material_count);
            material_lightmap_texture_names_.resize(material_base + material_count);
            if (material_list_chunk && material_list) {
                std::vector<const rws::Chunk*> materials;
                for (const auto& child : material_list_chunk->children)
                    if (child.type == 0x07) materials.push_back(&child);
                std::size_t next_material{};
                for (std::size_t slot = 0; slot < material_count; ++slot) {
                    const auto destination = material_base + slot;
                    const auto remap = material_list.value->remap[slot];
                    if (remap >= 0 && static_cast<std::size_t>(remap) < slot) {
                        const auto source = material_base + static_cast<std::size_t>(remap);
                        material_colors_[destination] = material_colors_[source];
                        material_textures_[destination] = material_textures_[source];
                        material_lightmap_textures_[destination] = material_lightmap_textures_[source];
                        material_texture_names_[destination] = material_texture_names_[source];
                        material_lightmap_texture_names_[destination] = material_lightmap_texture_names_[source];
                        continue;
                    }
                    if (next_material >= materials.size()) continue;
                    const auto& material_chunk = *materials[next_material++];
                    const auto material = rws::decode_material(material_chunk, bytes);
                    if (material) material_colors_[destination] = material.value->color;
                    if (const auto* texture_chunk = rws::find_child(material_chunk, 0x06)) {
                        const auto texture = rws::decode_texture(*texture_chunk, bytes);
                        if (texture && !texture.value->name.empty()) {
                            material_texture_names_[destination] = texture.value->name;
                            material_textures_[destination] = load_texture(texture.value->name);
                        }
                    }
                    const auto* extension = rws::find_child(material_chunk, 0x03);
                    const auto* effects_chunk = extension ? rws::find_child(*extension, 0x120) : nullptr;
                    if (effects_chunk) {
                        const auto effects = rws::decode_material_effects(*effects_chunk, 0x07, bytes);
                        if (effects && effects.value->has_dual_texture &&
                            !effects.value->dual_texture.name.empty()) {
                            material_lightmap_texture_names_[destination] = effects.value->dual_texture.name;
                            material_lightmap_textures_[destination] = load_texture(effects.value->dual_texture.name);
                        }
                    }
                }
            }

            const auto recovered_world = rws::recover_world(*world_chunk, bytes);
            for (const auto& recovered_sector : recovered_world.sectors) {
                const auto triangle_count = recovered_sector.triangle_count;
                const auto vertex_count = recovered_sector.vertex_count;
                const auto uv_sets = recovered_sector.texcoord_sets;
                std::vector<rws::Vec3> sector_vertices;
                sector_vertices.reserve(static_cast<std::size_t>(vertex_count));
                for (std::int32_t i = 0; i < vertex_count; ++i) {
                    const auto offset = recovered_sector.vertices_offset + static_cast<std::uint64_t>(i) * 12U;
                    const rws::Vec3 vertex{read_f32(bytes, offset), read_f32(bytes, offset + 4),
                                           read_f32(bytes, offset + 8)};
                    sector_vertices.push_back(vertex);
                    vertices_.push_back(vertex);
                    minimum.x = std::min(minimum.x, vertex.x); minimum.y = std::min(minimum.y, vertex.y);
                    minimum.z = std::min(minimum.z, vertex.z); maximum.x = std::max(maximum.x, vertex.x);
                    maximum.y = std::max(maximum.y, vertex.y); maximum.z = std::max(maximum.z, vertex.z);
                }
                struct SectorTriangle { std::array<std::uint16_t, 3> vertices; std::uint16_t material; };
                std::vector<SectorTriangle> sector_triangles;
                sector_triangles.reserve(static_cast<std::size_t>(triangle_count));
                for (std::int32_t i = 0; i < triangle_count; ++i) {
                    const auto triangle = rws::decode_recovered_world_triangle(recovered_sector, i, bytes);
                    if (triangle) sector_triangles.push_back({triangle.value->vertices,
                                                              triangle.value->material});
                }
                std::stable_sort(sector_triangles.begin(), sector_triangles.end(),
                    [](const auto& left, const auto& right) { return left.material < right.material; });
                for (const auto& triangle : sector_triangles) {
                    if (triangle.vertices[0] >= sector_vertices.size() ||
                        triangle.vertices[1] >= sector_vertices.size() ||
                        triangle.vertices[2] >= sector_vertices.size()) continue;
                    const auto local_material = std::clamp<std::int64_t>(
                        static_cast<std::int64_t>(recovered_sector.material_window_base) + triangle.material,
                        0, static_cast<std::int64_t>(material_count - 1));
                    const auto global_material = material_base + static_cast<std::size_t>(local_material);
                    if (draw_batches_.empty() || draw_batches_.back().material != global_material ||
                        draw_batches_.back().owner_offset != world_chunk->offset)
                    {
                        const auto& texture_name = material_texture_names_[global_material];
                        const bool floor_material = texture_name.size() >= 4 &&
                            texture_name.compare(0, 4, "FFLR") == 0;
                        draw_batches_.push_back({static_cast<std::uint16_t>(global_material),
                            static_cast<std::uint32_t>(gpu_vertices_.size()), 0,
                            world_chunk->offset, floor_material, PreviewLayer::visual_world});
                    }
                    const auto& a = sector_vertices[triangle.vertices[0]];
                    const auto& b = sector_vertices[triangle.vertices[1]];
                    const auto& c = sector_vertices[triangle.vertices[2]];
                    float nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
                    float ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
                    float nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                    const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (length > 0.0F) { nx /= length; ny /= length; nz /= length; }
                    for (const auto index : triangle.vertices) {
                        const auto& vertex = sector_vertices[index];
                        const auto base = uv_sets > 0 ? recovered_sector.texcoord_offsets[0] +
                            static_cast<std::uint64_t>(index) * 8U : 0U;
                        const auto lightmap = uv_sets > 1 ? recovered_sector.texcoord_offsets[1] +
                            static_cast<std::uint64_t>(index) * 8U : 0U;
                        const Uv base_uv = base ? Uv{read_f32(bytes, base), read_f32(bytes, base + 4)} : Uv{};
                        const Uv lightmap_uv = lightmap ?
                            Uv{read_f32(bytes, lightmap), read_f32(bytes, lightmap + 4)} : Uv{};
                        gpu_vertices_.push_back({vertex.x, vertex.y, vertex.z,
                            base_uv.u, base_uv.v, lightmap_uv.u, lightmap_uv.v,
                            lightmap_uv.u, lightmap_uv.v, nx, ny, nz, index});
                    }
                    faces_.push_back({});
                    draw_batches_.back().count += 3;
                    ++scene_world_triangle_count_;
                }
                ++scene_world_sector_count_;
            }
        }
    }

    if (!vertices_.empty()) {
        visual_center_ = {(minimum.x + maximum.x) * 0.5F, (minimum.y + maximum.y) * 0.5F,
                          (minimum.z + maximum.z) * 0.5F};
        const auto dx = maximum.x - minimum.x, dy = maximum.y - minimum.y,
                   dz = maximum.z - minimum.z;
        visual_radius_ = std::max(0.5F * std::sqrt(dx * dx + dy * dy + dz * dz), 0.001F);
    }

    auto append_collision = [&](const rws::Document& collision) {
        const auto collision_worlds = rws::recover_worlds(collision.chunks(), collision.bytes());
        if (collision_worlds.empty()) {
            collision_diagnostics_.emplace_back("Collision document has no World");
            return;
        }
        const auto& recovered = collision_worlds.front();
        collision_recovery_status_ = recovered.status;
        collision_declared_sector_count_ = recovered.header.world_sector_count;
        collision_sector_count_ = recovered.sectors.size();
        collision_triangle_count_ = static_cast<std::size_t>(recovered.recovered_triangles);
        collision_material_count_ = static_cast<std::size_t>(std::max(0, recovered.material_count));
        collision_diagnostics_ = recovered.diagnostics;
        collision_source_path_ = collision.source_path();
        collision_center_ = {(recovered.header.bounding_box_inf.x + recovered.header.bounding_box_sup.x) * 0.5F,
                             (recovered.header.bounding_box_inf.y + recovered.header.bounding_box_sup.y) * 0.5F,
                             (recovered.header.bounding_box_inf.z + recovered.header.bounding_box_sup.z) * 0.5F};
        const auto collision_dx = recovered.header.bounding_box_sup.x - recovered.header.bounding_box_inf.x;
        const auto collision_dy = recovered.header.bounding_box_sup.y - recovered.header.bounding_box_inf.y;
        const auto collision_dz = recovered.header.bounding_box_sup.z - recovered.header.bounding_box_inf.z;
        collision_radius_ = std::max(0.5F * std::sqrt(collision_dx * collision_dx +
            collision_dy * collision_dy + collision_dz * collision_dz), 0.001F);
        const auto collision_world = std::find_if(collision.chunks().begin(), collision.chunks().end(),
            [&](const rws::Chunk& chunk) { return chunk.type == 0x0B && chunk.offset == recovered.world_offset; });
        if (collision_world == collision.chunks().end() || recovered.sectors.empty()) return;

        const auto material_base = material_colors_.size();
        const auto material_count = std::max<std::size_t>(1U, collision_material_count_);
        material_colors_.resize(material_base + material_count);
        material_textures_.resize(material_base + material_count);
        material_lightmap_textures_.resize(material_base + material_count);
        material_texture_names_.resize(material_base + material_count);
        material_lightmap_texture_names_.resize(material_base + material_count);
        collision_surface_labels_.assign(material_count, "Unknown");
        for (std::size_t slot = 0; slot < material_count; ++slot)
            material_colors_[material_base + slot] = collision_surface_color({}, static_cast<std::uint16_t>(slot));

        if (const auto* list_chunk = rws::find_child(*collision_world, 0x08)) {
            const auto list = rws::decode_material_list(*list_chunk, collision.bytes());
            std::vector<const rws::Chunk*> materials;
            for (const auto& child : list_chunk->children)
                if (child.type == 0x07) materials.push_back(&child);
            std::size_t next_material{};
            for (std::size_t slot = 0; list && slot < material_count; ++slot) {
                const auto remap = list.value->remap[slot];
                if (remap >= 0 && static_cast<std::size_t>(remap) < slot) {
                    material_colors_[material_base + slot] = material_colors_[material_base + remap];
                    collision_surface_labels_[slot] = collision_surface_labels_[remap];
                    continue;
                }
                if (next_material >= materials.size()) continue;
                const auto* extension = rws::find_child(*materials[next_material++], 0x03);
                const auto* pyro = extension ? rws::find_child(*extension, 0xFFFFFF00U) : nullptr;
                const auto metadata = pyro ? rws::decode_pyro_extension(*pyro, 0x07, collision.bytes()) :
                    rws::DecodeResult<rws::PyroExtensionInfo>{};
                if (!metadata) continue;
                const auto name = std::string(metadata.value->object_name());
                const auto surface = metadata.value->material_surface_type();
                collision_surface_labels_[slot] = name.empty() ? "Unknown" : name;
                if (surface) collision_surface_labels_[slot] += " (ID " + std::to_string(*surface) + ')';
                material_colors_[material_base + slot] =
                    collision_surface_color(name, static_cast<std::uint16_t>(slot));
            }
        }

        for (const auto& sector : recovered.sectors) {
            std::vector<rws::Vec3> sector_vertices;
            sector_vertices.reserve(static_cast<std::size_t>(sector.vertex_count));
            for (std::int32_t i = 0; i < sector.vertex_count; ++i) {
                const auto offset = sector.vertices_offset + static_cast<std::uint64_t>(i) * 12U;
                const rws::Vec3 vertex{read_f32(collision.bytes(), offset),
                    read_f32(collision.bytes(), offset + 4U), read_f32(collision.bytes(), offset + 8U)};
                sector_vertices.push_back(vertex);
                vertices_.push_back(vertex);
                minimum.x = std::min(minimum.x, vertex.x); minimum.y = std::min(minimum.y, vertex.y);
                minimum.z = std::min(minimum.z, vertex.z); maximum.x = std::max(maximum.x, vertex.x);
                maximum.y = std::max(maximum.y, vertex.y); maximum.z = std::max(maximum.z, vertex.z);
            }
            struct CollisionTriangle { std::array<std::uint16_t, 3> vertices; std::uint16_t material; };
            std::vector<CollisionTriangle> triangles;
            for (std::int32_t i = 0; i < sector.triangle_count; ++i) {
                const auto decoded_triangle = rws::decode_recovered_world_triangle(sector, i, collision.bytes());
                if (!decoded_triangle) continue;
                const auto resolved = static_cast<std::int64_t>(sector.material_window_base) +
                                      decoded_triangle.value->material;
                if (decoded_triangle.value->vertices[0] >= sector_vertices.size() ||
                    decoded_triangle.value->vertices[1] >= sector_vertices.size() ||
                    decoded_triangle.value->vertices[2] >= sector_vertices.size() ||
                    resolved < 0 || resolved >= static_cast<std::int64_t>(material_count)) continue;
                triangles.push_back({decoded_triangle.value->vertices, static_cast<std::uint16_t>(resolved)});
            }
            std::stable_sort(triangles.begin(), triangles.end(), [](const auto& left, const auto& right) {
                return left.material < right.material;
            });
            for (const auto& triangle : triangles) {
                const auto global_material = material_base + triangle.material;
                if (draw_batches_.empty() || draw_batches_.back().material != global_material ||
                    draw_batches_.back().owner_offset != sector.chunk_offset ||
                    draw_batches_.back().layer != PreviewLayer::collision_world) {
                    draw_batches_.push_back({static_cast<std::uint16_t>(global_material),
                        static_cast<std::uint32_t>(gpu_vertices_.size()), 0, sector.chunk_offset,
                        false, PreviewLayer::collision_world});
                }
                const auto& a = sector_vertices[triangle.vertices[0]];
                const auto& b = sector_vertices[triangle.vertices[1]];
                const auto& c = sector_vertices[triangle.vertices[2]];
                float nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
                float ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
                float nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (length > 0.0F) { nx /= length; ny /= length; nz /= length; }
                for (const auto index : triangle.vertices) {
                    const auto& vertex = sector_vertices[index];
                    gpu_vertices_.push_back({vertex.x, vertex.y, vertex.z, 0, 0, 0, 0, 0, 0,
                                             nx, ny, nz, index});
                }
                faces_.push_back({});
                draw_batches_.back().count += 3;
            }
        }
    };

    visual_material_slot_count_ = material_colors_.size();
    if (collision_document) append_collision(*collision_document);
    if (main_is_collision) {
        show_visual_ = false;
        show_collision_ = true;
    } else {
        show_visual_ = true;
    }

    if (gpu_vertices_.empty()) {
        error_ = "No renderable standard Clump/Atomic instances were found";
        return false;
    }
    center_ = {(minimum.x + maximum.x) * 0.5F, (minimum.y + maximum.y) * 0.5F,
               (minimum.z + maximum.z) * 0.5F};
    radius_ = 0.0F;
    for (const auto& vertex : vertices_) {
        const auto x = vertex.x - center_.x, y = vertex.y - center_.y, z = vertex.z - center_.z;
        radius_ = std::max(radius_, std::sqrt(x * x + y * y + z * z));
    }
    radius_ = std::max(radius_, 0.001F);
    all_center_ = center_;
    all_radius_ = radius_;
    if (!create_gpu_resources()) return false;
    reset_view();
    return true;
}

void GeometryPreview::render_callback(const ImDrawList*, const ImDrawCmd* command) {
    static_cast<GeometryPreview*>(command->UserCallbackData)->render_gpu();
}

bool GeometryPreview::create_gpu_resources() {
    auto& gl = gl_api();
    if (!gl.create_shader || gpu_vertices_.empty()) {
        error_ = "OpenGL 3.3 functions are unavailable";
        return false;
    }
    constexpr const char* vertex_source = R"GLSL(#version 330 core
layout(location=0) in vec3 aPosition;
layout(location=1) in vec2 aBaseUv;
layout(location=2) in vec2 aLightmapUv;
layout(location=3) in vec2 aDebugUv;
layout(location=4) in vec3 aNormal;
uniform vec3 uCenter;
uniform float uYaw, uPitch, uDistance;
uniform vec2 uPan;
uniform float uAspect, uTanHalfFov, uNear, uFar;
uniform bool uYUp;
out vec2 vUv;
out vec2 vLightmapUv;
out vec2 vDebugUv;
out vec3 vNormal;
void main() {
    vec3 p = aPosition - uCenter;
    float cy=cos(uYaw), sy=sin(uYaw), cp=cos(uPitch), sp=sin(uPitch);
    vec3 view;
    if (uYUp) {
        float rx=cy*p.x-sy*p.z, rz=sy*p.x+cy*p.z;
        view=vec3(rx+uPan.x, cp*p.y-sp*rz+uPan.y, sp*p.y+cp*rz-uDistance);
        float nrx=cy*aNormal.x-sy*aNormal.z, nrz=sy*aNormal.x+cy*aNormal.z;
        vNormal=vec3(nrx, cp*aNormal.y-sp*nrz, sp*aNormal.y+cp*nrz);
    } else {
        float rx=cy*p.x-sy*p.y, ry=sy*p.x+cy*p.y;
        view=vec3(rx+uPan.x, cp*p.z-sp*ry+uPan.y, sp*p.z+cp*ry-uDistance);
        float nrx=cy*aNormal.x-sy*aNormal.y, nry=sy*aNormal.x+cy*aNormal.y;
        vNormal=vec3(nrx, cp*aNormal.z-sp*nry, sp*aNormal.z+cp*nry);
    }
    float f=1.0/uTanHalfFov;
    gl_Position=vec4(view.x*f/uAspect, view.y*f,
        ((uFar+uNear)/(uNear-uFar))*view.z+(2.0*uFar*uNear)/(uNear-uFar), -view.z);
    vUv=aBaseUv;
    vLightmapUv=aLightmapUv;
    vDebugUv=aDebugUv;
})GLSL";
    constexpr const char* fragment_source = R"GLSL(#version 330 core
in vec2 vUv;
in vec2 vLightmapUv;
in vec2 vDebugUv;
in vec3 vNormal;
uniform sampler2D uTexture;
uniform sampler2D uLightmapTexture;
uniform bool uUseTexture;
uniform bool uUseLightmap;
uniform bool uLightmapOnly;
uniform bool uUseDebugUv;
uniform bool uApplyLighting;
uniform bool uForceOpaque;
uniform float uLightmapIntensity;
uniform vec4 uBaseColor;
out vec4 FragColor;
void main() {
    float light=0.42+0.58*abs(dot(normalize(vNormal), normalize(vec3(0.35,0.55,0.75))));
    vec2 uv=uUseDebugUv ? vDebugUv : vUv;
    vec4 color=uUseTexture ? texture(uTexture,uv) : uBaseColor;
    if (uUseLightmap) {
        vec4 lightmap=texture(uLightmapTexture,vLightmapUv);
        vec3 lit=lightmap.rgb*uLightmapIntensity;
        color=uLightmapOnly ? vec4(lit,1.0) : vec4(color.rgb*lit,color.a);
    }
    if (uForceOpaque) color.a=1.0;
    if (color.a < 0.08) discard;
    FragColor=vec4(color.rgb*(uApplyLighting ? light : 1.0),color.a);
})GLSL";
    auto compile = [&](const GLenum type, const char* source) -> GLuint {
        const GLuint shader = gl.create_shader(type);
        gl.shader_source(shader, 1, &source, nullptr);
        gl.compile_shader(shader);
        GLint okay{};
        gl.get_shader_iv(shader, gl_compile_status, &okay);
        if (!okay) {
            std::array<char, 1024> log{};
            gl.get_shader_log(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
            error_ = "OpenGL shader compile failed: " + std::string(log.data());
            gl.delete_shader(shader);
            return 0;
        }
        return shader;
    };
    const GLuint vertex_shader = compile(gl_vertex_shader, vertex_source);
    if (!vertex_shader) return false;
    const GLuint fragment_shader = compile(gl_fragment_shader, fragment_source);
    if (!fragment_shader) { gl.delete_shader(vertex_shader); return false; }
    shader_program_ = gl.create_program();
    gl.attach_shader(shader_program_, vertex_shader);
    gl.attach_shader(shader_program_, fragment_shader);
    gl.link_program(shader_program_);
    gl.delete_shader(vertex_shader);
    gl.delete_shader(fragment_shader);
    GLint linked{};
    gl.get_program_iv(shader_program_, gl_link_status, &linked);
    if (!linked) {
        std::array<char, 1024> log{};
        gl.get_program_log(shader_program_, static_cast<GLsizei>(log.size()), nullptr, log.data());
        error_ = "OpenGL shader link failed: " + std::string(log.data());
        destroy_gpu_resources();
        return false;
    }
    gl.gen_vertex_arrays(1, &vertex_array_);
    gl.gen_buffers(1, &vertex_buffer_);
    gl.bind_vertex_array(vertex_array_);
    gl.bind_buffer(gl_array_buffer, vertex_buffer_);
    gl.buffer_data(gl_array_buffer, static_cast<GlSizePtr>(gpu_vertices_.size() * sizeof(GpuVertex)),
                   gpu_vertices_.data(), gl_static_draw);
    gl.enable_vertex_attrib_array(0);
    gl.vertex_attrib_pointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), reinterpret_cast<void*>(offsetof(GpuVertex, x)));
    gl.enable_vertex_attrib_array(1);
    gl.vertex_attrib_pointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                             reinterpret_cast<void*>(offsetof(GpuVertex, base_u)));
    gl.enable_vertex_attrib_array(2);
    gl.vertex_attrib_pointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                             reinterpret_cast<void*>(offsetof(GpuVertex, lightmap_u)));
    gl.enable_vertex_attrib_array(3);
    gl.vertex_attrib_pointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                             reinterpret_cast<void*>(offsetof(GpuVertex, debug_u)));
    gl.enable_vertex_attrib_array(4);
    gl.vertex_attrib_pointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                             reinterpret_cast<void*>(offsetof(GpuVertex, nx)));
    gl.bind_vertex_array(0);
    return true;
}

void GeometryPreview::destroy_gpu_resources() {
    auto& gl = gl_api();
    if (vertex_buffer_ && gl.delete_buffers) gl.delete_buffers(1, &vertex_buffer_);
    if (vertex_array_ && gl.delete_vertex_arrays) gl.delete_vertex_arrays(1, &vertex_array_);
    if (shader_program_ && gl.delete_program) gl.delete_program(shader_program_);
    vertex_buffer_ = vertex_array_ = shader_program_ = 0;
}

void GeometryPreview::render_gpu() {
    if (!shader_program_ || !vertex_array_ || draw_batches_.empty() ||
        canvas_width_ < 1.0F || canvas_height_ < 1.0F) return;
    auto& gl = gl_api();
    const auto& io = ImGui::GetIO();
    const int viewport_x = static_cast<int>(canvas_x_ * io.DisplayFramebufferScale.x);
    const int viewport_y = static_cast<int>((io.DisplaySize.y - canvas_y_ - canvas_height_) *
                                             io.DisplayFramebufferScale.y);
    const int viewport_width = std::max(1, static_cast<int>(canvas_width_ * io.DisplayFramebufferScale.x));
    const int viewport_height = std::max(1, static_cast<int>(canvas_height_ * io.DisplayFramebufferScale.y));

    glViewport(viewport_x, viewport_y, viewport_width, viewport_height);
    glScissor(viewport_x, viewport_y, viewport_width, viewport_height);
    glEnable(GL_SCISSOR_TEST);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    if (cull_backfaces_) { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); }
    else glDisable(GL_CULL_FACE);
    gl.use_program(shader_program_);
    gl.bind_vertex_array(vertex_array_);
    gl.active_texture(gl_texture0);

    const float pan_world_x = pan_x_ * distance_;
    const float pan_world_z = -pan_y_ * distance_;
    const float near_plane = std::max(distance_ * 0.001F,
        std::max(radius_ * 0.000001F, 0.001F));
    const float navigation_distance = std::sqrt(navigation_offset_.x * navigation_offset_.x +
        navigation_offset_.y * navigation_offset_.y + navigation_offset_.z * navigation_offset_.z);
    const float far_plane = std::max(distance_ + radius_ * 3.0F + navigation_distance,
                                     near_plane + 1.0F);
    gl.uniform_3f(gl.get_uniform_location(shader_program_, "uCenter"),
                  center_.x + navigation_offset_.x, center_.y + navigation_offset_.y,
                  center_.z + navigation_offset_.z);
    gl.uniform_1f(gl.get_uniform_location(shader_program_, "uYaw"), yaw_);
    gl.uniform_1f(gl.get_uniform_location(shader_program_, "uPitch"), pitch_);
    gl.uniform_1f(gl.get_uniform_location(shader_program_, "uDistance"), distance_);
    gl.uniform_2f(gl.get_uniform_location(shader_program_, "uPan"), pan_world_x, pan_world_z);
    gl.uniform_1f(gl.get_uniform_location(shader_program_, "uAspect"),
                  static_cast<float>(viewport_width) / static_cast<float>(viewport_height));
    gl.uniform_1f(gl.get_uniform_location(shader_program_, "uTanHalfFov"),
                  std::tan(25.0F * 3.14159265358979323846F / 180.0F));
    gl.uniform_1f(gl.get_uniform_location(shader_program_, "uNear"), near_plane);
    gl.uniform_1f(gl.get_uniform_location(shader_program_, "uFar"), far_plane);
    gl.uniform_1i(gl.get_uniform_location(shader_program_, "uYUp"), scene_mode_);
    gl.uniform_1i(gl.get_uniform_location(shader_program_, "uTexture"), 0);
    gl.uniform_1i(gl.get_uniform_location(shader_program_, "uLightmapTexture"), 1);
    const GLint use_texture_location = gl.get_uniform_location(shader_program_, "uUseTexture");
    const GLint use_lightmap_location = gl.get_uniform_location(shader_program_, "uUseLightmap");
    const GLint lightmap_only_location = gl.get_uniform_location(shader_program_, "uLightmapOnly");
    const GLint use_debug_uv_location = gl.get_uniform_location(shader_program_, "uUseDebugUv");
    const GLint base_color_location = gl.get_uniform_location(shader_program_, "uBaseColor");
    const GLint force_opaque_location = gl.get_uniform_location(shader_program_, "uForceOpaque");
    gl.uniform_1f(gl.get_uniform_location(shader_program_, "uLightmapIntensity"), lightmap_intensity_);
    gl.uniform_1i(use_debug_uv_location, view_style_ == 3 || view_style_ == 4);
    gl.uniform_1i(gl.get_uniform_location(shader_program_, "uApplyLighting"), view_style_ < 3);

    auto set_color = [&](const DrawBatch& batch) {
        const auto material = batch.material;
        if ((batch.layer == PreviewLayer::collision_world && collision_color_mode_ == 1) ||
            (batch.layer != PreviewLayer::collision_world && view_style_ == 1)) {
            const auto palette_material = batch.layer == PreviewLayer::collision_world &&
                    static_cast<std::size_t>(material) >= visual_material_slot_count_ ?
                static_cast<std::uint16_t>(static_cast<std::size_t>(material) -
                                           visual_material_slot_count_) : material;
            const auto packed = material_color(palette_material, 1.0F);
            gl.uniform_4f(base_color_location,
                static_cast<float>((packed >> IM_COL32_R_SHIFT) & 0xFFU) / 255.0F,
                static_cast<float>((packed >> IM_COL32_G_SHIFT) & 0xFFU) / 255.0F,
                static_cast<float>((packed >> IM_COL32_B_SHIFT) & 0xFFU) / 255.0F,
                batch.layer == PreviewLayer::collision_world ? collision_opacity_ : 1.0F);
            return;
        }
        if (batch.layer == PreviewLayer::collision_world && collision_color_mode_ == 2) {
            gl.uniform_4f(base_color_location, 0.95F, 0.25F, 0.72F, collision_opacity_);
            return;
        }
        const auto index = static_cast<std::size_t>(material);
        const auto rgba = index < material_colors_.size() ? material_colors_[index] :
            std::array<std::uint8_t, 4>{190, 190, 190, 255};
        gl.uniform_4f(base_color_location, rgba[0] / 255.0F, rgba[1] / 255.0F,
                      rgba[2] / 255.0F, batch.layer == PreviewLayer::collision_world ?
                          collision_opacity_ : rgba[3] / 255.0F);
    };

    if (view_style_ != 7 || show_collision_) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        for (const auto& batch : draw_batches_) {
            const bool collision = batch.layer == PreviewLayer::collision_world;
            if ((!collision && !show_visual_) || (collision && (!show_collision_ || collision_style_ == 2)))
                continue;
            if (!collision && view_style_ == 7) continue;
            if (collision) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);
                if (collision_style_ == 3) glDisable(GL_DEPTH_TEST);
                else glEnable(GL_DEPTH_TEST);
            } else {
                glDisable(GL_BLEND); glDepthMask(GL_TRUE); glEnable(GL_DEPTH_TEST);
            }
            const auto material = static_cast<std::size_t>(batch.material);
            GLuint texture{}, lightmap{};
            if (!collision && (view_style_ == 0 || view_style_ == 6) && (scene_mode_ || !uv_sets_.empty()) &&
                material < material_textures_.size())
                texture = material_textures_[material];
            else if ((view_style_ == 3 || view_style_ == 4) && selected_uv_set_ < uv_sets_.size())
                texture = checker_texture_;
            if ((view_style_ == 5 || view_style_ == 6) && (scene_mode_ || uv_sets_.size() > 1) &&
                material < material_lightmap_textures_.size())
                lightmap = material_lightmap_textures_[material];
            set_color(batch);
            gl.uniform_1i(use_texture_location, texture != 0);
            gl.uniform_1i(use_lightmap_location, lightmap != 0);
            gl.uniform_1i(lightmap_only_location, view_style_ == 5);
            gl.uniform_1i(force_opaque_location, !collision && batch.force_opaque);
            gl.active_texture(gl_texture0);
            glBindTexture(GL_TEXTURE_2D, texture);
            gl.active_texture(gl_texture1);
            glBindTexture(GL_TEXTURE_2D, lightmap);
            glDrawArrays(GL_TRIANGLES, static_cast<GLint>(batch.first), static_cast<GLsizei>(batch.count));
        }
    }
    if (view_style_ == 7 || wireframe_ || (show_collision_ &&
        (collision_style_ == 1 || collision_style_ == 2))) {
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        gl.uniform_1i(use_texture_location, 0);
        gl.uniform_1i(use_lightmap_location, 0);
        gl.uniform_1i(force_opaque_location, 1);
        const float color = view_style_ == 7 ? 0.84F : 0.09F;
        gl.uniform_4f(base_color_location, color, view_style_ == 7 ? 0.88F : 0.10F,
                      view_style_ == 7 ? 0.95F : 0.13F, 1.0F);
        glDepthFunc(GL_LEQUAL);
        glEnable(GL_POLYGON_OFFSET_LINE);
        glPolygonOffset(-1.0F, -1.0F);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        for (const auto& batch : draw_batches_) {
            const bool collision = batch.layer == PreviewLayer::collision_world;
            const bool draw_visual_wire = !collision && show_visual_ && (view_style_ == 7 || wireframe_);
            const bool draw_collision_wire = collision && show_collision_ &&
                (collision_style_ == 1 || collision_style_ == 2);
            if (!draw_visual_wire && !draw_collision_wire) continue;
            if (collision) gl.uniform_4f(base_color_location, 0.1F, 0.95F, 0.95F, 1.0F);
            else gl.uniform_4f(base_color_location, color, view_style_ == 7 ? 0.88F : 0.10F,
                               view_style_ == 7 ? 0.95F : 0.13F, 1.0F);
            glDrawArrays(GL_TRIANGLES, static_cast<GLint>(batch.first), static_cast<GLsizei>(batch.count));
        }
        glDisable(GL_POLYGON_OFFSET_LINE);
    }
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    gl.active_texture(gl_texture0);
    gl.bind_vertex_array(0);
    gl.use_program(0);
}

void GeometryPreview::draw(const rws::Chunk& geometry_chunk,
                           const std::span<const std::byte> bytes,
                           const std::filesystem::path& source_path) {
    if (chunk_offset_ != geometry_chunk.offset) load(geometry_chunk, bytes, source_path);

    constexpr const char* styles[] = {
        "Textured", "Material index", "Material color", "UV checker", "Lightmap UV",
        "Lightmap texture", "Base + lightmap", "Wireframe"};
    ImGui::SetNextItemWidth(165.0F);
    if (ImGui::Combo("View style", &view_style_, styles, static_cast<int>(std::size(styles))) &&
        view_style_ >= 4 && view_style_ <= 6 && uv_sets_.size() > 1)
        select_uv_set(1);
    ImGui::SameLine();
    if (view_style_ != 7) { ImGui::Checkbox("Wire overlay", &wireframe_); ImGui::SameLine(); }
    ImGui::Checkbox("Cull backfaces", &cull_backfaces_); ImGui::SameLine();
    if (ImGui::Button("Frame geometry")) reset_view(); ImGui::SameLine();
    if (ImGui::Button("Reload edited bytes")) load(geometry_chunk, bytes, source_path);
    if (view_style_ == 0) {
        ImGui::Text("DDS files: %zu loaded | material slots unresolved: %zu | UV set: %s", loaded_texture_count_,
            missing_texture_count_, !uv_sets_.empty() ? "present" : "missing");
        if (!texture_status_.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", texture_status_.c_str()); }
    }
    if (view_style_ == 3 || view_style_ == 4) {
        if (uv_sets_.empty()) {
            ImGui::TextColored(ImVec4(1, 0.55F, 0.25F, 1), "This geometry has no UV sets.");
        } else {
            const std::string preview = "UV set " + std::to_string(selected_uv_set_ + 1);
            ImGui::SetNextItemWidth(120.0F);
            if (ImGui::BeginCombo("UV channel", preview.c_str())) {
                for (std::size_t i = 0; i < uv_sets_.size(); ++i) {
                    const std::string label = "UV set " + std::to_string(i + 1) +
                        (i == 1 ? " (lightmap convention)" : "");
                    if (ImGui::Selectable(label.c_str(), selected_uv_set_ == i)) select_uv_set(i);
                    if (selected_uv_set_ == i) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%zu channel%s available", uv_sets_.size(), uv_sets_.size() == 1 ? "" : "s");
            if (view_style_ == 4 && uv_sets_.size() < 2)
                ImGui::TextColored(ImVec4(1, 0.55F, 0.25F, 1),
                                   "UV2 is absent; showing the selected channel instead.");
        }
    }
    if (view_style_ == 5 || view_style_ == 6) {
        ImGui::SetNextItemWidth(160.0F);
        ImGui::SliderFloat("Lightmap intensity", &lightmap_intensity_, 0.25F, 4.0F, "%.2fx");
        const auto resolved = std::count_if(material_lightmap_textures_.begin(),
            material_lightmap_textures_.end(), [](const unsigned int texture) { return texture != 0; });
        ImGui::Text("Embedded MatFX lightmaps: %zu/%zu material slots resolved",
                    static_cast<std::size_t>(resolved), material_lightmap_textures_.size());
        if (!texture_status_.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", texture_status_.c_str()); }
        if (uv_sets_.size() < 2)
            ImGui::TextColored(ImVec4(1, 0.55F, 0.25F, 1),
                               "UV2 is absent; MatFX lightmapping is disabled.");
        else
            ImGui::TextDisabled("MatFX lightmap coordinates: UV set 2");
    }
    ImGui::TextDisabled("WASD/QE: move | Left: look | Right: orbit | Middle: pan | Wheel: zoom | Shift: faster");

    const auto available = ImGui::GetContentRegionAvail();
    const ImVec2 size{std::max(available.x, 64.0F), std::max(available.y, 160.0F)};
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("geometry_canvas", size,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
    auto* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, IM_COL32(22, 25, 31, 255));
    draw_list->AddRect(origin, {origin.x + size.x, origin.y + size.y}, IM_COL32(70, 76, 88, 255));
    if (ImGui::IsItemHovered()) {
        const auto& io = ImGui::GetIO();
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) reset_view();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            target_yaw_ -= io.MouseDelta.x * 0.01F;
            target_pitch_ = std::clamp(target_pitch_ + io.MouseDelta.y * 0.01F, -1.5F, 1.5F);
            preserve_camera_position_ = true;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            target_yaw_ += io.MouseDelta.x * 0.01F;
            target_pitch_ = std::clamp(target_pitch_ + io.MouseDelta.y * 0.01F, -1.5F, 1.5F);
            preserve_camera_position_ = false;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            pan_camera(io.MouseDelta.x, io.MouseDelta.y);
        }
        if (io.MouseWheel != 0.0F) {
            const float minimum_distance = scene_mode_ ? std::max(radius_ * 0.0001F, 0.05F) :
                                                         std::max(radius_ * 0.02F, 0.001F);
            distance_ = std::clamp(distance_ * std::exp(-io.MouseWheel * 0.16F), minimum_distance,
                                   radius_ * 100.0F);
        }
    }
    update_keyboard_navigation();
    if (!error_.empty()) {
        draw_list->AddText({origin.x + 12, origin.y + 12}, IM_COL32(255, 120, 90, 255), error_.c_str());
        return;
    }

    canvas_x_ = origin.x; canvas_y_ = origin.y;
    canvas_width_ = size.x; canvas_height_ = size.y;
    draw_list->AddCallback(render_callback, this);
    draw_list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    const std::string statistics = std::to_string(vertices_.size()) + " vertices | " +
        std::to_string(faces_.size()) + " triangles | GPU depth test";
    draw_list->AddText({origin.x + 10, origin.y + 9}, IM_COL32(225, 229, 238, 255), statistics.c_str());
}

void GeometryPreview::draw_scene(const std::vector<rws::Chunk>& chunks,
                                 const std::span<const std::byte> bytes,
                                 const std::span<const rws::SceneInstance> instances,
                                 const std::filesystem::path& source_path,
                                 std::optional<std::uint64_t>& selected_chunk,
                                 const rws::Document* collision_document,
                                 const bool main_is_collision,
                                 const std::string_view collision_status) {
    if (!scene_mode_) load_scene(chunks, bytes, instances, source_path, collision_document,
                                 main_is_collision);

    ImGui::Text("Scene: %zu clumps | %zu atomic instances | %zu terrain sectors (%zu triangles) | %zu skipped",
                scene_clump_count_, scene_instance_count_, scene_world_sector_count_,
                scene_world_triangle_count_, scene_skipped_count_);
    ImGui::Text("CSF placements: %zu rendered | %zu unresolved",
                scene_custom_instance_count_, scene_unresolved_instance_count_);
    const bool has_collision = collision_sector_count_ != 0;
    const bool has_visual = std::any_of(draw_batches_.begin(), draw_batches_.end(),
        [](const DrawBatch& batch) { return batch.layer != PreviewLayer::collision_world; });
    ImGui::BeginDisabled(!has_visual);
    ImGui::Checkbox("Show visual scene", &show_visual_);
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(!has_collision);
    ImGui::Checkbox("Show level collision", &show_collision_);
    ImGui::EndDisabled();
    if (has_collision) {
        ImGui::Text("Collision: %zu/%d sectors | %zu triangles | %zu materials | %s",
            collision_sector_count_, collision_declared_sector_count_, collision_triangle_count_,
            collision_material_count_, rws::world_recovery_status_name(collision_recovery_status_));
        ImGui::TextDisabled("Companion: %s", collision_source_path_.filename().string().c_str());
        constexpr const char* collision_styles[] = {"Solid", "Solid + wire", "Wireframe", "X-ray"};
        constexpr const char* collision_colors[] = {"Surface", "Material index", "Single color"};
        ImGui::SetNextItemWidth(145.0F);
        ImGui::Combo("Collision style", &collision_style_, collision_styles,
                     static_cast<int>(std::size(collision_styles)));
        ImGui::SameLine(); ImGui::SetNextItemWidth(145.0F);
        ImGui::Combo("Color by", &collision_color_mode_, collision_colors,
                     static_cast<int>(std::size(collision_colors)));
        ImGui::SameLine(); ImGui::SetNextItemWidth(130.0F);
        ImGui::SliderFloat("Opacity", &collision_opacity_, 0.05F, 1.0F, "%.2f");
        if (!collision_diagnostics_.empty()) {
            ImGui::TextColored(ImVec4(1.0F, 0.65F, 0.2F, 1.0F), "%s",
                               collision_diagnostics_.front().c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                for (const auto& diagnostic : collision_diagnostics_)
                    ImGui::TextUnformatted(diagnostic.c_str());
                ImGui::EndTooltip();
            }
        }
        if (ImGui::TreeNode("Collision surfaces")) {
            for (std::size_t i = 0; i < collision_surface_labels_.size(); ++i)
                ImGui::Text("%zu: %s", i, collision_surface_labels_[i].c_str());
            ImGui::TreePop();
        }
    } else if (!collision_status.empty()) {
        ImGui::TextDisabled("Collision: %.*s", static_cast<int>(collision_status.size()),
                            collision_status.data());
    }
    constexpr std::array scene_styles{0, 1, 2, 5, 6, 7};
    constexpr const char* scene_style_names[] = {
        "Textured", "Material index", "Material color", "Lightmap texture",
        "Base + lightmap", "Wireframe"};
    auto selected_style = std::find(scene_styles.begin(), scene_styles.end(), view_style_);
    int scene_style = selected_style == scene_styles.end() ? 1 :
        static_cast<int>(std::distance(scene_styles.begin(), selected_style));
    ImGui::SetNextItemWidth(165.0F);
    if (ImGui::Combo("View style", &scene_style, scene_style_names,
                     static_cast<int>(std::size(scene_style_names))))
        view_style_ = scene_styles[static_cast<std::size_t>(scene_style)];
    ImGui::SameLine();
    if (view_style_ != 7) { ImGui::Checkbox("Wire overlay", &wireframe_); ImGui::SameLine(); }
    ImGui::Checkbox("Cull backfaces", &cull_backfaces_); ImGui::SameLine();
    if (ImGui::Button("Frame all")) frame_bounds(all_center_, all_radius_); ImGui::SameLine();
    ImGui::BeginDisabled(!has_visual);
    if (ImGui::Button("Frame visual")) frame_bounds(visual_center_, visual_radius_); ImGui::SameLine();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!has_collision);
    if (ImGui::Button("Frame collision")) frame_bounds(collision_center_, collision_radius_);
    ImGui::EndDisabled(); ImGui::SameLine();
    if (ImGui::Button("Reload edited bytes"))
        load_scene(chunks, bytes, instances, source_path, collision_document, main_is_collision);
    ImGui::SetNextItemWidth(150.0F);
    ImGui::SliderFloat("Move speed", &navigation_speed_, 0.05F, 4.0F, "%.2fx",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    ImGui::TextDisabled("camera distance %.1f", distance_);
    const float details_height = ImGui::GetTextLineHeightWithSpacing() * 3.0F;
    if (ImGui::BeginChild("scene_style_details", {0.0F, details_height}, ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        if (view_style_ == 0)
            ImGui::Text("DDS files: %zu loaded | material slots unresolved: %zu",
                        loaded_texture_count_, missing_texture_count_);
        if (view_style_ == 5 || view_style_ == 6) {
            ImGui::SetNextItemWidth(180.0F);
            ImGui::SliderFloat("Lightmap intensity", &lightmap_intensity_, 0.25F, 4.0F);
            const auto visual_end = material_lightmap_textures_.begin() +
                static_cast<std::ptrdiff_t>(std::min(visual_material_slot_count_,
                                                     material_lightmap_textures_.size()));
            const auto resolved = std::count_if(material_lightmap_textures_.begin(), visual_end,
                [](const unsigned int texture) { return texture != 0; });
            ImGui::Text("Standard-clump MatFX lightmaps: %zu/%zu material slots",
                        static_cast<std::size_t>(resolved), visual_material_slot_count_);
        }
        if (!texture_status_.empty()) ImGui::TextDisabled("%s", texture_status_.c_str());
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Click: select Clump/World | Left drag: look | Right: orbit | WASD/QE: move | Wheel: zoom");

    const auto available = ImGui::GetContentRegionAvail();
    const ImVec2 size{std::max(available.x, 64.0F), std::max(available.y, 160.0F)};
    const auto origin = ImGui::GetCursorScreenPos();
    canvas_x_ = origin.x; canvas_y_ = origin.y;
    canvas_width_ = size.x; canvas_height_ = size.y;
    ImGui::InvisibleButton("scene_canvas", size,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
    auto* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, IM_COL32(22, 25, 31, 255));
    draw_list->AddRect(origin, {origin.x + size.x, origin.y + size.y}, IM_COL32(70, 76, 88, 255));
    if (ImGui::IsItemHovered()) {
        const auto& io = ImGui::GetIO();
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            const auto drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            if (drag.x * drag.x + drag.y * drag.y < 16.0F) {
                if (const auto picked = pick_scene(io.MousePos.x, io.MousePos.y)) selected_chunk = *picked;
            }
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) reset_view();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            target_yaw_ -= io.MouseDelta.x * 0.01F;
            target_pitch_ = std::clamp(target_pitch_ + io.MouseDelta.y * 0.01F, -1.5F, 1.5F);
            preserve_camera_position_ = true;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            target_yaw_ += io.MouseDelta.x * 0.01F;
            target_pitch_ = std::clamp(target_pitch_ + io.MouseDelta.y * 0.01F, -1.5F, 1.5F);
            preserve_camera_position_ = false;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            pan_camera(io.MouseDelta.x, io.MouseDelta.y);
        }
        if (io.MouseWheel != 0.0F) {
            const float minimum_distance = scene_mode_ ? std::max(radius_ * 0.0001F, 0.05F) :
                                                         std::max(radius_ * 0.02F, 0.001F);
            distance_ = std::clamp(distance_ * std::exp(-io.MouseWheel * 0.16F), minimum_distance,
                                   radius_ * 100.0F);
        }
    }
    update_keyboard_navigation();
    if (!error_.empty()) {
        draw_list->AddText({origin.x + 12, origin.y + 12}, IM_COL32(255, 120, 90, 255), error_.c_str());
        return;
    }
    draw_list->AddCallback(render_callback, this);
    draw_list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    const std::string statistics = std::to_string(vertices_.size()) + " transformed vertices | " +
        std::to_string(faces_.size()) + " triangles | standard RWS scene";
    draw_list->AddText({origin.x + 10, origin.y + 9}, IM_COL32(225, 229, 238, 255), statistics.c_str());
}

} // namespace rwsman
