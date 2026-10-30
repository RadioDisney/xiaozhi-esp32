#include "mjpeg_player.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <src/misc/cache/instance/lv_image_cache.h>
#include <algorithm>
#include <cstring>
#include "display.h"

#ifndef CONFIG_IDF_TARGET_ESP32
#include "esp_jpeg_dec.h"
#endif

#define TAG "MjpegPlayer"

MjpegPlayer::MjpegPlayer(Display* display, lv_obj_t* target, lv_obj_t* label)
    : display_(display), target_(target), label_(label) {}

MjpegPlayer::~MjpegPlayer() {
    if (task_ != nullptr) {
        {
            DisplayLockGuard lock(display_);
            stopping_ = true;
            playing_ = false;
            ++generation_;
        }
        xTaskNotifyGive(task_);
        xSemaphoreTake(exited_, portMAX_DELAY);
        task_ = nullptr;
        DisplayLockGuard lock(display_);
        lv_image_set_src(target_, nullptr);
        for (auto& image : images_) {
            lv_image_cache_drop(&image);
        }
    }
    ReleaseBuffers();
}

void MjpegPlayer::ReleaseBuffers() {
#ifndef CONFIG_IDF_TARGET_ESP32
    if (decoder_ != nullptr) {
        jpeg_dec_close(decoder_);
        decoder_ = nullptr;
    }
#endif
    heap_caps_free(stream_);
    heap_caps_free(input_);
    stream_ = input_ = nullptr;
    for (auto*& pixels : pixels_) {
        heap_caps_free(pixels);
        pixels = nullptr;
    }
    if (exited_ != nullptr) {
        vSemaphoreDelete(exited_);
        exited_ = nullptr;
    }
}

