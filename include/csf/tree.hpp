#pragma once

#include "csf/document.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace csf {

// A mutable CSFFBS value tree. Labels and strings keep their exact stored bytes
// (normally Windows-1252 with a final NUL); integers and reals keep their raw
// 32-bit patterns so that NaN payloads and negative zero survive unchanged.
struct TreeNode {
    ValueKind kind{ValueKind::group}; // group, array, integer, real or string
    std::optional<std::string> label; // raw identifier bytes
    std::uint32_t raw{};              // integer or real bits
    std::string text;                 // raw string bytes
    std::vector<TreeNode> children;

    [[nodiscard]] bool is_container() const noexcept {
        return kind == ValueKind::group || kind == ValueKind::array;
    }
    // Label without its final NUL, in the stored Windows-1252 encoding.
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] bool is(std::string_view label_name) const noexcept { return name() == label_name; }
    [[nodiscard]] TreeNode* child(std::string_view label_name) noexcept;
    [[nodiscard]] const TreeNode* child(std::string_view label_name) const noexcept;
    // Index of the first child with this label, if any.
    [[nodiscard]] std::optional<std::size_t> child_index(std::string_view label_name) const noexcept;

    [[nodiscard]] std::optional<std::int32_t> as_int() const noexcept;
    [[nodiscard]] std::optional<float> as_real() const noexcept;
    // String content without its final NUL, in the stored encoding.
    [[nodiscard]] std::optional<std::string_view> as_string() const noexcept;
    void set_int(std::int32_t value) noexcept;
    void set_real(float value) noexcept;
    // Stores `windows_1252` followed by a final NUL; the empty string is stored
    // as zero bytes, as in every shipped file.
    void set_string(std::string_view windows_1252);

    [[nodiscard]] static TreeNode integer(std::optional<std::string_view> label, std::int32_t value);
    [[nodiscard]] static TreeNode real(std::optional<std::string_view> label, float value);
    [[nodiscard]] static TreeNode string(std::optional<std::string_view> label,
                                         std::string_view windows_1252);
    [[nodiscard]] static TreeNode group(std::optional<std::string_view> label);
    [[nodiscard]] static TreeNode array(std::optional<std::string_view> label);

    friend bool operator==(const TreeNode&, const TreeNode&) = default;
};

// Canonical CSFFBS writer. Every shipped file follows the same layout, which
// this class reproduces byte-for-byte for unedited trees:
//  - entries are written depth first; a labelled scalar is an identifier entry
//    (value 0xFFFFFFFF) followed by an unlabelled value entry with next = 0;
//  - containers carry their label index inline and their element count;
//  - each element's first entry stores the entry index of its next sibling,
//    or 0 for the last child;
//  - identifier and string tables hold unique values in first-use order.
class Tree {
public:
    std::array<std::byte, 8> prefix{}; // magic and reserved bytes
    std::uint32_t version{};
    std::vector<TreeNode> roots;
    std::vector<std::byte> trailing; // bytes after the tables, preserved verbatim

    // Throws when the document is not a structurally exact CSFFBS file.
    [[nodiscard]] static Tree from_document(const Document& document);
    [[nodiscard]] static Tree from_bytes(std::vector<std::byte> bytes);
    [[nodiscard]] std::vector<std::byte> serialize() const;
    [[nodiscard]] Document to_document() const;

    friend bool operator==(const Tree&, const Tree&) = default;
};

// Windows-1252 conversion for user-entered text. Returns nullopt when a
// character has no Windows-1252 representation.
[[nodiscard]] std::optional<std::string> utf8_to_windows_1252(std::string_view utf8);
[[nodiscard]] std::string windows_1252_to_utf8(std::string_view windows_1252);

} // namespace csf
