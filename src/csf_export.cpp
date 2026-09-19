#include "csf/export.hpp"

#include <bit>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace csf {
namespace {

void write_exclusive_file(const std::filesystem::path& path, const std::string_view contents) {
#ifdef _WIN32
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot exclusively create temporary export: " + path.string());
    std::size_t offset{};
    while (offset < contents.size()) {
        const auto amount = static_cast<DWORD>(std::min<std::size_t>(contents.size() - offset, 1U << 30U));
        DWORD written{};
        if (!WriteFile(handle, contents.data() + offset, amount, &written, nullptr) || written != amount) {
            CloseHandle(handle);
            throw std::runtime_error("Cannot write complete export: " + path.string());
        }
        offset += written;
    }
    if (!CloseHandle(handle)) throw std::runtime_error("Cannot close temporary export: " + path.string());
#else
    if (std::filesystem::exists(path)) throw std::runtime_error("Temporary export path already exists: " + path.string());
    std::ofstream stream(path, std::ios::binary | std::ios::out);
    if (!stream) throw std::runtime_error("Cannot create temporary export: " + path.string());
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!stream) throw std::runtime_error("Cannot write complete export: " + path.string());
#endif
}

std::string hex_bytes(const std::span<const std::byte> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto value : bytes) {
        const auto byte = std::to_integer<unsigned>(value);
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0FU]);
    }
    return result;
}

std::string escaped_raw(const RawString& value) {
    std::ostringstream output;
    const auto content_size = value.has_final_null && !value.bytes.empty()
                                  ? value.bytes.size() - 1
                                  : value.bytes.size();
    for (std::size_t index = 0; index < content_size; ++index) {
        const auto byte = std::to_integer<unsigned>(value.bytes[index]);
        switch (byte) {
        case '\\': output << "\\\\"; break;
        case '"': output << "\\\""; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (byte >= 0x20 && byte <= 0x7E) {
                output << static_cast<char>(byte);
            } else {
                output << "\\x" << std::hex << std::setw(2) << std::setfill('0') << byte
                       << std::dec << std::setfill(' ');
            }
        }
    }
    return output.str();
}

std::string json_escape(const std::string_view value) {
    std::ostringstream output;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
        case '\\': output << "\\\\"; break;
        case '"': output << "\\\""; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (byte < 0x20) {
                output << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<unsigned>(byte) << std::dec << std::setfill(' ');
            } else {
                output << character;
            }
        }
    }
    return output.str();
}

std::string float_text(const float value) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
    return output.str();
}

void text_nodes(std::ostringstream& output, const Document& document,
                const std::vector<Node>& nodes, const std::size_t depth) {
    for (const auto& node : nodes) {
        const auto& entry = document.entries()[node.entry_index];
        const auto kind = entry.kind();
        output << std::string(depth * 2, ' ');
        if (node.identifier_index) {
            if (const auto* identifier = document.identifier(*node.identifier_index)) {
                output << escaped_raw(*identifier) << ' ';
            } else {
                output << "<invalid-identifier:" << *node.identifier_index << "> ";
            }
        }
        if (const auto* integer = std::get_if<std::int32_t>(&node.scalar)) {
            output << *integer << '\n';
        } else if (const auto* real = std::get_if<float>(&node.scalar)) {
            output << float_text(*real) << '\n';
        } else if (const auto* string_index = std::get_if<std::uint32_t>(&node.scalar)) {
            if (const auto* string = document.string(*string_index)) {
                output << '"' << escaped_raw(*string) << "\"\n";
            } else {
                output << "<invalid-string:" << *string_index << ">\n";
            }
        } else if (kind && (*kind == ValueKind::group || *kind == ValueKind::array)) {
            const auto open = *kind == ValueKind::group ? '(' : '[';
            const auto close = *kind == ValueKind::group ? ')' : ']';
            output << open << " # entry=" << entry.entry_index << " offset=0x" << std::hex
                   << entry.source.offset << std::dec << " children=" << entry.raw_value_or_size << '\n';
            text_nodes(output, document, node.children, depth + 1);
            output << std::string(depth * 2, ' ') << close << '\n';
        } else {
            output << "<unknown-type:" << entry.raw_type << ">\n";
        }
    }
}

void json_nodes(std::ostringstream& output, const Document& document,
                const std::vector<Node>& nodes, const std::size_t depth) {
    const auto indent = [&](const std::size_t extra = 0) {
        output << std::string((depth + extra) * 2, ' ');
    };
    output << "[";
    if (!nodes.empty()) output << '\n';
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto& node = nodes[index];
        const auto& entry = document.entries()[node.entry_index];
        indent(1);
        output << "{\"entry_index\":" << node.entry_index;
        if (node.label_entry_index) output << ",\"label_entry_index\":" << *node.label_entry_index;
        if (node.identifier_index) output << ",\"identifier_index\":" << *node.identifier_index;
        if (const auto kind = entry.kind()) {
            output << ",\"kind\":\"" << value_kind_name(*kind) << '"';
        } else {
            output << ",\"kind\":\"unknown\"";
        }
        if (const auto* integer = std::get_if<std::int32_t>(&node.scalar)) {
            output << ",\"value\":" << *integer;
        } else if (const auto* real = std::get_if<float>(&node.scalar)) {
            output << ",\"value\":";
            if (std::isfinite(*real)) output << float_text(*real);
            else output << "null";
        } else if (const auto* string_index = std::get_if<std::uint32_t>(&node.scalar)) {
            output << ",\"string_index\":" << *string_index;
        }
        output << ",\"children\":";
        json_nodes(output, document, node.children, depth + 1);
        output << '}';
        if (index + 1 != nodes.size()) output << ',';
        output << '\n';
    }
    if (!nodes.empty()) indent();
    output << ']';
}

