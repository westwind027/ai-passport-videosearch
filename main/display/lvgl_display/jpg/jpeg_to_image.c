#include <esp_check.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <sys/param.h>

#include <string.h>

#include "esp_jpeg_common.h"
#include "esp_jpeg_dec.h"

#include "jpeg_to_image.h"

#ifdef CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL MAX(CONFIG_LOG_DEFAULT_LEVEL, ESP_LOG_DEBUG)
#endif  // CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
#include <esp_log.h>

#ifdef CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER
#include "driver/jpeg_decode.h"
#endif

#define TAG "jpeg_to_image"

static esp_err_t decode_with_new_jpeg(const uint8_t* src, size_t src_len, uint8_t** out,
                                      size_t* out_len, size_t* width, size_t* height,
                                      size_t* stride, const jpeg_resolution_t* scale,
                                      uint8_t* output_buffer, size_t output_capacity) {
    ESP_LOGD(TAG, "Decoding JPEG with software decoder");
    esp_err_t ret = ESP_OK;
    jpeg_error_t jpeg_ret = JPEG_ERR_OK;
    uint8_t* out_buf = NULL;
    bool output_buffer_owned = false;
    jpeg_dec_io_t jpeg_io = {0};
    jpeg_dec_header_info_t out_info = {0};

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.rotate = JPEG_ROTATE_0D;
    if (scale != NULL) {
        config.scale = *scale;
    }

    jpeg_dec_handle_t jpeg_dec = NULL;
    jpeg_ret = jpeg_dec_open(&config, &jpeg_dec);
    if (jpeg_ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open JPEG decoder");
        ret = jpeg_ret == JPEG_ERR_NO_MEM ? ESP_ERR_NO_MEM : ESP_FAIL;
        goto jpeg_dec_failed;
    }

    jpeg_io.inbuf = (uint8_t*)src;
    jpeg_io.inbuf_len = (int)src_len;

    jpeg_ret = jpeg_dec_parse_header(jpeg_dec, &jpeg_io, &out_info);
    if (jpeg_ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Failed to parse JPEG header");
        // esp_new_jpeg can allocate its working buffers while parsing the
        // header. Preserve NO_MEM here so the caller can select a smaller
        // display target instead of misreporting an allocation failure as a
        // malformed JPEG.
        ret = jpeg_ret == JPEG_ERR_NO_MEM ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
        goto jpeg_dec_failed;
    }

    ESP_LOGD(TAG, "JPEG header info: width=%d, height=%d", out_info.width, out_info.height);

    int decoder_out_len = 0;
    jpeg_ret = jpeg_dec_get_outbuf_len(jpeg_dec, &decoder_out_len);
    if (jpeg_ret != JPEG_ERR_OK || decoder_out_len <= 0) {
        ESP_LOGE(TAG, "Failed to get JPEG output buffer size");
        ret = jpeg_ret == JPEG_ERR_NO_MEM ? ESP_ERR_NO_MEM : ESP_FAIL;
        goto jpeg_dec_failed;
    }

    if (output_buffer != NULL) {
        if (((uintptr_t)output_buffer & 0x0F) != 0 || output_capacity < (size_t)decoder_out_len) {
            ESP_LOGE(TAG, "JPEG output buffer is too small or misaligned");
            ret = output_capacity < (size_t)decoder_out_len ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
            goto jpeg_dec_failed;
        }
        out_buf = output_buffer;
    } else {
        out_buf = jpeg_calloc_align((size_t)decoder_out_len, 16);
        if (out_buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for JPEG output buffer");
            ret = ESP_ERR_NO_MEM;
            goto jpeg_dec_failed;
        }
        output_buffer_owned = true;
    }

    jpeg_io.outbuf = out_buf;
    jpeg_ret = jpeg_dec_process(jpeg_dec, &jpeg_io);
    if (jpeg_ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Failed to decode JPEG");
        ret = jpeg_ret == JPEG_ERR_NO_MEM ? ESP_ERR_NO_MEM : ESP_FAIL;
        goto jpeg_dec_failed;
    }

    const size_t decoded_size =
        jpeg_io.out_size > 0 ? (size_t)jpeg_io.out_size : (size_t)decoder_out_len;
    if (decoded_size > (size_t)decoder_out_len) {
        ESP_LOGE(TAG, "JPEG decoder returned too much output: %zu > %d", decoded_size,
                 decoder_out_len);
        ret = ESP_ERR_INVALID_SIZE;
        goto jpeg_dec_failed;
    }

    const size_t decoded_width = scale != NULL ? scale->width : out_info.width;
    const size_t decoded_height = scale != NULL ? scale->height : out_info.height;
    ESP_LOG_BUFFER_HEXDUMP(TAG, out_buf, MIN(decoded_size, 256), ESP_LOG_DEBUG);

    *out = out_buf;
    out_buf = NULL;
    *out_len = decoded_size;
    *width = decoded_width;
    *height = decoded_height;
    *stride = decoded_width * 2;
    jpeg_dec_close(jpeg_dec);
    jpeg_dec = NULL;

    return ret;

jpeg_dec_failed:
    if (jpeg_dec) {
        jpeg_dec_close(jpeg_dec);
        jpeg_dec = NULL;
    }
    if (out_buf && output_buffer_owned) {
        jpeg_free_align(out_buf);
        out_buf = NULL;
    }

    *out = NULL;
    *out_len = 0;
    *width = 0;
    *height = 0;
    *stride = 0;
    return ret;
}

