#include "rws/texture_image.hpp"

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstring>
#include <string_view>
#include <stdexcept>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>

namespace rws {
namespace {

std::uint16_t read_u16(const std::span<const std::byte> bytes, const std::size_t offset) {
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[offset]) |
                                      (std::to_integer<std::uint16_t>(bytes[offset + 1]) << 8U));
}

std::uint32_t read_u32(const std::span<const std::byte> bytes, const std::size_t offset) {
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

bool read_file(const std::filesystem::path& path, std::vector<std::byte>& bytes,
               std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "Cannot open " + path.string();
        return false;
    }
    stream.seekg(0, std::ios::end);
    const auto file_size = stream.tellg();
    if (file_size < 0) {
        error = "Cannot determine file size for " + path.string();
        return false;
    }
    bytes.resize(static_cast<std::size_t>(file_size));
    stream.seekg(0, std::ios::beg);
    if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()), file_size)) {
        error = "Cannot read " + path.string();
        bytes.clear();
        return false;
    }
    return true;
}

bool decode_dds(const std::span<const std::byte> bytes, const std::string& label, int& width,
                int& height, std::vector<std::uint8_t>& rgba, std::string& error) {
    if (bytes.size() < 128 || std::to_integer<char>(bytes[0]) != 'D' ||
        std::to_integer<char>(bytes[1]) != 'D' || std::to_integer<char>(bytes[2]) != 'S' ||
        std::to_integer<char>(bytes[3]) != ' ') {
        error = "Invalid DDS header in " + label;
        return false;
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
        error = "Unsupported DDS format/dimensions in " + label;
        return false;
    }
    if (static_cast<std::size_t>(width) >
        std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(height) / 4U) {
        error = "DDS dimensions overflow in " + label;
        return false;
    }
    if (uncompressed) {
        const auto bytes_per_pixel = static_cast<std::size_t>(rgb_bits / 8U);
        const auto row_bytes = static_cast<std::size_t>(width) * bytes_per_pixel;
        constexpr std::uint32_t ddsd_pitch = 0x08U;
        const auto declared_pitch = static_cast<std::size_t>(read_u32(bytes, 20));
        const auto pitch = (read_u32(bytes, 8) & ddsd_pitch) && declared_pitch >= row_bytes
                               ? declared_pitch
                               : row_bytes;
        if (pitch > (bytes.size() - 128U) / static_cast<std::size_t>(height)) {
            error = "Truncated DDS image in " + label;
            return false;
        }
        rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 0);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const auto source = 128U + static_cast<std::size_t>(y) * pitch +
                                    static_cast<std::size_t>(x) * bytes_per_pixel;
                const auto packed =
                    bytes_per_pixel == 2U ? read_u16(bytes, source) : read_u32(bytes, source);
                const auto output = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                     static_cast<std::size_t>(x)) *
                                    4U;
                rgba[output] = masked_channel(packed, red_mask, 0);
                rgba[output + 1] = masked_channel(packed, green_mask, 0);
                rgba[output + 2] = masked_channel(packed, blue_mask, 0);
                rgba[output + 3] =
                    masked_channel(packed, alpha_mask, (pixel_flags & ddpf_alpha_pixels) ? 0 : 255);
            }
        return true;
    }

    const std::size_t block_size = dxt1 ? 8U : 16U;
    const auto blocks_x = static_cast<std::size_t>((width + 3) / 4);
    const auto blocks_y = static_cast<std::size_t>((height + 3) / 4);
    if (blocks_x > std::numeric_limits<std::size_t>::max() / blocks_y ||
        blocks_x * blocks_y > (bytes.size() - 128) / block_size) {
        error = "Truncated DDS image in " + label;
        return false;
    }
    rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 0);
    std::size_t cursor = 128;
    for (std::size_t by = 0; by < blocks_y; ++by)
        for (std::size_t bx = 0; bx < blocks_x; ++bx) {
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
                        palette[i + 1] =
                            static_cast<std::uint8_t>(((7U - i) * alpha0 + i * alpha1) / 7U);
                } else {
                    for (unsigned i = 1; i <= 4; ++i)
                        palette[i + 1] =
                            static_cast<std::uint8_t>(((5U - i) * alpha0 + i * alpha1) / 5U);
                    palette[6] = 0;
                    palette[7] = 255;
                }
                std::uint64_t alpha_indices{};
                for (unsigned i = 0; i < 6; ++i)
                    alpha_indices |= static_cast<std::uint64_t>(
                                         std::to_integer<std::uint8_t>(bytes[cursor + 2 + i]))
                                     << (i * 8U);
                alpha = alpha_indices;
                cursor += 8;
                // Store the decoded palette below; DXT5's alpha indices are three bits.
                for (unsigned py = 0; py < 4; ++py)
                    for (unsigned px = 0; px < 4; ++px) {
                        const auto pixel = py * 4U + px;
                        const auto x = bx * 4U + px, y = by * 4U + py;
                        if (x >= static_cast<std::size_t>(width) ||
                            y >= static_cast<std::size_t>(height))
                            continue;
                        const auto output = (y * static_cast<std::size_t>(width) + x) * 4U;
                        rgba[output + 3] = palette[(alpha_indices >> (pixel * 3U)) & 7U];
                    }
            }
            const auto c0_raw = read_u16(bytes, cursor), c1_raw = read_u16(bytes, cursor + 2);
            std::array<std::array<std::uint8_t, 4>, 4> colors{};
            colors[0] = color_565(c0_raw);
            colors[1] = color_565(c1_raw);
            if (dxt3 || dxt5 || c0_raw > c1_raw) {
                for (unsigned channel = 0; channel < 3; ++channel) {
                    colors[2][channel] = static_cast<std::uint8_t>(
                        (2U * colors[0][channel] + colors[1][channel]) / 3U);
                    colors[3][channel] = static_cast<std::uint8_t>(
                        (colors[0][channel] + 2U * colors[1][channel]) / 3U);
                }
                colors[2][3] = colors[3][3] = 255;
            } else {
                for (unsigned channel = 0; channel < 3; ++channel)
                    colors[2][channel] =
                        static_cast<std::uint8_t>((colors[0][channel] + colors[1][channel]) / 2U);
                colors[2][3] = 255;
                colors[3] = {0, 0, 0, 0};
            }
            const auto indices = read_u32(bytes, cursor + 4);
            cursor += 8;
            for (unsigned py = 0; py < 4; ++py)
                for (unsigned px = 0; px < 4; ++px) {
                    const auto x = bx * 4U + px, y = by * 4U + py;
                    if (x >= static_cast<std::size_t>(width) ||
                        y >= static_cast<std::size_t>(height))
                        continue;
                    const auto pixel = py * 4U + px;
                    auto color = colors[(indices >> (pixel * 2U)) & 3U];
                    if (dxt3)
                        color[3] = static_cast<std::uint8_t>(((alpha >> (pixel * 4U)) & 15U) * 17U);
                    if (dxt5) {
                        const auto output = (y * static_cast<std::size_t>(width) + x) * 4U;
                        color[3] = rgba[output + 3];
                    }
                    const auto output = (y * static_cast<std::size_t>(width) + x) * 4U;
                    std::copy(color.begin(), color.end(),
                              rgba.begin() + static_cast<std::ptrdiff_t>(output));
                }
        }
    return true;
}

