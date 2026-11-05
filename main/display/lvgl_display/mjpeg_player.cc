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
            source_.reset();
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

bool MjpegPlayer::Initialize(bool memory_stream) {
#ifdef CONFIG_IDF_TARGET_ESP32
    return false;
#else
    if (memory_stream && stream_ == nullptr) {
        stream_ = static_cast<uint8_t*>(
            heap_caps_malloc(MjpegClip::kMaxStreamBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (stream_ == nullptr)
            return false;
    }
    if (task_ != nullptr)
        return true;
    input_ = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, MjpegClip::kMaxFrameBytes,
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    for (auto*& pixels : pixels_) {
        pixels = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(16, kPixelBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    exited_ = xSemaphoreCreateBinary();
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    if (!input_ || !pixels_[0] || !pixels_[1] || !exited_ ||
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
    if (!Initialize(true))
        return false;
    // One bounded copy per emotion change detaches playback from assets mmap lifetime.
    // The worker reads this cache under the display lock, then decodes its own input.
    std::memcpy(stream_, data.data(), data.size());
    clip_ = *parsed;
    clip_->data = {stream_, data.size()};
    source_.reset();
    ++generation_;
    playing_ = !paused_;
    xTaskNotifyGive(task_);
    return true;
}

bool MjpegPlayer::StartSource(std::shared_ptr<MjpegFrameSource> source) {
    if (!source)
        return false;
    DisplayLockGuard lock(display_);
    if (!Initialize(false))
        return false;
    clip_.reset();
    source_ = std::move(source);
    ++generation_;
    playing_ = !paused_;
    xTaskNotifyGive(task_);
    return true;
}

void MjpegPlayer::Stop() {
    DisplayLockGuard lock(display_);
    playing_ = false;
    clip_.reset();
    source_.reset();
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
    playing_ = clip_.has_value() || source_ != nullptr;
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
    int64_t started = 0, io_us = 0, decode_us = 0;
    uint32_t displayed = 0, skipped_frames = 0, reported_loop = 0;
    std::shared_ptr<MjpegFrameSource> active_source;
    auto fail = [this](uint32_t generation, const char* reason) {
        DisplayLockGuard lock(display_);
        if (generation == generation_)
            playing_ = false;
        ESP_LOGE(TAG, "%s", reason);
    };
    while (true) {
        MjpegFrame frame{};
        uint16_t width = 0, height = 0, x = 0, y = 0;
        uint32_t frame_count = 0, loops = 0, generation = 0;
        int back = 0;
        bool ready = false;
        bool changed = false;
        std::shared_ptr<MjpegFrameSource> source;
        {
            DisplayLockGuard lock(display_);
            if (stopping_)
                break;
            source = source_;
            if (playing_ && (clip_ || source_)) {
                generation = generation_;
                changed = generation != current_generation;
                if (changed) {
                    current_generation = generation;
                    frame_index = loops_done = 0;
                    deadline = esp_timer_get_time();
                    started = deadline;
                    io_us = decode_us = 0;
                    displayed = skipped_frames = reported_loop = 0;
                }
                back = 1 - front_;
                if (!source) {
                    frame = clip_->frames[frame_index];
                    width = clip_->width;
                    height = clip_->height;
                    x = clip_->x;
                    y = clip_->y;
                    frame_count = clip_->frame_count;
                    loops = clip_->loops;
                    std::memcpy(input_, stream_ + frame.offset, frame.size);
                }
                ready = true;
            }
        }
        if (!ready) {
            // Keep paused sources pinned on this worker. Stop/replacement can
            // then release the member under the GUI lock without closing files.
            active_source = std::move(source);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (source) {
            active_source = source;
            if (changed && !source->Open()) {
                fail(generation, "MJPEG source could not open");
                continue;
            }
            const auto& info = source->Info();
            width = info.width;
            height = info.height;
            x = info.x;
            y = info.y;
            frame_count = info.frame_count;
            loops = info.loops;
            if (!width || !height || width > 466 || height > 466 || x + width > display_->width() ||
                y + height > display_->height() || !frame_count || frame_count > 256 ||
                loops > 10000 || frame_index >= frame_count) {
                fail(generation, "Invalid MJPEG source geometry or frame count");
                continue;
            }
            frame = source->Frame(frame_index);
            if (frame.size < 4 || frame.size > MjpegClip::kMaxFrameBytes ||
                frame.duration_ms < 10 || frame.duration_ms > 10000) {
                fail(generation, "Invalid MJPEG source frame");
                continue;
            }
            const int64_t read_started = esp_timer_get_time();
            const bool read = source->ReadFrame(frame_index, {input_, frame.size});
            io_us += esp_timer_get_time() - read_started;
            if (!read) {
                fail(generation, "MJPEG source frame read failed");
                continue;
            }
        } else {
            active_source.reset();
        }
        jpeg_dec_io_t io{};
        io.inbuf = input_;
        io.inbuf_len = frame.size;
        io.outbuf = pixels_[back];
        jpeg_dec_header_info_t info{};
        int output_size = 0;
        if (source)
            source->BeginDecodeMeasurement();
        const int64_t decode_started = esp_timer_get_time();
        const bool decoded = jpeg_dec_parse_header(decoder_, &io, &info) == JPEG_ERR_OK &&
                             info.width == width && info.height == height &&
                             jpeg_dec_get_outbuf_len(decoder_, &output_size) == JPEG_ERR_OK &&
                             output_size == width * height * 2 && output_size > 0 &&
                             static_cast<size_t>(output_size) <= kPixelBytes &&
                             jpeg_dec_process(decoder_, &io) == JPEG_ERR_OK;
        decode_us += esp_timer_get_time() - decode_started;
        if (source)
            source->EndDecodeMeasurement();
        if (!decoded) {
            fail(generation, "JPEG frame decode failed");
            continue;
        }
        while (IsCurrent(generation)) {
            const int64_t remaining = deadline - esp_timer_get_time();
            if (remaining <= 0)
                break;
            const auto milliseconds = static_cast<uint32_t>((remaining + 999) / 1000);
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
            ++displayed;
            if (source && frame_index == 0 && loops_done == 0)
                ESP_LOGI(TAG, "MJPEG source visible: %ux%u, %lu frames", width, height,
                         static_cast<unsigned long>(frame_count));
            deadline += static_cast<int64_t>(frame.duration_ms) * 1000;
            if (++frame_index == frame_count) {
                frame_index = 0;
                ++loops_done;
                if (loops != 0 && loops_done >= loops)
                    playing_ = false;
            }
            const auto duration_at = [&](uint32_t index) {
                return source ? source->Frame(index).duration_ms : clip_->frames[index].duration_ms;
            };
            // Keep source timing when I/O or decoding takes longer than a frame.
            for (size_t skipped = 0; playing_ && skipped < 256; ++skipped) {
                const auto duration = static_cast<int64_t>(duration_at(frame_index)) * 1000;
                if (deadline + duration > esp_timer_get_time())
                    break;
                if (loops != 0 && loops_done + 1 >= loops && frame_index + 1 == frame_count)
                    break;
                deadline += duration;
                ++skipped_frames;
                if (++frame_index == frame_count) {
                    frame_index = 0;
                    ++loops_done;
                    if (loops != 0 && loops_done >= loops)
                        playing_ = false;
                }
            }
            if (deadline < esp_timer_get_time() - 1000000)
                deadline = esp_timer_get_time();
            if (source && loops_done != reported_loop) {
                reported_loop = loops_done;
                ESP_LOGI(TAG,
                         "MJPEG stats: displayed=%lu skipped=%lu read_us=%lld decode_us=%lld "
                         "elapsed_us=%lld",
                         static_cast<unsigned long>(displayed),
                         static_cast<unsigned long>(skipped_frames), static_cast<long long>(io_us),
                         static_cast<long long>(decode_us),
                         static_cast<long long>(esp_timer_get_time() - started));
            }
        }
    }
    active_source.reset();
#endif
    xSemaphoreGive(exited_);
    vTaskDelete(nullptr);
}
