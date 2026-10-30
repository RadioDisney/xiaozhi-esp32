#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>

struct MjpegFrame {
    uint32_t offset;
    uint32_t size;
    uint32_t duration_ms;
};

enum class MjpegError { Header, Geometry, FrameCount, StreamSize, FrameBounds, Timing, Jpeg };

struct MjpegClip {
    static constexpr size_t kMaxFrames = 64;
    static constexpr size_t kMaxStreamBytes = 1024 * 1024;
    static constexpr size_t kMaxFrameBytes = 128 * 1024;

    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t x = 0;
    uint16_t y = 0;
    uint32_t frame_count = 0;
    uint32_t loops = 0;  // Zero means repeat indefinitely.
    std::span<const uint8_t> data;
    std::array<MjpegFrame, kMaxFrames> frames{};

    static std::expected<MjpegClip, MjpegError> Parse(std::span<const uint8_t> data,
                                                      std::span<const uint8_t> index);
};
