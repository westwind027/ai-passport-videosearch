#include "video_search_client.h"

#include "jpg/jpeg_to_image.h"
#include "search_url.h"

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <new>
#include <string_view>
#include <utility>

namespace {

constexpr char kTag[] = "VideoSearch";
constexpr int kHttpTimeoutMs = 15000;
// The first request preserves the normal twelve-result API. If the response
// crosses the C3 memory budget, retry with progressively smaller result sets.
constexpr std::array<size_t, 4> kSearchResultLimits{{12, 6, 3, 1}};
// The server exposes tiny (<=160x120) and small (<=320x240) variants. The C3
// has no PSRAM and only ~26 KiB of free internal heap while the audio engine
// runs, so keep one reusable input buffer instead of allocating for every
// image. The native small variant can exceed 12 KiB after JPEG headers and
// quality changes; keep one reusable 14-KiB input buffer so the full-screen
// request is accepted without allocating a new receive area for every image.
constexpr size_t kImageReceiveBufferBytes = search_memory_budget::kNetworkReceiveBufferBytes;
constexpr size_t kMaxImageWidth = 80;
constexpr size_t kMaxImageHeight = 56;
constexpr size_t kMaxDecodedImageBytes = kMaxImageWidth * kMaxImageHeight * sizeof(uint16_t);
// Block mode emits one JPEG MCU band, normally 8 or 16 rows. Keep enough
// space for the largest supported small image band without allocating a full
// 320x240 RGB565 frame.
constexpr size_t kMaxStreamImageWidth = 320;
constexpr size_t kMaxStreamImageHeight = 240;
constexpr size_t kMaxImageBlockBytes = kMaxStreamImageWidth * 16 * sizeof(uint16_t);
// One shared decode buffer serves both paths; it must hold the larger of the
// full-screen block band and the scaled-down still image.
constexpr size_t kMaxDecodeBufferBytes =
    kMaxDecodedImageBytes > kMaxImageBlockBytes ? kMaxDecodedImageBytes : kMaxImageBlockBytes;
constexpr uint32_t kImageHeapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
// The synchronous IDF HTTP client executes on VideoSearchClient's static
// worker and does not spawn a second receive task. Its connection state and
// 1-KiB read buffer still need bounded internal heap headroom.
constexpr size_t kHttpLargestBlockHeadroomBytes =
    search_memory_budget::kHttpLargestBlockHeadroomBytes;
constexpr size_t kHttpTotalHeadroomBytes = search_memory_budget::kHttpTotalHeadroomBytes;
constexpr int kPlayerControlTimeoutMs = 3000;

static_assert(kMaxDecodeBufferBytes >= kMaxImageBlockBytes,
              "the reusable decode buffer must fit one JPEG block");
static_assert(kMaxDecodeBufferBytes == search_memory_budget::kJpegDecodeBufferBytes,
              "the fixed decode workspace must fit the largest JPEG block exactly");

size_t AlignToJpegScale(size_t value) {
    if (value == 0) {
        return 8;
    }
    return ((value + 4) / 8) * 8;
}

size_t AlignDownJpegScale(size_t value) { return (value / 8) * 8; }

size_t AlignUpJpegScale(size_t value) { return ((value + 7) / 8) * 8; }

// esp_new_jpeg requires scaled dimensions to be multiples of eight and
// supports downscaling by at most 1/8. Keep the aspect ratio as closely as
// possible while staying inside the supplied C3 display image budget.
bool CalculateScaledImageSize(size_t source_width, size_t source_height, size_t& target_width,
                              size_t& target_height, size_t max_width, size_t max_height) {
    max_width = AlignDownJpegScale(std::min(source_width, max_width));
    max_height = AlignDownJpegScale(std::min(source_height, max_height));
    if (source_width == 0 || source_height == 0 || max_width < 8 || max_height < 8) {
        return false;
    }

    const uint64_t width_limited = static_cast<uint64_t>(source_width) * max_height;
    const uint64_t height_limited = static_cast<uint64_t>(source_height) * max_width;
    if (width_limited >= height_limited) {
        target_width = max_width;
        target_height = AlignToJpegScale(
            (static_cast<uint64_t>(source_height) * target_width + source_width / 2) /
            source_width);
    } else {
        target_height = max_height;
        target_width = AlignToJpegScale(
            (static_cast<uint64_t>(source_width) * target_height + source_height / 2) /
            source_height);
    }

    // The decoder's 1/8 limit means the output cannot be smaller than the
    // source dimension divided by eight. Round that lower bound up to the
    // decoder's required alignment.
    target_width = std::max(target_width, AlignUpJpegScale((source_width + 7) / 8));
    target_height = std::max(target_height, AlignUpJpegScale((source_height + 7) / 8));

    // Do not ask the decoder to upscale a very narrow source image.
    if (target_width > source_width) {
        target_width = (source_width / 8) * 8;
    }
    if (target_height > source_height) {
        target_height = (source_height / 8) * 8;
    }

    return target_width >= 8 && target_height >= 8 && target_width <= max_width &&
           target_height <= max_height;
}

// The C3 heap can have enough total free memory but no single block large
// enough for a decoded image. Choose an aligned target that fits the fixed
// output buffer. The decoder's own temporary working memory is checked by the
// JPEG wrapper and is not charged against this reusable output buffer twice.
bool SelectScaledImageSize(size_t source_width, size_t source_height, size_t max_width,
                           size_t max_height, size_t output_capacity, size_t& target_width,
                           size_t& target_height, size_t& decoded_bytes) {
    max_width = AlignDownJpegScale(std::min(source_width, max_width));
    max_height = AlignDownJpegScale(std::min(source_height, max_height));

    while (max_width >= 8 && max_height >= 8) {
        size_t candidate_width = 0;
        size_t candidate_height = 0;
        if (CalculateScaledImageSize(source_width, source_height, candidate_width, candidate_height,
                                     max_width, max_height)) {
            const size_t candidate_bytes = candidate_width * candidate_height * sizeof(uint16_t);
            if (candidate_bytes <= output_capacity) {
                target_width = candidate_width;
                target_height = candidate_height;
                decoded_bytes = candidate_bytes;
                return true;
            }
        }

        max_width = max_width >= 16 ? max_width - 8 : 0;
        max_height = max_height >= 16 ? max_height - 8 : 0;
    }

    target_width = 0;
    target_height = 0;
    decoded_bytes = 0;
    return false;
}

bool IsJpegSofMarker(uint8_t marker) {
    return (marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
           (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF);
}

bool IsJpegStandaloneMarker(uint8_t marker) {
    return marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9);
}

// Read the dimensions from the JPEG header before decoding. This prevents an
// oversized source image from being decoded at full RGB565 resolution on the
// C3 without PSRAM.
bool ReadJpegDimensions(const uint8_t* jpeg, size_t jpeg_size, size_t& width, size_t& height) {
    width = 0;
    height = 0;
    if (jpeg == nullptr || jpeg_size < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        return false;
    }

    size_t offset = 2;
    while (offset + 1 < jpeg_size) {
        if (jpeg[offset] != 0xFF) {
            ++offset;
            continue;
        }
        while (offset < jpeg_size && jpeg[offset] == 0xFF) {
            ++offset;
        }
        if (offset >= jpeg_size) {
            return false;
        }

        const uint8_t marker = jpeg[offset++];
        if (marker == 0x00 || IsJpegStandaloneMarker(marker)) {
            continue;
        }
        if (offset + 2 > jpeg_size) {
            return false;
        }

        const size_t segment_length =
            (static_cast<size_t>(jpeg[offset]) << 8) | static_cast<size_t>(jpeg[offset + 1]);
        if (segment_length < 2 || offset + segment_length > jpeg_size) {
            return false;
        }
        if (IsJpegSofMarker(marker)) {
            if (segment_length < 7) {
                return false;
            }
            height = (static_cast<size_t>(jpeg[offset + 3]) << 8) |
                     static_cast<size_t>(jpeg[offset + 4]);
            width = (static_cast<size_t>(jpeg[offset + 5]) << 8) |
                    static_cast<size_t>(jpeg[offset + 6]);
            return width != 0 && height != 0;
        }

        // segment_length includes its own two-byte length field.
        offset += segment_length;
    }
    return false;
}

enum class BoundedHttpStatus {
    Success,
    ResponseTooLarge,
    Cancelled,
    Failure,
};

struct BoundedHttpContext {
    uint8_t* buffer = nullptr;
    size_t capacity = 0;
    size_t size = 0;
    bool overflow = false;
    bool cancelled = false;
    const std::function<bool()>* should_abort = nullptr;
};

struct BoundedHttpResult {
    BoundedHttpStatus status = BoundedHttpStatus::Failure;
    esp_err_t error = ESP_FAIL;
    int http_status = 0;
    size_t body_size = 0;
};

esp_err_t HandleBoundedHttpEvent(esp_http_client_event_t* event) {
    auto* context = static_cast<BoundedHttpContext*>(event->user_data);
    if (context == nullptr) {
        return ESP_FAIL;
    }
    if (context->should_abort != nullptr && *context->should_abort && (*context->should_abort)()) {
        context->cancelled = true;
        return ESP_FAIL;
    }
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }

