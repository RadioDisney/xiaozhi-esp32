#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <material_symbols.h>
#include <wifi_manager.h>
#include <cstring>
#include "M5IOE1.h"
#include "M5PM1.h"
#include "application.h"
#include "assets/lang_config.h"
#include "backlight.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "esp_lcd_co5300.h"
#include "jpg/jpeg_to_image.h"
#include "lvgl_theme.h"
#include "wifi_board.h"

#define TAG "M5StackStopwatch"
#define LCD_OPCODE_WRITE_CMD (0x02ULL)

extern const uint8_t stopwatch_background_jpg_start[] asm("_binary_stopwatch_background_jpg_start");
extern const uint8_t stopwatch_background_jpg_end[] asm("_binary_stopwatch_background_jpg_end");

// CO5300 AMOLED: initialize at full brightness, then restore the saved setting.
static const co5300_lcd_init_cmd_t vendor_specific_init[] = {
    // {cmd, { data }, data_size, delay_ms}
    {0xFE, (uint8_t []){0x00}, 0, 0},
    {0xC4, (uint8_t []){0x80}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 0, 10}, // RGB565
    {0x35, (uint8_t []){0x00}, 0, 10},
    {0x53, (uint8_t []){0x20}, 1, 10},
    {0x51, (uint8_t []){0xFF}, 1, 10},
    {0x63, (uint8_t []){0xFF}, 1, 10},
    {0x2A, (uint8_t []){0x00, 0x00, 0x01, 0xD1}, 4, 0}, // Column address: 0-465
    {0x2B, (uint8_t []){0x00, 0x00, 0x01, 0xD1}, 4, 0}, // Row address: 0-465
    {0x11, (uint8_t []){0x00}, 0, 120}, // Exit sleep
    {0x29, (uint8_t []){0x00}, 0, 20},  // Display on
};

class RoundLcdDisplay : public SpiLcdDisplay {
private:
    std::shared_ptr<LvglAllocatedImage> background_image_;
    lv_obj_t* charging_bolt_ = nullptr;

    void InitializeBackground() {
        if (background_image_ != nullptr) {
            return;
        }

        uint8_t* pixels = nullptr;
        size_t length = 0;
        size_t width = 0;
        size_t height = 0;
        size_t stride = 0;
        const size_t jpeg_size = stopwatch_background_jpg_end - stopwatch_background_jpg_start;
        esp_err_t err = jpeg_to_image(stopwatch_background_jpg_start, jpeg_size, &pixels, &length,
                                      &width, &height, &stride);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to decode background: %s", esp_err_to_name(err));
            return;
        }
        if (width != DISPLAY_WIDTH || height != DISPLAY_HEIGHT || stride != width * 2 ||
            length != stride * height) {
            ESP_LOGE(TAG, "Unexpected background dimensions: %zu x %zu, stride %zu", width, height,
                     stride);
            heap_caps_free(pixels);
            return;
        }

        background_image_ = std::make_shared<LvglAllocatedImage>(pixels, length, width, height,
                                                                 stride, LV_COLOR_FORMAT_RGB565);
    }