typedef struct {
    uint8_t* dimension_bytes;
    uint8_t original_dimensions[4];
    size_t width;
    size_t height;
    bool patched;
} jpeg_block_dimension_patch_t;

static bool is_jpeg_sof_marker(uint8_t marker) {
    return (marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
           (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF);
}

static bool is_jpeg_standalone_marker(uint8_t marker) {
    return marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9);
}

static bool find_jpeg_dimensions(uint8_t* src, size_t src_len, uint8_t** dimension_bytes,
                                 size_t* width, size_t* height) {
    if (src == NULL || src_len < 4 || src[0] != 0xFF || src[1] != 0xD8 || dimension_bytes == NULL ||
        width == NULL || height == NULL) {
        return false;
    }

    size_t offset = 2;
    while (offset + 1 < src_len) {
        if (src[offset] != 0xFF) {
            ++offset;
            continue;
        }
        while (offset < src_len && src[offset] == 0xFF) {
            ++offset;
        }
        if (offset >= src_len) {
            return false;
        }

        const uint8_t marker = src[offset++];
        if (marker == 0x00 || is_jpeg_standalone_marker(marker)) {
            continue;
        }
        if (offset + 2 > src_len) {
            return false;
        }

        const size_t segment_length = ((size_t)src[offset] << 8) | (size_t)src[offset + 1];
        if (segment_length < 2 || offset + segment_length > src_len) {
            return false;
        }
        if (is_jpeg_sof_marker(marker)) {
            if (segment_length < 7) {
                return false;
            }
            *height = ((size_t)src[offset + 3] << 8) | (size_t)src[offset + 4];
            *width = ((size_t)src[offset + 5] << 8) | (size_t)src[offset + 6];
            *dimension_bytes = src + offset + 3;
            return *width != 0 && *height != 0;
        }
        offset += segment_length;
    }
    return false;
}

static bool prepare_jpeg_block_dimensions(uint8_t* src, size_t src_len,
                                          jpeg_block_dimension_patch_t* patch) {
    if (patch == NULL) {
        return false;
    }
    memset(patch, 0, sizeof(*patch));
    if (!find_jpeg_dimensions(src, src_len, &patch->dimension_bytes, &patch->width,
                              &patch->height)) {
        return false;
    }

    memcpy(patch->original_dimensions, patch->dimension_bytes, sizeof(patch->original_dimensions));
    const size_t padded_width = (patch->width + 7) & ~((size_t)7);
    const size_t padded_height = (patch->height + 7) & ~((size_t)7);
    if (padded_width > UINT16_MAX || padded_height > UINT16_MAX) {
        return false;
    }
    if (padded_width != patch->width || padded_height != patch->height) {
        patch->dimension_bytes[0] = (uint8_t)(padded_height >> 8);
        patch->dimension_bytes[1] = (uint8_t)padded_height;
        patch->dimension_bytes[2] = (uint8_t)(padded_width >> 8);
        patch->dimension_bytes[3] = (uint8_t)padded_width;
        patch->patched = true;
    }
    return true;
}

