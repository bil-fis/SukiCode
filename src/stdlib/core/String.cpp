// SukiCode String type implementation.

#include "String.h"
#include <algorithm>
#include <sstream>

namespace suki::stdlib {

size_t String::count() const {
    size_t count = 0;
    for (size_t i = 0; i < data_->size(); ) {
        unsigned char c = static_cast<unsigned char>((*data_)[i]);
        if (c < 0x80) {
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            i += 4;
        } else {
            i += 1; // invalid UTF-8
        }
        count++;
    }
    return count;
}

String String::uppercased() const {
    std::string result = *data_;
    std::transform(result.begin(), result.end(), result.begin(), ::toupper);
    return String(std::move(result));
}

String String::lowercased() const {
    std::string result = *data_;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return String(std::move(result));
}

String String::trimmingWhitespace() const {
    size_t start = data_->find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return String();
    size_t end = data_->find_last_not_of(" \t\n\r");
    return String(data_->substr(start, end - start + 1));
}

std::vector<String> String::split(char separator) const {
    std::vector<String> result;
    std::istringstream stream(*data_);
    std::string item;
    while (std::getline(stream, item, separator)) {
        result.push_back(String(std::move(item)));
    }
    return result;
}

String String::replacing(const String& target, const String& replacement) const {
    std::string result = *data_;
    size_t pos = 0;
    while ((pos = result.find(*target.data_, pos)) != std::string::npos) {
        result.replace(pos, target.data_->size(), *replacement.data_);
        pos += replacement.data_->size();
    }
    return String(std::move(result));
}

std::vector<UnicodeScalar> String::unicodeScalars() const {
    std::vector<UnicodeScalar> result;
    for (size_t i = 0; i < data_->size(); ) {
        unsigned char c = static_cast<unsigned char>((*data_)[i]);
        UnicodeScalar scalar = 0;

        if (c < 0x80) {
            scalar = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            scalar = c & 0x1F;
            if (i + 1 < data_->size()) {
                scalar = (scalar << 6) | (static_cast<unsigned char>((*data_)[i + 1]) & 0x3F);
            }
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            scalar = c & 0x0F;
            if (i + 2 < data_->size()) {
                scalar = (scalar << 6) | (static_cast<unsigned char>((*data_)[i + 1]) & 0x3F);
                scalar = (scalar << 6) | (static_cast<unsigned char>((*data_)[i + 2]) & 0x3F);
            }
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            scalar = c & 0x07;
            if (i + 3 < data_->size()) {
                scalar = (scalar << 6) | (static_cast<unsigned char>((*data_)[i + 1]) & 0x3F);
                scalar = (scalar << 6) | (static_cast<unsigned char>((*data_)[i + 2]) & 0x3F);
                scalar = (scalar << 6) | (static_cast<unsigned char>((*data_)[i + 3]) & 0x3F);
            }
            i += 4;
        } else {
            scalar = c; // invalid UTF-8
            i += 1;
        }

        result.push_back(scalar);
    }
    return result;
}

} // namespace suki::stdlib