std::string lower_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return extension;
}

} // namespace

bool decode_png(const std::span<const std::byte> bytes, int& width, int& height,
                std::vector<std::uint8_t>& rgba, std::string& error) {
    if (bytes.empty() ||
        bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "Unsupported PNG size";
        return false;
    }
    int decoded_width = 0, decoded_height = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                                            static_cast<int>(bytes.size()), &decoded_width,
                                            &decoded_height, &channels, 4);
    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        error = std::string("Cannot decode PNG image: ") + (reason != nullptr ? reason
                                                                             : "unknown error");
        return false;
    }
    struct PixelCleanup {
        stbi_uc* pixels{};
        ~PixelCleanup() {
            if (pixels != nullptr) stbi_image_free(pixels);
        }
    } cleanup{pixels};
    if (decoded_width <= 0 || decoded_height <= 0 || decoded_width > 16384 ||
        decoded_height > 16384 ||
        static_cast<std::size_t>(decoded_width) >
            std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(decoded_height) /
                4U) {
        error = "Unsupported PNG format/dimensions";
        return false;
    }
    const auto stride = static_cast<std::size_t>(decoded_width) * 4U;
    rgba.resize(stride * static_cast<std::size_t>(decoded_height));
    std::memcpy(rgba.data(), pixels, rgba.size());
    width = decoded_width;
    height = decoded_height;
    return true;
}

