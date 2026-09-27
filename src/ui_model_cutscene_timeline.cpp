#include "rwsman/cutscene_timeline.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rwsman {
namespace {

csf::Vec3 parse_vec3(const std::string& text) {
    const auto points = csf::parse_op_points(text);
    if (points.size() != 1) throw std::invalid_argument("expected x,y,z, not '" + text + "'");
    return points.front().position;
}

float parse_seconds(const std::string& text) {
    try {
        std::size_t used{};
        const float value = std::stof(text, &used);
        if (used == text.size()) return value;
    } catch (const std::exception&) {
    }
    throw std::invalid_argument("expected seconds=<number>, not '" + text + "'");
}

bool same(const csf::Vec3 a, const csf::Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

TimelineShot shot_of(const csf::OpLine& line) {
    TimelineShot shot;
    shot.camera = parse_vec3(line.get("camera"));
    shot.end = line.find("end") ? parse_vec3(line.get("end")) : shot.camera;
    shot.target = parse_vec3(line.get("target"));
    shot.seconds = parse_seconds(line.get("seconds", "4"));
    return shot;
}

std::optional<std::int32_t> first_id(const csf::OpLine& line, const std::string_view key) {
    try {
        if (const auto* value = line.find(key)) return std::stoi(*value);
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

} // namespace

float timeline_length(const std::span<const TimelineShot> shots) {
    float total = 0.0F;
    for (const auto& shot : shots) total += shot.seconds;
    return total;
}

float shot_start(const std::span<const TimelineShot> shots, const std::size_t index) {
    return timeline_length(shots.first(std::min(index, shots.size())));
}

std::optional<TimelinePosition> timeline_position(const std::span<const TimelineShot> shots, float seconds) {
    if (shots.empty()) return std::nullopt;
    seconds = std::max(seconds, 0.0F);
    for (std::size_t i = 0; i < shots.size(); ++i) {
        const float length = std::max(shots[i].seconds, 0.001F);
        if (seconds < length || i + 1 == shots.size()) return TimelinePosition{i, std::min(seconds / length, 1.0F)};
        seconds -= length;
    }
    return std::nullopt;
}

ShotView shot_view(const TimelineShot& shot, const float fraction) {
    const float t = std::clamp(fraction, 0.0F, 1.0F);
    return {{shot.camera.x + t * (shot.end.x - shot.camera.x), shot.camera.y + t * (shot.end.y - shot.camera.y),
             shot.camera.z + t * (shot.end.z - shot.camera.z)},
            shot.target};
}

float snap_shot_seconds(const float seconds) {
    return std::clamp(std::round(seconds * 10.0F) / 10.0F, min_shot_seconds, max_shot_seconds);
}

std::vector<TimelineShot> timeline_shots(const std::span<const std::string> lines) {
    std::vector<TimelineShot> shots;
    for (const auto& text : lines) {
        const auto line = csf::parse_op_line(text);
        if (line.op != "shot") continue;
        auto shot = shot_of(line);
        shot.line = text;
        shots.push_back(std::move(shot));
    }
    return shots;
}

std::vector<std::string> lines_with_shots(const std::span<const std::string> lines,
                                          const std::span<const TimelineShot> shots) {
    std::vector<std::string> result;
    bool intro = false;
    for (const auto& text : lines) {
        const auto line = csf::parse_op_line(text);
        if (line.op == "shot") continue;
        if (line.op == "intro" && !intro) {
            intro = true;
            for (const auto& shot : shots) {
                auto written = shot.line.empty() ? csf::OpLine{"shot", {}} : csf::parse_op_line(shot.line);
                if (!shot.line.empty()) {
                    const auto before = shot_of(written);
                    const bool moved = !same(before.camera, shot.camera) || !same(before.end, shot.end) ||
                                       !same(before.target, shot.target);
                    if (!moved && before.seconds == shot.seconds) {
                        result.push_back(shot.line);
                        continue;
                    }
                    if (moved) {
                        written.erase("aim");
                        written.erase("heading");
                    }
                    written.erase("speed");
                }
                written.set("camera", csf::op_vec3(shot.camera));
                written.set("end", csf::op_vec3(shot.end));
                written.set("target", csf::op_vec3(shot.target));
                written.set("seconds", csf::op_number(shot.seconds));
                result.push_back(csf::format_op_line(written));
            }
        }
        result.push_back(text);
    }
    if (!intro) throw std::invalid_argument("the lines have no intro line");
    return result;
}

std::optional<ShotRecordRole> shot_record_role(const std::span<const std::string> lines,
                                               const csf::MissionRecordId::Type type, const std::int32_t id,
                                               const std::int32_t point) {
    std::size_t count = 0;
    std::optional<csf::OpLine> intro;
    for (const auto& text : lines) try {
            auto line = csf::parse_op_line(text);
            if (line.op == "shot") ++count;
            if (line.op == "intro") intro = std::move(line);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    if (!intro || count == 0) return std::nullopt;
    const auto within = [&](const std::optional<std::int32_t> first, const std::int32_t per_shot)
        -> std::optional<std::pair<std::size_t, std::int32_t>> {
        if (!first || id < *first) return std::nullopt;
        const auto offset = id - *first;
        const auto shot = static_cast<std::size_t>(offset / per_shot);
        if (shot >= count) return std::nullopt;
        return std::pair{shot, offset % per_shot};
    };
    using Type = csf::MissionRecordId::Type;
    switch (type) {
    case Type::dummy:
        if (const auto at = within(first_id(*intro, "dummy"), 1)) return ShotRecordRole{at->first, ShotPart::camera};
        break;
    case Type::actor:
        if (const auto at = within(first_id(*intro, "actor"), 2))
            return ShotRecordRole{at->first, at->second == 0 ? ShotPart::camera : ShotPart::target};
        break;
    case Type::navigation_group:
        if (const auto at = within(first_id(*intro, "group"), 1); at && (point == 1 || point == 2))
            return ShotRecordRole{at->first, point == 1 ? ShotPart::camera : ShotPart::end};
        break;
    default:
        break;
    }
    return std::nullopt;
}

std::optional<ShotRecord> shot_record(const std::span<const std::string> lines, const std::size_t shot,
                                      const ShotPart part) {
    std::size_t count = 0;
    std::optional<csf::OpLine> intro;
    for (const auto& text : lines) try {
            auto line = csf::parse_op_line(text);
            if (line.op == "shot") ++count;
            if (line.op == "intro") intro = std::move(line);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    if (!intro || shot >= count) return std::nullopt;
    const auto k = static_cast<std::int32_t>(shot);
    using Type = csf::MissionRecordId::Type;
    if (part == ShotPart::end) {
        if (const auto group = first_id(*intro, "group")) return ShotRecord{Type::navigation_group, *group + k, 2};
    } else if (const auto actor = first_id(*intro, "actor")) {
        return ShotRecord{Type::actor, *actor + 2 * k + (part == ShotPart::target ? 1 : 0), 0};
    }
    return std::nullopt;
}

csf::Vec3 shot_part_position(const TimelineShot& shot, const ShotPart part) {
    switch (part) {
    case ShotPart::camera: return shot.camera;
    case ShotPart::end: return shot.end;
    case ShotPart::target: return shot.target;
    }
    return shot.camera;
}

void set_shot_part(TimelineShot& shot, const ShotPart part, const csf::Vec3 position) {
    switch (part) {
    case ShotPart::camera: shot.camera = position; break;
    case ShotPart::end: shot.end = position; break;
    case ShotPart::target: shot.target = position; break;
    }
}

} // namespace rwsman
