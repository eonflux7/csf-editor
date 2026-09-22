#pragma once

#include "csf/document.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace csf {

using EditValue = std::variant<std::int32_t, float, std::uint32_t, std::vector<std::byte>>;

enum class EditTargetKind { integer, real, string_reference, string_value };

struct EditTarget {
    EditTargetKind kind{EditTargetKind::integer};
    std::uint32_t entry_index{};
    std::uint32_t table_index{};
};

struct EditValidation {
    bool valid{true};
    bool blocking{false};
    std::string message;
};

struct EditCommand {
    EditTarget target;
    EditValue before;
    EditValue after;
    EditValidation validation;
    std::string meaning;
};

struct SaveValidation {
    bool passed{};
    std::vector<std::string> diagnostics;
};

struct SaveResult {
    std::filesystem::path output_path;
    std::filesystem::path manifest_path;
    std::string source_sha256;
    std::string output_sha256;
    SaveValidation validation;
};

enum class AuthoringCategory { database_scalar, reference, spatial, player_property, script_flag };

struct EditableField {
    std::uint32_t entry_index{};
    EditTargetKind kind{EditTargetKind::integer};
    AuthoringCategory category{AuthoringCategory::database_scalar};
    std::string label;
    std::string meaning;
};

// Returns only the deliberately reviewed first-slice fields. Numeric entries
// absent from this list remain inspection-only even though EditSession supplies
// the serializer primitive used by typed views.
[[nodiscard]] std::vector<EditableField> authorable_fields(const Document& document);

// Transactional, deliberately bounded CSFFBS authoring. Unknown records, header
// bytes, table ordering and trailing bytes remain authoritative raw data.
class EditSession {
public:
    [[nodiscard]] static EditSession load(const std::filesystem::path& path);
    [[nodiscard]] static EditSession from_document(Document document);

    [[nodiscard]] const Document& document() const noexcept { return document_; }
    [[nodiscard]] const std::vector<EditCommand>& changes() const noexcept { return history_; }
    [[nodiscard]] std::size_t history_position() const noexcept { return cursor_; }
    [[nodiscard]] bool dirty() const noexcept { return cursor_ != 0; }
    [[nodiscard]] bool can_undo() const noexcept { return cursor_ != 0; }
    [[nodiscard]] bool can_redo() const noexcept { return cursor_ < history_.size(); }

    EditValidation set_integer(std::uint32_t entry_index, std::int32_t value,
                               std::string meaning = {});
    EditValidation set_real(std::uint32_t entry_index, float value,
                            std::string meaning = {});
    // Copy-on-write is the safe default: only this entry receives a newly
    // appended string. Global replacement explicitly changes every use.
    EditValidation set_string(std::uint32_t entry_index, std::span<const std::byte> bytes,
                              bool global_replace = false, std::string meaning = {});
    EditValidation set_string(std::uint32_t entry_index, std::string_view windows_1252,
                              bool global_replace = false, std::string meaning = {});

    bool undo();
    bool redo();
    void discard();

    [[nodiscard]] std::vector<std::byte> serialize() const;
    [[nodiscard]] std::string change_manifest_json(const std::filesystem::path& output,
                                                   const SaveValidation& validation) const;
    [[nodiscard]] SaveResult save_copy(const std::filesystem::path& output,
                                       std::optional<std::filesystem::path> manifest = std::nullopt)
        const;

private:
    explicit EditSession(Document document);
    void commit(EditCommand command);
    void apply(const EditCommand& command, bool forward);
    void reparse();

    Document original_;
    Document document_;
    std::vector<std::byte> bytes_;
    std::vector<EditCommand> history_;
    std::size_t cursor_{};
};

[[nodiscard]] std::string sha256(std::span<const std::byte> bytes);
[[nodiscard]] const char* edit_target_kind_name(EditTargetKind kind) noexcept;
[[nodiscard]] const char* authoring_category_name(AuthoringCategory category) noexcept;

} // namespace csf