    const size_t data_size = static_cast<size_t>(event->data_len);
    if (context->buffer == nullptr || data_size > context->capacity - context->size) {
        context->overflow = true;
        return ESP_FAIL;
    }
    std::memcpy(context->buffer + context->size, event->data, data_size);
    context->size += data_size;
    return ESP_OK;
}

BoundedHttpResult PerformBoundedHttpRequest(const std::string& url, esp_http_client_method_t method,
                                            const char* accept, const char* content_type,
                                            const std::string* request_body,
                                            uint8_t* response_buffer, size_t response_capacity,
                                            int timeout_ms,
                                            const std::function<bool()>& should_abort = {}) {
    BoundedHttpContext context;
    context.buffer = response_buffer;
    context.capacity = response_capacity;
    context.should_abort = &should_abort;

    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.method = method;
    config.timeout_ms = timeout_ms;
    config.event_handler = HandleBoundedHttpEvent;
    config.user_data = &context;
    config.buffer_size = 1024;
    config.buffer_size_tx = 512;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    BoundedHttpResult result;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        result.error = ESP_ERR_NO_MEM;
        return result;
    }

    if (accept != nullptr) {
        esp_http_client_set_header(client, "Accept", accept);
    }
    if (content_type != nullptr) {
        esp_http_client_set_header(client, "Content-Type", content_type);
    }
    if (request_body != nullptr) {
        esp_http_client_set_post_field(client, request_body->data(),
                                       static_cast<int>(request_body->size()));
    }

    result.error = esp_http_client_perform(client);
    result.http_status = esp_http_client_get_status_code(client);
    result.body_size = context.size;
    esp_http_client_cleanup(client);

    if (context.cancelled) {
        result.status = BoundedHttpStatus::Cancelled;
    } else if (context.overflow) {
        result.status = BoundedHttpStatus::ResponseTooLarge;
    } else if (result.error == ESP_OK) {
        result.status = BoundedHttpStatus::Success;
    }
    return result;
}

