#include "rwsman/log.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace rwsman {

const char* log_level_name(const LogLevel level) noexcept {
    switch (level) {
    case LogLevel::info:
        return "info";
    case LogLevel::ok:
        return "ok";
    case LogLevel::warn:
        return "warn";
    case LogLevel::error:
        return "error";
    }
    return "info";
}

void LogBuffer::push(const LogLevel level, std::string message) {
    push(std::chrono::system_clock::now(), level, std::move(message));
}

void LogBuffer::push(const std::chrono::system_clock::time_point time, const LogLevel level,
                     std::string message) {
    const std::lock_guard lock(mutex_);
    entries_.push_back({time, level, std::move(message)});
    while (entries_.size() > capacity_) entries_.pop_front();
    ++revision_;
}

std::vector<LogEntry> LogBuffer::snapshot() const {
    const std::lock_guard lock(mutex_);
    return {entries_.begin(), entries_.end()};
}

std::optional<LogEntry> LogBuffer::latest() const {
    const std::lock_guard lock(mutex_);
    if (entries_.empty()) return std::nullopt;
    return entries_.back();
}

std::size_t LogBuffer::size() const {
    const std::lock_guard lock(mutex_);
    return entries_.size();
}

std::size_t LogBuffer::count(const LogLevel level) const {
    const std::lock_guard lock(mutex_);
    return static_cast<std::size_t>(std::ranges::count(entries_, level, &LogEntry::level));
}

std::uint64_t LogBuffer::revision() const {
    const std::lock_guard lock(mutex_);
    return revision_;
}

void LogBuffer::clear() {
    const std::lock_guard lock(mutex_);
    entries_.clear();
    ++revision_;
}

std::string format_log_time(const std::chrono::system_clock::time_point time) {
    const auto seconds = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", local.tm_hour, local.tm_min,
                  local.tm_sec);
    return buffer;
}

std::string format_log_line(const LogEntry& entry) {
    std::string level = log_level_name(entry.level);
    level.resize(5, ' ');
    return format_log_time(entry.time) + ' ' + level + ' ' + entry.message;
}

std::string LogBuffer::to_text() const {
    std::string text;
    for (const auto& entry : snapshot()) {
        text += format_log_line(entry);
        text += '\n';
    }
    return text;
}

} // namespace rwsman
