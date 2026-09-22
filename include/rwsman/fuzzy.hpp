#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace rwsman {

struct FuzzyMatch {
    int score{};
    // Byte offsets in the candidate that matched a query character, ascending.
    std::vector<std::uint32_t> positions;
};

// Case-insensitive subsequence match. Every query character must appear in the
// candidate in order. The score rewards matches at word starts (after a space,
// '_', '-', '.', '/', ':', '[' or a lowercase-to-uppercase change), consecutive
// matches, a prefix match, and an exact match, and penalizes gaps. An empty query
// matches everything with score 0. Returns nullopt when the query is not a
// subsequence.
[[nodiscard]] std::optional<FuzzyMatch> fuzzy_match(std::string_view query,
                                                    std::string_view candidate);

} // namespace rwsman