enum class SearchAttemptStatus {
    Success,
    RetryableFailure,
    Failure,
};

constexpr char kSearchResponseTooLargeError[] = "search response is too large";
constexpr char kSearchResponseAllocationError[] = "search response allocation failed";

bool IsRetryableSearchError(const std::string& error) {
    return error == kSearchResponseTooLargeError || error == kSearchResponseAllocationError;
}

SearchAttemptStatus RunSearchAttempt(const std::string& service_url, const std::string& query,
                                     size_t result_limit, SearchResponse& response,
                                     std::string& error, uint8_t* response_buffer,
                                     size_t response_capacity) {
    response.Clear();
    error.clear();

    try {
        const std::string endpoint = BuildSearchRequestUrl(service_url, query, result_limit);
        if (endpoint.empty()) {
            error = "failed to build search request";
            return SearchAttemptStatus::Failure;
        }

        const BoundedHttpResult http =
            PerformBoundedHttpRequest(endpoint, HTTP_METHOD_GET, "application/json", nullptr,
                                      nullptr, response_buffer, response_capacity, kHttpTimeoutMs);
        if (http.status == BoundedHttpStatus::ResponseTooLarge) {
            error = kSearchResponseTooLargeError;
            return SearchAttemptStatus::RetryableFailure;
        }
        if (http.status == BoundedHttpStatus::Cancelled) {
            error = "search cancelled";
            return SearchAttemptStatus::Failure;
        }
        if (http.status != BoundedHttpStatus::Success) {
            error = "failed to open search request";
            ESP_LOGE(kTag, "%s: %s", error.c_str(), esp_err_to_name(http.error));
            return SearchAttemptStatus::Failure;
        }
        if (http.http_status < 200 || http.http_status >= 300) {
            error = "search service returned HTTP " + std::to_string(http.http_status);
            return SearchAttemptStatus::Failure;
        }

        const std::string_view body(reinterpret_cast<const char*>(response_buffer), http.body_size);
        if (!ParseSearchResponse(body, service_url, response, error, result_limit)) {
            response.Clear();
            return IsRetryableSearchError(error) ? SearchAttemptStatus::RetryableFailure
                                                 : SearchAttemptStatus::Failure;
        }
        return SearchAttemptStatus::Success;
    } catch (const std::bad_alloc&) {
        response.Clear();
        error = kSearchResponseAllocationError;
        return SearchAttemptStatus::RetryableFailure;
    }
}