bool decode_texture_image(const std::filesystem::path& path, int& width, int& height,
                          std::vector<std::uint8_t>& rgba, std::string& error) {
    const auto extension = lower_extension(path);
    if (extension != ".dds" && extension != ".png") {
        error = "Unsupported preview texture format: " + path.string();
        return false;
    }
    std::vector<std::byte> bytes;
    if (!read_file(path, bytes, error)) return false;
    if (extension == ".dds") return decode_dds(bytes, path.string(), width, height, rgba, error);
    if (decode_png(bytes, width, height, rgba, error)) return true;
    error += " in " + path.string();
    return false;
}

ImageDifference compare_rgba_images(const int width_a, const int height_a, const std::span<const std::uint8_t> a,
                                    const int width_b, const int height_b, const std::span<const std::uint8_t> b,
                                    const int tolerance) {
    ImageDifference result;
    const auto pixels = [](const int width, const int height) {
        return width > 0 && height > 0 ? static_cast<std::size_t>(width) * static_cast<std::size_t>(height) : 0U;
    };
    result.total_pixels = pixels(width_b, height_b);
    result.same_size = width_a == width_b && height_a == height_b && a.size() == pixels(width_a, height_a) * 4U &&
                       b.size() == result.total_pixels * 4U;
    result.diff_rgba.assign(result.total_pixels * 4U, 0);
    if (!result.same_size) {
        result.changed_pixels = result.total_pixels;
        result.max_channel_delta = 255;
        for (std::size_t i = 0; i < result.diff_rgba.size(); i += 4) {
            result.diff_rgba[i] = result.diff_rgba[i + 2] = 255;
            result.diff_rgba[i + 3] = 255;
        }
        return result;
    }
    for (std::size_t i = 0; i < b.size(); i += 4) {
        int delta = 0;
        for (std::size_t c = 0; c < 3; ++c) delta = std::max(delta, std::abs(int{a[i + c]} - int{b[i + c]}));
        result.max_channel_delta = std::max(result.max_channel_delta, delta);
        const bool changed = delta > tolerance;
        result.changed_pixels += changed;
        for (std::size_t c = 0; c < 3; ++c)
            result.diff_rgba[i + c] = changed ? (c == 1 ? 0 : 255) : static_cast<std::uint8_t>(b[i + c] / 4);
        result.diff_rgba[i + 3] = 255;
    }
    return result;
}

