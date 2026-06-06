#pragma once
// SukiCode I18n 模块 - 国际化支持
// Internationalization support: localization, number/date/currency formatting.

#include <string>
#include <unordered_map>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <chrono>
#include <functional>

namespace suki::i18n {

// 本地化字符串 / Localized string
class LocalizedString {
public:
    // 注册翻译 / Register translation
    static void registerTranslation(const std::string& locale, const std::string& key,
                                     const std::string& value) {
        translations_[locale][key] = value;
    }

    // 设置当前区域 / Set current locale
    static void setLocale(const std::string& locale) {
        currentLocale_ = locale;
    }

    // 获取当前区域 / Get current locale
    static const std::string& locale() { return currentLocale_; }

    // 获取翻译 / Get translation
    static std::string get(const std::string& key, const std::string& fallback = "") {
        auto locIt = translations_.find(currentLocale_);
        if (locIt != translations_.end()) {
            auto keyIt = locIt->second.find(key);
            if (keyIt != locIt->second.end()) return keyIt->second;
        }
        // 尝试默认区域 / Try default locale
        auto defIt = translations_.find("en");
        if (defIt != translations_.end()) {
            auto keyIt = defIt->second.find(key);
            if (keyIt != defIt->second.end()) return keyIt->second;
        }
        return fallback.empty() ? key : fallback;
    }

private:
    static inline std::string currentLocale_ = "en";
    static inline std::unordered_map<std::string, std::unordered_map<std::string, std::string>> translations_;
};

// 数字格式化 / Number formatting
class NumberFormatter {
public:
    // 格式化整数 / Format integer
    static std::string formatInt(int64_t value, const std::string& locale = "en") {
        std::string result = std::to_string(value < 0 ? -value : value);
        // 添加千位分隔符 / Add thousands separator
        char sep = (locale == "de" || locale == "fr") ? '.' : ',';
        int count = 0;
        for (int i = static_cast<int>(result.size()) - 1; i > 0; i--) {
            count++;
            if (count % 3 == 0) {
                result.insert(i, 1, sep);
            }
        }
        if (value < 0) result = "-" + result;
        return result;
    }

    // 格式化浮点数 / Format floating point
    static std::string formatDouble(double value, int decimals = 2,
                                     const std::string& locale = "en") {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(decimals) << value;
        std::string result = oss.str();

        // 替换小数点 / Replace decimal point
        if (locale == "de" || locale == "fr" || locale == "ru") {
            size_t dot = result.find('.');
            if (dot != std::string::npos) result[dot] = ',';
        }
        return result;
    }

    // 格式化百分比 / Format percentage
    static std::string formatPercent(double value, int decimals = 1,
                                      const std::string& locale = "en") {
        return formatDouble(value * 100, decimals, locale) + "%";
    }
};

// 货币格式化 / Currency formatting
class CurrencyFormatter {
public:
    struct CurrencyInfo {
        std::string code;
        std::string symbol;
        int decimals;
        std::string symbolPosition; // "before" or "after"
    };

    // 注册货币 / Register currency
    static void registerCurrency(const std::string& code, const CurrencyInfo& info) {
        currencies_[code] = info;
    }

    // 格式化货币 / Format currency
    static std::string format(double amount, const std::string& currencyCode,
                               const std::string& locale = "en") {
        auto it = currencies_.find(currencyCode);
        if (it == currencies_.end()) {
            return NumberFormatter::formatDouble(amount, 2, locale) + " " + currencyCode;
        }

        const auto& info = it->second;
        std::string formatted = NumberFormatter::formatDouble(amount, info.decimals, locale);

        if (info.symbolPosition == "before") {
            return info.symbol + formatted;
        } else {
            return formatted + " " + info.symbol;
        }
    }

private:
    static inline std::unordered_map<std::string, CurrencyInfo> currencies_ = {
        {"USD", {"USD", "$", 2, "before"}},
        {"EUR", {"EUR", "€", 2, "after"}},
        {"GBP", {"GBP", "£", 2, "before"}},
        {"JPY", {"JPY", "¥", 0, "before"}},
        {"CNY", {"CNY", "¥", 2, "before"}},
    };
};

// 日期格式化 / Date formatting
class DateFormatter {
public:
    // 格式化日期 / Format date
    static std::string format(const std::string& formatStr, const std::string& locale = "en") {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::tm tm;
#ifdef _WIN32
        localtime_s(&tm, &time);
#else
        localtime_r(&time, &tm);
#endif

        char buf[256];
        strftime(buf, sizeof(buf), formatStr.c_str(), &tm);
        return std::string(buf);
    }

    // ISO 8601 格式 / ISO 8601 format
    static std::string iso8601() {
        return format("%Y-%m-%dT%H:%M:%SZ");
    }

    // 本地化日期 / Localized date
    static std::string localized(const std::string& locale = "en") {
        if (locale == "ja") return format("%Y年%m月%d日");
        if (locale == "de") return format("%d.%m.%Y");
        if (locale == "fr") return format("%d/%m/%Y");
        return format("%Y-%m-%d");
    }
};

// Unicode 规范化 / Unicode normalization
class Unicode {
public:
    // NFC 规范化（简化实现）/ NFC normalization (simplified)
    static std::string normalizeNFC(const std::string& input) {
        // 简化实现：直接返回输入
        // 实际应用需要完整的 Unicode 规范化算法
        return input;
    }

    // NFD 规范化（简化实现）/ NFD normalization (simplified)
    static std::string normalizeNFD(const std::string& input) {
        return input;
    }
};

} // namespace suki::i18n
