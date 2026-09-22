#include "rwsman/fuzzy.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <limits>

namespace rwsman {
namespace {

constexpr int no_score = std::numeric_limits<int>::min() / 2;

char lower(const char value) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

bool is_separator(const char value) {
    return value == ' ' || value == '_' || value == '-' || value == '.' || value == '/' ||
           value == ':' || value == '[' || value == '(' || value == '\\';
}

bool is_word_start(const std::string_view text, const std::size_t index) {
    if (index == 0) return true;
    const char previous = text[index - 1], current = text[index];
    if (is_separator(previous)) return true;
    return std::islower(static_cast<unsigned char>(previous)) &&
           std::isupper(static_cast<unsigned char>(current));
}

} // namespace

std::optional<FuzzyMatch> fuzzy_match(const std::string_view query,
                                      const std::string_view candidate) {
    if (query.empty()) return FuzzyMatch{};
    const auto n = query.size(), m = candidate.size();
    if (n > m) return std::nullopt;
    {
        // Cheap rejection before the O(n*m) pass: the query must be a subsequence.
        std::size_t next = 0;
        for (std::size_t j = 0; j < m && next < n; ++j)
            if (lower(candidate[j]) == lower(query[next])) ++next;
        if (next != n) return std::nullopt;
    }

    // score[i][j]: best score matching query[0..i] with query[i] placed at
    // candidate[j]. Dynamic programming keeps the result optimal instead of
    // greedy, so "lgt" prefers "LGT_01" over a scattered match.
    constexpr int gap_penalty = 1, consecutive_bonus = 5;
    std::vector<std::vector<int>> score(n, std::vector<int>(m, no_score));
    std::vector<std::vector<std::int32_t>> back(n, std::vector<std::int32_t>(m, -1));
    for (std::size_t i = 0; i < n; ++i) {
        // running = max over k < j of score[i-1][k] + gap_penalty * k
        int running = no_score;
        std::int32_t running_index = -1;
        for (std::size_t j = 0; j < m; ++j) {
            if (i > 0 && j > 0 && score[i - 1][j - 1] != no_score) {
                const int value = score[i - 1][j - 1] + gap_penalty * static_cast<int>(j - 1);
                if (value >= running) {
                    running = value;
                    running_index = static_cast<std::int32_t>(j - 1);
                }
            }
            if (lower(query[i]) != lower(candidate[j])) continue;
            int bonus = 1;
            if (is_word_start(candidate, j)) bonus += 8;
            if (query[i] == candidate[j]) bonus += 1;
            if (i == 0) {
                if (j == 0) bonus += 6;
                score[i][j] = bonus - static_cast<int>(std::min<std::size_t>(j, 6));
                continue;
            }
            if (running != no_score) {
                score[i][j] = running - gap_penalty * static_cast<int>(j - 1) + bonus;
                back[i][j] = running_index;
            }
            if (j > 0 && score[i - 1][j - 1] != no_score &&
                score[i - 1][j - 1] + bonus + consecutive_bonus > score[i][j]) {
                score[i][j] = score[i - 1][j - 1] + bonus + consecutive_bonus;
                back[i][j] = static_cast<std::int32_t>(j - 1);
            }
        }
    }
    std::size_t end = m;
    int best = no_score;
    for (std::size_t j = 0; j < m; ++j)
        if (score[n - 1][j] != no_score && score[n - 1][j] > best) {
            best = score[n - 1][j];
            end = j;
        }
    if (end == m) return std::nullopt;

    FuzzyMatch result;
    result.positions.resize(n);
    auto column = static_cast<std::int32_t>(end);
    for (std::size_t i = n; i-- > 0;) {
        result.positions[i] = static_cast<std::uint32_t>(column);
        column = back[i][static_cast<std::size_t>(column)];
    }
    result.score = best;
    if (n == m && std::equal(query.begin(), query.end(), candidate.begin(), [](char a, char b) {
            return lower(a) == lower(b);
        }))
        result.score += 40;
    // Shorter candidates win ties: they are more specific.
    result.score -= static_cast<int>(std::min<std::size_t>(m - n, 30)) / 3;
    return result;
}

} // namespace rwsman
