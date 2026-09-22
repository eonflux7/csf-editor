#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#endif

#include "file_dialogs.hpp"

#include "app_actions.hpp"
#include "app_util.hpp"
#include "mission_editing.hpp"
#include "mission_loader.hpp"

#include <portable-file-dialogs.h>

#include <memory>
#include <string>
#include <vector>

namespace rwsman {

// Type-erased pending dialog (the header is heavy, so AppState only sees this).
struct PendingDialog {
    DialogKind kind{DialogKind::document};
    std::unique_ptr<pfd::open_file> file;
    std::unique_ptr<pfd::select_folder> folder;
    std::unique_ptr<pfd::save_file> save;
};

namespace {

std::string start_path(const AppState& state, const std::filesystem::path& hint) {
    if (!hint.empty()) return path_utf8(hint);
    if (!state.settings.resource_root.empty()) return path_utf8(state.settings.resource_root);
    return {};
}

template <std::size_t N>
void copy_path(std::array<char, N>& buffer, const std::filesystem::path& path) {
    const auto text = path_utf8(path);
    const auto size = std::min(text.size(), N - 1);
    std::copy_n(text.data(), size, buffer.data());
    buffer[size] = '\0';
}

} // namespace

bool file_dialog_open(const AppState& state) {
    return state.dialog != nullptr;
}

void request_file_dialog(AppState& state, const DialogKind kind, const std::filesystem::path& start_directory) {
    if (state.dialog) return;
    if (!pfd::settings::available()) {
        state.warn("No native file dialog is available (install zenity or kdialog), or drop a file on the window");
        return;
    }
    auto pending = std::make_shared<PendingDialog>();
    pending->kind = kind;
    const auto start = start_path(state, start_directory);
    switch (kind) {
    case DialogKind::document:
        pending->file = std::make_unique<pfd::open_file>(
            "Open RenderWare file", start,
            std::vector<std::string>{"RenderWare models, streams, animation", "*.rpc *.rws *.anm", "All files", "*"});
        break;
    case DialogKind::companion:
        pending->file = std::make_unique<pfd::open_file>(
            "Open collision companion", start,
            std::vector<std::string>{"RenderWare streams", "*.rws", "All files", "*"});
        break;
    case DialogKind::mission:
        pending->file = std::make_unique<pfd::open_file>(
            "Open mission scene", start, std::vector<std::string>{"CSF mission scenes", "*.scn", "All files", "*"});
        break;
    case DialogKind::resource_root:
        pending->folder = std::make_unique<pfd::select_folder>("Choose the resource root", start);
        break;
    case DialogKind::game_root:
        pending->folder = std::make_unique<pfd::select_folder>("Choose the game installation folder", start);
        break;
    case DialogKind::projects_root:
        pending->folder = std::make_unique<pfd::select_folder>("Choose the mission projects folder", start);
        break;
    case DialogKind::open_project:
        pending->folder = std::make_unique<pfd::select_folder>("Open a mission project folder", start);
        break;
    case DialogKind::save_project:
        pending->folder = std::make_unique<pfd::select_folder>("Choose a folder for the mission project", start);
        break;
    case DialogKind::import_donor:
        pending->folder = std::make_unique<pfd::select_folder>("Choose an unpacked mission to import from", start);
        break;
    case DialogKind::export_original:
        pending->file = std::make_unique<pfd::open_file>(
            "Choose the shipped mission archive", start,
            std::vector<std::string>{"Mission archives", "*.pak", "All files", "*"});
        break;
    case DialogKind::export_output:
        pending->save = std::make_unique<pfd::save_file>(
            "Write the mission archive to", start,
            std::vector<std::string>{"Mission archives", "*.pak", "All files", "*"}, pfd::opt::force_overwrite);
        break;
    }
    state.dialog = std::move(pending);
}

void poll_file_dialogs(AppState& state) {
    if (!state.dialog) return;
    auto& pending = *state.dialog;
    std::filesystem::path chosen;
    if (pending.file) {
        if (!pending.file->ready(0)) return;
        const auto result = pending.file->result();
        if (!result.empty()) chosen = std::filesystem::path(std::u8string(result.front().begin(), result.front().end()));
    } else if (pending.folder) {
        if (!pending.folder->ready(0)) return;
        const auto result = pending.folder->result();
        if (!result.empty()) chosen = std::filesystem::path(std::u8string(result.begin(), result.end()));
    } else if (pending.save) {
        if (!pending.save->ready(0)) return;
        const auto result = pending.save->result();
        if (!result.empty()) chosen = std::filesystem::path(std::u8string(result.begin(), result.end()));
    }
    const auto kind = pending.kind;
    state.dialog.reset();
    if (chosen.empty()) return; // Cancelled.
    switch (kind) {
    case DialogKind::document:
        load_document(state, chosen);
        break;
    case DialogKind::mission:
        start_mission_load(state, chosen);
        break;
    case DialogKind::companion:
        pair_collision(state, chosen, true);
        break;
    case DialogKind::resource_root:
        state.settings.resource_root = chosen;
        state.settings_dirty = true;
        rescan_resource_root(state);
        break;
    case DialogKind::game_root:
        state.settings.game_root = chosen;
        state.settings_dirty = true;
        break;
    case DialogKind::projects_root:
        state.settings.projects_root = chosen;
        state.settings_dirty = true;
        break;
    case DialogKind::open_project:
        open_mission_project(state, chosen);
        break;
    case DialogKind::save_project:
        save_mission_project(state, chosen);
        break;
    case DialogKind::import_donor:
        copy_path(state.ui.import_donor, chosen);
        break;
    case DialogKind::export_original:
        copy_path(state.ui.export_original, chosen);
        break;
    case DialogKind::export_output:
        copy_path(state.ui.export_output, chosen);
        break;
    }
}

} // namespace rwsman
