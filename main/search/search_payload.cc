#include "search_payload.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace {

constexpr size_t kDefaultMaxResults = 12;

size_t SkipWhitespace(std::string_view payload, size_t offset) {
    while (offset < payload.size()) {
        const unsigned char character = static_cast<unsigned char>(payload[offset]);
        if (character != ' ' && character != '\t' && character != '\r' && character != '\n') {
            break;
        }
        ++offset;
    }
    return offset;
}

bool FindStringEnd(std::string_view payload, size_t start, size_t& end) {
    if (start >= payload.size() || payload[start] != '"') {
        return false;
    }

    bool escaped = false;
    for (size_t offset = start + 1; offset < payload.size(); ++offset) {
        const char character = payload[offset];
        if (escaped) {
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if (character == '"') {
            end = offset + 1;
            return true;
        }
    }
    return false;
}

// Return the exclusive end of an object/array value. Strings are skipped as
// opaque values so braces inside captions or URLs do not affect the depth.
bool FindContainerEnd(std::string_view payload, size_t start, size_t& end) {
    if (start >= payload.size() || (payload[start] != '{' && payload[start] != '[')) {
        return false;
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t offset = start; offset < payload.size(); ++offset) {
        const char character = payload[offset];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                in_string = false;
            }
            continue;
        }

        if (character == '"') {
            in_string = true;
        } else if (character == '{' || character == '[') {
            ++depth;
        } else if (character == '}' || character == ']') {
            --depth;
            if (depth == 0) {
                end = offset + 1;
                return true;
            }
            if (depth < 0) {
                return false;
            }
        }
    }
    return false;
}

bool SkipJsonValue(std::string_view payload, size_t start, size_t& end) {
    if (start >= payload.size()) {
        return false;
    }

    if (payload[start] == '"') {
        return FindStringEnd(payload, start, end);
    }
    if (payload[start] == '{' || payload[start] == '[') {
        return FindContainerEnd(payload, start, end);
    }

    size_t offset = start;
    while (offset < payload.size()) {
        const char character = payload[offset];
        if (character == ',' || character == '}' || character == ']' || character == ' ' ||
            character == '\t' || character == '\r' || character == '\n') {
            break;
        }
        ++offset;
    }
    if (offset == start) {
        return false;
    }
    end = offset;
    return true;
}

// Locate a top-level object member without building a JSON tree. The parser
// keeps only offsets into the caller-owned bounded response buffer.
bool FindTopLevelField(std::string_view payload, const char* field, size_t& value_start,
                       size_t& value_end) {
    size_t offset = SkipWhitespace(payload, 0);
    if (offset >= payload.size() || payload[offset] != '{') {
        return false;
    }
    ++offset;

    while (true) {
        offset = SkipWhitespace(payload, offset);
        if (offset >= payload.size() || payload[offset] == '}') {
            return false;
        }

        const size_t key_start = offset;
        size_t key_end = 0;
        if (!FindStringEnd(payload, key_start, key_end)) {
            return false;
        }
        const size_t field_length = std::strlen(field);
        const bool is_target =
            key_end - key_start - 2 == field_length &&
            std::memcmp(payload.data() + key_start + 1, field, field_length) == 0;

        offset = SkipWhitespace(payload, key_end);
        if (offset >= payload.size() || payload[offset] != ':') {
            return false;
        }
        offset = SkipWhitespace(payload, offset + 1);
        if (!SkipJsonValue(payload, offset, value_end)) {
            return false;
        }
        if (is_target) {
            value_start = offset;
            return true;
        }

        offset = SkipWhitespace(payload, value_end);
        if (offset >= payload.size() || payload[offset] != ',') {
            return false;
        }
        ++offset;
    }
}