bool write_png_rgba(const std::filesystem::path& path, const int width, const int height,
                    const std::span<const std::uint8_t> rgba, std::string& error) {
    if (width <= 0 || height <= 0 ||
        rgba.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U) {
        error = "PNG pixel buffer does not match the image dimensions";
        return false;
    }
    std::error_code exists_error;
    if (std::filesystem::exists(path, exists_error)) {
        error = "Refusing to overwrite existing file " + path.string();
        return false;
    }
    std::vector<std::uint8_t> encoded;
    const auto append = [](void* context, void* data, const int size) {
        auto& output = *static_cast<std::vector<std::uint8_t>*>(context);
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        output.insert(output.end(), bytes, bytes + size);
    };
    if (stbi_write_png_to_func(append, &encoded, width, height, 4, rgba.data(), width * 4) == 0) {
        error = "PNG encoding failed";
        return false;
    }
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(encoded.data()),
                 static_cast<std::streamsize>(encoded.size()));
    if (!output) {
        error = "Could not write " + path.string();
        return false;
    }
    return true;
}

namespace {

std::uint16_t to_565(const float r, const float g, const float b) {
    const auto q = [](const float v, const int bits) {
        const int levels = (1 << bits) - 1;
        return static_cast<std::uint16_t>(std::clamp(static_cast<int>(std::lround(v / 255.0F * levels)), 0, levels));
    };
    return static_cast<std::uint16_t>(q(r, 5) << 11 | q(g, 6) << 5 | q(b, 5));
}

std::array<float, 3> from_565(const std::uint16_t c) {
    const auto r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return {static_cast<float>((r << 3) | (r >> 2)), static_cast<float>((g << 2) | (g >> 4)),
            static_cast<float>((b << 3) | (b >> 2))};
}

// One 4x4 block: endpoints at the extremes of the colours along their main
// axis, four-colour mode (endpoint 0 > endpoint 1), each texel to the nearest.
void encode_block(const std::array<std::array<float, 3>, 16>& texels, std::vector<std::byte>& out) {
    std::array<float, 3> mean{};
    for (const auto& t : texels)
        for (int c = 0; c < 3; ++c) mean[c] += t[c] / 16.0F;
    // Power iteration for the principal axis of the covariance.
    std::array<float, 6> cov{};  // xx xy xz yy yz zz
    for (const auto& t : texels) {
        const float x = t[0] - mean[0], y = t[1] - mean[1], z = t[2] - mean[2];
        cov[0] += x * x; cov[1] += x * y; cov[2] += x * z; cov[3] += y * y; cov[4] += y * z; cov[5] += z * z;
    }
    std::array<float, 3> axis{1.0F, 1.0F, 1.0F};
    for (int i = 0; i < 8; ++i) {
        const std::array<float, 3> next{cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2],
                                        cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
                                        cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2]};
        const float length = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
        if (length < 1e-6F) break;
        axis = {next[0] / length, next[1] / length, next[2] / length};
    }
    float low = std::numeric_limits<float>::max(), high = std::numeric_limits<float>::lowest();
    for (const auto& t : texels) {
        const float d = (t[0] - mean[0]) * axis[0] + (t[1] - mean[1]) * axis[1] + (t[2] - mean[2]) * axis[2];
        low = std::min(low, d);
        high = std::max(high, d);
    }
    auto c0 = to_565(mean[0] + axis[0] * high, mean[1] + axis[1] * high, mean[2] + axis[2] * high);
    auto c1 = to_565(mean[0] + axis[0] * low, mean[1] + axis[1] * low, mean[2] + axis[2] * low);
    if (c0 < c1) std::swap(c0, c1);
    std::uint32_t indices = 0;
    if (c0 != c1) {
        const auto a = from_565(c0), b = from_565(c1);
        std::array<std::array<float, 3>, 4> palette{a, b, {}, {}};
        for (int c = 0; c < 3; ++c) {
            palette[2][c] = (2 * a[c] + b[c]) / 3.0F;
            palette[3][c] = (a[c] + 2 * b[c]) / 3.0F;
        }
        for (std::size_t i = 0; i < 16; ++i) {
            std::uint32_t best = 0;
            float best_distance = std::numeric_limits<float>::max();
            for (std::uint32_t k = 0; k < 4; ++k) {
                float d = 0;
                for (int c = 0; c < 3; ++c) d += (texels[i][c] - palette[k][c]) * (texels[i][c] - palette[k][c]);
                if (d < best_distance) {
                    best_distance = d;
                    best = k;
                }
            }
            indices |= best << (2 * i);
        }
    }
    for (const auto value : {static_cast<std::uint32_t>(c0), static_cast<std::uint32_t>(c1)}) {
        out.push_back(static_cast<std::byte>(value & 0xFF));
        out.push_back(static_cast<std::byte>(value >> 8));
    }
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::byte>((indices >> shift) & 0xFF));
}

