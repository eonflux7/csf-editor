#pragma once

#include "app_state.hpp"
#include "mission_overlays.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace rwsman {

// Everything a mission load produces. It holds CPU data only: parsing and
// resolution run on a worker thread, and every OpenGL upload happens later on
// the main thread when the preview draws.
struct MissionLoadResult {
    std::filesystem::path input;
    MissionState mission;
    std::unique_ptr<rws::Document> visual;
    std::unique_ptr<rws::Document> collision;
    std::filesystem::path collision_path;
    std::vector<GeometryPreview::MissionActorModel> actor_models;
    MissionOverlays overlays;
    double seconds{};
};

// Resolves and loads the preview model of every actor that has exactly one
// visual model, with its first resolvable third-person weapon. Prototypes are
// shared through `cache`; `cancelled` is polled between actors.
[[nodiscard]] std::vector<GeometryPreview::MissionActorModel>
build_actor_models(const csf::MissionScene& scene, const std::vector<csf::ActorAssociation>& associations,
                   const csf::WeaponDatabase& weapons, const csf::ResourceIndex& index,
                   ActorModelCache& cache, const std::function<void(float)>& progress = {},
                   const std::function<bool()>& cancelled = {});

struct MissionLoadFailure {
    std::filesystem::path input;
    std::string stage;
    std::string message;
    bool cancelled{};
};

// Progress snapshot for the overlay.
struct LoadProgress {
    bool active{};
    std::string stage;      // Human-readable current stage.
    int stage_index{};      // 1-based.
    int stage_count{};
    float fraction{};       // 0..1 across all stages.
    std::filesystem::path input;
    bool cancel_requested{};
};

// Runs one mission load at a time on a worker thread. The previous mission
// stays untouched until the caller commits a successful result, so a failed or
// cancelled load never leaves the app half-loaded.
class MissionLoader {
public:
    MissionLoader() = default;
    ~MissionLoader();
    MissionLoader(const MissionLoader&) = delete;
    MissionLoader& operator=(const MissionLoader&) = delete;

    // Returns false when a load is already running. With `project`, the mod
    // project's authored files replace their source copies.
    bool start(const std::filesystem::path& scene, std::filesystem::path debug_log_directory,
               std::optional<std::filesystem::path> project = std::nullopt);
    void cancel();
    [[nodiscard]] LoadProgress progress() const;
    [[nodiscard]] bool busy() const noexcept { return running_.load(); }

    using Outcome = std::variant<std::monostate, MissionLoadResult, MissionLoadFailure>;
    // Takes the finished outcome, or monostate while running or idle.
    Outcome poll();

private:
    void run(std::filesystem::path scene, std::filesystem::path debug_log_directory,
             std::optional<std::filesystem::path> project);
    void set_stage(int index, const char* text);

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> finished_{false};
    mutable std::mutex mutex_;
    std::string stage_;
    int stage_index_{};
    std::atomic<float> fraction_{};
    std::filesystem::path input_;
    Outcome outcome_;
};

} // namespace rwsman