template <size_t Capacity>
bool DecodeJsonString(std::string_view payload, size_t start, size_t end,
                      BoundedSearchString<Capacity>& value) {
    value.Clear();
    if (start >= end || payload[start] != '"' || end < start + 2 || payload[end - 1] != '"') {
        return false;
    }

    for (size_t offset = start + 1; offset + 1 < end; ++offset) {
        const char character = payload[offset];
        if (character != '\\') {
            value.PushBack(character);
            continue;
        }
        ++offset;
        if (offset >= end - 1) {
            value.Clear();
            return false;
        }
        switch (payload[offset]) {
            case '"':
                value.PushBack('"');
                break;
            case '\\':
                value.PushBack('\\');
                break;
            case '/':
                value.PushBack('/');
                break;
            case 'b':
                value.PushBack('\b');
                break;
            case 'f':
                value.PushBack('\f');
                break;
            case 'n':
                value.PushBack('\n');
                break;
            case 'r':
                value.PushBack('\r');
                break;
            case 't':
                value.PushBack('\t');
                break;
            default:
                // Search responses use UTF-8 strings directly. Preserve an
                // unknown escape rather than silently corrupting a caption.
                value.PushBack('\\');
                value.PushBack(payload[offset]);
                break;
        }
    }
    value.FinalizeUtf8();
    return true;
}

template <size_t Capacity>
bool StringValue(std::string_view object, const char* key, BoundedSearchString<Capacity>& value) {
    size_t value_start = 0;
    size_t value_end = 0;
    if (!FindTopLevelField(object, key, value_start, value_end) || value_start >= value_end ||
        object[value_start] != '"') {
        value.Clear();
        return false;
    }
    return DecodeJsonString(object, value_start, value_end, value);
}

double NumberValue(std::string_view object, const char* key, double fallback = 0.0) {
    size_t value_start = 0;
    size_t value_end = 0;
    if (!FindTopLevelField(object, key, value_start, value_end) || value_start >= value_end) {
        return fallback;
    }
    constexpr size_t kNumberBufferBytes = 48;
    const size_t value_size = value_end - value_start;
    if (value_size >= kNumberBufferBytes) {
        return fallback;
    }
    std::array<char, kNumberBufferBytes> value{};
    std::memcpy(value.data(), object.data() + value_start, value_size);
    char* parsed_end = nullptr;
    const double parsed = std::strtod(value.data(), &parsed_end);
    return parsed_end == value.data() + value_size ? parsed : fallback;
}

std::string_view ObjectValue(std::string_view object, const char* key) {
    size_t value_start = 0;
    size_t value_end = 0;
    if (!FindTopLevelField(object, key, value_start, value_end) || value_start >= value_end ||
        object[value_start] != '{') {
        return {};
    }
    return object.substr(value_start, value_end - value_start);
}

