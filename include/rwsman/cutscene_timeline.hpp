#pragma once

#include "csf/mission_edit.hpp"
#include "csf/mission_ops.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The intro cutscene as a timeline (docs/plans/editor-ux-redesign.md, E11):
// the intro component's shot lines as shots with durations, the camera at a
// time, which shot and part a record of the component is, and the lines back
// after an edit. The Timeline panel and the viewport draw it.
namespace rwsman {

// A travelling shot: the camera moves from `camera` to `end` at constant speed
// in `seconds`, keeping `target` in view.
struct TimelineShot {
    csf::Vec3 camera, end, target;
    float seconds{4.0F};
    std::string line;  // the shot line it was read from; empty for a new shot
};

inline constexpr float min_shot_seconds = 0.5F;
inline constexpr float max_shot_seconds = 60.0F;

[[nodiscard]] float timeline_length(std::span<const TimelineShot> shots);
// When shot `index` starts, in seconds from the start of the cutscene.
[[nodiscard]] float shot_start(std::span<const TimelineShot> shots, std::size_t index);

struct TimelinePosition {
    std::size_t shot{};
    float fraction{};  // 0 at the shot's start, 1 at its end
};
// The shot playing at `seconds` (clamped to the timeline); at a cut, the one
// that starts there. Nothing without shots.
[[nodiscard]] std::optional<TimelinePosition> timeline_position(std::span<const TimelineShot> shots, float seconds);

struct ShotView {
    csf::Vec3 eye, target;
};
[[nodiscard]] ShotView shot_view(const TimelineShot& shot, float fraction);

// A duration as the timeline sets it: tenths of a second, within the bounds.
[[nodiscard]] float snap_shot_seconds(float seconds);

// The shots of the intro component's lines, in order. Throws
// std::invalid_argument on a malformed line.
[[nodiscard]] std::vector<TimelineShot> timeline_shots(std::span<const std::string> lines);
// The component's lines with these shots in place of its shot lines, before
// its intro line. A shot whose line still says the same is kept verbatim (the
// exact aim, heading and speed of hello world's shots survive); a changed one
// drops the values that follow from what changed (aim and heading from the
// positions, speed from them and the duration). Throws std::invalid_argument
// on a malformed line or without an intro line.
[[nodiscard]] std::vector<std::string> lines_with_shots(std::span<const std::string> lines,
                                                        std::span<const TimelineShot> shots);

// The part of a shot a record of the intro is: the camera actor, its dummy and
// its path's first point are the camera, the path's second point the end, the
// target actor the target. From the intro line's first IDs (each shot takes
// the next dummy and group and two actors).
enum class ShotPart : unsigned char { camera, end, target };
struct ShotRecordRole {
    std::size_t shot{};
    ShotPart part{};
};
// `point` is the navigation point's ID within its group (1 or 2).
[[nodiscard]] std::optional<ShotRecordRole> shot_record_role(std::span<const std::string> lines,
                                                             csf::MissionRecordId::Type type, std::int32_t id,
                                                             std::int32_t point = 0);
// The record that stands for a shot's part: the camera actor, the path's
// second point, the target actor.
struct ShotRecord {
    csf::MissionRecordId::Type type{};
    std::int32_t id{}, point{};  // `point` for a path point
};
[[nodiscard]] std::optional<ShotRecord> shot_record(std::span<const std::string> lines, std::size_t shot,
                                                    ShotPart part);
[[nodiscard]] csf::Vec3 shot_part_position(const TimelineShot& shot, ShotPart part);
void set_shot_part(TimelineShot& shot, ShotPart part, csf::Vec3 position);

} // namespace rwsman
