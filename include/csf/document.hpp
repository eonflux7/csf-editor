#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace csf {

enum class ValueKind : std::uint16_t {
    identifier = 0,
    group = 1,
    array = 2,
    integer = 3,
    real = 4,
    string = 5,
};

enum class ParseState { exact, partial, unsupported, non_csffbs };

struct SourceRange {
    std::uint64_t offset{};
    std::uint64_t size{};
};

struct Header {
    std::array<std::byte, 6> magic{};
    std::array<std::byte, 2> reserved{};
    std::uint32_t version{};
    std::uint32_t entry_count{};
    std::uint32_t identifier_count{};
    std::uint32_t string_count{};
    SourceRange source{};
};

struct Diagnostic {
    enum class Severity { warning, error };
    Severity severity{Severity::warning};
    std::uint64_t offset{};
    std::optional<std::uint32_t> entry_index;
    std::string message;
};

struct Entry {
    std::uint32_t raw_next_entry{};
    std::uint32_t raw_value_or_size{};
    std::int16_t raw_identifier_index{};
    std::uint16_t raw_type{};
    std::uint32_t entry_index{};
    SourceRange source{};

    [[nodiscard]] std::optional<ValueKind> kind() const noexcept;
};

struct RawString {
    std::uint32_t table_index{};
    std::uint32_t declared_length{};
    SourceRange source{};
    std::vector<std::byte> bytes;
    bool complete{};
    bool has_final_null{};
    bool has_embedded_null{};

    // The game corpus uses Windows-1252. This conversion is for display only;
    // bytes remains the authoritative representation.
    [[nodiscard]] std::string display_utf8() const;
};

struct Node {
    std::uint32_t entry_index{};
    std::optional<std::uint32_t> label_entry_index;
    std::optional<std::uint32_t> identifier_index;
    std::variant<std::monostate, std::int32_t, float, std::uint32_t> scalar;
    std::vector<Node> children;
};

class Document {
public:
    [[nodiscard]] static Document load(const std::filesystem::path& path);
    [[nodiscard]] static Document from_bytes(std::vector<std::byte> bytes);
    // Parses in-memory bytes that represent `source_path` (for example an edited copy).
    [[nodiscard]] static Document from_bytes(std::vector<std::byte> bytes,
                                             std::filesystem::path source_path);
    [[nodiscard]] static bool sniff(std::span<const std::byte> bytes) noexcept;

    [[nodiscard]] const std::filesystem::path& source_path() const noexcept { return source_path_; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
    [[nodiscard]] const Header& header() const noexcept { return header_; }
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const std::vector<RawString>& identifiers() const noexcept {
        return identifiers_;
    }
    [[nodiscard]] const std::vector<RawString>& strings() const noexcept { return strings_; }
    [[nodiscard]] const std::vector<Node>& roots() const noexcept { return roots_; }
    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }
    [[nodiscard]] std::span<const std::byte> trailing_bytes() const noexcept;
    [[nodiscard]] ParseState state() const noexcept { return state_; }
    [[nodiscard]] bool has_errors() const noexcept;

    [[nodiscard]] const RawString* identifier(std::uint32_t index) const noexcept;
    [[nodiscard]] const RawString* string(std::uint32_t index) const noexcept;

private:
    void parse();
    void build_tree();
    void error(std::uint64_t offset, std::string message,
               std::optional<std::uint32_t> entry_index = std::nullopt);
    void warning(std::uint64_t offset, std::string message,
                 std::optional<std::uint32_t> entry_index = std::nullopt);

    std::filesystem::path source_path_;
    std::vector<std::byte> bytes_;
    Header header_;
    std::vector<Entry> entries_;
    std::vector<RawString> identifiers_;
    std::vector<RawString> strings_;
    std::vector<Node> roots_;
    std::vector<Diagnostic> diagnostics_;
    std::uint64_t trailing_offset_{};
    std::uint64_t trailing_size_{};
    bool structural_error_{};
    bool unsupported_entry_{};
    ParseState state_{ParseState::partial};
};

[[nodiscard]] const char* value_kind_name(ValueKind kind) noexcept;
[[nodiscard]] const char* parse_state_name(ParseState state) noexcept;

} // namespace csf
