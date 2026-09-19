#pragma once

#include <stddef.h>
#include <stdint.h>

// A transient RGB565 image block. The data pointer is valid only while the
// producer is executing the block callback; consumers must finish using it
// before returning from that callback.
struct Rgb565ImageBlock {
    const uint8_t* data = nullptr;
    size_t data_size = 0;
    size_t image_width = 0;
    size_t image_height = 0;
    size_t x = 0;
    size_t y = 0;
    size_t width = 0;
    size_t height = 0;
    size_t stride = 0;
};
