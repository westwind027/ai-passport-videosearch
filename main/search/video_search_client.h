#pragma once

#include "image_block.h"
#include "search_memory_budget.h"
#include "search_payload.h"
#include "search_url.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

struct DecodedSearchImage {
    uint8_t* data = nullptr;
    size_t size = 0;
    size_t width = 0;
    size_t height = 0;
    size_t stride = 0;
    // Search images normally point into VideoSearchClient's reusable decode
    // buffer. Keep this flag for callers that receive an independently
    // allocated image through the legacy path.
    bool owns_data = true;

    ~DecodedSearchImage();
    DecodedSearchImage() = default;
    DecodedSearchImage(const DecodedSearchImage&) = delete;
    DecodedSearchImage& operator=(const DecodedSearchImage&) = delete;
    DecodedSearchImage(DecodedSearchImage&& other) noexcept;
    DecodedSearchImage& operator=(DecodedSearchImage&& other) noexcept;
};

class VideoSearchClient {
public:
    using SearchCallback = std::function<void(uint32_t request_id, bool success,
                                              const SearchResponse* response, std::string error)>;
    using ImageCallback = std::function<void(uint32_t request_id, bool success,
                                             DecodedSearchImage image, std::string error)>;
    using ImageBlockCallback =
        std::function<bool(uint32_t request_id, const Rgb565ImageBlock& block)>;
    using ImageCompleteCallback = std::function<void(
        uint32_t request_id, bool success, size_t width, size_t height, std::string error)>;
    using PlayerControlCallback = std::function<void(bool success, std::string error)>;

    VideoSearchClient() = default;
    ~VideoSearchClient();

    VideoSearchClient(const VideoSearchClient&) = delete;
    VideoSearchClient& operator=(const VideoSearchClient&) = delete;

    // Starts one bounded background request. Returns zero if a request is
    // already running or the configured service URL is not supported.
    uint32_t Search(const std::string& service_url, const std::string& query,
                    SearchCallback callback);

    // Downloads and decodes one result image. Tiny is used for the home-page
    // preview; Small is used for the full-screen viewer. Both variants are
    // decoded to the C3-safe RGB565 budget.
    uint32_t LoadImage(uint32_t request_id, const std::string& image_url, ImageCallback callback);
    uint32_t LoadImage(uint32_t request_id, const std::string& image_url,
                       SearchImageVariant variant, ImageCallback callback);

    // Streams a JPEG as transient RGB565 blocks. The block callback executes
    // on the image worker and must consume block.data before returning. The
    // completion callback runs after the decoder and HTTP resources close.
    uint32_t LoadImageBlocks(uint32_t request_id, const std::string& image_url,
                             SearchImageVariant variant, ImageBlockCallback block_callback,
                             ImageCompleteCallback complete_callback);

    // Sends one short POST (backend player control) through the shared
    // worker instead of a detached task: remote commands reuse the worker
    // stack and are serialized with search/image transfers, so they never
    // race the image pipeline for the last free heap blocks. Returns false
    // (nothing is sent) when the worker is busy or the arguments are empty.
    bool SendPlayerControl(const std::string& url, const std::string& json_body,
                           PlayerControlCallback callback);

    // Marks the active request cancelled. The HTTP adapter still owns its
    // bounded timeout; the callback receives a cancelled result afterwards.
    void Cancel();

    // Verifies and reports the compile-time image workspace. The receive and
    // decode buffers are object-owned static storage, so no runtime allocation
    // or release is allowed for their complete lifetime.
    void InitializeWorkspace();

    bool IsBusy() const;

private:
    static constexpr uint32_t kWorkerStackBytes = 8192;

    enum class Operation {
        None,
        Search,
        Image,
        ImageBlocks,
        PlayerControl,
    };

    static void TaskEntry(void* argument);
    void RunTask();
    void HandleTaskException(const char* error);
    bool IsCancelRequested() const;
    // Requests cancellation and gives a draining worker a bounded window to
    // exit, so a fast follow-up request is not silently dropped by the
    // worker_running_ guard.
    bool CancelAndWaitForWorkerExit(uint32_t timeout_ms);
    static bool DispatchJpegBlock(const uint8_t* data, size_t data_size, size_t image_width,
                                  size_t image_height, size_t x, size_t y, size_t width,
                                  size_t height, size_t stride, void* user_data);
    bool HasSynchronousHttpHeadroom() const;

    mutable std::mutex mutex_;
    TaskHandle_t task_ = nullptr;
    StaticTask_t task_storage_{};
    std::array<StackType_t, kWorkerStackBytes> task_stack_{};
    bool worker_running_ = false;
    bool cancel_requested_ = false;
    Operation operation_ = Operation::None;
    uint32_t next_request_id_ = 0;
    uint32_t request_id_ = 0;
    std::string service_url_;
    std::string query_;
    std::string image_url_;
    SearchImageVariant image_variant_ = SearchImageVariant::Tiny;
    SearchCallback search_callback_;
    ImageCallback image_callback_;
    ImageBlockCallback image_block_callback_;
    ImageCompleteCallback image_complete_callback_;
    alignas(16) std::array<
        uint8_t, search_memory_budget::kNetworkReceiveBufferBytes> image_receive_buffer_{};
    alignas(16)
        std::array<uint8_t, search_memory_budget::kJpegDecodeBufferBytes> image_decode_buffer_{};
    SearchResponse search_response_;
    std::string player_control_url_;
    std::string player_control_body_;
    PlayerControlCallback player_control_callback_;
};
