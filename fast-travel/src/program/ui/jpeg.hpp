// A small baseline JPEG reader for the area pictures. It streams the file one row of 8x8 blocks at a time, so its
// memory does not depend on the picture's height, and scales the picture to the menu's sizes as the rows arrive.
#pragma once

#include <cstddef>
#include <cstdint>

namespace warp::ui::jpeg {

// Reads up to `bytes` at `offset` of the file into `buffer`; returns the count read (0 at the end or on failure).
using ReadFn = size_t (*)(void* context, int64_t offset, void* buffer, size_t bytes);

struct Target {
    uint8_t* rgb = nullptr;  // RGB888, rows top-down; nullptr skips this target
    int width = 0, height = 0;
};

constexpr int kMaxWidth = 4096;
constexpr int kMaxHeight = 4096;
constexpr int kMaxSmallWidth = 64;

// Decodes a baseline JPEG (grey, or colour with 4:4:4, 4:2:2 or 4:2:0 sampling), crops it to 16:9 about its centre
// and scales it into the targets: bilinear for `large`, the average of the covered pixels for `small` (at most
// kMaxSmallWidth wide). Without `large`, only each block's average colour is computed, which is much faster and
// enough for a small picture. Returns nullptr on success, else a short reason. It uses shared work buffers, so one
// decode runs at a time.
const char* decode(ReadFn read, void* context, Target large, Target small);

}  // namespace warp::ui::jpeg
