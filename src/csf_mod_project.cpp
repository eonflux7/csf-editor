#include "csf/mod_project.hpp"

#include "csf/authoring.hpp"
#include "csf/document.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

#ifdef RWSMAN_HAVE_PAKMAN
#include "pakman/archive.hpp"
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <cerrno>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace csf {
namespace {

std::string path_string(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path path_from_utf8(const std::string_view value) {
    std::u8string converted;
    converted.assign(reinterpret_cast<const char8_t*>(value.data()),
                     reinterpret_cast<const char8_t*>(value.data() + value.size()));
    return std::filesystem::path(converted);
}

std::string json_escape(const std::string_view value) {
    std::string out;
    for (const unsigned char c : value) {
        if (c == '\\' || c == '"') { out.push_back('\\'); out.push_back(static_cast<char>(c)); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out.push_back(static_cast<char>(c));
    }
    return out;
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Cannot open file: " + path_string(path));
    const auto end = input.tellg();
    if (end < 0) throw std::runtime_error("Cannot determine file size: " + path_string(path));
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input && !bytes.empty()) throw std::runtime_error("Cannot read file: " + path_string(path));
    return bytes;
}

std::string file_hash(const std::filesystem::path& path) { return sha256(read_bytes(path)); }

std::string packaged_archive_hash(const std::filesystem::path& archive_path) {
    auto manifest_path = archive_path;
    manifest_path += ".package.json";
    if (!std::filesystem::is_regular_file(manifest_path))
        throw std::runtime_error("PAK package manifest is missing");
    const auto bytes = read_bytes(manifest_path);
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    constexpr std::string_view key = "\"archive_sha256\":\"";
    const auto start = text.find(key);
    if (start == std::string::npos || start + key.size() + 64 > text.size())
        throw std::runtime_error("PAK package manifest has no valid archive hash");
    const auto value = text.substr(start + key.size(), 64);
    if (!std::ranges::all_of(value, [](const unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
        throw std::runtime_error("PAK package manifest has an invalid archive hash");
    return value;
}

void copy_verified(const std::filesystem::path& source, const std::filesystem::path& destination,
                   const std::string& expected) {
    if (!destination.parent_path().empty())
        std::filesystem::create_directories(destination.parent_path());
    auto temporary = destination;
    temporary += ".tmp";
    std::filesystem::copy_file(source, temporary, std::filesystem::copy_options::overwrite_existing);
    if (!expected.empty() && file_hash(temporary) != expected) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("Copied file hash differs for " + path_string(destination));
    }
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(destination, error);
        error.clear();
        std::filesystem::rename(temporary, destination, error);
    }
    if (error) throw std::runtime_error("Cannot publish file: " + error.message());
}

bool safe_relative(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_path()) return false;
    for (const auto& part : path)
        if (part == ".." || part == ".") return false;
    return true;
}

bool same_path(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (a.empty() || b.empty()) return false;
    std::error_code error;
    const auto ca = std::filesystem::weakly_canonical(a, error);
    if (error) return false;
    const auto cb = std::filesystem::weakly_canonical(b, error);
    return !error && ca == cb;
}

bool below(const std::filesystem::path& child, const std::filesystem::path& parent) {
    std::error_code error;
    const auto c = std::filesystem::weakly_canonical(child, error);
    if (error) return false;
    const auto p = std::filesystem::weakly_canonical(parent, error);
    if (error) return false;
    auto ci = c.begin();
    for (auto pi = p.begin(); pi != p.end(); ++pi, ++ci)
        if (ci == c.end() || *ci != *pi) return false;
    return true;
}

std::string timestamp();

void run_process(const std::vector<std::string>& arguments) {
    if (arguments.empty()) throw std::runtime_error("Cannot run an empty command");
#ifdef _WIN32
    std::vector<std::wstring> storage;
    storage.reserve(arguments.size());
    for (const auto& argument : arguments) {
        const auto needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argument.data(),
                                                static_cast<int>(argument.size()), nullptr, 0);
        if (needed <= 0) throw std::runtime_error("Command argument is not valid UTF-8");
        std::wstring wide(static_cast<std::size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argument.data(),
                            static_cast<int>(argument.size()), wide.data(), needed);
        storage.push_back(std::move(wide));
    }
    std::vector<const wchar_t*> argv;
    argv.reserve(storage.size() + 1);
    for (const auto& value : storage) argv.push_back(value.c_str());
    argv.push_back(nullptr);
    const auto status = _wspawnvp(_P_WAIT, argv.front(), argv.data());
    if (status == -1) throw std::runtime_error("Cannot start pakman-cli");
    if (status != 0) throw std::runtime_error("pakman-cli failed with exit code " +
                                              std::to_string(status));
#else
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    const auto child = fork();
    if (child < 0) throw std::runtime_error("Cannot fork pakman-cli: " + std::string(std::strerror(errno)));
    if (child == 0) {
        execvp(argv.front(), argv.data());
        _exit(127);
    }
    int status{};
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        throw std::runtime_error("Cannot wait for pakman-cli: " + std::string(std::strerror(errno)));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        const auto code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        throw std::runtime_error("pakman-cli failed with exit code " + std::to_string(code));
    }
#endif
}

std::string process_argument(const std::filesystem::path& path) {
    return path_string(path);
}

void publish_file(const std::filesystem::path& temporary,
                  const std::filesystem::path& destination, const bool overwrite) {
    std::error_code error;
    const bool existed = std::filesystem::exists(destination, error) && !error;
    if (existed && !overwrite)
        throw std::runtime_error("Archive already exists; pass --overwrite to replace it");
    if (!destination.parent_path().empty())
        std::filesystem::create_directories(destination.parent_path());
    auto previous = destination;
    previous += ".previous-" + timestamp();
    if (existed) {
        std::filesystem::rename(destination, previous, error);
        if (error) throw std::runtime_error("Cannot preserve previous archive: " + error.message());
    }
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        if (existed) { std::error_code ignored; std::filesystem::rename(previous, destination, ignored); }
        throw std::runtime_error("Cannot publish archive: " + error.message());
    }
    if (existed) std::filesystem::remove(previous, error);
}