public:
    static void rounder_event_cb(lv_event_t* e) {
        lv_area_t* area = static_cast<lv_area_t*>(lv_event_get_param(e));
        area->x1 = (area->x1 >> 1) << 1;
        area->y1 = (area->y1 >> 1) << 1;
        area->x2 = ((area->x2 >> 1) << 1) + 1;
        area->y2 = ((area->y2 >> 1) << 1) + 1;
    }

    RoundLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                    esp_lcd_panel_handle_t panel_handle,
                    int width,
                    int height,
                    int offset_x,
                    int offset_y,
                    bool mirror_x,
                    bool mirror_y,
                    bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {}

    void SetupUI() override {
        SpiLcdDisplay::SetupUI();
        InitializeBackground();
        SetTheme(current_theme_);
        DisplayLockGuard lock(this);

        // Horizontal inset so labels stay inside the round mask
        lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES * 0.2, 0);
        lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES * 0.2, 0);

        // Keep text in the transparent upper margin of the 466x466 artwork.
        lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, DISPLAY_STATUS_BAR_TOP_OFF);
        lv_obj_set_width(status_label_, DISPLAY_STATUS_TEXT_WIDTH);
        lv_obj_set_width(notification_label_, DISPLAY_STATUS_TEXT_WIDTH);
        lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);

        // The portrait ends above this single-line subtitle strip.
        if (bottom_bar_ != nullptr) {
            lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -DISPLAY_CHAT_BAR_BOTTOM_OFF);
            lv_obj_set_width(chat_message_label_, DISPLAY_CHAT_TEXT_WIDTH);
        }

        // emoji_box_: keep default LV_ALIGN_CENTER from SpiLcdDisplay::SetupUI()

        // Icons sit beside the status text and above the visible ears.
        if (top_bar_ != nullptr) {
            lv_obj_set_width(top_bar_, DISPLAY_TOP_BAR_WIDTH);
            lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, DISPLAY_TOP_BAR_TOP_OFF);
            lv_obj_set_style_pad_top(top_bar_, 0, 0);
            lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
            auto* right_icons = lv_obj_get_child(top_bar_, 1);
            if (right_icons != nullptr) {
                lv_obj_set_style_pad_column(right_icons, 0, 0);
            }
        }

        if (battery_label_ != nullptr && charging_bolt_ == nullptr) {
            // Center the bolt inside the body of the existing 30px battery glyph.
            static const lv_point_precise_t points[] = {{15, 11}, {11, 15}, {15, 15}, {12, 18}};
            charging_bolt_ = lv_line_create(battery_label_);
            lv_line_set_points(charging_bolt_, points, 4);
            lv_obj_set_pos(charging_bolt_, 0, 0);
            lv_obj_set_style_line_width(charging_bolt_, 2, 0);
            lv_obj_set_style_line_rounded(charging_bolt_, true, 0);
            lv_obj_set_style_line_color(
                charging_bolt_, lv_obj_get_style_text_color(battery_label_, LV_PART_MAIN), 0);
            lv_obj_add_flag(charging_bolt_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(charging_bolt_, LV_OBJ_FLAG_IGNORE_LAYOUT);
        }

        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    }

    void UpdateStatusBar(bool update_all) override {
        SpiLcdDisplay::UpdateStatusBar(update_all);
        DisplayLockGuard lock(this);
        if (battery_label_ == nullptr || charging_bolt_ == nullptr) {
            return;
        }
        const bool charging =
            battery_icon_ != nullptr &&
            std::strcmp(battery_icon_, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_BOLT) == 0;
        if (charging) {
            if (std::strcmp(lv_label_get_text(battery_label_),
                            MATERIAL_SYMBOLS_BATTERY_ANDROID_0) != 0) {
                lv_label_set_text(battery_label_, MATERIAL_SYMBOLS_BATTERY_ANDROID_0);
            }
            lv_obj_remove_flag(charging_bolt_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(charging_bolt_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void SetTheme(Theme* theme) override {
        if (theme == nullptr) {
            return;
        }
        DisplayLockGuard lock(this);
        if (background_image_ != nullptr) {
            auto* lvgl_theme = static_cast<LvglTheme*>(theme);
            lvgl_theme->set_background_image(background_image_);
            lvgl_theme->set_text_color(lv_color_hex(0x123A52));
        }
        SpiLcdDisplay::SetTheme(theme);
        if (charging_bolt_ != nullptr && battery_label_ != nullptr) {
            lv_obj_set_style_line_color(
                charging_bolt_, lv_obj_get_style_text_color(battery_label_, LV_PART_MAIN), 0);
        }
        if (background_image_ != nullptr) {
            // Keep the same background visible when fonts or themes are refreshed.
            if (top_bar_ != nullptr) {
                lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
            }
            if (bottom_bar_ != nullptr) {
                lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);
            }
        }
    }
};

class StopwatchBacklight : public Backlight {
public:
    StopwatchBacklight(esp_lcd_panel_io_handle_t panel_io, Display* display)
        : panel_io_(panel_io), display_(display) {}

protected:
    void SetBrightnessImpl(uint8_t brightness) override {
        DisplayLockGuard lock(display_);
        uint8_t data[] = {static_cast<uint8_t>((255 * brightness) / 100)};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;

        esp_err_t err = esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, data, sizeof(data));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set display brightness: %s", esp_err_to_name(err));
        }
    }

private:
    esp_lcd_panel_io_handle_t panel_io_;
    Display* display_;
};