static void restore_jpeg_block_dimensions(jpeg_block_dimension_patch_t* patch) {
    if (patch != NULL && patch->patched && patch->dimension_bytes != NULL) {
        memcpy(patch->dimension_bytes, patch->original_dimensions,
               sizeof(patch->original_dimensions));
        patch->patched = false;
    }
}

static esp_err_t decode_blocks_with_new_jpeg(uint8_t* src, size_t src_len, uint8_t* output_buffer,
                                             size_t output_capacity, size_t* width, size_t* height,
                                             size_t* stride, jpeg_image_block_callback_t callback,
                                             void* user_data) {
    ESP_LOGD(TAG, "Decoding JPEG with software block decoder");
    jpeg_error_t jpeg_ret = JPEG_ERR_OK;
    jpeg_dec_handle_t jpeg_dec = NULL;
    jpeg_dec_io_t jpeg_io = {0};
    jpeg_dec_header_info_t out_info = {0};

    if (src == NULL || src_len == 0 || output_buffer == NULL || output_capacity == 0 ||
        width == NULL || height == NULL || stride == NULL || callback == NULL ||
        ((uintptr_t)output_buffer & 0x0F) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *width = 0;
    *height = 0;
    *stride = 0;

    jpeg_block_dimension_patch_t dimension_patch;
    const bool has_source_dimensions =
        prepare_jpeg_block_dimensions(src, src_len, &dimension_patch);

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    // The SPI panel consumes RGB565 MSB-first. The normal LVGL path uses
    // little-endian RGB565 and swaps bytes during its flush; block rendering
    // bypasses LVGL, so request the panel byte order directly.
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
    config.block_enable = true;

    jpeg_ret = jpeg_dec_open(&config, &jpeg_dec);
    if (jpeg_ret != JPEG_ERR_OK) {
        restore_jpeg_block_dimensions(&dimension_patch);
        return jpeg_ret == JPEG_ERR_NO_MEM ? ESP_ERR_NO_MEM : ESP_FAIL;
    }

    jpeg_io.inbuf = (uint8_t*)src;
    jpeg_io.inbuf_len = (int)src_len;
    jpeg_ret = jpeg_dec_parse_header(jpeg_dec, &jpeg_io, &out_info);
    if (jpeg_ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Block header parse failed: ret=%d", jpeg_ret);
        ESP_LOGE(TAG, "Failed to parse JPEG header for block decode");
        goto jpeg_block_failed;
    }
    ESP_LOGI(TAG, "Block header: %ux%u, input=%u bytes", out_info.width, out_info.height,
             (unsigned)src_len);

    const size_t source_width = has_source_dimensions ? dimension_patch.width : out_info.width;
    const size_t source_height = has_source_dimensions ? dimension_patch.height : out_info.height;

    int block_size = 0;
    jpeg_ret = jpeg_dec_get_outbuf_len(jpeg_dec, &block_size);
    if (jpeg_ret != JPEG_ERR_OK || block_size <= 0) {
        ESP_LOGE(TAG, "Block output-size query failed: ret=%d size=%d", jpeg_ret, block_size);
        ESP_LOGE(TAG, "Failed to get JPEG block buffer size");
        goto jpeg_block_failed;
    }
    ESP_LOGI(TAG, "Block buffer: %d bytes, capacity=%u", block_size, (unsigned)output_capacity);
    if ((size_t)block_size > output_capacity) {
        ESP_LOGE(TAG, "JPEG block buffer is too small: %d > %u", block_size,
                 (unsigned)output_capacity);
        jpeg_ret = JPEG_ERR_NO_MEM;
        goto jpeg_block_failed;
    }

    int process_count = 0;
    jpeg_ret = jpeg_dec_get_process_count(jpeg_dec, &process_count);
    if (jpeg_ret != JPEG_ERR_OK || process_count <= 0) {
        ESP_LOGE(TAG, "Process-count query failed: ret=%d count=%d", jpeg_ret, process_count);
        ESP_LOGE(TAG, "Failed to get JPEG block process count");
        goto jpeg_block_failed;
    }

    const size_t image_stride = (size_t)out_info.width * 2;
    ESP_LOGI(TAG, "Block decode plan: count=%d stride=%u", process_count, (unsigned)image_stride);
    if (out_info.width == 0 || out_info.height == 0 || image_stride == 0 ||
        (size_t)block_size < image_stride) {
        jpeg_ret = JPEG_ERR_INVALID_PARAM;
        goto jpeg_block_failed;
    }

    jpeg_io.outbuf = output_buffer;
    size_t block_y = 0;
    for (int block_index = 0; block_index < process_count; ++block_index) {
        jpeg_ret = jpeg_dec_process(jpeg_dec, &jpeg_io);
        if (jpeg_ret != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "Block process failed: index=%d ret=%d", block_index, jpeg_ret);
            ESP_LOGE(TAG, "Failed to decode JPEG block %d", block_index);
            goto jpeg_block_failed;
        }

        if (jpeg_io.out_size <= 0 || (size_t)jpeg_io.out_size > (size_t)block_size ||
            ((size_t)jpeg_io.out_size % image_stride) != 0) {
            ESP_LOGE(TAG, "Invalid block output: index=%d out=%d max=%d stride=%u", block_index,
                     jpeg_io.out_size, block_size, (unsigned)image_stride);
            ESP_LOGE(TAG, "Invalid JPEG block output size: %d", jpeg_io.out_size);
            jpeg_ret = JPEG_ERR_FAIL;
            goto jpeg_block_failed;
        }

        const size_t decoded_block_height = (size_t)jpeg_io.out_size / image_stride;
        if (block_y >= out_info.height) {
            jpeg_ret = JPEG_ERR_FAIL;
            goto jpeg_block_failed;
        }
        const size_t visible_block_height =
            block_y < source_height ? MIN(decoded_block_height, source_height - block_y) : 0;
        if (visible_block_height > 0 &&
            !callback(output_buffer, visible_block_height * image_stride, source_width,
                      source_height, 0, block_y, source_width, visible_block_height, image_stride,
                      user_data)) {
            ESP_LOGE(TAG, "Block callback failed: index=%d y=%u height=%u", block_index,
                     (unsigned)block_y,
                     (unsigned)MIN(decoded_block_height, source_height - block_y));
            jpeg_ret = JPEG_ERR_FAIL;
            goto jpeg_block_failed;
        }
        block_y += decoded_block_height;
    }

    if (block_y < out_info.height) {
        ESP_LOGE(TAG, "JPEG block decoder stopped early: %u/%u rows", (unsigned)block_y,
                 out_info.height);
        jpeg_ret = JPEG_ERR_FAIL;
        goto jpeg_block_failed;
    }

    *width = source_width;
    *height = source_height;
    *stride = image_stride;
    jpeg_dec_close(jpeg_dec);
    restore_jpeg_block_dimensions(&dimension_patch);
    return ESP_OK;

jpeg_block_failed:
    if (jpeg_dec != NULL) {
        jpeg_dec_close(jpeg_dec);
    }
    restore_jpeg_block_dimensions(&dimension_patch);
    *width = 0;
    *height = 0;
    *stride = 0;
    return jpeg_ret == JPEG_ERR_NO_MEM ? ESP_ERR_NO_MEM : ESP_FAIL;
}

