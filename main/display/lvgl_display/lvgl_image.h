#pragma once

#include <lvgl.h>

// Wrap around lv_img_dsc_t
class LvglImage {
public:
    virtual const lv_img_dsc_t* image_dsc() const = 0;
    virtual bool IsGif() const { return false; }
    virtual bool IsMjpeg() const { return false; }
    virtual const uint8_t* RawData() const { return nullptr; }
    virtual size_t RawSize() const { return 0; }
    virtual const uint8_t* IndexData() const { return nullptr; }
    virtual size_t IndexSize() const { return 0; }
    virtual ~LvglImage() = default;
};

class LvglRawImage : public LvglImage {
public:
    LvglRawImage(void* data, size_t size, void* index = nullptr, size_t index_size = 0);
    virtual const lv_img_dsc_t* image_dsc() const override { return &image_dsc_; }
    virtual bool IsGif() const;
    virtual bool IsMjpeg() const override { return index_data_ != nullptr; }
    virtual const uint8_t* RawData() const override { return raw_data_; }
    virtual size_t RawSize() const override { return raw_size_; }
    virtual const uint8_t* IndexData() const override { return index_data_; }
    virtual size_t IndexSize() const override { return index_size_; }

private:
    lv_img_dsc_t image_dsc_;
    const uint8_t* raw_data_ = nullptr;
    size_t raw_size_ = 0;
    const uint8_t* index_data_ = nullptr;
    size_t index_size_ = 0;
};

class LvglCBinImage : public LvglImage {
public:
    LvglCBinImage(void* data);
    virtual ~LvglCBinImage();
    virtual const lv_img_dsc_t* image_dsc() const override { return image_dsc_; }

private:
    lv_img_dsc_t* image_dsc_ = nullptr;
};

class LvglAllocatedImage : public LvglImage {
public:
    LvglAllocatedImage(void* data, size_t size);
    LvglAllocatedImage(void* data, size_t size, int width, int height, int stride,
                       int color_format);
    virtual ~LvglAllocatedImage();
    virtual const lv_img_dsc_t* image_dsc() const override { return &image_dsc_; }
    bool IsValid() const { return valid_; }

private:
    lv_img_dsc_t image_dsc_;
    bool valid_ = false;
};
