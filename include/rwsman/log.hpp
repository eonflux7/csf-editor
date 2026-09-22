#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rwsman {

enum class LogLevel : std::uint8_t { info, ok, warn, error };

[[nodiscard]] const char* log_level_name(LogLevel level) noexcept;

struct LogEntry {
    std::chrono::system_clock::time_point time;
    LogLevel level{LogLevel::info};
    std::string message;
};

// Bounded, timestamped operation log. Thread-safe so a background load can
// report progress; readers take a snapshot.
class LogBuffer {
public:
    explicit LogBuffer(std::size_t capacity = 2000) : capacity_(capacity ? capacity : 1) {}

    void push(LogLevel level, std::string message);
    void push(std::chrono::system_clock::time_point time, LogLevel level, std::string message);
    [[nodiscard]] std::vector<LogEntry> snapshot() const;
    [[nodiscard]] std::optional<LogEntry> latest() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t count(LogLevel level) const;
    // Increases on every push or clear, so a view can tell when to refresh.
    [[nodiscard]] std::uint64_t revision() const;
    void clear();
    // One "HH:MM:SS level message" line per entry, oldest first.
    [[nodiscard]] std::string to_text() const;

private:
    mutable std::mutex mutex_;
    std::deque<LogEntry> entries_;
    std::size_t capacity_;
    std::uint64_t revision_{};
};

[[nodiscard]] std::string format_log_time(std::chrono::system_clock::time_point time);
[[nodiscard]] std::string format_log_line(const LogEntry& entry);

} // namespace rwsman
