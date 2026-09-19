#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

template <size_t Capacity>
class BoundedSearchString {
public:
    static_assert(Capacity > 0, "A bounded search string needs storage");
    static constexpr size_t kCapacity = Capacity;

    void Clear() {
        size_ = 0;
        storage_[0] = '\0';
    }

    void Assign(std::string_view value) {
        const size_t copy_size = std::min(value.size(), Capacity);
        if (copy_size > 0) {
            std::memcpy(storage_.data(), value.data(), copy_size);
        }
        size_ = copy_size;
        FinalizeUtf8();
    }

    bool Append(std::string_view value) {
        const size_t available = Capacity - size_;
        const size_t copy_size = std::min(value.size(), available);
        if (copy_size > 0) {
            std::memcpy(storage_.data() + size_, value.data(), copy_size);
            size_ += copy_size;
        }
        storage_[size_] = '\0';
        return copy_size == value.size();
    }

    bool PushBack(char value) {
        if (size_ == Capacity) {
            return false;
        }
        storage_[size_++] = value;
        storage_[size_] = '\0';
        return true;
    }

    void FinalizeUtf8() {
        if (size_ == Capacity) {
            size_t codepoint_start = size_;
            while (codepoint_start > 0 &&
                   (static_cast<unsigned char>(storage_[codepoint_start - 1]) & 0xC0U) == 0x80U) {
                --codepoint_start;
            }
            if (codepoint_start > 0) {
                --codepoint_start;
                const unsigned char lead = static_cast<unsigned char>(storage_[codepoint_start]);
                const size_t expected = lead < 0x80U ? 1 : lead < 0xE0U ? 2 : lead < 0xF0U ? 3 : 4;
                if (codepoint_start + expected > size_) {
                    size_ = codepoint_start;
                }
            }
        }
        storage_[size_] = '\0';
    }

    bool empty() const { return size_ == 0; }
    size_t size() const { return size_; }
    const char* c_str() const { return storage_.data(); }
    std::string_view view() const { return {storage_.data(), size_}; }

private:
    std::array<char, Capacity + 1> storage_{};
    size_t size_ = 0;
};

inline constexpr size_t kMaxSearchResults = 12;
using SearchMediaId = BoundedSearchString<48>;
using SearchTitle = BoundedSearchString<64>;
using SearchImageUrl = BoundedSearchString<192>;
using SearchCaption = BoundedSearchString<96>;
using SearchQuery = BoundedSearchString<96>;

struct SearchResult {
    SearchMediaId media_id;
    SearchTitle title;
    SearchImageUrl image_url;
    SearchCaption caption;
    double start = 0.0;
    // Exact timestamp of the preview frame itself. For container-indexed
    // remote videos this keyframe can sit before the scene boundary; the
    // web player starts playback here, so remote play must use it too.
    // Negative means the service did not provide the field.
    double preview_time = -1.0;
};

class SearchResults {
public:
    void Clear() { size_ = 0; }
    bool empty() const { return size_ == 0; }
    size_t size() const { return size_; }
    size_t capacity() const { return kMaxSearchResults; }

    SearchResult& operator[](size_t index) { return storage_[index]; }
    const SearchResult& operator[](size_t index) const { return storage_[index]; }

    bool PushBack(SearchResult result) {
        if (size_ == storage_.size()) {
            return false;
        }
        storage_[size_++] = std::move(result);
        return true;
    }

private:
    std::array<SearchResult, kMaxSearchResults> storage_{};
    size_t size_ = 0;
};

struct SearchResponse {
    SearchQuery query;
    SearchResults results;

    void Clear() {
        query.Clear();
        results.Clear();
    }
};

static_assert(sizeof(SearchResponse) <= 6 * 1024,
              "The fixed search result workspace must stay within its SRAM budget");

// Parses the device-facing search response. image_url is preferred; the
// existing video-search scene.preview field remains supported as a fallback.
// The default response limit is twelve results for the C3 client.
bool ParseSearchResponse(std::string_view payload, std::string_view service_url,
                         SearchResponse& response, std::string& error);

// Parses at most max_results result items, capped at kMaxSearchResults. The
// parser stores all values directly in the caller-owned fixed result array.
bool ParseSearchResponse(std::string_view payload, std::string_view service_url,
                         SearchResponse& response, std::string& error, size_t max_results);
