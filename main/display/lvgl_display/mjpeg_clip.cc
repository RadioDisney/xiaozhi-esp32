#include "mjpeg_clip.h"

#include <cstring>

namespace {
uint16_t Read16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t Read32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}
}  // namespace

std::expected<MjpegClip, MjpegError> MjpegClip::Parse(std::span<const uint8_t> data,
                                                      std::span<const uint8_t> index) {
    if (index.size() < 24 || std::memcmp(index.data(), "MJI1", 4) != 0) {
        return std::unexpected(MjpegError::Header);
    }
    MjpegClip clip;
    clip.width = Read16(index.data() + 4);
    clip.height = Read16(index.data() + 6);
    clip.x = Read16(index.data() + 8);
    clip.y = Read16(index.data() + 10);
    clip.frame_count = Read32(index.data() + 12);
    clip.loops = Read32(index.data() + 20);
    if (clip.width == 0 || clip.height == 0 || clip.width > 466 || clip.height > 466 ||
        clip.x > 466 - clip.width || clip.y > 466 - clip.height) {
        return std::unexpected(MjpegError::Geometry);
    }
    if (clip.frame_count == 0 || clip.frame_count > kMaxFrames ||
        index.size() != 24 + clip.frame_count * 12) {
        return std::unexpected(MjpegError::FrameCount);
    }
    if (data.size() > kMaxStreamBytes || Read32(index.data() + 16) != data.size()) {
        return std::unexpected(MjpegError::StreamSize);
    }
    if (clip.loops > 10000 || (clip.frame_count == 1 && clip.loops != 1)) {
        return std::unexpected(MjpegError::Timing);
    }
    uint32_t expected_offset = 0;
    for (size_t i = 0; i < clip.frame_count; ++i) {
        const auto* entry = index.data() + 24 + i * 12;
        auto& frame = clip.frames[i];
        frame = {Read32(entry), Read32(entry + 4), Read32(entry + 8)};
        if (frame.offset != expected_offset || frame.offset > data.size() || frame.size < 4 ||
            frame.size > kMaxFrameBytes || frame.size > data.size() - frame.offset) {
            return std::unexpected(MjpegError::FrameBounds);
        }
        if (frame.duration_ms < 10 || frame.duration_ms > 10000) {
            return std::unexpected(MjpegError::Timing);
        }
        auto jpeg = data.subspan(frame.offset, frame.size);
        if (jpeg[0] != 0xff || jpeg[1] != 0xd8 || jpeg[jpeg.size() - 2] != 0xff ||
            jpeg.back() != 0xd9) {
            return std::unexpected(MjpegError::Jpeg);
        }
        expected_offset += frame.size;
    }
    if (expected_offset != data.size()) {
        return std::unexpected(MjpegError::StreamSize);
    }
    clip.data = data;
    return clip;
}
