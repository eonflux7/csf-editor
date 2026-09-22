#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace csf {

struct ModFile {
    std::filesystem::path relative_path;
    std::filesystem::path authored_path; // local-only, omitted from distributable manifest
    std::filesystem::path change_manifest;
    std::string source_sha256;
    std::string output_sha256;
    std::vector<std::string> semantic_targets;
};

struct ModConflict {
    std::filesystem::path relative_path;
    std::vector<std::string> semantic_targets;
    std::string reason;
};

struct OverlayDiagnostic {
    enum class Kind { introduced, baseline, stale_source, invalid_output };
    Kind kind{Kind::introduced};
    std::filesystem::path relative_path;
    std::string message;
};

struct OverlayReport {
    bool passed{};
    std::vector<OverlayDiagnostic> diagnostics;
};

struct DeploymentFile {
    std::filesystem::path relative_path;
    std::string before_sha256;
    std::string after_sha256;
    bool previously_existed{};
};

struct DeploymentResult {
    bool dry_run{};
    std::filesystem::path manifest_path;
    std::filesystem::path backup_root;
    std::vector<DeploymentFile> files;
};

struct PakOptions {
    std::filesystem::path pakman_cli{"pakman-cli"};
    std::string type{"stored"};
    std::string platform{"pc"};
    bool overwrite{};
};

struct MissionPakOptions {
    // Used only when this build has no built-in pak-man (see builtin_pak_support).
    std::filesystem::path pakman_cli{"pakman-cli"};
    bool overwrite{};
};

struct MissionPakResult {
    std::filesystem::path archive_path;
    std::filesystem::path manifest_path;
    std::string archive_sha256;
    std::string original_sha256;
    std::size_t replaced{}, added{}, copied{};
    std::string packer;
};

struct PackageResult {
    std::filesystem::path archive_path;
    std::filesystem::path manifest_path;
    std::string archive_sha256;
    std::string pakman_sha256;
};

class ModProject {
public:
    static constexpr unsigned format_version = 1;

    std::string name;
    std::string game{"commandos-strike-force"};
    std::string tool_version;
    std::filesystem::path workspace_root;
    std::filesystem::path source_root; // local config only
    std::vector<ModFile> files;

    [[nodiscard]] static ModProject create(std::filesystem::path workspace,
                                           std::filesystem::path source_root, std::string name,
                                           std::string tool_version = {});
    [[nodiscard]] static ModProject load(const std::filesystem::path& workspace);
    void save() const;
    void add_file(ModFile file);

    [[nodiscard]] std::string manifest_json() const;
    [[nodiscard]] std::string local_config_json() const;
    [[nodiscard]] OverlayReport validate() const;
    [[nodiscard]] OverlayReport build(const std::filesystem::path& staging_root) const;
    [[nodiscard]] PackageResult package(const std::filesystem::path& staging_root,
                                        const std::filesystem::path& archive_path,
                                        const PakOptions& options = {}) const;
    // Rebuilds a complete replacement for a shipped mission archive (for
    // example maps/Ransom.pak): the original's header, entry order, path bytes,
    // duplicates, timestamps and unchanged compressed records are kept, and only
    // the project's files are replaced or appended. The result is verified and
    // written to a new path with an adjacent .package.json.
    [[nodiscard]] MissionPakResult export_mission_pak(const std::filesystem::path& original_archive,
                                                      const std::filesystem::path& output_archive,
                                                      const MissionPakOptions& options = {}) const;
    // True when pak-man's library is linked in; otherwise pakman-cli is run.
    [[nodiscard]] static bool builtin_pak_support() noexcept;
    [[nodiscard]] std::filesystem::path resolve_overlay(
        const std::filesystem::path& staging_root,
        const std::filesystem::path& relative_path) const;

    [[nodiscard]] static std::vector<ModConflict> conflicts(const ModProject& first,
                                                            const ModProject& second);
    [[nodiscard]] DeploymentResult deploy(const std::filesystem::path& staging_root,
                                          const std::filesystem::path& test_root,
                                          bool dry_run) const;
    [[nodiscard]] DeploymentResult deploy_package(
        const std::filesystem::path& archive_path,
        const std::filesystem::path& test_root,
        const std::filesystem::path& game_relative_archive_path,
        const PakOptions& options, bool dry_run) const;
    static void rollback(const std::filesystem::path& deployment_manifest);
};

[[nodiscard]] const char* overlay_diagnostic_kind_name(OverlayDiagnostic::Kind kind) noexcept;

} // namespace csf