void publish_directory(const std::filesystem::path& temporary,
                       const std::filesystem::path& destination) {
    std::error_code error;
    auto previous = destination;
    previous += ".previous-" + timestamp();
    const bool existed = std::filesystem::exists(destination, error) && !error;
    if (existed) {
        std::filesystem::rename(destination, previous, error);
        if (error) throw std::runtime_error("Cannot preserve previous staging tree: " + error.message());
    }
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        if (existed) { std::error_code ignored; std::filesystem::rename(previous, destination, ignored); }
        throw std::runtime_error("Cannot publish staging tree: " + error.message());
    }
    if (existed) std::filesystem::remove_all(previous, error);
}

std::string timestamp() {
    return std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
}

// Full archive verification. An explicitly chosen pakman-cli is honoured;
// otherwise the linked pak-man library does it when it is available.
void verify_archive(const std::filesystem::path& archive, const std::filesystem::path& pakman_cli) {
#ifdef RWSMAN_HAVE_PAKMAN
    if (pakman_cli.empty() || pakman_cli == std::filesystem::path{"pakman-cli"}) {
        (void)pakman::Archive::open(archive).verify(true);
        return;
    }
#endif
    run_process({process_argument(pakman_cli), "verify", process_argument(archive)});
}

} // namespace

const char* overlay_diagnostic_kind_name(const OverlayDiagnostic::Kind kind) noexcept {
    switch (kind) {
    case OverlayDiagnostic::Kind::introduced: return "introduced";
    case OverlayDiagnostic::Kind::baseline: return "baseline";
    case OverlayDiagnostic::Kind::stale_source: return "stale-source";
    case OverlayDiagnostic::Kind::invalid_output: return "invalid-output";
    }
    return "unknown";
}

