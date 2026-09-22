#include "csf/tree.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace csf {
namespace {

constexpr std::array<std::uint16_t, 32> windows_1252_high{
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

std::string terminated(const std::string_view value) {
    std::string result(value);
    result.push_back('\0');
    return result;
}

std::string_view strip_final_nul(const std::string_view value) noexcept {
    return !value.empty() && value.back() == '\0' ? value.substr(0, value.size() - 1) : value;
}

std::string bytes_string(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

TreeNode convert(const Document& document, const Node& node) {
    const auto& entry = document.entries().at(node.entry_index);
    const auto kind = entry.kind();
    if (!kind || *kind == ValueKind::identifier)
        throw std::runtime_error("CSFFBS tree contains an unsupported entry");
    TreeNode result;
    result.kind = *kind;
    if (node.identifier_index) {
        const auto* label = document.identifier(*node.identifier_index);
        if (!label || !label->complete) throw std::runtime_error("CSFFBS label is unavailable");
        result.label = bytes_string(label->bytes);
    }
    if (result.is_container()) {
        if (entry.raw_value_or_size != node.children.size())
            throw std::runtime_error("CSFFBS container count does not match its children");
        result.children.reserve(node.children.size());
        for (const auto& child : node.children) result.children.push_back(convert(document, child));
    } else if (*kind == ValueKind::string) {
        const auto* value = document.string(entry.raw_value_or_size);
        if (!value || !value->complete) throw std::runtime_error("CSFFBS string is unavailable");
        result.text = bytes_string(value->bytes);
    } else {
        result.raw = entry.raw_value_or_size;
    }
    return result;
}

void put_u16(std::vector<std::byte>& out, const std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFFU));
    out.push_back(static_cast<std::byte>(value >> 8U));
}

void put_u32(std::vector<std::byte>& out, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
}

class Writer {
public:
    struct RawEntry {
        std::uint32_t next{};
        std::uint32_t value{};
        std::int16_t identifier{-1};
        std::uint16_t type{};
    };

    std::vector<RawEntry> entries;
    std::vector<const std::string*> identifiers, strings;

    // Returns the index of the node's first entry, which carries the sibling link.
    std::size_t emit(const TreeNode& node) {
        if (node.is_container()) {
            const auto self = entries.size();
            entries.push_back({0, checked_count(node.children.size()),
                               node.label ? intern_identifier(*node.label) : std::int16_t{-1},
                               static_cast<std::uint16_t>(node.kind)});
            emit_siblings(node.children);
            return self;
        }
        if (node.kind != ValueKind::integer && node.kind != ValueKind::real &&
            node.kind != ValueKind::string)
            throw std::runtime_error("CSFFBS tree node has an invalid kind");
        std::optional<std::size_t> label_entry;
        if (node.label) {
            label_entry = entries.size();
            entries.push_back({0, 0xFFFFFFFFU, intern_identifier(*node.label),
                               static_cast<std::uint16_t>(ValueKind::identifier)});
        }
        const auto value = node.kind == ValueKind::string ? intern(strings, string_ids_, node.text)
                                                          : node.raw;
        entries.push_back({0, value, -1, static_cast<std::uint16_t>(node.kind)});
        return label_entry.value_or(entries.size() - 1);
    }

