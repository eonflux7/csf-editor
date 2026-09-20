#include "csf/document.hpp"

#include <algorithm>
#include <bit>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace csf {
namespace {

constexpr std::uint64_t header_size = 24;
constexpr std::uint64_t entry_size = 12;
constexpr std::size_t max_nesting_depth = 256;
constexpr std::uint32_t max_table_records = 16U * 1024U * 1024U;
constexpr std::array magic_bytes{
    std::byte{'C'}, std::byte{'S'}, std::byte{'F'}, std::byte{'F'}, std::byte{'B'}, std::byte{'S'},
};

bool range_fits(const std::uint64_t offset, const std::uint64_t size,
                const std::uint64_t total) noexcept {
    return offset <= total && size <= total - offset;
}

std::uint16_t read_u16(const std::span<const std::byte> data, const std::uint64_t offset) {
    const auto i = static_cast<std::size_t>(offset);
    return std::to_integer<std::uint16_t>(data[i]) |
           (std::to_integer<std::uint16_t>(data[i + 1]) << 8U);
}

std::uint32_t read_u32(const std::span<const std::byte> data, const std::uint64_t offset) {
    const auto i = static_cast<std::size_t>(offset);
    return std::to_integer<std::uint32_t>(data[i]) |
           (std::to_integer<std::uint32_t>(data[i + 1]) << 8U) |
           (std::to_integer<std::uint32_t>(data[i + 2]) << 16U) |
           (std::to_integer<std::uint32_t>(data[i + 3]) << 24U);
}

void append_utf8(std::string& output, const std::uint32_t codepoint) {
    if (codepoint < 0x80) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
        output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }
}

std::uint32_t windows_1252_codepoint(const std::uint8_t value) {
    constexpr std::array<std::uint16_t, 32> controls{
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
    };
    if (value >= 0x80 && value <= 0x9F) return controls[value - 0x80];
    return value;
}

std::vector<Node>* children_at(std::vector<Node>& roots, const std::vector<std::size_t>& path) {
    auto* children = &roots;
    for (const auto index : path)
        children = &(*children)[index].children;
    return children;
}

} // namespace

std::optional<ValueKind> Entry::kind() const noexcept {
    if (raw_type <= static_cast<std::uint16_t>(ValueKind::string)) {
        return static_cast<ValueKind>(raw_type);
    }
    return std::nullopt;
}

std::string RawString::display_utf8() const {
    std::string output;
    const auto content_size = has_final_null && !bytes.empty() ? bytes.size() - 1 : bytes.size();
    output.reserve(content_size);
    for (std::size_t i = 0; i < content_size; ++i) {
        append_utf8(output, windows_1252_codepoint(std::to_integer<std::uint8_t>(bytes[i])));
    }
    return output;
}

Document Document::load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Cannot open CSFFBS file: " + path.string());
    const auto end = input.tellg();
    if (end < 0) throw std::runtime_error("Cannot determine CSFFBS file size: " + path.string());
    const auto size = static_cast<std::uint64_t>(end);
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("CSFFBS file is too large for this process");
    }
    Document result;
    result.source_path_ = path;
    result.bytes_.resize(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(result.bytes_.data()), static_cast<std::streamsize>(size));
    if (!input && size != 0)
        throw std::runtime_error("Cannot read complete CSFFBS file: " + path.string());
    result.parse();
    return result;
}

Document Document::from_bytes(std::vector<std::byte> bytes) {
    Document result;
    result.bytes_ = std::move(bytes);
    result.parse();
    return result;
}

bool Document::sniff(const std::span<const std::byte> bytes) noexcept {
    return bytes.size() >= magic_bytes.size() &&
           std::equal(magic_bytes.begin(), magic_bytes.end(), bytes.begin());
}

bool Document::has_errors() const noexcept {
    return std::ranges::any_of(diagnostics_, [](const Diagnostic& diagnostic) {
        return diagnostic.severity == Diagnostic::Severity::error;
    });
}

const RawString* Document::identifier(const std::uint32_t index) const noexcept {
    return index < identifiers_.size() ? &identifiers_[index] : nullptr;
}

