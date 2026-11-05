#pragma once

#include "mjpeg_clip.h"
#include "mjpeg_source.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <memory>
#include <optional>

class Display;

// GUI state is protected by the display lock; JPEG decoding runs on a low-priority task.
class MjpegPlayer {
public:
    MjpegPlayer(Display* display, lv_obj_t* target, lv_obj_t* label);
    ~MjpegPlayer();
    bool Start(std::span<const uint8_t> data, std::span<const uint8_t> index);
    bool StartSource(std::shared_ptr<MjpegFrameSource> source);
    void Stop();
    void Pause();
    void Resume();

private:
    static constexpr size_t kPixelBytes = 466 * 466 * 2;
    Display* display_;
    lv_obj_t* target_;
    lv_obj_t* label_;
    TaskHandle_t task_ = nullptr;
    SemaphoreHandle_t exited_ = nullptr;
    void* decoder_ = nullptr;
    uint8_t* stream_ = nullptr;
    uint8_t* input_ = nullptr;
    uint8_t* pixels_[2] = {};
    lv_image_dsc_t images_[2] = {};
    std::optional<MjpegClip> clip_;
    std::shared_ptr<MjpegFrameSource> source_;
    uint32_t generation_ = 0;
    int front_ = 0;
    bool playing_ = false;
    bool paused_ = false;
    bool stopping_ = false;

    bool Initialize(bool memory_stream);
    void ReleaseBuffers();
    bool IsCurrent(uint32_t generation);
    void Run();
};