struct ImageBlockDispatchContext {
    VideoSearchClient* client = nullptr;
    uint32_t request_id = 0;
    const VideoSearchClient::ImageBlockCallback* callback = nullptr;
};

}  // namespace

DecodedSearchImage::~DecodedSearchImage() {
    if (owns_data && data != nullptr) {
        heap_caps_free(data);
        data = nullptr;
    }
}

DecodedSearchImage::DecodedSearchImage(DecodedSearchImage&& other) noexcept
    : data(other.data),
      size(other.size),
      width(other.width),
      height(other.height),
      stride(other.stride),
      owns_data(other.owns_data) {
    other.data = nullptr;
    other.size = 0;
    other.width = 0;
    other.height = 0;
    other.stride = 0;
    other.owns_data = true;
}

DecodedSearchImage& DecodedSearchImage::operator=(DecodedSearchImage&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (owns_data && data != nullptr) {
        heap_caps_free(data);
    }
    data = other.data;
    size = other.size;
    width = other.width;
    height = other.height;
    stride = other.stride;
    owns_data = other.owns_data;
    other.data = nullptr;
    other.size = 0;
    other.width = 0;
    other.height = 0;
    other.stride = 0;
    other.owns_data = true;
    return *this;
}

VideoSearchClient::~VideoSearchClient() { Cancel(); }

bool VideoSearchClient::HasSynchronousHttpHeadroom() const {
    return heap_caps_get_largest_free_block(kImageHeapCaps) >= kHttpLargestBlockHeadroomBytes &&
           heap_caps_get_free_size(kImageHeapCaps) >= kHttpTotalHeadroomBytes;
}

void VideoSearchClient::InitializeWorkspace() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_ == nullptr) {
        task_ = xTaskCreateStatic(TaskEntry, "video_search", task_stack_.size(), this, 2,
                                  task_stack_.data(), &task_storage_);
        if (task_ == nullptr) {
            ESP_LOGE(kTag, "Failed to create the static search worker");
            return;
        }
    }
    ESP_LOGI(kTag,
             "Fixed search workspace: receive=%u@%p decode=%u@%p results=%u worker_stack=%u "
             "heap free=%u largest=%u",
             static_cast<unsigned>(image_receive_buffer_.size()), image_receive_buffer_.data(),
             static_cast<unsigned>(image_decode_buffer_.size()), image_decode_buffer_.data(),
             static_cast<unsigned>(sizeof(search_response_)),
             static_cast<unsigned>(task_stack_.size()),
             static_cast<unsigned>(heap_caps_get_free_size(kImageHeapCaps)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(kImageHeapCaps)));
}

uint32_t VideoSearchClient::Search(const std::string& service_url, const std::string& query,
                                   SearchCallback callback) {
    if (!IsSupportedSearchUrl(service_url) || query.empty() || !callback) {
        return 0;
    }

    CancelAndWaitForWorkerExit(1000);

    std::lock_guard<std::mutex> lock(mutex_);
    if (task_ == nullptr || worker_running_) {
        return 0;
    }

    ++next_request_id_;
    if (next_request_id_ == 0) {
        ++next_request_id_;
    }
    request_id_ = next_request_id_;
    operation_ = Operation::Search;
    service_url_ = service_url;
    query_ = query;
    search_callback_ = std::move(callback);
    image_callback_ = nullptr;
    image_block_callback_ = nullptr;
    image_complete_callback_ = nullptr;
    player_control_callback_ = nullptr;
    player_control_url_.clear();
    player_control_body_.clear();
    image_url_.clear();
    search_response_.Clear();
    cancel_requested_ = false;
    worker_running_ = true;

    xTaskNotifyGive(task_);
    return request_id_;
}