ModProject ModProject::create(std::filesystem::path workspace,
                              std::filesystem::path source, std::string project_name,
                              std::string version) {
    if (workspace.empty() || source.empty()) throw std::runtime_error("Workspace and source roots are required");
    if (same_path(workspace, source) || below(workspace, source) || below(source, workspace))
        throw std::runtime_error("Mod workspace and source root must be separate trees");
    ModProject project;
    project.workspace_root = std::move(workspace);
    project.source_root = std::move(source);
    project.name = std::move(project_name);
    project.tool_version = std::move(version);
    project.save();
    return project;
}

void ModProject::save() const {
    if (workspace_root.empty()) throw std::runtime_error("Project workspace root is empty");
    std::filesystem::create_directories(workspace_root);
    {
        std::ofstream output(workspace_root / "mod-project.json", std::ios::trunc);
        output << manifest_json();
        if (!output) throw std::runtime_error("Cannot write mod-project.json");
    }
    {
        std::ofstream output(workspace_root / "local-config.json", std::ios::trunc);
        output << local_config_json();
        if (!output) throw std::runtime_error("Cannot write local-config.json");
    }
    // A private, quoted state file avoids accepting arbitrary JSON while keeping
    // the two public schemas human-readable and distributable.
    std::ofstream state(workspace_root / ".csf-mod-state", std::ios::trunc);
    state << "csf-mod-state-1\n" << std::quoted(name) << ' ' << std::quoted(game) << ' '
          << std::quoted(tool_version) << ' ' << std::quoted(path_string(source_root)) << '\n';
    for (const auto& file : files) {
        state << "file " << std::quoted(path_string(file.relative_path)) << ' '
              << std::quoted(path_string(file.authored_path)) << ' '
              << std::quoted(path_string(file.change_manifest)) << ' '
              << std::quoted(file.source_sha256) << ' ' << std::quoted(file.output_sha256) << ' '
              << file.semantic_targets.size();
        for (const auto& target : file.semantic_targets) state << ' ' << std::quoted(target);
        state << '\n';
    }
    if (!state) throw std::runtime_error("Cannot write local project state");
}

ModProject ModProject::load(const std::filesystem::path& workspace) {
    std::ifstream state(workspace / ".csf-mod-state");
    std::string magic;
    std::getline(state, magic);
    if (!state || magic != "csf-mod-state-1") throw std::runtime_error("Unsupported or missing mod project state");
    ModProject project;
    project.workspace_root = workspace;
    std::string source;
    state >> std::quoted(project.name) >> std::quoted(project.game) >> std::quoted(project.tool_version)
          >> std::quoted(source);
    project.source_root = path_from_utf8(source);
    std::string record;
    while (state >> record) {
        if (record != "file") throw std::runtime_error("Unknown mod project state record");
        ModFile file;
        std::string relative, authored, manifest;
        std::size_t targets{};
        state >> std::quoted(relative) >> std::quoted(authored) >> std::quoted(manifest)
              >> std::quoted(file.source_sha256) >> std::quoted(file.output_sha256) >> targets;
        file.relative_path = path_from_utf8(relative);
        file.authored_path = path_from_utf8(authored);
        file.change_manifest = path_from_utf8(manifest);
        for (std::size_t i = 0; i < targets; ++i) {
            std::string target;
            state >> std::quoted(target);
            file.semantic_targets.push_back(std::move(target));
        }
        if (!state) throw std::runtime_error("Truncated mod project state");
        project.files.push_back(std::move(file));
    }
    return project;
}

