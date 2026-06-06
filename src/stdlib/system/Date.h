#pragma once
// SukiCode Date 类型 - 日期和时间
// Date and time operations.

#include <ctime>
#include <string>
#include <chrono>
#include <thread>
#include <functional>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <memory>

namespace suki::stdlib {

class Date {
public:
    // 获取当前时间 / Get current time
    static Date now() {
        Date d;
        d.timePoint_ = std::chrono::system_clock::now();
        return d;
    }

    // 从时间戳创建 / Create from timestamp
    static Date fromTimestamp(double seconds) {
        Date d;
        d.timePoint_ = std::chrono::system_clock::from_time_t(static_cast<time_t>(seconds));
        return d;
    }

    // 时间戳（秒）/ Timestamp in seconds
    double timestamp() const {
        auto epoch = timePoint_.time_since_epoch();
        return std::chrono::duration<double>(epoch).count();
    }

    // 组件 / Components
    int year() const { return toTm().tm_year + 1900; }
    int month() const { return toTm().tm_mon + 1; }
    int day() const { return toTm().tm_mday; }
    int hour() const { return toTm().tm_hour; }
    int minute() const { return toTm().tm_min; }
    int second() const { return toTm().tm_sec; }
    int weekday() const { return toTm().tm_wday; } // 0=Sunday

    // 格式化 / Formatting
    std::string formatted(const std::string& fmt = "%Y-%m-%d %H:%M:%S") const {
        auto tm = toTm();
        char buf[128];
        strftime(buf, sizeof(buf), fmt.c_str(), &tm);
        return std::string(buf);
    }

    // 时间差 / Time difference
    double timeIntervalSince(const Date& other) const {
        return timestamp() - other.timestamp();
    }

    // 日期算术 / Date arithmetic
    Date operator+(const TimeInterval& interval) const {
        Date d;
        d.timePoint_ = timePoint_ + std::chrono::milliseconds((int)interval.milliseconds());
        return d;
    }

    Date operator-(const TimeInterval& interval) const {
        Date d;
        d.timePoint_ = timePoint_ - std::chrono::milliseconds((int)interval.milliseconds());
        return d;
    }

    TimeInterval operator-(const Date& other) const {
        return TimeInterval::seconds(timeIntervalSince(other));
    }

    // ISO 8601 格式 / ISO 8601 format
    std::string iso8601() const {
        return formatted("%Y-%m-%dT%H:%M:%SZ");
    }

    // 从字符串解析 / Parse from string (basic: "YYYY-MM-DD HH:MM:SS")
    static Date fromString(const std::string& str, const std::string& fmt = "%Y-%m-%d %H:%M:%S") {
        Date d;
        std::tm tm = {};
#ifdef _WIN32
        std::istringstream ss(str);
        ss >> std::get_time(&tm, fmt.c_str());
#else
        strptime(str.c_str(), fmt.c_str(), &tm);
#endif
        auto time = std::mktime(&tm);
        d.timePoint_ = std::chrono::system_clock::from_time_t(time);
        return d;
    }

    // 比较 / Comparison
    bool operator==(const Date& other) const { return timePoint_ == other.timePoint_; }
    bool operator!=(const Date& other) const { return timePoint_ != other.timePoint_; }
    bool operator<(const Date& other) const { return timePoint_ < other.timePoint_; }
    bool operator>(const Date& other) const { return timePoint_ > other.timePoint_; }
    bool operator<=(const Date& other) const { return timePoint_ <= other.timePoint_; }
    bool operator>=(const Date& other) const { return timePoint_ >= other.timePoint_; }

private:
    std::chrono::system_clock::time_point timePoint_;
    std::tm toTm() const {
        auto time = std::chrono::system_clock::to_time_t(timePoint_);
        std::tm result;
#ifdef _WIN32
        localtime_s(&result, &time);
#else
        localtime_r(&time, &result);
#endif
        return result;
    }
};

// 时间间隔 / Time interval
class TimeInterval {
public:
    static TimeInterval seconds(double s) { return TimeInterval(s); }
    static TimeInterval milliseconds(double ms) { return TimeInterval(ms / 1000.0); }
    static TimeInterval minutes(double m) { return TimeInterval(m * 60.0); }
    static TimeInterval hours(double h) { return TimeInterval(h * 3600.0); }
    static TimeInterval days(double d) { return TimeInterval(d * 86400.0); }

    double seconds() const { return seconds_; }
    double milliseconds() const { return seconds_ * 1000.0; }

    TimeInterval operator+(const TimeInterval& other) const { return TimeInterval(seconds_ + other.seconds_); }
    TimeInterval operator-(const TimeInterval& other) const { return TimeInterval(seconds_ - other.seconds_); }
    TimeInterval operator*(double factor) const { return TimeInterval(seconds_ * factor); }
    TimeInterval operator/(double factor) const { return TimeInterval(seconds_ / factor); }

    bool operator==(const TimeInterval& other) const { return seconds_ == other.seconds_; }
    bool operator!=(const TimeInterval& other) const { return seconds_ != other.seconds_; }
    bool operator<(const TimeInterval& other) const { return seconds_ < other.seconds_; }
    bool operator>(const TimeInterval& other) const { return seconds_ > other.seconds_; }
    bool operator<=(const TimeInterval& other) const { return seconds_ <= other.seconds_; }
    bool operator>=(const TimeInterval& other) const { return seconds_ >= other.seconds_; }

private:
    explicit TimeInterval(double s) : seconds_(s) {}
    double seconds_;
};

// 定时器句柄 / Timer handle (for cancellation)
class TimerHandle {
public:
    TimerHandle() : cancelled_(std::make_shared<std::atomic<bool>>(false)) {}
    void cancel() { *cancelled_ = true; }
    bool isCancelled() const { return *cancelled_; }
    std::shared_ptr<std::atomic<bool>> token() const { return cancelled_; }
private:
    std::shared_ptr<std::atomic<bool>> cancelled_;
};

// 定时器 / Timer
class Timer {
public:
    using Callback = std::function<void()>;

    // 一次性定时器 / One-shot timer
    static TimerHandle scheduleAfter(TimeInterval delay, Callback callback) {
        TimerHandle handle;
        auto token = handle.token();
        std::thread([delay, callback, token]() {
            std::this_thread::sleep_for(std::chrono::milliseconds((int)delay.milliseconds()));
            if (!*token) callback();
        }).detach();
        return handle;
    }

    // 重复定时器 / Repeating timer
    static TimerHandle scheduleRepeating(TimeInterval interval, Callback callback) {
        TimerHandle handle;
        auto token = handle.token();
        std::thread([interval, callback, token]() {
            while (!*token) {
                std::this_thread::sleep_for(std::chrono::milliseconds((int)interval.milliseconds()));
                if (!*token) callback();
            }
        }).detach();
        return handle;
    }
};

} // namespace suki::stdlib