uint32_t VideoSearchClient::LoadImage(uint32_t request_id, const std::string& image_url,
                                      ImageCallback callback) {
    return LoadImage(request_id, image_url, SearchImageVariant::Tiny, std::move(callback));
}

uint32_t VideoSearchClient::LoadImage(uint32_t request_id, const std::string& image_url,
                                      SearchImageVariant variant, ImageCallback callback) {
    if (request_id == 0 || !IsSupportedSearchUrl(image_url) || !callback) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (task_ == nullptr || worker_running_) {
        return 0;
    }

    request_id_ = request_id;
    operation_ = Operation::Image;
    image_url_ = image_url;
    image_variant_ = variant;
    service_url_.clear();
    query_.clear();
    search_callback_ = nullptr;
    image_callback_ = std::move(callback);
    image_block_callback_ = nullptr;
    image_complete_callback_ = nullptr;
    player_control_callback_ = nullptr;
    player_control_url_.clear();
    player_control_body_.clear();
    cancel_requested_ = false;
    worker_running_ = true;

    xTaskNotifyGive(task_);
    return request_id;
}

uint32_t VideoSearchClient::LoadImageBlocks(uint32_t request_id, const std::string& image_url,
                                            SearchImageVariant variant,
                                            ImageBlockCallback block_callback,
                                            ImageCompleteCallback complete_callback) {
    if (request_id == 0 || !IsSupportedSearchUrl(image_url) || !block_callback ||
        !complete_callback) {
        return 0;
    }

    CancelAndWaitForWorkerExit(1000);

    std::lock_guard<std::mutex> lock(mutex_);
    if (task_ == nullptr || worker_running_) {
        return 0;
    }

    request_id_ = request_id;
    operation_ = Operation::ImageBlocks;
    image_url_ = image_url;
    image_variant_ = variant;
    service_url_.clear();
    query_.clear();
    search_callback_ = nullptr;
    image_callback_ = nullptr;
    image_block_callback_ = std::move(block_callback);
    image_complete_callback_ = std::move(complete_callback);
    player_control_callback_ = nullptr;
    player_control_url_.clear();
    player_control_body_.clear();
    cancel_requested_ = false;
    worker_running_ = true;

    xTaskNotifyGive(task_);
    return request_id;
}

bool VideoSearchClient::SendPlayerControl(const std::string& url, const std::string& json_body,
                                          PlayerControlCallback callback) {
    if (url.empty() || json_body.empty() || !callback) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    // Never preempt a running search or image transfer: cancelling visible
    // work just to queue a best-effort command would be worse than dropping
    // it. The caller surfaces the busy state to the user.
    if (task_ == nullptr || worker_running_) {
        return false;
    }

    request_id_ = 0;
    operation_ = Operation::PlayerControl;
    player_control_url_ = url;
    player_control_body_ = json_body;
    player_control_callback_ = std::move(callback);
    service_url_.clear();
    query_.clear();
    image_url_.clear();
    image_variant_ = SearchImageVariant::Tiny;
    search_callback_ = nullptr;
    image_callback_ = nullptr;
    image_block_callback_ = nullptr;
    image_complete_callback_ = nullptr;
    cancel_requested_ = false;
    worker_running_ = true;

    xTaskNotifyGive(task_);
    return true;
}

void VideoSearchClient::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancel_requested_ = true;
}

bool VideoSearchClient::IsCancelRequested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancel_requested_;
}

bool VideoSearchClient::CancelAndWaitForWorkerExit(uint32_t timeout_ms) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancel_requested_ = true;
    }
    constexpr uint32_t kPollIntervalMs = 20;
    for (uint32_t waited = 0; waited < timeout_ms; waited += kPollIntervalMs) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!worker_running_) {
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return !worker_running_;
}

bool VideoSearchClient::DispatchJpegBlock(const uint8_t* data, size_t data_size, size_t image_width,
                                          size_t image_height, size_t x, size_t y, size_t width,
                                          size_t height, size_t stride, void* user_data) {
    auto* context = static_cast<ImageBlockDispatchContext*>(user_data);
    if (context == nullptr || context->client == nullptr || context->callback == nullptr ||
        !*context->callback || context->client->IsCancelRequested()) {
        return false;
    }

    Rgb565ImageBlock block;
    block.data = data;
    block.data_size = data_size;
    block.image_width = image_width;
    block.image_height = image_height;
    block.x = x;
    block.y = y;
    block.width = width;
    block.height = height;
    block.stride = stride;
    return (*context->callback)(context->request_id, block);
}

