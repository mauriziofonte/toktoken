#ifndef UTILS_HPP
#define UTILS_HPP

#include <string>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace utils {

inline std::string format_string(const std::string &prefix, const std::string &value) {
    std::ostringstream oss;
    oss << "[" << prefix << "] " << value;
    return oss.str();
}

inline int parse_value(const std::string &input) {
    if (input.empty()) {
        return -1;
    }
    try {
        size_t pos = 0;
        int result = std::stoi(input, &pos);
        return (pos == input.size()) ? result : -1;
    } catch (...) {
        return -1;
    }
}

inline std::string trim(const std::string &s) {
    auto start = std::find_if_not(s.begin(), s.end(),
        [](unsigned char ch) { return std::isspace(ch); });
    auto end = std::find_if_not(s.rbegin(), s.rend(),
        [](unsigned char ch) { return std::isspace(ch); }).base();
    return (start < end) ? std::string(start, end) : std::string();
}

} // namespace utils

#endif // UTILS_HPP