bool MjpegPlayer::Initialize() {
#ifdef CONFIG_IDF_TARGET_ESP32
    return false;
#else
    if (task_ != nullptr)
        return true;
    stream_ = static_cast<uint8_t*>(
        heap_caps_malloc(MjpegClip::kMaxStreamBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    input_ = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, MjpegClip::kMaxFrameBytes,
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    for (auto*& pixels : pixels_) {
        pixels = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(16, kPixelBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    exited_ = xSemaphoreCreateBinary();
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    if (!stream_ || !input_ || !pixels_[0] || !pixels_[1] || !exited_ ||
        jpeg_dec_open(&config, &decoder_) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Cannot allocate bounded MJPEG buffers");
        ReleaseBuffers();
        return false;
    }
    if (xTaskCreate([](void* self) { static_cast<MjpegPlayer*>(self)->Run(); }, "mjpeg_decode",
                    8192, this, 1, &task_) != pdPASS) {
        task_ = nullptr;
        ReleaseBuffers();
        return false;
    }
    return true;
#endif
}

bool MjpegPlayer::Start(std::span<const uint8_t> data, std::span<const uint8_t> index) {
    auto parsed = MjpegClip::Parse(data, index);
    if (!parsed || parsed->x + parsed->width > display_->width() ||
        parsed->y + parsed->height > display_->height()) {
        ESP_LOGE(TAG, "Invalid or oversized indexed JPEG/MJPEG clip");
        return false;
    }
    DisplayLockGuard lock(display_);
    if (!Initialize())
        return false;
    // One bounded copy per emotion change detaches playback from assets mmap lifetime.
    // The worker reads this cache under the display lock, then decodes its own input.
    std::memcpy(stream_, data.data(), data.size());
    clip_ = *parsed;
    clip_->data = {stream_, data.size()};
    ++generation_;
    playing_ = !paused_;
    xTaskNotifyGive(task_);
    return true;
}

void MjpegPlayer::Stop() {
    DisplayLockGuard lock(display_);
    playing_ = false;
    clip_.reset();
    ++generation_;
    if (task_ != nullptr)
        xTaskNotifyGive(task_);
}

void MjpegPlayer::Pause() {
    DisplayLockGuard lock(display_);
    paused_ = true;
    playing_ = false;
    ++generation_;
    if (task_ != nullptr)
        xTaskNotifyGive(task_);
}

void MjpegPlayer::Resume() {
    DisplayLockGuard lock(display_);
    paused_ = false;
    playing_ = clip_.has_value();
    ++generation_;
    if (task_ != nullptr)
        xTaskNotifyGive(task_);
}

bool MjpegPlayer::IsCurrent(uint32_t generation) {
    DisplayLockGuard lock(display_);
    return !stopping_ && playing_ && generation == generation_;
}

void MjpegPlayer::Run() {
#ifndef CONFIG_IDF_TARGET_ESP32
    uint32_t current_generation = 0;
    uint32_t frame_index = 0;
    uint32_t loops_done = 0;
    int64_t deadline = 0;
    while (true) {
        MjpegFrame frame{};
        uint16_t width = 0, height = 0, x = 0, y = 0;
        uint32_t generation = 0;
        int back = 0;
        bool ready = false;
        {
            DisplayLockGuard lock(display_);
            if (stopping_)
                break;
            if (playing_ && clip_) {
                generation = generation_;
                if (generation != current_generation) {
                    current_generation = generation;
                    frame_index = loops_done = 0;
                    deadline = esp_timer_get_time();
                }
                frame = clip_->frames[frame_index];
                width = clip_->width;
                height = clip_->height;
                x = clip_->x;
                y = clip_->y;
                back = 1 - front_;
                std::memcpy(input_, stream_ + frame.offset, frame.size);
                ready = true;
            }
        }
        if (!ready) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        jpeg_dec_io_t io{};
        io.inbuf = input_;
        io.inbuf_len = frame.size;
        io.outbuf = pixels_[back];
        jpeg_dec_header_info_t info{};
        int output_size = 0;
        bool decoded = jpeg_dec_parse_header(decoder_, &io, &info) == JPEG_ERR_OK &&
                       info.width == width && info.height == height &&
                       jpeg_dec_get_outbuf_len(decoder_, &output_size) == JPEG_ERR_OK &&
                       output_size == width * height * 2 && output_size > 0 &&
                       static_cast<size_t>(output_size) <= kPixelBytes &&
                       jpeg_dec_process(decoder_, &io) == JPEG_ERR_OK;
        if (!decoded) {
            DisplayLockGuard lock(display_);
            if (generation == generation_)
                playing_ = false;
            ESP_LOGE(TAG, "JPEG frame decode failed: %lu", static_cast<unsigned long>(frame_index));
            continue;
        }
        // Decode ahead, but never hold the GUI lock while decoding or waiting.
        while (IsCurrent(generation)) {
            const int64_t remaining = deadline - esp_timer_get_time();
            if (remaining <= 0)
                break;
            auto milliseconds = static_cast<uint32_t>((remaining + 999) / 1000);
            ulTaskNotifyTake(pdTRUE, std::max<TickType_t>(1, pdMS_TO_TICKS(milliseconds)));
        }
        {
            DisplayLockGuard lock(display_);
            if (stopping_ || !playing_ || generation != generation_)
                continue;
            auto& image = images_[back];
            lv_image_cache_drop(&image);
            image = {};
            image.header.magic = LV_IMAGE_HEADER_MAGIC;
            image.header.cf = LV_COLOR_FORMAT_RGB565;
            image.header.w = width;
            image.header.h = height;
            image.header.stride = width * 2;
            image.data_size = width * height * 2;
            image.data = pixels_[back];
            lv_image_set_src(target_, &image);
            lv_obj_align(target_, LV_ALIGN_TOP_LEFT, x, y);
            lv_obj_add_flag(label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(target_, LV_OBJ_FLAG_HIDDEN);
            front_ = back;
            deadline += static_cast<int64_t>(frame.duration_ms) * 1000;
            ++frame_index;
            if (frame_index == clip_->frame_count) {
                frame_index = 0;
                ++loops_done;
                if (clip_->loops != 0 && loops_done >= clip_->loops)
                    playing_ = false;
            }
            // Drop overdue frames instead of building a queue or slowing down clip timing.
            for (size_t skipped = 0; playing_ && skipped < MjpegClip::kMaxFrames; ++skipped) {
                const auto duration =
                    static_cast<int64_t>(clip_->frames[frame_index].duration_ms) * 1000;
                if (deadline + duration > esp_timer_get_time())
                    break;
                if (clip_->loops != 0 && loops_done + 1 >= clip_->loops &&
                    frame_index + 1 == clip_->frame_count)
                    break;
                deadline += duration;
                if (++frame_index == clip_->frame_count) {
                    frame_index = 0;
                    ++loops_done;
                    if (clip_->loops != 0 && loops_done >= clip_->loops)
                        playing_ = false;
                }
            }
            if (deadline < esp_timer_get_time() - 1000000)
                deadline = esp_timer_get_time();
        }
    }
#endif
    xSemaphoreGive(exited_);
    vTaskDelete(nullptr);
}
