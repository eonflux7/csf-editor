#pragma once

#include "rwsman/selection.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rwsman {

enum class SymbolKind : std::uint8_t {
    actor,
    navigation_group,
    navigation_point,
    dummy,
    area,
    light,
    effect,
    scene_object,
    script,
    variable,
    class_record, // A BDD object definition (Objetos.bdd class).
    animation,
    chunk,
    instance,
    resource,
    placement, // An authoring project's placement: a building, donor piece or props.
    count
};

[[nodiscard]] const char* symbol_kind_name(SymbolKind kind) noexcept;

struct SearchEntry {
    SymbolKind kind{SymbolKind::chunk};
    std::string label;    // Primary text: name, class, script title.
    std::string detail;   // Secondary text shown dimmed: "class 55, ID 1".
    std::string haystack; // Optional extra searchable text (script operands).
    SelectionRef target;
    std::optional<std::uint64_t> offset;     // Source offset, printed as hex.
    std::optional<std::uint64_t> end_offset; // Exclusive end, for containment.
    std::optional<std::uint64_t> id;         // Entry index or object ID for "#17".
    std::string group;                       // Explorer grouping (script folder).
    // Index of the entry this one nests under (a navigation point's group).
    std::optional<std::size_t> parent;
};

struct SearchResult {
    const SearchEntry* entry{};
    int score{};
    std::vector<std::uint32_t> label_positions; // Matched bytes in entry->label.
};

// Immutable-after-build symbol index for the command palette, Go to, and the
// explorer filters. Build it once per document load; queries never rebuild text.
class SearchIndex {
public:
    void clear() noexcept {
        ++generation_;
        entries_.clear();
        lowered_.clear();
        for (auto& list : by_kind_) list.clear();
    }
    void add(SearchEntry entry);
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    // Changes on every clear() and add(). Holders of entry pointers compare it, since a
    // rebuild can keep the same size while freeing the old entries.
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] const std::vector<SearchEntry>& entries() const noexcept { return entries_; }
    // Indexes of every entry of `kind`, in insertion order.
    [[nodiscard]] const std::vector<std::size_t>& indices_of(SymbolKind kind) const noexcept {
        return by_kind_[static_cast<std::size_t>(kind)];
    }

    // Ranked results, best first. Query forms:
    //   0x2C79D6  entries at that offset, then the smallest entry containing it
    //   #17       entries whose id or entry index is 17
    //   17        exact id matches first, then name matches
    //   text      fuzzy match on label (weighted) and detail
    //   kind:text only entries of that kind: actor:, zone:, route:, marker:, light:,
    //             script:, variable:, class:, animation:, chunk:, resource:
    // `gameplay_first` (Mission mode) ranks actors, zones, routes, scripts and
    // classes well above file internals (chunks, instances, resources).
    [[nodiscard]] std::vector<SearchResult> query(std::string_view text, std::size_t limit = 50,
                                                  bool gameplay_first = false) const;

    // Case-insensitive substring filter on label, detail, and haystack. `needle`
    // must already be lower-case. Used by the Explorer trees.
    [[nodiscard]] bool contains(std::size_t index, std::string_view needle) const;

private:
    std::vector<SearchEntry> entries_;
    // Lower-cased label + detail + haystack, built once in add().
    std::vector<std::string> lowered_;
    std::array<std::vector<std::size_t>, static_cast<std::size_t>(SymbolKind::count)> by_kind_;
    std::uint64_t generation_{};
};

// Parses "0x2C79D6" / "0X2c79d6". Returns nullopt unless the whole string is a
// hexadecimal literal.
[[nodiscard]] std::optional<std::uint64_t> parse_hex_offset(std::string_view text);

} // namespace rwsman
