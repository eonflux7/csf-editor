#pragma once

#include "app_state.hpp"
#include "rwsman/ui_script.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace rwsman {

class UiAutomation;

struct UiScriptOptions {
    std::string name;                         // shown in messages ("patrol.uiscript")
    std::filesystem::path base_directory;     // resolves relative paths in open steps
    std::filesystem::path output_directory;   // screenshots, dumps and failure reports
    std::filesystem::path golden_directory;   // `compare` references
    bool overwrite{};                         // replace existing output files
    bool update_goldens{};                    // `compare` writes the reference instead
    int channel_tolerance{12};                // per channel, 0-255
    double changed_percent{0.1};              // default budget for `compare`
};

// Runs a UI script one step per frame (docs/plans/editor-ux-redesign.md, T2):
// every step waits until the app is idle (no mission load, authoring job or
// file dialog), widget steps wait up to a second of frames for their target
// to appear, and the first failure ends the run with a report on stderr and
// a state dump next to the outputs.
class UiScriptRunner {
    enum class Capture : std::uint8_t { none, screenshot, compare };

public:
    UiScriptRunner(std::vector<UiScriptStep> steps, UiScriptOptions options);

    // Between the platform backend's NewFrame and ImGui::NewFrame.
    void before_frame(AppState& state, UiAutomation& automation);
    // After the frame is rendered, before the buffers are swapped.
    void after_render(AppState& state, UiAutomation& automation);

    [[nodiscard]] bool done() const noexcept {
        return failed_ || skipped_ || (next_ >= steps_.size() && capture_ == Capture::none);
    }
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    // A `require` step found its variable unset: the run ends, not failed.
    [[nodiscard]] bool skipped() const noexcept { return skipped_; }

private:
    bool busy(const AppState& state) const;
    void run_step(AppState& state, UiAutomation& automation, const UiScriptStep& step);
    void fail(const AppState& state, const std::string& message);
    std::filesystem::path output_path(const std::string& name) const;
    void capture(AppState& state);

    std::vector<UiScriptStep> steps_;
    UiScriptOptions options_;
    std::size_t next_{};
    int idle_frames_{}, wait_frames_{}, retry_frames_{};
    bool failed_{}, skipped_{};
    Capture capture_{Capture::none};
    std::string capture_name_;
    StateSnapshot remembered_;
    double capture_budget_{};
    const UiScriptStep* current_{};
    std::optional<Workspace> last_workspace_;
    // A `monkey` step in progress: one random action per idle frame (T10).
    struct Monkey {
        std::mt19937 random;
        int remaining{};
        std::vector<std::string> commands;  // the command IDs it may run
        std::vector<std::string> recent;    // the last actions, for a failure report
    };
    std::optional<Monkey> monkey_;
    void monkey_action(AppState& state, UiAutomation& automation, Monkey& monkey);
};

} // namespace rwsman