#ifdef CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER
static esp_err_t decode_with_hardware_jpeg(const uint8_t* src, size_t src_len, uint8_t** out,
                                           size_t* out_len, size_t* width, size_t* height,
                                           size_t* stride) {
    ESP_LOGD(TAG, "Decoding JPEG with hardware decoder");
    esp_err_t ret = ESP_OK;

    jpeg_decoder_handle_t jpeg_dec = NULL;
    uint8_t* bit_stream = NULL;
    uint8_t* out_buf = NULL;
    size_t out_buf_len = 0;
    size_t tx_buffer_size = 0;
    size_t rx_buffer_size = 0;

    jpeg_decode_engine_cfg_t eng_cfg = {
        .intr_priority = 1,
        .timeout_ms = 1000,
    };

    jpeg_decode_cfg_t decode_cfg_rgb = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };

    ret = jpeg_new_decoder_engine(&eng_cfg, &jpeg_dec);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create JPEG decoder engine");
        goto jpeg_hw_dec_failed;
    }

    jpeg_decode_memory_alloc_cfg_t tx_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };

    jpeg_decode_memory_alloc_cfg_t rx_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };

    bit_stream = (uint8_t*)jpeg_alloc_decoder_mem(src_len, &tx_mem_cfg, &tx_buffer_size);
    if (bit_stream == NULL || tx_buffer_size < src_len) {
        ESP_LOGE(TAG, "Failed to allocate memory for JPEG bit stream");
        ret = ESP_ERR_NO_MEM;
        goto jpeg_hw_dec_failed;
    }

    memcpy(bit_stream, src, src_len);

    jpeg_decode_picture_info_t header_info;
    ESP_GOTO_ON_ERROR(jpeg_decoder_get_info(bit_stream, src_len, &header_info), jpeg_hw_dec_failed,
                      TAG, "Failed to get JPEG header info");

    ESP_LOGD(TAG, "JPEG header info: width=%d, height=%d, sample_method=%d", header_info.width,
             header_info.height, (int)header_info.sample_method);

    switch (header_info.sample_method) {
        case JPEG_DOWN_SAMPLING_GRAY:
        case JPEG_DOWN_SAMPLING_YUV444:
            out_buf_len = header_info.width * header_info.height * 2;
            *stride = header_info.width * 2;
            break;
        case JPEG_DOWN_SAMPLING_YUV422:
        case JPEG_DOWN_SAMPLING_YUV420:
            out_buf_len = ((header_info.width + 15) & ~15) * ((header_info.height + 15) & ~15) * 2;
            *stride = ((header_info.width + 15) & ~15) * 2;
            break;
        default:
            ESP_LOGE(TAG, "Unsupported JPEG sample method");
            ret = ESP_ERR_NOT_SUPPORTED;
            goto jpeg_hw_dec_failed;
    }

    out_buf = (uint8_t*)jpeg_alloc_decoder_mem(out_buf_len, &rx_mem_cfg, &rx_buffer_size);
    if (out_buf == NULL || rx_buffer_size < out_buf_len) {
        ESP_LOGE(TAG, "Failed to allocate memory for JPEG output buffer");
        ret = ESP_ERR_NO_MEM;
        goto jpeg_hw_dec_failed;
    }

    uint32_t out_size = 0;

    ESP_GOTO_ON_ERROR(jpeg_decoder_process(jpeg_dec, &decode_cfg_rgb, bit_stream, src_len, out_buf,
                                           out_buf_len, &out_size),
                      jpeg_hw_dec_failed, TAG, "Failed to decode JPEG");

    ESP_LOGD(TAG, "Expected %d bytes, got %" PRIu32 " bytes", out_buf_len, out_size);

    if (out_size != out_buf_len) {
        ESP_LOGE(TAG, "Decoded image size mismatch: Expected %zu bytes, got %" PRIu32 " bytes",
                 out_buf_len, out_size);
        ret = ESP_ERR_INVALID_SIZE;
        goto jpeg_hw_dec_failed;
    }

    if (header_info.sample_method == JPEG_DOWN_SAMPLING_GRAY) {
        // convert GRAY8 to RGB565
        uint32_t i = header_info.width * header_info.height;
        do {
            --i;
            uint8_t r = (out_buf[i] >> 3) & 0x1F;
            uint8_t g = (out_buf[i] >> 2) & 0x3F;
            // b is same as r
            uint16_t rgb565 = (r << 11) | (g << 5) | r;
            out_buf[2 * i + 1] = (rgb565 >> 8) & 0xFF;
            out_buf[2 * i] = rgb565 & 0xFF;
        } while (i != 0);
        out_size = header_info.width * header_info.height * 2;
        ESP_LOGD(TAG, "Converted GRAY8 to RGB565, new size: %zu", out_size);
    }

    ESP_LOG_BUFFER_HEXDUMP(TAG, out_buf, MIN(out_size, 256), ESP_LOG_DEBUG);

    *out = out_buf;
    out_buf = NULL;
    *out_len = (size_t)out_size;
    jpeg_del_decoder_engine(jpeg_dec);
    jpeg_dec = NULL;
    heap_caps_free(bit_stream);
    bit_stream = NULL;
    *width = header_info.width;
    *height = header_info.height;

    return ret;

