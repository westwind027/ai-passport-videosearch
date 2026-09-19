#pragma once

#include <cstddef>
#include <string>

enum class SearchImageVariant {
    Tiny,
    Small,
};

// Returns true only for URLs that the device HTTP adapter is allowed to open.
bool IsSupportedSearchUrl(const std::string& url);

// Keeps an absolute URL unchanged and resolves a leading-slash path against
// the configured search service. Returns an empty string for unsupported or
// malformed input.
std::string ResolveSearchUrl(const std::string& service_url, const std::string& value);

// Builds the device-facing GET endpoint used by video-semantic-search.
// Returns an empty string when either the service URL or query is invalid.
std::string BuildSearchRequestUrl(const std::string& service_url, const std::string& query);

// Builds the same endpoint with an explicit result limit. Search retries use
// this overload to reduce the response working set after a size/allocation
// failure on the C3.
std::string BuildSearchRequestUrl(const std::string& service_url, const std::string& query,
                                  size_t limit);

// Resolves the backend player control endpoint (/v1/player/control) against
// the configured search service URL. Returns an empty string when the service
// URL is not usable.
std::string BuildPlayerControlUrl(const std::string& service_url);

// Adds or replaces the image-size query for a result URL. Existing size and
// custom width/height parameters are removed so the selected C3-safe variant
// always wins; unrelated query parameters and fragments are preserved.
// Tiny uses an 80x60 server bound for the home preview. Small uses the
// service's native small variant so the full-screen stream can use its full
// supported resolution; the service preserves the source aspect ratio.
std::string BuildSearchImageUrl(const std::string& image_url, SearchImageVariant variant);
