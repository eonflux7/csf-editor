#include "rwsman/search_index.hpp"

#include "rwsman/fuzzy.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <functional>

namespace rwsman {
namespace {

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

std::optional<std::uint64_t> parse_decimal(const std::string_view text) {
    if (text.empty()) return std::nullopt;
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

// Kinds that are usually what the user wants rank slightly higher on ties.
int kind_weight(const SymbolKind kind) {
    switch (kind) {
    case SymbolKind::actor:
    case SymbolKind::script:
    case SymbolKind::placement:
        return 3;
    case SymbolKind::class_record:
    case SymbolKind::navigation_group:
    case SymbolKind::dummy:
    case SymbolKind::area:
    case SymbolKind::light:
    case SymbolKind::effect:
        return 2;
    case SymbolKind::chunk:
    case SymbolKind::instance:
    case SymbolKind::resource:
        return 0;
    default:
        return 1;
    }
}

// Extra weight in Mission mode: what an author looks for comes first.
int gameplay_weight(const SymbolKind kind) {
    switch (kind) {
    case SymbolKind::actor:
    case SymbolKind::area:
    case SymbolKind::navigation_group:
    case SymbolKind::script:
    case SymbolKind::dummy:
    case SymbolKind::class_record:
    case SymbolKind::placement:
        return 60;
    case SymbolKind::navigation_point:
    case SymbolKind::light:
    case SymbolKind::effect:
    case SymbolKind::variable:
    case SymbolKind::animation:
        return 20;
    case SymbolKind::chunk:
    case SymbolKind::instance:
    case SymbolKind::resource:
    case SymbolKind::scene_object:
        return -40;
    case SymbolKind::count:
        break;
    }
    return 0;
}

// "actor:" and the other kind prefixes of a query: the kinds they allow.
std::optional<std::vector<SymbolKind>> prefix_kinds(const std::string_view prefix) {
    using K = SymbolKind;
    static const std::pair<std::string_view, std::vector<SymbolKind>> table[]{
        {"actor", {K::actor}},
        {"zone", {K::area}},
        {"area", {K::area}},
        {"route", {K::navigation_group, K::navigation_point}},
        {"nav", {K::navigation_group, K::navigation_point}},
        {"marker", {K::dummy}},
        {"dummy", {K::dummy}},
        {"light", {K::light}},
        {"effect", {K::effect}},
        {"script", {K::script}},
        {"placement", {K::placement}},
        {"building", {K::placement}},
        {"variable", {K::variable}},
        {"class", {K::class_record}},
        {"animation", {K::animation}},
        {"chunk", {K::chunk}},
        {"instance", {K::instance}},
        {"resource", {K::resource}},
    };
    for (const auto& [name, kinds] : table)
        if (name == prefix) return kinds;
    return std::nullopt;
}

} // namespace

const char* symbol_kind_name(const SymbolKind kind) noexcept {
    switch (kind) {
    case SymbolKind::actor:
        return "actor";
    case SymbolKind::navigation_group:
        return "nav group";
    case SymbolKind::navigation_point:
        return "nav point";
    case SymbolKind::dummy:
        return "dummy";
    case SymbolKind::area:
        return "area";
    case SymbolKind::light:
        return "light";
    case SymbolKind::effect:
        return "effect";
    case SymbolKind::scene_object:
        return "scene object";
    case SymbolKind::placement:
        return "placement";
    case SymbolKind::script:
        return "script";
    case SymbolKind::variable:
        return "variable";
    case SymbolKind::class_record:
        return "class";
    case SymbolKind::animation:
        return "animation";
    case SymbolKind::chunk:
        return "chunk";
    case SymbolKind::instance:
        return "instance";
    case SymbolKind::resource:
        return "resource";
    case SymbolKind::count:
        break;
    }
    return "symbol";
}

std::optional<std::uint64_t> parse_hex_offset(const std::string_view text) {
    if (text.size() < 3 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) return std::nullopt;
    std::uint64_t value{};
    const auto digits = text.substr(2);
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
    if (error != std::errc{} || end != digits.data() + digits.size()) return std::nullopt;
    return value;
}

void SearchIndex::add(SearchEntry entry) {
    ++generation_;
    lowered_.push_back(lower(entry.label) + '\n' + lower(entry.detail) + '\n' + lower(entry.haystack));
    by_kind_[static_cast<std::size_t>(entry.kind)].push_back(entries_.size());
    entries_.push_back(std::move(entry));
}

bool SearchIndex::contains(const std::size_t index, const std::string_view needle) const {
    return needle.empty() || (index < lowered_.size() && lowered_[index].find(needle) != std::string::npos);
}

std::vector<SearchResult> SearchIndex::query(const std::string_view raw, const std::size_t limit,
                                             const bool gameplay_first) const {
    std::vector<SearchResult> results;
    auto text = trim(raw);
    std::optional<std::vector<SymbolKind>> only;
    if (const auto colon = text.find(':'); colon != std::string_view::npos && colon > 0) {
        if (auto kinds = prefix_kinds(lower(std::string(text.substr(0, colon))))) {
            only = std::move(kinds);
            text = trim(text.substr(colon + 1));
        }
    }
    if (limit == 0 || (text.empty() && !only)) return results;
    const auto finish = [&] {
        if (only)
            std::erase_if(results, [&](const SearchResult& r) { return std::ranges::find(*only, r.entry->kind) == only->end(); });
        if (gameplay_first)
            for (auto& result : results) result.score += gameplay_weight(result.entry->kind);
        std::ranges::stable_sort(results, std::greater{}, &SearchResult::score);
        if (results.size() > limit) results.resize(limit);
        return results;
    };

    if (const auto offset = parse_hex_offset(text)) {
        for (const auto& entry : entries_) {
            if (!entry.offset) continue;
            if (*entry.offset == *offset) {
                results.push_back({&entry, 1000, {}});
            } else if (entry.end_offset && *entry.offset < *offset && *offset < *entry.end_offset) {
                // Smaller containing ranges are more specific and rank higher.
                const auto span = *entry.end_offset - *entry.offset;
                results.push_back(
                    {&entry, 500 - static_cast<int>(std::min<std::uint64_t>(span / 64, 400)), {}});
            }
        }
        return finish();
    }

    if (text.empty()) {
        // "actor:" alone lists every entry of the kind.
        for (const auto& kind : *only)
            for (const auto index : indices_of(kind)) results.push_back({&entries_[index], 0, {}});
        return finish();
    }
    const bool explicit_id = text.front() == '#';
    if (const auto number = parse_decimal(explicit_id ? text.substr(1) : text)) {
        for (const auto& entry : entries_)
            if (entry.id && *entry.id == *number)
                results.push_back({&entry, 1000 + kind_weight(entry.kind), {}});
    }
    if (explicit_id) return finish();

    const auto id_results = results.size();
    for (const auto& entry : entries_) {
        const auto by_label = fuzzy_match(text, entry.label);
        const auto by_detail =
            entry.detail.empty() ? std::nullopt : fuzzy_match(text, entry.detail);
        if (!by_label && !by_detail) continue;
        if (id_results != 0 &&
            std::any_of(results.begin(), results.begin() + static_cast<std::ptrdiff_t>(id_results),
                        [&](const SearchResult& r) { return r.entry == &entry; }))
            continue;
        int score = by_label ? by_label->score * 2 : by_detail->score;
        if (by_label && by_detail) score = std::max(score, by_detail->score);
        SearchResult result{&entry, score + kind_weight(entry.kind), {}};
        if (by_label) result.label_positions = by_label->positions;
        results.push_back(std::move(result));
    }
    return finish();
}

} // namespace rwsman