void ModProject::add_file(ModFile file) {
    if (!safe_relative(file.relative_path)) throw std::runtime_error("Mod file path must be safe and game-relative");
    if (!std::filesystem::is_regular_file(file.authored_path)) throw std::runtime_error("Authored file does not exist");
    const auto actual = file_hash(file.authored_path);
    if (file.output_sha256.empty()) file.output_sha256 = actual;
    if (actual != file.output_sha256) throw std::runtime_error("Authored output hash does not match its manifest");
    const auto found = std::ranges::find_if(files, [&](const ModFile& value) {
        return value.relative_path.generic_string() == file.relative_path.generic_string();
    });
    if (found == files.end()) files.push_back(std::move(file));
    else *found = std::move(file);
    std::ranges::sort(files, {}, [](const ModFile& value) { return value.relative_path.generic_string(); });
}

std::string ModProject::manifest_json() const {
    std::ostringstream out;
    out << "{\n  \"format_version\":" << format_version << ",\n  \"name\":\"" << json_escape(name)
        << "\",\n  \"game\":\"" << json_escape(game) << "\",\n  \"tool_version\":\""
        << json_escape(tool_version) << "\",\n  \"files\":[";
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& file = files[i];
        if (i) out << ',';
        out << "\n    {\"relative_path\":\"" << json_escape(path_string(file.relative_path))
            << "\",\"staged_path\":\"files/" << json_escape(path_string(file.relative_path))
            << "\",\"source_sha256\":\"" << file.source_sha256 << "\",\"output_sha256\":\""
            << file.output_sha256 << "\",\"change_manifest\":\"changes/"
            << json_escape(path_string(file.relative_path)) << ".json\",\"validation\":\"passed\",\"semantic_targets\":[";
        for (std::size_t j = 0; j < file.semantic_targets.size(); ++j) {
            if (j) out << ',';
            out << '"' << json_escape(file.semantic_targets[j]) << '"';
        }
        out << "]}";
    }
    if (!files.empty()) out << '\n';
    out << "  ],\n  \"dependencies\":[],\n  \"conflicts\":[]\n}\n";
    return out.str();
}

std::string ModProject::local_config_json() const {
    return "{\n  \"format_version\":1,\n  \"source_root\":\"" +
           json_escape(path_string(source_root)) + "\"\n}\n";
}

OverlayReport ModProject::validate() const {
    OverlayReport report{true, {}};
    for (const auto& file : files) {
        if (!safe_relative(file.relative_path)) {
            report.diagnostics.push_back({OverlayDiagnostic::Kind::invalid_output, file.relative_path,
                                          "Path is not a safe game-relative path"});
            continue;
        }
        const auto source = source_root / file.relative_path;
        if (!file.source_sha256.empty()) {
            if (!std::filesystem::is_regular_file(source) || file_hash(source) != file.source_sha256)
                report.diagnostics.push_back({OverlayDiagnostic::Kind::stale_source, file.relative_path,
                                              "Source is missing or its hash changed"});
        }
        if (!std::filesystem::is_regular_file(file.authored_path) ||
            file_hash(file.authored_path) != file.output_sha256) {
            report.diagnostics.push_back({OverlayDiagnostic::Kind::invalid_output, file.relative_path,
                                          "Authored file is missing or its hash changed"});
            continue;
        }
        const auto bytes = read_bytes(file.authored_path);
        if (Document::sniff(bytes)) {
            const auto edited = Document::from_bytes(bytes);
            if (edited.has_errors()) report.diagnostics.push_back(
                {OverlayDiagnostic::Kind::introduced, file.relative_path,
                 "Staged CSFFBS output has structural errors"});
            if (std::filesystem::is_regular_file(source)) {
                const auto baseline = Document::load(source);
                if (baseline.has_errors()) report.diagnostics.push_back(
                    {OverlayDiagnostic::Kind::baseline, file.relative_path,
                     "Source CSFFBS already has structural errors"});
            }
        }
    }
    report.passed = std::ranges::none_of(report.diagnostics, [](const OverlayDiagnostic& value) {
        return value.kind != OverlayDiagnostic::Kind::baseline;
    });
    return report;
}