jpeg_hw_dec_failed:
    if (out_buf) {
        heap_caps_free(out_buf);
        out_buf = NULL;
    }
    if (bit_stream) {
        heap_caps_free(bit_stream);
        bit_stream = NULL;
    }
    if (jpeg_dec) {
        jpeg_del_decoder_engine(jpeg_dec);
        jpeg_dec = NULL;
    }
    *out = NULL;
    *out_len = 0;
    *width = 0;
    *height = 0;
    *stride = 0;
    return ret;
}
#endif  // CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER

esp_err_t jpeg_to_image(const uint8_t* src, size_t src_len, uint8_t** out, size_t* out_len,
                        size_t* width, size_t* height, size_t* stride) {
#ifdef CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif  // CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
    if (src == NULL || src_len == 0 || out == NULL || out_len == NULL || width == NULL ||
        height == NULL || stride == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }
#ifdef CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER
    esp_err_t ret = decode_with_hardware_jpeg(src, src_len, out, out_len, width, height, stride);
    if (ret == ESP_OK) {
        return ret;
    }
    ESP_LOGW(TAG, "Failed to decode with hardware JPEG, fallback to software decoder");
    // Fallback to esp_new_jpeg
#endif
    return decode_with_new_jpeg(src, src_len, out, out_len, width, height, stride, NULL, NULL, 0);
}

