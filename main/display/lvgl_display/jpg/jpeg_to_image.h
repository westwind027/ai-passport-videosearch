#include "sdkconfig.h"
#ifndef CONFIG_IDF_TARGET_ESP32

#include <esp_err.h>

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Decodes a JPEG image from memory to raw RGB565 pixel data
 *
 * This function attempts to decode a JPEG image using hardware acceleration first (if enabled),
 * falling back to a software decoder if hardware decoding fails or is unavailable.
 *
 * @param[in] src Pointer to the JPEG bitstream in memory
 * @param[in] src_len Length of the JPEG bitstream in bytes
 * @param[out] out Pointer to a buffer pointer that will be set to the decoded image data.
 *             This buffer is allocated internally and MUST be freed by the caller using
 * heap_caps_free().
 * @param[out] out_len Pointer to a variable that will receive the size of the decoded image data in
 * bytes
 * @param[out] width Pointer to a variable that will receive the image width in pixels
 * @param[out] height Pointer to a variable that will receive the image height in pixels
 * @param[out] stride Pointer to a variable that will receive the image stride in bytes
 *
 * @return ESP_OK on successful decoding
 * @return ESP_ERR_INVALID_ARG on invalid parameters
 * @return ESP_ERR_NO_MEM on memory allocation failure
 * @return ESP_FAIL on failure
 *
 * @attention Memory Management for `*out`:
 *            - The function allocates memory for the decoded image internally
 *            - On success, the caller takes ownership of this memory and SHOULD free it using
 * heap_caps_free()
 *            - On failure, `*out` is guaranteed to be NULL and no freeing is required
 *            - Example usage:
 *              @code{.c}
 *              uint8_t *image = NULL;
 *              size_t len, width, height;
 *              if (jpeg_to_image(jpeg_data, jpeg_len, &image, &len, &width, &height)) {
 *                  // Use image data...
 *                  heap_caps_free(image);  // Critical: use heap_caps_free
 *              }
 *              @endcode
 *
 * @note Configuration dependency:
 *       - When CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER is enabled, hardware acceleration is
 * attempted first
 *       - Both hardware and software paths allocate memory that requires heap_caps_free() for
 * deallocation
 *       - The decoded image format is always RGB565 (2 bytes per pixel)
 *
 * @note When using hardware decoder, the decoded image dimensions might be aligned up to 16-byte
 * boundaries. For YUV420 or YUV422 compressed images, both width and height will be rounded up to
 * the nearest multiple of 16. See details at
 *       <https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/jpeg.html#jpeg-decoder-engine>
 *
 */
esp_err_t jpeg_to_image(const uint8_t* src, size_t src_len, uint8_t** out, size_t* out_len,
                        size_t* width, size_t* height, size_t* stride);

/**
 * @brief Decodes a JPEG into a caller-owned, 16-byte-aligned RGB565 buffer.
 *
 * The output buffer remains owned by the caller and is not freed by this
 * function. This is used by the C3 search client to reuse one fixed buffer
 * across image requests.
 */
esp_err_t jpeg_to_image_into(const uint8_t* src, size_t src_len, uint8_t* output_buffer,
                             size_t output_capacity, uint8_t** out, size_t* out_len,
                             size_t* width, size_t* height, size_t* stride);

/**
 * @brief Decodes a JPEG to a bounded RGB565 resolution using esp_new_jpeg's
 * software scaler.
 *
 * The requested dimensions must be non-zero multiples of eight. This entry
 * point intentionally uses the software decoder so it is available on the
 * ESP32-C3, which has no JPEG decoder peripheral. The caller must ensure the
 * requested resolution is supported by the decoder's scale-ratio limit.
 */
esp_err_t jpeg_to_image_scaled(const uint8_t* src, size_t src_len, uint8_t** out, size_t* out_len,
                               size_t* width, size_t* height, size_t* stride, size_t target_width,
                               size_t target_height);

/**
 * @brief Scaled variant of jpeg_to_image_into().
 */
esp_err_t jpeg_to_image_scaled_into(const uint8_t* src, size_t src_len, uint8_t* output_buffer,
                                    size_t output_capacity, uint8_t** out, size_t* out_len,
                                    size_t* width, size_t* height, size_t* stride,
                                    size_t target_width, size_t target_height);

/**
 * @brief Decode a JPEG incrementally into RGB565 big-endian blocks.
 *
 * The decoder calls the callback once per JPEG MCU block (normally 8 or 16
 * rows). The output buffer is reused for every callback, so the callback must
 * consume the data synchronously and must not retain the pointer. This mode
 * does not allocate a full decoded image and does not support decoder-side
 * scaling, clipping, or rotation.
 *
 * @param[in,out] src Writable JPEG bitstream in memory. For block mode, the
 *                  SOF dimensions are temporarily rounded up to an 8-pixel
 *                  boundary when the source has a partial MCU row/column;
 *                  they are restored before this function returns.
 * @param[in] src_len JPEG bitstream length
 * @param[in] output_buffer Caller-owned, 16-byte-aligned reusable block buffer
 * @param[in] output_capacity Capacity of output_buffer in bytes
 * @param[out] width Source image width in pixels (before temporary padding)
 * @param[out] height Source image height in pixels (before temporary padding)
 * @param[out] stride Decoded block row stride in bytes. It may be wider than
 *                    width because the decoder operates on padded MCU rows.
 * @param[in] callback Synchronous block consumer
 * @param[in] user_data Opaque callback context
 */
typedef bool (*jpeg_image_block_callback_t)(const uint8_t* data, size_t data_size,
                                            size_t image_width, size_t image_height, size_t x,
                                            size_t y, size_t width, size_t height, size_t stride,
                                            void* user_data);

esp_err_t jpeg_to_image_blocks(uint8_t* src, size_t src_len, uint8_t* output_buffer,
                               size_t output_capacity, size_t* width, size_t* height,
                               size_t* stride, jpeg_image_block_callback_t callback,
                               void* user_data);

#ifdef __cplusplus
}
#endif

#endif  // CONFIG_IDF_TARGET_ESP32