const RawString* Document::string(const std::uint32_t index) const noexcept {
    return index < strings_.size() ? &strings_[index] : nullptr;
}

std::span<const std::byte> Document::trailing_bytes() const noexcept {
    if (trailing_size_ == 0 || trailing_offset_ > bytes_.size() ||
        trailing_size_ > bytes_.size() - trailing_offset_) {
        return {};
    }
    return std::span<const std::byte>(bytes_).subspan(static_cast<std::size_t>(trailing_offset_),
                                                      static_cast<std::size_t>(trailing_size_));
}

void Document::error(const std::uint64_t offset, std::string message,
                     const std::optional<std::uint32_t> entry_index) {
    diagnostics_.push_back({Diagnostic::Severity::error, offset, entry_index, std::move(message)});
    structural_error_ = true;
    if (state_ != ParseState::non_csffbs) state_ = ParseState::partial;
}

void Document::warning(const std::uint64_t offset, std::string message,
                       const std::optional<std::uint32_t> entry_index) {
    diagnostics_.push_back(
        {Diagnostic::Severity::warning, offset, entry_index, std::move(message)});
}

void Document::parse() {
    trailing_offset_ = 0;
    trailing_size_ = 0;
    const auto data = std::span<const std::byte>(bytes_);
    const auto total = static_cast<std::uint64_t>(data.size());
    if (!sniff(data)) {
        state_ = ParseState::non_csffbs;
        diagnostics_.push_back({Diagnostic::Severity::error, 0, std::nullopt,
                                "File does not begin with CSFFBS magic"});
        return;
    }
    if (total < header_size) {
        error(total, "Truncated CSFFBS header: expected 24 bytes");
        return;
    }

    std::copy_n(data.begin(), 6, header_.magic.begin());
    std::copy_n(data.begin() + 6, 2, header_.reserved.begin());
    header_.version = read_u32(data, 8);
    header_.entry_count = read_u32(data, 12);
    header_.identifier_count = read_u32(data, 16);
    header_.string_count = read_u32(data, 20);
    header_.source = {0, header_size};

    if (header_.entry_count > max_table_records || header_.identifier_count > max_table_records ||
        header_.string_count > max_table_records) {
        error(12, "A table count exceeds the implementation safety limit");
        return;
    }
    if (header_.entry_count > (total - header_size) / entry_size) {
        const auto available = (total - header_size) / entry_size;
        error(12, "Entry table is truncated: declared " + std::to_string(header_.entry_count) +
                      ", at most " + std::to_string(available) + " complete records fit");
        const auto complete = static_cast<std::uint32_t>(available);
        entries_.reserve(complete);
        for (std::uint32_t index = 0; index < complete; ++index) {
            const auto offset = header_size + static_cast<std::uint64_t>(index) * entry_size;
            entries_.push_back({read_u32(data, offset),
                                read_u32(data, offset + 4),
                                static_cast<std::int16_t>(read_u16(data, offset + 8)),
                                read_u16(data, offset + 10),
                                index,
                                {offset, entry_size}});
        }
        build_tree();
        return;
    }

    entries_.reserve(header_.entry_count);
    std::uint64_t cursor = header_size;
    for (std::uint32_t index = 0; index < header_.entry_count; ++index, cursor += entry_size) {
        entries_.push_back({read_u32(data, cursor),
                            read_u32(data, cursor + 4),
                            static_cast<std::int16_t>(read_u16(data, cursor + 8)),
                            read_u16(data, cursor + 10),
                            index,
                            {cursor, entry_size}});
    }

    const auto minimum_string_headers =
        static_cast<std::uint64_t>(header_.identifier_count) + header_.string_count;
    if (minimum_string_headers > (total - cursor) / 4U) {
        error(cursor, "Identifier and string counts exceed the remaining table space");
        build_tree();
        return;
    }

    auto parse_strings = [&](const std::uint32_t count, std::vector<RawString>& output,
                             const char* table_name) -> bool {
        output.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            if (!range_fits(cursor, 4, total)) {
                error(cursor, std::string("Truncated ") + table_name + " length at index " +
                                  std::to_string(index));
                return false;
            }
            const auto record_offset = cursor;
            const auto length = read_u32(data, cursor);
            cursor += 4;
            RawString value;
            value.table_index = index;
            value.declared_length = length;
            value.source.offset = record_offset;
            if (!range_fits(cursor, length, total)) {
                const auto available = total - cursor;
                value.bytes.assign(data.begin() + static_cast<std::ptrdiff_t>(cursor), data.end());
                value.source.size = 4 + available;
                value.complete = false;
                output.push_back(std::move(value));
                error(record_offset, std::string("Truncated ") + table_name + " payload at index " +
                                         std::to_string(index) + ": declared " +
                                         std::to_string(length) + " bytes, only " +
                                         std::to_string(available) + " remain");
                cursor = total;
                return false;
            }
            value.bytes.assign(data.begin() + static_cast<std::ptrdiff_t>(cursor),
                               data.begin() + static_cast<std::ptrdiff_t>(cursor + length));
            value.source.size = 4 + length;
            value.complete = true;
            value.has_final_null = length > 0 && value.bytes.back() == std::byte{0};
            const auto content_end =
                value.has_final_null ? value.bytes.end() - 1 : value.bytes.end();
            value.has_embedded_null =
                std::find(value.bytes.begin(), content_end, std::byte{0}) != content_end;
            if (length > 0 && !value.has_final_null) {
                warning(record_offset, std::string(table_name) + " index " + std::to_string(index) +
                                           " has no final null byte");
            }
            if (value.has_embedded_null) {
                warning(record_offset, std::string(table_name) + " index " + std::to_string(index) +
                                           " contains an embedded null byte");
            }
            output.push_back(std::move(value));
            cursor += length;
        }
        return true;
    };

    if (!parse_strings(header_.identifier_count, identifiers_, "identifier")) {
        build_tree();
        return;
    }
    if (!parse_strings(header_.string_count, strings_, "string")) {
        build_tree();
        return;
    }
    if (cursor < total) {
        trailing_offset_ = cursor;
        trailing_size_ = total - cursor;
        warning(cursor, "Trailing bytes after the declared tables are preserved");
    }

    build_tree();
    if (!structural_error_)
        state_ = unsupported_entry_ ? ParseState::unsupported : ParseState::exact;
}

