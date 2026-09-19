#pragma once

#include <cstddef>

namespace search_memory_budget {

inline constexpr size_t kNetworkReceiveBufferBytes = 14 * 1024;
inline constexpr size_t kJpegDecodeBufferBytes = 320 * 16 * sizeof(uint16_t);
inline constexpr size_t kLcdLineBufferBytes = 320 * sizeof(uint16_t);
inline constexpr size_t kHttpLargestBlockHeadroomBytes = 4 * 1024;
inline constexpr size_t kHttpTotalHeadroomBytes = 8 * 1024;

}  // namespace search_memory_budget
