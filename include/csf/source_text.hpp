#pragma once

#include "csf/tree.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace csf {

// Recompilable CSFFBS source text. Unlike export_text, parsing the printed text
// reproduces the exact tree, so an unedited print/parse/serialize cycle is
// byte-identical.
//
//   ( ... )      group            [ ... ]      array
//   .NAME value  labelled value   "Na me": v   label that is not a bare .word
//   42  -7       integer          2.0  1e-05   real (always has '.', 'e' or is %bits)
//   %7fc00000    real by raw bits (NaN, infinity)
//   WORD "text"  string; "" is zero bytes; "..."~ has no final NUL;
//                \\ \" \n \r \t \xHH escapes
//   # comment    to the end of the line
//
// Script instruction lists (.ACCIONES and .CONDICIONES) print as blocks with one
// instruction per line, indented by control flow:
//
//   .ACCIONES {
//     WHILE (BOOL TRUE)
//       PLAY_ANMBDD (THIS) (ANM_BDD 1937)
//     WEND
//   }
//
// Each line is an unlabelled group whose items are the words and values on the
// line; parentheses may continue a line. `@ value` on a line inserts that value
// as the instruction itself.
class SourceTextError : public std::runtime_error {
public:
    SourceTextError(std::size_t line, std::size_t column, const std::string& message);
    [[nodiscard]] std::size_t line() const noexcept { return line_; }
    [[nodiscard]] std::size_t column() const noexcept { return column_; }

private:
    std::size_t line_{}, column_{};
};

[[nodiscard]] std::string to_source_text(const TreeNode& node, std::size_t indent = 0);
[[nodiscard]] std::string to_source_text(const std::vector<TreeNode>& nodes);
// Parses zero or more values.
[[nodiscard]] std::vector<TreeNode> parse_source_text(std::string_view text);
// Parses exactly one value.
[[nodiscard]] TreeNode parse_source_value(std::string_view text);

} // namespace csf