void Document::build_tree() {
    struct Frame {
        std::vector<std::size_t> path;
        std::uint64_t remaining{};
        std::uint32_t entry_index{};
    };
    std::vector<Frame> stack;
    std::optional<std::uint32_t> pending_label_entry;

    auto consume_parent = [&](const Entry& entry) {
        if (stack.empty()) return;
        if (stack.back().remaining == 0) {
            error(entry.source.offset, "Container element count underflow", entry.entry_index);
        } else {
            --stack.back().remaining;
        }
    };
    auto close_complete = [&] {
        while (!stack.empty() && stack.back().remaining == 0)
            stack.pop_back();
    };

    for (const auto& entry : entries_) {
        close_complete();
        const auto kind = entry.kind();
        if (!kind) {
            diagnostics_.push_back({Diagnostic::Severity::error, entry.source.offset + 10,
                                    entry.entry_index,
                                    "Unknown entry type " + std::to_string(entry.raw_type)});
            unsupported_entry_ = true;
            if (pending_label_entry) {
                const auto& label = entries_[*pending_label_entry];
                error(label.source.offset,
                      "Identifier before an unknown entry cannot be attached safely",
                      label.entry_index);
                pending_label_entry.reset();
            }
            Node node;
            node.entry_index = entry.entry_index;
            auto* children = stack.empty() ? &roots_ : children_at(roots_, stack.back().path);
            children->push_back(std::move(node));
            consume_parent(entry);
            continue;
        }

        if (*kind == ValueKind::identifier) {
            if (entry.raw_identifier_index < 0 ||
                static_cast<std::size_t>(entry.raw_identifier_index) >= identifiers_.size()) {
                error(entry.source.offset + 8,
                      "Identifier entry references an invalid identifier index", entry.entry_index);
            }
            if (pending_label_entry) {
                const auto& previous = entries_[*pending_label_entry];
                error(previous.source.offset, "Identifier is not followed by a scalar value",
                      previous.entry_index);
            }
            pending_label_entry = entry.entry_index;
            continue;
        }

        Node node;
        node.entry_index = entry.entry_index;
        if (*kind == ValueKind::group || *kind == ValueKind::array) {
            if (pending_label_entry) {
                const auto& previous = entries_[*pending_label_entry];
                error(previous.source.offset,
                      "Identifier before a container cannot be attached safely",
                      previous.entry_index);
                pending_label_entry.reset();
            }
            if (entry.raw_identifier_index >= 0) {
                node.identifier_index = static_cast<std::uint32_t>(entry.raw_identifier_index);
                if (static_cast<std::size_t>(entry.raw_identifier_index) >= identifiers_.size()) {
                    error(entry.source.offset + 8,
                          "Container references an invalid identifier index", entry.entry_index);
                }
            } else if (entry.raw_identifier_index != -1) {
                error(entry.source.offset + 8, "Container identifier index is negative but not -1",
                      entry.entry_index);
            }
        } else {
            if (pending_label_entry) {
                const auto& label = entries_[*pending_label_entry];
                node.label_entry_index = pending_label_entry;
                if (label.raw_identifier_index >= 0) {
                    node.identifier_index = static_cast<std::uint32_t>(label.raw_identifier_index);
                }
                pending_label_entry.reset();
            }
            if (*kind == ValueKind::integer) {
                node.scalar = static_cast<std::int32_t>(entry.raw_value_or_size);
            } else if (*kind == ValueKind::real) {
                node.scalar = std::bit_cast<float>(entry.raw_value_or_size);
            } else if (*kind == ValueKind::string) {
                node.scalar = entry.raw_value_or_size;
                if (entry.raw_value_or_size >= strings_.size()) {
                    error(entry.source.offset + 4,
                          "String entry references an invalid string index", entry.entry_index);
                }
            }
        }

        auto* children = stack.empty() ? &roots_ : children_at(roots_, stack.back().path);
        const auto child_index = children->size();
        children->push_back(std::move(node));
        consume_parent(entry);

        if (*kind == ValueKind::group || *kind == ValueKind::array) {
            if (entry.raw_value_or_size > entries_.size() - entry.entry_index - 1ULL) {
                error(entry.source.offset + 4,
                      "Container declares more values than remaining entries can hold",
                      entry.entry_index);
            }
            if (entry.raw_value_or_size != 0) {
                if (stack.size() >= max_nesting_depth) {
                    error(entry.source.offset, "Maximum container nesting depth exceeded",
                          entry.entry_index);
                    continue;
                }
                auto path = stack.empty() ? std::vector<std::size_t>{} : stack.back().path;
                path.push_back(child_index);
                stack.push_back({std::move(path), entry.raw_value_or_size, entry.entry_index});
            }
        }
    }

    if (pending_label_entry) {
        const auto& entry = entries_[*pending_label_entry];
        error(entry.source.offset, "Identifier at end of entry table has no scalar value",
              entry.entry_index);
    }
    for (const auto& frame : stack) {
        if (frame.remaining != 0) {
            const auto& entry = entries_[frame.entry_index];
            error(entry.source.offset,
                  "Container is unclosed with " + std::to_string(frame.remaining) +
                      " value(s) missing",
                  frame.entry_index);
        }
    }
}

const char* value_kind_name(const ValueKind kind) noexcept {
    switch (kind) {
    case ValueKind::identifier:
        return "identifier";
    case ValueKind::group:
        return "group";
    case ValueKind::array:
        return "array";
    case ValueKind::integer:
        return "integer";
    case ValueKind::real:
        return "real";
    case ValueKind::string:
        return "string";
    }
    return "unknown";
}

const char* parse_state_name(const ParseState state) noexcept {
    switch (state) {
    case ParseState::exact:
        return "exact";
    case ParseState::partial:
        return "partial";
    case ParseState::unsupported:
        return "unsupported";
    case ParseState::non_csffbs:
        return "non-CSFFBS";
    }
    return "partial";
}

} // namespace csf
