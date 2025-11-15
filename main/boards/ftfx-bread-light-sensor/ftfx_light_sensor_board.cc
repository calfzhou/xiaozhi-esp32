#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "assets/lang_config.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "FtfxLightSensorBoard"
#define SILENT_TIMEOUT_US 20000000  // 20 秒静音超时

class FtfxLightSensorBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    Button light_sensor_button_;
    bool light_is_on_ = false;
    esp_timer_handle_t silent_timer_handle_ = nullptr;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });
        touch_button_.OnPressDown([this]() {
            Application::GetInstance().StartListening();
        });
        touch_button_.OnPressUp([this]() {
            Application::GetInstance().StopListening();
        });

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });

        light_sensor_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "Light sensor virtual button long pressed");
            if (!light_is_on_) {
                ESP_LOGI(TAG, "Toggling light state to ON");
                light_is_on_ = true;
                auto& app = Application::GetInstance();
                auto state = app.GetDeviceState();
                if (state == kDeviceStateIdle) {
                    ESP_LOGI(TAG, "Invoking wake word due to light ON");
                    app.WakeWordInvoke("（用户打开冰箱门了）");
                }
            }
        });

        light_sensor_button_.OnPressUp([this]() {
            ESP_LOGI(TAG, "Light sensor virtual button released");
            ESP_LOGI(TAG, "Toggling light state to OFF");
            light_is_on_ = false;
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
                app.SetDeviceState(kDeviceStateIdle);
            }
        });
    }

    // 物联网初始化，逐步迁移到 MCP 协议
    void InitializeTools() {
        static LampController lamp(LAMP_GPIO);
    }

public:
    FtfxLightSensorBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
        light_sensor_button_(LIGHT_SENSOR_DO_GPIO, false, 2000) {
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        InitializeTools();

        esp_timer_create_args_t silent_timer_args = {
            .callback = [](void* arg) {
                ESP_LOGI(TAG, "Silent timer triggered");
                FtfxLightSensorBoard* board = (FtfxLightSensorBoard*)arg;
                if (!board->light_is_on_) {
                    ESP_LOGI(TAG, "Light is off, ignoring silent timer");
                    return;
                }
                auto& app = Application::GetInstance();
                auto state = app.GetDeviceState();
                if (state == kDeviceStateListening) {
                    ESP_LOGI(TAG, "Stopping listening due to silence timeout");
                    app.StopListening();
                    app.Schedule([&app]() {
                        ESP_LOGI(TAG, "Invoking wake word due to silence timeout");
                        app.WakeWordInvoke("（用户没说话）");
                    });
                }
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "silent_timer",
            .skip_unhandled_events = true
        };
        esp_timer_create(&silent_timer_args, &silent_timer_handle_);

        auto& device_event_manager = DeviceStateEventManager::GetInstance();
        device_event_manager.RegisterStateChangeCallback([this](DeviceState previous_state, DeviceState current_state) {
            if (silent_timer_handle_ == nullptr) {
                return;
            }
            if (current_state == kDeviceStateListening && light_is_on_) {
                if (esp_timer_is_active(silent_timer_handle_)) {
                    ESP_LOGI(TAG, "Restarting silent timer");
                    esp_timer_stop(silent_timer_handle_);
                } else {
                    ESP_LOGI(TAG, "Starting silent timer");
                    esp_timer_start_once(silent_timer_handle_, SILENT_TIMEOUT_US);
                }
            } else if (esp_timer_is_active(silent_timer_handle_)) {
                ESP_LOGI(TAG, "Stopping silent timer");
                esp_timer_stop(silent_timer_handle_);
            }
        });
    }

    ~FtfxLightSensorBoard() {
        if (silent_timer_handle_ != nullptr) {
            esp_timer_stop(silent_timer_handle_);
            esp_timer_delete(silent_timer_handle_);
        }
    }

    bool IsLightOn() const { return light_is_on_; }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(FtfxLightSensorBoard);
