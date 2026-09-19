#include "search_query.h"

#include <cctype>

namespace {

bool EndsWith(const std::string& value, const char* suffix) {
    const std::size_t suffix_length = std::char_traits<char>::length(suffix);
    return value.size() >= suffix_length &&
           value.compare(value.size() - suffix_length, suffix_length, suffix) == 0;
}

void TrimTrailingWhitespace(std::string& value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
}

}  // namespace

std::string NormalizeSearchQuery(const std::string& text) {
    std::string result = text;

    std::size_t first = 0;
    while (first < result.size() && std::isspace(static_cast<unsigned char>(result[first]))) {
        ++first;
    }
    if (first > 0) {
        result.erase(0, first);
    }

    // XiaoZhi's STT text may carry sentence punctuation. Remove only terminal
    // punctuation; punctuation inside a phrase remains part of the query.
    constexpr const char* kTerminalPunctuation[] = {
        "。", "．", ".", "！", "!", "？", "?", "…",
    };
    bool removed = true;
    while (removed && !result.empty()) {
        removed = false;
        TrimTrailingWhitespace(result);
        for (const char* punctuation : kTerminalPunctuation) {
            if (EndsWith(result, punctuation)) {
                result.erase(result.size() - std::char_traits<char>::length(punctuation));
                removed = true;
                break;
            }
        }
    }
    TrimTrailingWhitespace(result);
    return result;
}
