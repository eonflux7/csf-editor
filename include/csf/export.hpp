#pragma once

#include "csf/document.hpp"

#include <filesystem>
#include <string>

namespace csf {

// Deterministic inspection formats. They are not recompilable source formats and
// do not replace the authoritative raw tables in Document.
[[nodiscard]] std::string export_text(const Document& document);
[[nodiscard]] std::string export_json(const Document& document);

// Writes only to a path that does not already exist and is not the input path.
// Parent directories must already exist.
void write_new_export(const Document& document, const std::filesystem::path& output,
                      const std::string& contents);

} // namespace csf