bool IsWhitespace(unsigned char character) {
    return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

std::string_view Trim(std::string_view value) {
    while (!value.empty() && IsWhitespace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && IsWhitespace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

bool HasHttpScheme(std::string_view value) {
    constexpr std::string_view kHttp = "http://";
    constexpr std::string_view kHttps = "https://";
    const auto starts_with = [value](std::string_view scheme) {
        if (value.size() < scheme.size()) {
            return false;
        }
        for (size_t index = 0; index < scheme.size(); ++index) {
            if (static_cast<char>(std::tolower(static_cast<unsigned char>(value[index]))) !=
                scheme[index]) {
                return false;
            }
        }
        return true;
    };
    return starts_with(kHttp) || starts_with(kHttps);
}

bool ResolveImageUrl(std::string_view service_url, std::string_view raw_url,
                     SearchImageUrl& resolved) {
    resolved.Clear();
    raw_url = Trim(raw_url);
    if (HasHttpScheme(raw_url)) {
        if (raw_url.size() > SearchImageUrl::kCapacity) {
            return false;
        }
        resolved.Assign(raw_url);
        return !resolved.empty();
    }

    service_url = Trim(service_url);
    while (!service_url.empty() && service_url.back() == '/') {
        service_url.remove_suffix(1);
    }
    if (raw_url.empty() || raw_url.front() != '/' || !HasHttpScheme(service_url) ||
        service_url.size() + raw_url.size() > SearchImageUrl::kCapacity) {
        return false;
    }
    resolved.Assign(service_url);
    resolved.Append(raw_url);
    resolved.FinalizeUtf8();
    return !resolved.empty();
}

bool ImageValue(std::string_view result, std::string_view scene, SearchImageUrl& value) {
    constexpr const char* kDirectKeys[] = {"image_url", "preview_url", "image", "preview"};
    for (const char* key : kDirectKeys) {
        if (StringValue(result, key, value) && !value.empty()) {
            return true;
        }
    }

    constexpr const char* kSceneKeys[] = {"image_url", "preview_url", "image", "preview"};
    for (const char* key : kSceneKeys) {
        if (StringValue(scene, key, value) && !value.empty()) {
            return true;
        }
    }
    value.Clear();
    return false;
}

SearchResult ParseResultItem(std::string_view item, std::string_view service_url) {
    SearchResult result;
    if (!StringValue(item, "media_id", result.media_id) || result.media_id.empty()) {
        StringValue(item, "id", result.media_id);
    }
    if (!StringValue(item, "title", result.title) || result.title.empty()) {
        StringValue(item, "name", result.title);
    }
    const std::string_view scene = ObjectValue(item, "scene");
    if (!StringValue(item, "caption", result.caption) || result.caption.empty()) {
        StringValue(item, "description", result.caption);
    }
    if (result.caption.empty()) {
        StringValue(scene, "caption", result.caption);
    }
    result.start = NumberValue(item, "start", NumberValue(scene, "start"));
    // The web player starts playback at the preview frame's own timestamp
    // (falling back to the scene start); remote play must match that. A
    // missing field stays negative so callers can apply the same fallback.
    result.preview_time = NumberValue(item, "preview_time", -1.0);
    if (result.preview_time < 0.0) {
        result.preview_time = NumberValue(scene, "preview_time", -1.0);
    }

    SearchImageUrl raw_image_url;
    if (ImageValue(item, scene, raw_image_url)) {
        ResolveImageUrl(service_url, raw_image_url.view(), result.image_url);
    }
    return result;
}

}  // namespace

bool ParseSearchResponse(std::string_view payload, std::string_view service_url,
                         SearchResponse& response, std::string& error) {
    return ParseSearchResponse(payload, service_url, response, error, kDefaultMaxResults);
}

bool ParseSearchResponse(std::string_view payload, std::string_view service_url,
                         SearchResponse& response, std::string& error, size_t max_results) {
    response.Clear();
    error.clear();

    if (max_results == 0) {
        error = "search response result limit is invalid";
        return false;
    }

    const size_t first = SkipWhitespace(payload, 0);
    if (first >= payload.size() || payload[first] != '{') {
        error = "search response must be a JSON object";
        return false;
    }

    size_t value_start = 0;
    size_t value_end = 0;
    if (FindTopLevelField(payload, "query", value_start, value_end) && value_start < value_end &&
        payload[value_start] == '"') {
        DecodeJsonString(payload, value_start, value_end, response.query);
    }

    size_t results_start = 0;
    size_t results_end = 0;
    if (!FindTopLevelField(payload, "results", results_start, results_end) ||
        results_start >= results_end || payload[results_start] != '[') {
        if (!FindTopLevelField(payload, "items", results_start, results_end) ||
            results_start >= results_end || payload[results_start] != '[') {
            error = "search response has no results array";
            return false;
        }
    }

    max_results = std::min(max_results, kMaxSearchResults);
    size_t offset = SkipWhitespace(payload, results_start + 1);
    const size_t array_end = results_end - 1;
    while (offset < array_end && response.results.size() < max_results) {
        if (payload[offset] == ',') {
            offset = SkipWhitespace(payload, offset + 1);
            continue;
        }

        size_t item_end = 0;
        if (!SkipJsonValue(payload, offset, item_end) || item_end > array_end) {
            error = "search response has invalid result";
            return false;
        }
        if (payload[offset] == '{') {
            const std::string_view item = payload.substr(offset, item_end - offset);
            response.results.PushBack(ParseResultItem(item, service_url));
        }
        offset = SkipWhitespace(payload, item_end);
    }

    return true;
}
