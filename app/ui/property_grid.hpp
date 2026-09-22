#pragma once

#include "app_state.hpp"
#include "ui/theme.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace rwsman::ui {

// A byte range in a document that backs a displayed value. Rows that carry one
// get a "raw" disclosure that shows the bytes in place.
struct RawSlice {
    const csf::Document* csf{};      // Bytes come from this CSFFBS file...
    const rws::Document* rws{};      // ...or this RenderWare file.
    std::uint64_t offset{};
    std::uint64_t size{};
    std::optional<std::uint32_t> entry; // CSFFBS entry index, when known.
    [[nodiscard]] bool valid() const noexcept { return (csf || rws) && size != 0; }
};

// Finds the CSFFBS field entry called `field` under `record` and returns the
// bytes it occupies. Returns an invalid slice when the field is absent.
[[nodiscard]] RawSlice raw_slice_for_field(const csf::Document& document, const csf::Node& record,
                                           std::string_view field);
// The bytes of a whole record entry.
[[nodiscard]] RawSlice raw_slice_for_entry(const csf::Document& document, std::uint32_t entry);

struct PropertyOptions {
    Provenance provenance{Provenance::proven};
    bool show_badge{true};
    std::string evidence; // Tooltip: where the value came from.
    RawSlice raw;
};

// Two-column property table: dim key, monospace value, trailing badge and raw
// disclosure. Every value copies itself on click and explains its provenance in
// a tooltip. Values are read-only in this build; the lock icon marks where the
// guarded-authoring editors (Phase 06) will attach.
class PropertyGrid {
public:
    PropertyGrid(AppState& state, const char* id);
    ~PropertyGrid();
    PropertyGrid(const PropertyGrid&) = delete;
    PropertyGrid& operator=(const PropertyGrid&) = delete;
    explicit operator bool() const noexcept { return open_; }

    using Options = PropertyOptions;

    void text(const char* key, std::string_view value, const Options& options = {});
    void integer(const char* key, std::int64_t value, const Options& options = {});
    void real(const char* key, double value, const Options& options = {});
    void boolean(const char* key, bool value, const Options& options = {});
    void vec3(const char* key, double x, double y, double z, const Options& options = {});
    // `value` is in degrees when `degrees` is true, radians otherwise.
    void angle(const char* key, double value, bool degrees, const Options& options = {});
    void offset(const char* key, std::uint64_t value, const Options& options = {});
    // Clickable value that navigates to `target`.
    void link(const char* key, std::string_view label, const SelectionRef& target,
              const Options& options = {});
    // Free-form value drawn by the caller inside the value cell.
    template <class Draw>
    void custom(const char* key, Draw&& draw, const Options& options = {}) {
        begin_row(key);
        draw();
        end_row(options);
    }

private:
    void begin_row(const char* key);
    void end_row(const Options& options);
    void value_text(const std::string& shown, const std::string& copy, const Options& options);

    AppState& state_;
    std::string id_;
    bool open_{};
    int row_{};
    std::string key_;
    ImVec2 row_min_{};
};

// A collapsible inspector section whose open state is remembered per selection
// kind for the session. Returns true when the body should be drawn; the caller
// must call `ImGui::TreePop()` through `end_section()` afterwards.
bool begin_section(AppState& state, const char* title, bool default_open = true);
void end_section();

// Small hex dump used by the raw disclosure and Hex previews (max `limit` bytes).
void draw_hex_dump(std::span<const std::byte> bytes, std::uint64_t base_offset, std::size_t limit = 96);

} // namespace rwsman::ui