OverlayReport ModProject::build(const std::filesystem::path& staging_root) const {
    auto report = validate();
    if (!report.passed) return report;
    if (same_path(staging_root, source_root) || same_path(staging_root, workspace_root) ||
        below(staging_root, source_root) || below(source_root, staging_root) ||
        below(staging_root, workspace_root) || below(workspace_root, staging_root))
        throw std::runtime_error("Staging root must not alias the source or project workspace");
    if (std::filesystem::exists(staging_root) &&
        !std::filesystem::is_regular_file(staging_root / "mod-project.json"))
        throw std::runtime_error("Refusing to replace a staging directory not owned by csf-mod");
    auto temporary = staging_root;
    temporary += ".building-" + timestamp();
    std::filesystem::create_directories(temporary / "files");
    try {
        for (const auto& file : files) {
            copy_verified(file.authored_path, temporary / "files" / file.relative_path,
                          file.output_sha256);
            if (!file.change_manifest.empty() && std::filesystem::is_regular_file(file.change_manifest))
            {
                auto change_destination = temporary / "changes" / file.relative_path;
                change_destination += ".json";
                copy_verified(file.change_manifest, change_destination, {});
            }
        }
        std::ofstream manifest(temporary / "mod-project.json", std::ios::trunc);
        manifest << manifest_json();
        manifest.close();
        publish_directory(temporary, staging_root);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(temporary, ignored);
        throw;
    }
    return report;
}