    void emit_siblings(const std::vector<TreeNode>& nodes) {
        std::optional<std::size_t> previous;
        for (const auto& node : nodes) {
            const auto start = entries.size();
            if (previous) entries[*previous].next = checked_count(start);
            previous = emit(node);
        }
    }

private:
    static std::uint32_t checked_count(const std::size_t value) {
        if (value > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("CSFFBS tree is too large");
        return static_cast<std::uint32_t>(value);
    }

    std::int16_t intern_identifier(const std::string& label) {
        const auto index = intern(identifiers, identifier_ids_, label);
        if (index > static_cast<std::uint32_t>(std::numeric_limits<std::int16_t>::max()))
            throw std::runtime_error("CSFFBS identifier table exceeds 32767 labels");
        return static_cast<std::int16_t>(index);
    }

    static std::uint32_t intern(std::vector<const std::string*>& table,
                                std::unordered_map<std::string_view, std::uint32_t>& ids,
                                const std::string& value) {
        const auto [found, inserted] = ids.try_emplace(value, checked_count(table.size()));
        if (inserted) table.push_back(&value);
        return found->second;
    }

    std::unordered_map<std::string_view, std::uint32_t> identifier_ids_, string_ids_;
};

} // namespace

std::string_view TreeNode::name() const noexcept {
    return label ? strip_final_nul(*label) : std::string_view{};
}

TreeNode* TreeNode::child(const std::string_view label_name) noexcept {
    const auto found = std::ranges::find_if(
        children, [&](const TreeNode& value) { return value.label && value.is(label_name); });
    return found == children.end() ? nullptr : &*found;
}

const TreeNode* TreeNode::child(const std::string_view label_name) const noexcept {
    return const_cast<TreeNode*>(this)->child(label_name);
}

std::optional<std::size_t> TreeNode::child_index(const std::string_view label_name) const noexcept {
    for (std::size_t i = 0; i < children.size(); ++i)
        if (children[i].label && children[i].is(label_name)) return i;
    return std::nullopt;
}

std::optional<std::int32_t> TreeNode::as_int() const noexcept {
    if (kind != ValueKind::integer) return std::nullopt;
    return static_cast<std::int32_t>(raw);
}

std::optional<float> TreeNode::as_real() const noexcept {
    if (kind != ValueKind::real) return std::nullopt;
    return std::bit_cast<float>(raw);
}

std::optional<std::string_view> TreeNode::as_string() const noexcept {
    if (kind != ValueKind::string) return std::nullopt;
    return strip_final_nul(text);
}

void TreeNode::set_int(const std::int32_t value) noexcept {
    kind = ValueKind::integer;
    raw = static_cast<std::uint32_t>(value);
}

void TreeNode::set_real(const float value) noexcept {
    kind = ValueKind::real;
    raw = std::bit_cast<std::uint32_t>(value);
}

void TreeNode::set_string(const std::string_view windows_1252) {
    kind = ValueKind::string;
    text = windows_1252.empty() ? std::string{} : terminated(windows_1252);
}

TreeNode TreeNode::integer(const std::optional<std::string_view> label, const std::int32_t value) {
    TreeNode node;
    if (label) node.label = terminated(*label);
    node.set_int(value);
    return node;
}

TreeNode TreeNode::real(const std::optional<std::string_view> label, const float value) {
    TreeNode node;
    if (label) node.label = terminated(*label);
    node.set_real(value);
    return node;
}

TreeNode TreeNode::string(const std::optional<std::string_view> label,
                          const std::string_view windows_1252) {
    TreeNode node;
    if (label) node.label = terminated(*label);
    node.set_string(windows_1252);
    return node;
}

TreeNode TreeNode::group(const std::optional<std::string_view> label) {
    TreeNode node;
    node.kind = ValueKind::group;
    if (label) node.label = terminated(*label);
    return node;
}

TreeNode TreeNode::array(const std::optional<std::string_view> label) {
    TreeNode node = group(label);
    node.kind = ValueKind::array;
    return node;
}

Tree Tree::from_document(const Document& document) {
    if (document.state() != ParseState::exact || document.has_errors())
        throw std::runtime_error("Only structurally exact CSFFBS documents can be edited");
    Tree tree;
    std::copy_n(document.bytes().begin(), tree.prefix.size(), tree.prefix.begin());
    tree.version = document.header().version;
    tree.roots.reserve(document.roots().size());
    for (const auto& root : document.roots()) tree.roots.push_back(convert(document, root));
    const auto trailing = document.trailing_bytes();
    tree.trailing.assign(trailing.begin(), trailing.end());
    return tree;
}

Tree Tree::from_bytes(std::vector<std::byte> bytes) {
    return from_document(Document::from_bytes(std::move(bytes)));
}

std::vector<std::byte> Tree::serialize() const {
    Writer writer;
    writer.emit_siblings(roots);
    std::vector<std::byte> out;
    out.reserve(24 + writer.entries.size() * 12);
    out.insert(out.end(), prefix.begin(), prefix.end());
    put_u32(out, version);
    put_u32(out, static_cast<std::uint32_t>(writer.entries.size()));
    put_u32(out, static_cast<std::uint32_t>(writer.identifiers.size()));
    put_u32(out, static_cast<std::uint32_t>(writer.strings.size()));
    for (const auto& entry : writer.entries) {
        put_u32(out, entry.next);
        put_u32(out, entry.value);
        put_u16(out, static_cast<std::uint16_t>(entry.identifier));
        put_u16(out, entry.type);
    }
    for (const auto* table : {&writer.identifiers, &writer.strings})
        for (const auto* value : *table) {
            put_u32(out, static_cast<std::uint32_t>(value->size()));
            const auto* data = reinterpret_cast<const std::byte*>(value->data());
            out.insert(out.end(), data, data + value->size());
        }
    out.insert(out.end(), trailing.begin(), trailing.end());
    return out;
}

Document Tree::to_document() const { return Document::from_bytes(serialize()); }

std::optional<std::string> utf8_to_windows_1252(const std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size();) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        std::uint32_t codepoint{};
        std::size_t length{};
        if (lead < 0x80) {
            codepoint = lead;
            length = 1;
        } else if ((lead & 0xE0U) == 0xC0U) {
            codepoint = lead & 0x1FU;
            length = 2;
        } else if ((lead & 0xF0U) == 0xE0U) {
            codepoint = lead & 0x0FU;
            length = 3;
        } else {
            return std::nullopt;
        }
        if (i + length > utf8.size()) return std::nullopt;
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(utf8[i + k]);
            if ((next & 0xC0U) != 0x80U) return std::nullopt;
            codepoint = (codepoint << 6U) | (next & 0x3FU);
        }
        i += length;
        if (codepoint < 0x80 || (codepoint >= 0xA0 && codepoint <= 0xFF)) {
            out.push_back(static_cast<char>(codepoint));
            continue;
        }
        const auto found = std::ranges::find(windows_1252_high, codepoint);
        if (found == windows_1252_high.end()) return std::nullopt;
        out.push_back(static_cast<char>(0x80 + (found - windows_1252_high.begin())));
    }
    return out;
}

std::string windows_1252_to_utf8(const std::string_view windows_1252) {
    std::string out;
    out.reserve(windows_1252.size());
    for (const auto byte : windows_1252) {
        const auto value = static_cast<unsigned char>(byte);
        const std::uint32_t codepoint =
            value >= 0x80 && value <= 0x9F ? windows_1252_high[value - 0x80] : value;
        if (codepoint < 0x80) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint < 0x800) {
            out.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
    }
    return out;
}

} // namespace csf