void json_strings(std::ostringstream& output, const std::vector<RawString>& values,
                  const std::string_view indent) {
    output << '[';
    if (!values.empty()) output << '\n';
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto& value = values[index];
        output << indent << "  {\"table_index\":" << value.table_index
               << ",\"offset\":" << value.source.offset
               << ",\"size\":" << value.source.size
               << ",\"declared_length\":" << value.declared_length
               << ",\"complete\":" << (value.complete ? "true" : "false")
               << ",\"has_final_null\":" << (value.has_final_null ? "true" : "false")
               << ",\"has_embedded_null\":" << (value.has_embedded_null ? "true" : "false")
               << ",\"display\":\"" << json_escape(value.display_utf8())
               << "\",\"raw_hex\":\"" << hex_bytes(value.bytes) << "\"}";
        if (index + 1 != values.size()) output << ',';
        output << '\n';
    }
    if (!values.empty()) output << indent;
    output << ']';
}

} // namespace

std::string export_text(const Document& document) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "# CSFFBS inspection text; not recompilable source\n"
           << "# state=" << parse_state_name(document.state())
           << " entries=" << document.entries().size()
           << " identifiers=" << document.identifiers().size()
           << " strings=" << document.strings().size() << '\n';
    text_nodes(output, document, document.roots(), 0);
    return output.str();
}

std::string export_json(const Document& document) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    const auto& header = document.header();
    output << "{\n  \"format\":\"CSFFBS\",\n  \"state\":\"" << parse_state_name(document.state())
           << "\",\n  \"header\":{\"reserved_hex\":\""
           << hex_bytes(header.reserved) << "\",\"version\":" << header.version
           << ",\"entry_count\":" << header.entry_count
           << ",\"identifier_count\":" << header.identifier_count
           << ",\"string_count\":" << header.string_count << "},\n  \"entries\":[";
    if (!document.entries().empty()) output << '\n';
    for (std::size_t index = 0; index < document.entries().size(); ++index) {
        const auto& entry = document.entries()[index];
        output << "    {\"entry_index\":" << entry.entry_index
               << ",\"offset\":" << entry.source.offset
               << ",\"size\":" << entry.source.size
               << ",\"raw_next_entry\":" << entry.raw_next_entry
               << ",\"raw_value_or_size\":" << entry.raw_value_or_size
               << ",\"raw_identifier_index\":" << entry.raw_identifier_index
               << ",\"raw_type\":" << entry.raw_type;
        if (const auto kind = entry.kind()) output << ",\"kind\":\"" << value_kind_name(*kind) << '"';
        output << '}';
        if (index + 1 != document.entries().size()) output << ',';
        output << '\n';
    }
    output << "  ],\n  \"identifiers\":";
    json_strings(output, document.identifiers(), "  ");
    output << ",\n  \"strings\":";
    json_strings(output, document.strings(), "  ");
    output << ",\n  \"roots\":";
    json_nodes(output, document, document.roots(), 1);
    output << ",\n  \"diagnostics\":[";
    if (!document.diagnostics().empty()) output << '\n';
    for (std::size_t index = 0; index < document.diagnostics().size(); ++index) {
        const auto& diagnostic = document.diagnostics()[index];
        output << "    {\"severity\":\""
               << (diagnostic.severity == Diagnostic::Severity::error ? "error" : "warning")
               << "\",\"offset\":" << diagnostic.offset;
        if (diagnostic.entry_index) output << ",\"entry_index\":" << *diagnostic.entry_index;
        output << ",\"message\":\"" << json_escape(diagnostic.message) << "\"}";
        if (index + 1 != document.diagnostics().size()) output << ',';
        output << '\n';
    }
    output << "  ],\n  \"trailing_bytes_hex\":\"" << hex_bytes(document.trailing_bytes()) << "\"\n}\n";
    return output.str();
}

void write_new_export(const Document& document, const std::filesystem::path& output,
                      const std::string& contents) {
    if (output.empty()) throw std::runtime_error("Export path is empty");
    std::error_code error;
    if (std::filesystem::exists(output, error)) {
        throw std::runtime_error("Refusing to overwrite existing export: " + output.string());
    }
    if (error) throw std::runtime_error("Cannot inspect export path: " + error.message());
    if (!document.source_path().empty()) {
        const auto source = std::filesystem::weakly_canonical(document.source_path(), error);
        if (error) throw std::runtime_error("Cannot resolve input path: " + error.message());
        const auto destination = std::filesystem::weakly_canonical(output, error);
        if (error) throw std::runtime_error("Cannot resolve export path: " + error.message());
        if (source == destination) throw std::runtime_error("Refusing to overwrite the input document");
    }
    auto temporary = output;
    temporary += ".csf-info.tmp";
    try {
        write_exclusive_file(temporary, contents);
        if (!std::filesystem::copy_file(temporary, output, std::filesystem::copy_options::none, error)) {
            if (error) throw std::runtime_error("Cannot publish export without overwrite: " + error.message());
            throw std::runtime_error("Refusing to overwrite existing export: " + output.string());
        }
        std::filesystem::remove(temporary, error);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

} // namespace csf