class M5StackStopwatchBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    M5PM1 pmic_;
    M5IOE1 ioe_;
    Button button1_;
    Button button2_;
    RoundLcdDisplay* display_;
    StopwatchBacklight* backlight_;

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {.enable_internal_pullup = 1},
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        if (ioe_.begin(i2c_bus_, M5IOE1_I2C_ADDR, M5IOE1_I2C_FREQ_100K, M5IOE1_INT_MODE_POLLING) != M5IOE1_OK) {
            ESP_LOGE(TAG, "M5IOE1 begin failed");
            return;
        }

        if (pmic_.begin(i2c_bus_, M5PM1_DEFAULT_ADDR, M5PM1_I2C_FREQ_100K) != M5PM1_OK) {
            ESP_LOGE(TAG, "M5PM1 begin failed");
            return;
        }

        pmic_.setChargeEnable(true);
        pmic_.setBoostEnable(true);
        pmic_.pinMode(PMIC_PIN_CHARGE_STATE, INPUT);
        pmic_.pinMode(PMIC_PIN_CHARGE_PROG, OUTPUT);
        pmic_.digitalWrite(PMIC_PIN_CHARGE_PROG, LOW); // Set charge current to 425mA

        ioe_.pinMode(IOE_PIN_LCD_POWER, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_LCD_POWER, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_LCD_POWER, HIGH);

        ioe_.pinMode(IOE_PIN_LCD_RST, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_LCD_RST, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_LCD_RST, HIGH);

        // Release the CST820B touch controller from reset (M5IOE1 G4).
        ioe_.pinMode(IOE_PIN_TOUCH_RST, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_TOUCH_RST, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_TOUCH_RST, HIGH);

        ioe_.pinMode(IOE_PIN_CODEC_POWER, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_CODEC_POWER, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_CODEC_POWER, HIGH);

        ioe_.pinMode(IOE_PIN_PA_EN, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_PA_EN, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_PA_EN, HIGH);

        ioe_.pinMode(IOE_PIN_MOTOR, OUTPUT);
        ioe_.setDriveMode(IOE_PIN_MOTOR, M5IOE1_DRIVE_PUSHPULL);
        ioe_.digitalWrite(IOE_PIN_MOTOR, LOW);

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = DISPLAY_QSPI_SCK;
        buscfg.data0_io_num = DISPLAY_QSPI_D0;
        buscfg.data1_io_num = DISPLAY_QSPI_D1;
        buscfg.data2_io_num = DISPLAY_QSPI_D2;
        buscfg.data3_io_num = DISPLAY_QSPI_D3;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_QSPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        ioe_.digitalWrite(IOE_PIN_LCD_RST, LOW);
        vTaskDelay(pdMS_TO_TICKS(10));
        ioe_.digitalWrite(IOE_PIN_LCD_RST, HIGH);
        vTaskDelay(pdMS_TO_TICKS(120));

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_QSPI_CS;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_QSPI_HOST, &io_config, &panel_io));

        co5300_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(co5300_lcd_init_cmd_t),
            .flags = {.use_qspi_interface = 1},
        };

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = &vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel));

        esp_lcd_panel_set_gap(panel, 0x06, 0);
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

        display_ = new RoundLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                       DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                       DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new StopwatchBacklight(panel_io, display_);
        backlight_->RestoreBrightness();
    }

    void InitializeButtons() {
        // Button1: wake / toggle conversation
        button1_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiManager::GetInstance().IsConnected()) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // Button2: volume 0 -> 10 -> ... -> 100 -> 0
        button2_.OnClick([this]() {
            auto* codec = GetAudioCodec();
            int volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(std::string(Lang::Strings::VOLUME) + ":" + std::to_string(volume) + "%");
        });
    }

public:
    M5StackStopwatchBoard()
        : i2c_bus_(nullptr),
          button1_(BUTTON1_GPIO),
          button2_(BUTTON2_GPIO),
          display_(nullptr),
          backlight_(nullptr) {
        InitializeI2c();
        InitializeSpi();
        InitializeDisplay();
        InitializeButtons();
    }

    AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            i2c_bus_,
            I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_GPIO_PA,
            AUDIO_CODEC_ES8311_ADDR,
            false);
        return &audio_codec;
    }

    Display* GetDisplay() override {
        return display_;
    }

    Backlight* GetBacklight() override {
        return backlight_;
    }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        uint16_t voltage_mv = 0;
        if (pmic_.readVbat(&voltage_mv) != M5PM1_OK) {
            return false;
        }

        int charge_state_level = pmic_.digitalRead(PMIC_PIN_CHARGE_STATE);
        if (charge_state_level < 0) {
            // Charge status read failed; leave charging/discharging unknown
            charging = false;
            discharging = false;
        } else {
            // M5PM1 charge status is active low: 0 means charging, 1 means not charging
            charging = (charge_state_level == 0);
            discharging = !charging;
        }

        const int BATTERY_MIN_VOLTAGE = 3400;
        const int BATTERY_MAX_VOLTAGE = 4200;
        if (voltage_mv < BATTERY_MIN_VOLTAGE) {
            level = 0;
        } else if (voltage_mv > BATTERY_MAX_VOLTAGE) {
            level = 100;
        } else {
            level = ((voltage_mv - BATTERY_MIN_VOLTAGE) * 100) / (BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE);
        }
        return true;
    }
};

DECLARE_BOARD(M5StackStopwatchBoard);