bool VideoSearchClient::IsBusy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return worker_running_;
}

void VideoSearchClient::TaskEntry(void* argument) {
    auto* client = static_cast<VideoSearchClient*>(argument);
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        try {
            client->RunTask();
        } catch (const std::bad_alloc&) {
            ESP_LOGE(kTag, "Search task ran out of memory");
            client->HandleTaskException("search ran out of memory");
        } catch (const std::exception& exception) {
            ESP_LOGE(kTag, "Search task exception: %s", exception.what());
            client->HandleTaskException("search task failed");
        } catch (...) {
            ESP_LOGE(kTag, "Search task failed with an unknown exception");
            client->HandleTaskException("search task failed");
        }
    }
}

void VideoSearchClient::HandleTaskException(const char* error) {
    uint32_t request_id = 0;
    Operation operation = Operation::None;
    SearchCallback search_callback;
    ImageCallback image_callback;
    ImageCompleteCallback image_complete_callback;
    PlayerControlCallback player_control_callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        request_id = request_id_;
        operation = operation_;
        search_callback = std::move(search_callback_);
        image_callback = std::move(image_callback_);
        image_complete_callback = std::move(image_complete_callback_);
        player_control_callback = std::move(player_control_callback_);
        worker_running_ = false;
        service_url_.clear();
        query_.clear();
        image_url_.clear();
        image_variant_ = SearchImageVariant::Tiny;
        operation_ = Operation::None;
        cancel_requested_ = false;
        player_control_url_.clear();
        player_control_body_.clear();
        image_block_callback_ = nullptr;
    }

    if (operation == Operation::Search && search_callback) {
        search_response_.Clear();
        search_callback(request_id, false, nullptr,
                        error != nullptr ? error : "search task failed");
    } else if (operation == Operation::Image && image_callback) {
        image_callback(request_id, false, {}, error != nullptr ? error : "search task failed");
    } else if (operation == Operation::ImageBlocks && image_complete_callback) {
        image_complete_callback(request_id, false, 0, 0,
                                error != nullptr ? error : "search task failed");
    } else if (operation == Operation::PlayerControl && player_control_callback) {
        player_control_callback(false, error != nullptr ? error : "player control task failed");
    }
}