esp_err_t jpeg_to_image_into(const uint8_t* src, size_t src_len, uint8_t* output_buffer,
                             size_t output_capacity, uint8_t** out, size_t* out_len, size_t* width,
                             size_t* height, size_t* stride) {
    if (src == NULL || src_len == 0 || output_buffer == NULL || output_capacity == 0 ||
        out == NULL || out_len == NULL || width == NULL || height == NULL || stride == NULL) {
        ESP_LOGE(TAG, "Invalid JPEG output buffer parameters");
        return ESP_ERR_INVALID_ARG;
    }

    return decode_with_new_jpeg(src, src_len, out, out_len, width, height, stride, NULL,
                                output_buffer, output_capacity);
}

esp_err_t jpeg_to_image_scaled(const uint8_t* src, size_t src_len, uint8_t** out, size_t* out_len,
                               size_t* width, size_t* height, size_t* stride, size_t target_width,
                               size_t target_height) {
#ifdef CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif  // CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
    if (src == NULL || src_len == 0 || out == NULL || out_len == NULL || width == NULL ||
        height == NULL || stride == NULL || target_width == 0 || target_height == 0 ||
        target_width > UINT16_MAX || target_height > UINT16_MAX || (target_width % 8) != 0 ||
        (target_height % 8) != 0) {
        ESP_LOGE(TAG, "Invalid scaled JPEG parameters");
        return ESP_ERR_INVALID_ARG;
    }

    const jpeg_resolution_t scale = {
        .width = (uint16_t)target_width,
        .height = (uint16_t)target_height,
    };
    return decode_with_new_jpeg(src, src_len, out, out_len, width, height, stride, &scale, NULL, 0);
}

esp_err_t jpeg_to_image_scaled_into(const uint8_t* src, size_t src_len, uint8_t* output_buffer,
                                    size_t output_capacity, uint8_t** out, size_t* out_len,
                                    size_t* width, size_t* height, size_t* stride,
                                    size_t target_width, size_t target_height) {
    if (src == NULL || src_len == 0 || output_buffer == NULL || output_capacity == 0 ||
        out == NULL || out_len == NULL || width == NULL || height == NULL || stride == NULL ||
        target_width == 0 || target_height == 0 || target_width > UINT16_MAX ||
        target_height > UINT16_MAX || (target_width % 8) != 0 || (target_height % 8) != 0) {
        ESP_LOGE(TAG, "Invalid scaled JPEG output buffer parameters");
        return ESP_ERR_INVALID_ARG;
    }

    const jpeg_resolution_t scale = {
        .width = (uint16_t)target_width,
        .height = (uint16_t)target_height,
    };
    return decode_with_new_jpeg(src, src_len, out, out_len, width, height, stride, &scale,
                                output_buffer, output_capacity);
}

esp_err_t jpeg_to_image_blocks(uint8_t* src, size_t src_len, uint8_t* output_buffer,
                               size_t output_capacity, size_t* width, size_t* height,
                               size_t* stride, jpeg_image_block_callback_t callback,
                               void* user_data) {
    return decode_blocks_with_new_jpeg(src, src_len, output_buffer, output_capacity, width, height,
                                       stride, callback, user_data);
}