void put_u32(std::vector<std::byte>& out, const std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
}

} // namespace

std::vector<std::byte> encode_dds_dxt1(const int width, const int height, const std::span<const std::uint8_t> rgba) {
    const auto power_of_two = [](const int v) { return v > 0 && (v & (v - 1)) == 0; };
    if (!power_of_two(width) || !power_of_two(height) ||
        rgba.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4)
        throw std::runtime_error("DXT1 lightmaps need power-of-two RGBA images");
    std::uint32_t levels = 1;
    for (int w = width, h = height; w > 1 || h > 1; w = std::max(w / 2, 1), h = std::max(h / 2, 1)) ++levels;
    std::vector<std::byte> out;
    for (const char c : std::string_view("DDS ", 4)) out.push_back(static_cast<std::byte>(c));
    put_u32(out, 124);
    put_u32(out, 0xA1007);  // caps, height, width, pixel format, mip count, linear size
    put_u32(out, static_cast<std::uint32_t>(height));
    put_u32(out, static_cast<std::uint32_t>(width));
    put_u32(out, static_cast<std::uint32_t>(std::max(width / 4, 1) * std::max(height / 4, 1) * 8));
    put_u32(out, 0);
    put_u32(out, levels);
    for (int i = 0; i < 11; ++i) put_u32(out, 0);
    put_u32(out, 32);  // pixel format
    put_u32(out, 0x4);  // FOURCC
    for (const char c : std::string_view("DXT1", 4)) out.push_back(static_cast<std::byte>(c));
    for (int i = 0; i < 5; ++i) put_u32(out, 0);
    put_u32(out, 0x401008);  // complex, texture, mipmap
    for (int i = 0; i < 4; ++i) put_u32(out, 0);
    // Levels as floats, each a 2x2 box filter of the one above.
    std::vector<float> level(rgba.size() / 4 * 3);
    for (std::size_t i = 0; i < rgba.size() / 4; ++i)
        for (int c = 0; c < 3; ++c) level[i * 3 + c] = rgba[i * 4 + c];
    int w = width, h = height;
    for (std::uint32_t l = 0; l < levels; ++l) {
        for (int by = 0; by < h; by += 4)
            for (int bx = 0; bx < w; bx += 4) {
                std::array<std::array<float, 3>, 16> texels{};
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x) {
                        const auto sx = std::min(bx + x, w - 1), sy = std::min(by + y, h - 1);
                        const auto i = static_cast<std::size_t>(sy * w + sx) * 3;
                        texels[static_cast<std::size_t>(y * 4 + x)] = {level[i], level[i + 1], level[i + 2]};
                    }
                encode_block(texels, out);
            }
        if (l + 1 == levels) break;
        const int nw = std::max(w / 2, 1), nh = std::max(h / 2, 1);
        std::vector<float> next(static_cast<std::size_t>(nw * nh) * 3);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int c = 0; c < 3; ++c) {
                    float sum = 0;
                    int count = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            const int sx = std::min(x * 2 + dx, w - 1), sy = std::min(y * 2 + dy, h - 1);
                            sum += level[static_cast<std::size_t>(sy * w + sx) * 3 + c];
                            ++count;
                        }
                    next[static_cast<std::size_t>(y * nw + x) * 3 + c] = sum / static_cast<float>(count);
                }
        level = std::move(next);
        w = nw;
        h = nh;
    }
    return out;
}

} // namespace rws