PackageResult ModProject::package(const std::filesystem::path& staging_root,
                                  const std::filesystem::path& archive_path,
                                  const PakOptions& options) const {
    if (archive_path.empty()) throw std::runtime_error("Archive output path is required");
    if (options.pakman_cli.empty()) throw std::runtime_error("pakman-cli path is required");
    if (options.type != "stored" && options.type != "compressed")
        throw std::runtime_error("PAK type must be stored or compressed");
    if (options.platform != "pc" && options.platform != "ps2" &&
        options.platform != "xbox" && options.platform != "ps2-prototype")
        throw std::runtime_error("PAK platform must be pc, ps2, xbox, or ps2-prototype");
    if (options.type == "compressed" && options.platform == "ps2-prototype")
        throw std::runtime_error("pakman-cli does not support compressed PS2 prototype archives");
    if (same_path(archive_path, options.pakman_cli))
        throw std::runtime_error("Archive output must not replace pakman-cli");
    if (same_path(archive_path, source_root) || below(archive_path, source_root) ||
        same_path(archive_path, staging_root) || below(archive_path, staging_root))
        throw std::runtime_error("Archive output must not be inside a source or staging tree");
    if (std::filesystem::exists(archive_path)) {
        if (!std::filesystem::is_regular_file(archive_path))
            throw std::runtime_error("Archive output exists and is not a regular file");
        if (!options.overwrite)
            throw std::runtime_error("Archive already exists; pass --overwrite to replace it");
    }

    const auto validation = build(staging_root);
    if (!validation.passed) throw std::runtime_error("Cannot package an invalid overlay");

    auto temporary = archive_path;
    temporary += ".packaging-" + timestamp();
    if (std::filesystem::exists(temporary))
        throw std::runtime_error("Temporary archive path already exists");
    try {
        run_process({process_argument(options.pakman_cli), "create",
                     process_argument(staging_root / "files"), "-o", process_argument(temporary),
                     "--type", options.type, "--platform", options.platform});
        if (!std::filesystem::is_regular_file(temporary))
            throw std::runtime_error("pakman-cli reported success without creating an archive");
        run_process({process_argument(options.pakman_cli), "verify", process_argument(temporary)});
        const auto archive_hash = file_hash(temporary);
        publish_file(temporary, archive_path, options.overwrite);

        PackageResult result;
        result.archive_path = archive_path;
        result.manifest_path = archive_path;
        result.manifest_path += ".package.json";
        result.archive_sha256 = archive_hash;
        if (std::filesystem::is_regular_file(options.pakman_cli))
            result.pakman_sha256 = file_hash(options.pakman_cli);
        std::ofstream manifest(result.manifest_path, std::ios::trunc);
        manifest << "{\n  \"format_version\":1,\n  \"archive\":\""
                 << json_escape(path_string(archive_path.filename())) << "\",\n  \"archive_sha256\":\""
                 << result.archive_sha256 << "\",\n  \"packer\":\"pakman-cli\",\n  \"packer_sha256\":\""
                 << result.pakman_sha256 << "\",\n  \"type\":\"" << options.type
                 << "\",\n  \"platform\":\"" << options.platform << "\"\n}\n";
        if (!manifest) throw std::runtime_error("Cannot write PAK package manifest");
        return result;
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

std::filesystem::path ModProject::resolve_overlay(const std::filesystem::path& staging,
                                                  const std::filesystem::path& relative) const {
    if (!safe_relative(relative)) throw std::runtime_error("Overlay lookup path is unsafe");
    const auto staged = staging / "files" / relative;
    return std::filesystem::is_regular_file(staged) ? staged : source_root / relative;
}

std::vector<ModConflict> ModProject::conflicts(const ModProject& first, const ModProject& second) {
    std::vector<ModConflict> result;
    for (const auto& a : first.files) for (const auto& b : second.files) {
        if (a.relative_path.generic_string() != b.relative_path.generic_string()) continue;
        std::vector<std::string> shared;
        for (const auto& target : a.semantic_targets)
            if (std::ranges::find(b.semantic_targets, target) != b.semantic_targets.end())
                shared.push_back(target);
        const bool semantic = !shared.empty();
        result.push_back({a.relative_path, std::move(shared),
                          semantic ? "Both mods change the same semantic target"
                                   : "Both mods replace the same file"});
    }
    return result;
}

DeploymentResult ModProject::deploy(const std::filesystem::path& staging,
                                    const std::filesystem::path& test_root,
                                    const bool dry_run) const {
    if (!std::filesystem::is_directory(test_root)) throw std::runtime_error("Test installation root must already exist");
    if (same_path(test_root, source_root) || same_path(test_root, workspace_root) ||
        same_path(test_root, staging) || below(test_root, source_root) ||
        below(source_root, test_root) || below(test_root, workspace_root) ||
        below(workspace_root, test_root) || below(test_root, staging) || below(staging, test_root))
        throw std::runtime_error("Deployment root aliases a protected source/project/staging tree");
    DeploymentResult result;
    result.dry_run = dry_run;
    result.backup_root = test_root / ".csf-mod-backups" / timestamp();
    result.manifest_path = result.backup_root / "deployment.state";
    for (const auto& file : files) {
        const auto source = staging / "files" / file.relative_path;
        if (!std::filesystem::is_regular_file(source) || file_hash(source) != file.output_sha256)
            throw std::runtime_error("Staging file is missing or invalid: " + path_string(file.relative_path));
        const auto target = test_root / file.relative_path;
        const bool existed = std::filesystem::is_regular_file(target);
        result.files.push_back({file.relative_path, existed ? file_hash(target) : std::string{},
                                file.output_sha256, existed});
    }
    if (dry_run) return result;
    std::filesystem::create_directories(result.backup_root);
    for (const auto& file : result.files) {
        const auto target = test_root / file.relative_path;
        if (file.previously_existed)
            copy_verified(target, result.backup_root / "files" / file.relative_path, file.before_sha256);
    }
    std::ofstream manifest(result.manifest_path, std::ios::trunc);
    manifest << "csf-deployment-1\n" << std::quoted(path_string(test_root)) << '\n';
    for (const auto& file : result.files)
        manifest << "file " << std::quoted(path_string(file.relative_path)) << ' '
                 << std::quoted(file.before_sha256) << ' ' << std::quoted(file.after_sha256) << ' '
                 << file.previously_existed << '\n';
    if (!manifest) throw std::runtime_error("Cannot write deployment manifest");
    manifest.close();
    for (const auto& file : result.files)
        copy_verified(staging / "files" / file.relative_path,
                      test_root / file.relative_path, file.after_sha256);
    return result;
}

bool ModProject::builtin_pak_support() noexcept {
#ifdef RWSMAN_HAVE_PAKMAN
    return true;
#else
    return false;
#endif
}

MissionPakResult ModProject::export_mission_pak(const std::filesystem::path& original_archive,
                                                const std::filesystem::path& output_archive,
                                                const MissionPakOptions& options) const {
    if (!std::filesystem::is_regular_file(original_archive))
        throw std::runtime_error("Original mission archive does not exist");
    if (output_archive.empty()) throw std::runtime_error("Archive output path is required");
    if (same_path(output_archive, original_archive))
        throw std::runtime_error("Refusing to overwrite the original mission archive");
    if (same_path(output_archive, source_root) || below(output_archive, source_root) ||
        below(output_archive, workspace_root))
        throw std::runtime_error("Archive output must not be inside the source or project tree");
    if (std::filesystem::exists(output_archive) && !options.overwrite)
        throw std::runtime_error("Archive already exists; pass --overwrite to replace it");
    const auto validation = validate();
    if (!validation.passed) throw std::runtime_error("Cannot export a project that fails validation");

    MissionPakResult result;
    result.archive_path = output_archive;
    result.original_sha256 = file_hash(original_archive);
    if (!output_archive.parent_path().empty())
        std::filesystem::create_directories(output_archive.parent_path());
#ifdef RWSMAN_HAVE_PAKMAN
    std::vector<pakman::Replacement> replacements;
    for (const auto& file : files)
        replacements.push_back({path_string(file.relative_path), file.authored_path});
    pakman::RebuildOptions rebuild;
    rebuild.overwrite = options.overwrite;
    const auto rebuilt = pakman::rebuild_archive(original_archive, replacements, output_archive, rebuild);
    result.replaced = rebuilt.replaced;
    result.added = rebuilt.added;
    result.copied = rebuilt.copied;
    result.packer = "pakman_core (built in)";
#else
    auto temporary = output_archive;
    temporary += ".packaging-" + timestamp();
    std::vector<std::string> arguments{process_argument(options.pakman_cli), "rebuild",
                                       process_argument(original_archive), "-o",
                                       process_argument(temporary)};
    for (const auto& file : files) {
        arguments.emplace_back("--replace");
        arguments.push_back(path_string(file.relative_path) + "=" + process_argument(file.authored_path));
    }
    try {
        run_process(arguments);
        run_process({process_argument(options.pakman_cli), "verify", process_argument(temporary)});
        publish_file(temporary, output_archive, options.overwrite);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
    result.replaced = files.size();
    result.packer = "pakman-cli";
#endif
    result.archive_sha256 = file_hash(output_archive);
    result.manifest_path = output_archive;
    result.manifest_path += ".package.json";
    std::ofstream manifest(result.manifest_path, std::ios::trunc);
    manifest << "{\n  \"format_version\":1,\n  \"kind\":\"mission-replacement\",\n  \"archive\":\""
             << json_escape(path_string(output_archive.filename())) << "\",\n  \"archive_sha256\":\""
             << result.archive_sha256 << "\",\n  \"original\":\""
             << json_escape(path_string(original_archive.filename())) << "\",\n  \"original_sha256\":\""
             << result.original_sha256 << "\",\n  \"packer\":\"" << json_escape(result.packer)
             << "\",\n  \"files\":[";
    for (std::size_t i = 0; i < files.size(); ++i)
        manifest << (i ? "," : "") << "\n    {\"path\":\"" << json_escape(path_string(files[i].relative_path))
                 << "\",\"sha256\":\"" << files[i].output_sha256 << "\"}";
    manifest << (files.empty() ? "" : "\n  ") << "]\n}\n";
    if (!manifest) throw std::runtime_error("Cannot write PAK package manifest");
    return result;
}

DeploymentResult ModProject::deploy_package(
    const std::filesystem::path& archive_path, const std::filesystem::path& test_root,
    const std::filesystem::path& game_relative_archive_path, const PakOptions& options,
    const bool dry_run) const {
    if (!safe_relative(game_relative_archive_path))
        throw std::runtime_error("Deployed PAK path must be safe and game-relative");
    if (!std::filesystem::is_regular_file(archive_path))
        throw std::runtime_error("Packaged archive does not exist");
    if (!std::filesystem::is_directory(test_root))
        throw std::runtime_error("Test installation root must already exist");
    if (same_path(test_root, source_root) || same_path(test_root, workspace_root) ||
        below(test_root, source_root) || below(source_root, test_root) ||
        below(test_root, workspace_root) || below(workspace_root, test_root) ||
        below(archive_path, test_root))
        throw std::runtime_error("Deployment root aliases a protected source/project/package tree");

    const auto archive_hash = file_hash(archive_path);
    if (archive_hash != packaged_archive_hash(archive_path))
        throw std::runtime_error("Packaged archive hash differs from its package manifest");
    verify_archive(archive_path, options.pakman_cli);
    const auto target = test_root / game_relative_archive_path;
    const bool existed = std::filesystem::is_regular_file(target);
    DeploymentResult result;
    result.dry_run = dry_run;
    result.backup_root = test_root / ".csf-mod-backups" / timestamp();
    result.manifest_path = result.backup_root / "deployment.state";
    result.files.push_back({game_relative_archive_path,
                            existed ? file_hash(target) : std::string{}, archive_hash, existed});
    if (dry_run) return result;

    std::filesystem::create_directories(result.backup_root);
    if (existed)
        copy_verified(target, result.backup_root / "files" / game_relative_archive_path,
                      result.files.front().before_sha256);
    std::ofstream manifest(result.manifest_path, std::ios::trunc);
    manifest << "csf-deployment-1\n" << std::quoted(path_string(test_root)) << '\n'
             << "file " << std::quoted(path_string(game_relative_archive_path)) << ' '
             << std::quoted(result.files.front().before_sha256) << ' '
             << std::quoted(archive_hash) << ' ' << existed << '\n';
    if (!manifest) throw std::runtime_error("Cannot write deployment manifest");
    manifest.close();
    copy_verified(archive_path, target, archive_hash);
    return result;
}

void ModProject::rollback(const std::filesystem::path& deployment_manifest) {
    std::ifstream input(deployment_manifest);
    std::string magic, target_text;
    std::getline(input, magic);
    input >> std::quoted(target_text);
    if (!input || magic != "csf-deployment-1") throw std::runtime_error("Unsupported deployment manifest");
    const auto target_root = path_from_utf8(target_text);
    const auto backup_root = deployment_manifest.parent_path();
    std::string record;
    while (input >> record) {
        std::string relative_text, before, after;
        bool existed{};
        input >> std::quoted(relative_text) >> std::quoted(before) >> std::quoted(after) >> existed;
        const auto relative = path_from_utf8(relative_text);
        if (record != "file" || !safe_relative(relative)) throw std::runtime_error("Invalid rollback record");
        const auto target = target_root / relative;
        if (!std::filesystem::is_regular_file(target)) {
            if (!existed) continue; // A not-yet-written new file in a partial deployment.
            throw std::runtime_error("Refusing rollback because a deployed target is missing: " + path_string(relative));
        }
        const auto current = file_hash(target);
        if (existed && current == before) continue; // Not reached by an interrupted deployment.
        if (current != after)
            throw std::runtime_error("Refusing rollback because deployed file was changed: " + path_string(relative));
        if (existed) copy_verified(backup_root / "files" / relative, target, before);
        else std::filesystem::remove(target);
    }
}

} // namespace csf