void VideoSearchClient::RunTask() {
    uint32_t request_id = 0;
    std::string service_url;
    std::string query;
    SearchCallback callback;
    ImageCallback image_callback;
    ImageBlockCallback image_block_callback;
    ImageCompleteCallback image_complete_callback;
    Operation operation = Operation::None;
    std::string image_url;
    SearchImageVariant image_variant = SearchImageVariant::Tiny;
    std::string player_control_url;
    std::string player_control_body;
    PlayerControlCallback player_control_callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        request_id = request_id_;
        operation = operation_;
        service_url = std::move(service_url_);
        query = std::move(query_);
        image_url = std::move(image_url_);
        image_variant = image_variant_;
        callback = std::move(search_callback_);
        image_callback = std::move(image_callback_);
        image_block_callback = std::move(image_block_callback_);
        image_complete_callback = std::move(image_complete_callback_);
        player_control_url = std::move(player_control_url_);
        player_control_body = std::move(player_control_body_);
        player_control_callback = std::move(player_control_callback_);
    }

    DecodedSearchImage image;
    std::string error;
    bool success = false;
    size_t streamed_width = 0;
    size_t streamed_height = 0;

    // Search traffic uses IDF's blocking client on this static worker. It
    // does not spawn a second receive task or build a dynamic body queue. The
    // image receive and decode workspaces remain at fixed addresses through
    // every search and page flip.
    ESP_LOGI(kTag, "worker op=%d start: heap free=%u largest=%u", static_cast<int>(operation),
             static_cast<unsigned>(heap_caps_get_free_size(kImageHeapCaps)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(kImageHeapCaps)));

    if (operation == Operation::Search) {
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancelled = cancel_requested_;
        }

        if (cancelled) {
            error = "search cancelled";
        } else if (!HasSynchronousHttpHeadroom()) {
            error = "search needs more free network memory";
        } else {
            for (const size_t result_limit : kSearchResultLimits) {
                if (IsCancelRequested()) {
                    error = "search cancelled";
                    break;
                }

                std::string attempt_error;
                const SearchAttemptStatus attempt = RunSearchAttempt(
                    service_url, query, result_limit, search_response_, attempt_error,
                    image_receive_buffer_.data(), image_receive_buffer_.size());
                if (attempt == SearchAttemptStatus::Success) {
                    success = true;
                    if (result_limit != kSearchResultLimits.front()) {
                        ESP_LOGW(kTag, "Search degraded to %u results after memory pressure",
                                 static_cast<unsigned>(result_limit));
                    }
                    break;
                }

                error = std::move(attempt_error);
                if (attempt != SearchAttemptStatus::RetryableFailure) {
                    break;
                }
                ESP_LOGW(kTag, "Search response exceeded memory budget; retrying with limit=%u",
                         static_cast<unsigned>(result_limit == 1 ? 1 : result_limit / 2));
            }
        }
    } else if (operation == Operation::Image || operation == Operation::ImageBlocks) {
        if (image_url.empty()) {
            error = "search image URL is empty";
        } else {
            const std::string image_endpoint = BuildSearchImageUrl(image_url, image_variant);
            if (image_endpoint.empty()) {
                error = "search image URL is invalid";
            } else if (!HasSynchronousHttpHeadroom()) {
                error = "search image needs more free network memory";
            } else {
                const BoundedHttpResult http = PerformBoundedHttpRequest(
                    image_endpoint, HTTP_METHOD_GET, "image/jpeg", nullptr, nullptr,
                    image_receive_buffer_.data(), image_receive_buffer_.size(), kHttpTimeoutMs,
                    [this]() { return IsCancelRequested(); });
                if (http.status == BoundedHttpStatus::ResponseTooLarge) {
                    ESP_LOGW(kTag, "Search image exceeds the %u-byte limit",
                             static_cast<unsigned>(image_receive_buffer_.size()));
                    error = "search image is too large";
                } else if (http.status == BoundedHttpStatus::Cancelled) {
                    error = "search cancelled";
                } else if (http.status != BoundedHttpStatus::Success) {
                    error = "failed to open search image";
                    ESP_LOGE(kTag, "%s: %s", error.c_str(), esp_err_to_name(http.error));
                } else if (http.http_status < 200 || http.http_status >= 300) {
                    error = "search image returned HTTP " + std::to_string(http.http_status);
                } else {
                    const size_t encoded_size = http.body_size;
                    size_t jpeg_width = 0;
                    size_t jpeg_height = 0;
                    if (!ReadJpegDimensions(image_receive_buffer_.data(), encoded_size, jpeg_width,
                                            jpeg_height)) {
                        ESP_LOGW(kTag, "Search image is not a valid JPEG");
                        error = "search image is not a valid JPEG";
                    } else {
                        ESP_LOGI(kTag, "Image bytes=%u jpeg=%ux%u mode=%s", (unsigned)encoded_size,
                                 (unsigned)jpeg_width, (unsigned)jpeg_height,
                                 operation == Operation::ImageBlocks ? "blocks" : "scaled");
                    }
                    if (error.empty() && operation == Operation::ImageBlocks) {
                        if (jpeg_width > kMaxStreamImageWidth ||
                            jpeg_height > kMaxStreamImageHeight) {
                            error = "search image dimensions are too large";
                        } else {
                            ImageBlockDispatchContext context;
                            context.client = this;
                            context.request_id = request_id;
                            context.callback = &image_block_callback;
                            const esp_err_t decode_result = jpeg_to_image_blocks(
                                image_receive_buffer_.data(), encoded_size,
                                image_decode_buffer_.data(), image_decode_buffer_.size(),
                                &streamed_width, &streamed_height, &image.stride,
                                &VideoSearchClient::DispatchJpegBlock, &context);
                            success = decode_result == ESP_OK;
                            if (!success) {
                                streamed_width = 0;
                                streamed_height = 0;
                                error = IsCancelRequested() ? "search cancelled"
                                        : decode_result == ESP_ERR_NO_MEM
                                            ? "search image exceeds decoder workspace"
                                            : "failed to decode search image blocks";
                            }
                        }
                    } else if (error.empty()) {
                        size_t target_width = jpeg_width;
                        size_t target_height = jpeg_height;
                        bool needs_scale =
                            jpeg_width > kMaxImageWidth || jpeg_height > kMaxImageHeight;
                        const size_t full_decoded_bytes =
                            jpeg_width * jpeg_height * sizeof(uint16_t);
                        size_t decoded_bytes = full_decoded_bytes;
                        if (!needs_scale && full_decoded_bytes > kMaxDecodedImageBytes) {
                            needs_scale = true;
                        }
                        if (needs_scale &&
                            !SelectScaledImageSize(jpeg_width, jpeg_height, kMaxImageWidth,
                                                   kMaxImageHeight, kMaxDecodedImageBytes,
                                                   target_width, target_height, decoded_bytes)) {
                            error = "search image dimensions are unsupported";
                        } else {
                            const esp_err_t decode_result =
                                needs_scale
                                    ? jpeg_to_image_scaled_into(
                                          image_receive_buffer_.data(), encoded_size,
                                          image_decode_buffer_.data(), kMaxDecodedImageBytes,
                                          &image.data, &image.size, &image.width, &image.height,
                                          &image.stride, target_width, target_height)
                                    : jpeg_to_image_into(image_receive_buffer_.data(), encoded_size,
                                                         image_decode_buffer_.data(),
                                                         kMaxDecodedImageBytes, &image.data,
                                                         &image.size, &image.width, &image.height,
                                                         &image.stride);
                            success = decode_result == ESP_OK;
                            if (!success) {
                                error = decode_result == ESP_ERR_NO_MEM
                                            ? "search image exceeds decoder workspace"
                                            : "failed to decode search image";
                            } else if (image.width > kMaxImageWidth ||
                                       image.height > kMaxImageHeight ||
                                       image.size > kMaxDecodedImageBytes) {
                                error = "search image dimensions are too large";
                                success = false;
                            } else {
                                image.owns_data = false;
                            }
                        }
                    }
                }
            }
        }
    } else if (operation == Operation::PlayerControl) {
        // Remote-player commands are best-effort and tiny. Keep the pinned
        // image buffers exactly as they are: releasing and re-allocating the
        // decode buffer around one small POST would churn the heap for no
        // benefit.
        if (!HasSynchronousHttpHeadroom()) {
            error = "player control needs more free network memory";
        } else {
            const BoundedHttpResult http = PerformBoundedHttpRequest(
                player_control_url, HTTP_METHOD_POST, nullptr, "application/json",
                &player_control_body, image_receive_buffer_.data(), image_receive_buffer_.size(),
                kPlayerControlTimeoutMs, [this]() { return IsCancelRequested(); });
            if (http.status == BoundedHttpStatus::ResponseTooLarge) {
                error = "player control response is too large";
            } else if (http.status == BoundedHttpStatus::Cancelled) {
                error = "search cancelled";
            } else if (http.status != BoundedHttpStatus::Success) {
                error = "failed to open player control request";
                ESP_LOGE(kTag, "%s: %s", error.c_str(), esp_err_to_name(http.error));
            } else if (http.http_status < 200 || http.http_status >= 300) {
                error = "player control returned HTTP " + std::to_string(http.http_status);
            } else {
                success = true;
                ESP_LOGI(kTag, "player control accepted (HTTP %d)", http.http_status);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancel_requested_) {
            success = false;
            if (operation == Operation::Search) {
                search_response_.Clear();
            }
            error = "search cancelled";
        }
        worker_running_ = false;
        service_url_.clear();
        query_.clear();
        image_url_.clear();
        image_variant_ = SearchImageVariant::Tiny;
        operation_ = Operation::None;
        player_control_url_.clear();
        player_control_body_.clear();
        player_control_callback_ = nullptr;
        search_callback_ = nullptr;
        image_callback_ = nullptr;
        image_block_callback_ = nullptr;
        image_complete_callback_ = nullptr;
    }

    ESP_LOGI(kTag, "worker op=%d done: heap free=%u largest=%u", static_cast<int>(operation),
             static_cast<unsigned>(heap_caps_get_free_size(kImageHeapCaps)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(kImageHeapCaps)));
    if (operation == Operation::Search && callback) {
        callback(request_id, success, success ? &search_response_ : nullptr, std::move(error));
    } else if (operation == Operation::Image && image_callback) {
        image_callback(request_id, success, std::move(image), std::move(error));
    } else if (operation == Operation::ImageBlocks && image_complete_callback) {
        image_complete_callback(request_id, success, streamed_width, streamed_height,
                                std::move(error));
    } else if (operation == Operation::PlayerControl && player_control_callback) {
        player_control_callback(success, std::move(error));
    }
}
