#include "search_url.h"

#include <algorithm>
#include <cctype>

namespace {

bool IsUnreserved(unsigned char character) {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '-' || character == '.' ||
           character == '_' || character == '~';
}

std::string PercentEncode(const std::string& value) {
    constexpr char kHexDigits[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (unsigned char character : value) {
        if (IsUnreserved(character)) {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHexDigits[character >> 4]);
            encoded.push_back(kHexDigits[character & 0x0F]);
        }
    }
    return encoded;
}

bool HasScheme(const std::string& value, const char* scheme) {
    const size_t length = std::char_traits<char>::length(scheme);
    if (value.size() < length) {
        return false;
    }

    return std::equal(
        value.begin(), value.begin() + static_cast<std::ptrdiff_t>(length), scheme,
        [](char left, char right) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(left))) == right;
        });
}

std::string Trim(const std::string& value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char character) {
                          return std::isspace(character) != 0;
                      }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

}  // namespace

bool IsSupportedSearchUrl(const std::string& url) {
    const std::string trimmed = Trim(url);
    return (HasScheme(trimmed, "http://") || HasScheme(trimmed, "https://")) &&
           trimmed.find_first_not_of(" \t\r\n", trimmed.find("://") + 3) != std::string::npos;
}

std::string ResolveSearchUrl(const std::string& service_url, const std::string& value) {
    const std::string target = Trim(value);
    if (IsSupportedSearchUrl(target)) {
        return target;
    }
    if (target.empty() || target.front() != '/' || !IsSupportedSearchUrl(service_url)) {
        return {};
    }

    std::string base = Trim(service_url);
    while (base.size() > 0 && base.back() == '/') {
        base.pop_back();
    }
    return base + target;
}

std::string BuildSearchRequestUrl(const std::string& service_url, const std::string& query) {
    return BuildSearchRequestUrl(service_url, query, 12);
}

std::string BuildSearchRequestUrl(const std::string& service_url, const std::string& query,
                                  size_t limit) {
    const std::string trimmed_query = Trim(query);
    const std::string endpoint = ResolveSearchUrl(service_url, "/v1/search");
    if (endpoint.empty() || trimmed_query.empty() || limit == 0) {
        return {};
    }

    return endpoint + "?q=" + PercentEncode(trimmed_query) + "&limit=" + std::to_string(limit) +
           "&mode=scene";
}

std::string BuildPlayerControlUrl(const std::string& service_url) {
    return ResolveSearchUrl(service_url, "/v1/player/control");
}

std::string BuildSearchImageUrl(const std::string& image_url, SearchImageVariant variant) {
    const std::string target = Trim(image_url);
    if (!IsSupportedSearchUrl(target)) {
        return {};
    }

    const char* size = variant == SearchImageVariant::Small ? "small" : "tiny";
    const size_t fragment_start = target.find('#');
    const std::string path_and_query = target.substr(0, fragment_start);
    const std::string fragment =
        fragment_start == std::string::npos ? std::string() : target.substr(fragment_start);

    const size_t query_start = path_and_query.find('?');
    const std::string path = path_and_query.substr(0, query_start);
    const std::string query = query_start == std::string::npos
                                  ? std::string()
                                  : path_and_query.substr(query_start + 1);

    std::string filtered_query;
    size_t parameter_start = 0;
    while (parameter_start <= query.size()) {
        const size_t parameter_end = query.find('&', parameter_start);
        const std::string parameter = query.substr(
            parameter_start, parameter_end == std::string::npos ? std::string::npos
                                                                  : parameter_end - parameter_start);
        const size_t equals = parameter.find('=');
        const std::string key = parameter.substr(0, equals);
        if (!parameter.empty() && key != "size" && key != "width" && key != "height") {
            if (!filtered_query.empty()) {
                filtered_query += '&';
            }
            filtered_query += parameter;
        }

        if (parameter_end == std::string::npos) {
            break;
        }
        parameter_start = parameter_end + 1;
    }

    if (!filtered_query.empty()) {
        filtered_query += '&';
    }
    filtered_query += "size=";
    filtered_query += size;
    if (variant == SearchImageVariant::Tiny) {
        filtered_query += "&width=80&height=60";
    }

    return path + "?" + filtered_query + fragment;
}
