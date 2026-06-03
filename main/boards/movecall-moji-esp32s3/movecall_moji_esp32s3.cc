#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/single_led.h"

#include <esp_log.h>
#include <esp_efuse_table.h>
#include <driver/i2c_master.h>

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_gc9a01.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#define TAG "MovecallMojiESP32S3"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

class CustomLcdDisplay : public SpiLcdDisplay {
private:
    enum class CliMode {
        kBooting,
        kReady,
        kThinking,
        kListening,
        kWorking,
        kCodexWorking,
        kTesting,
        kDone,
        kError,
        kSleeping,
    };

    lv_obj_t* cli_frame_ = nullptr;
    lv_obj_t* cli_emoji_label_ = nullptr;
    lv_obj_t* cli_status_label_ = nullptr;
    lv_obj_t* cli_message_label_ = nullptr;
    esp_timer_handle_t cli_timer_ = nullptr;
    CliMode cli_mode_ = CliMode::kBooting;
    int animation_frame_ = 0;
    int done_frames_left_ = 0;
    bool had_active_session_ = false;
    bool cli_control_started_ = false;
    bool cli_control_override_ = false;

    static constexpr uint32_t kCliBackgroundColor = 0x000000;
    static constexpr uint32_t kCliForegroundColor = 0x00A8FF;
    static constexpr int kCliControlPort = 3333;

    static void OnCliTimer(void* arg) {
        static_cast<CustomLcdDisplay*>(arg)->AnimateCliFace();
    }

    static void OnCliControlTask(void* arg) {
        static_cast<CustomLcdDisplay*>(arg)->RunCliControlServer();
    }

    static bool Contains(const char* text, const char* needle) {
        if (text == nullptr || needle == nullptr) {
            return false;
        }

        std::string haystack(text);
        std::string target(needle);
        std::transform(haystack.begin(), haystack.end(), haystack.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(target.begin(), target.end(), target.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return haystack.find(target) != std::string::npos;
    }

    static std::string NormalizeCommand(const char* data, int len) {
        std::string command(data, len);
        while (!command.empty() && std::isspace(static_cast<unsigned char>(command.front()))) {
            command.erase(command.begin());
        }
        while (!command.empty() && std::isspace(static_cast<unsigned char>(command.back()))) {
            command.pop_back();
        }
        std::transform(command.begin(), command.end(), command.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return command;
    }

    bool ApplyCliControlCommand(const std::string& command) {
        if (command == "boot" || command == "booting") {
            cli_control_override_ = true;
            SetCliMode(CliMode::kBooting, true);
        } else if (command == "ready" || command == "idle") {
            cli_control_override_ = false;
            SetCliMode(CliMode::kReady, true);
        } else if (command == "think" || command == "thinking" || command == "codex-thinking") {
            cli_control_override_ = true;
            SetCliMode(CliMode::kThinking, true);
        } else if (command == "listen" || command == "listening") {
            cli_control_override_ = true;
            SetCliMode(CliMode::kListening, true);
        } else if (command == "work" || command == "working") {
            cli_control_override_ = true;
            SetCliMode(CliMode::kWorking, true);
        } else if (command == "code" || command == "coding" || command == "codex-coding" || command == "codex-working" || command == "writing-code") {
            cli_control_override_ = true;
            SetCliMode(CliMode::kCodexWorking, true);
        } else if (command == "test" || command == "testing") {
            cli_control_override_ = true;
            SetCliMode(CliMode::kTesting, true);
        } else if (command == "done" || command == "complete" || command == "completed") {
            cli_control_override_ = true;
            done_frames_left_ = 8;
            SetCliMode(CliMode::kDone, true);
        } else if (command == "error" || command == "failed" || command == "fail") {
            cli_control_override_ = true;
            SetCliMode(CliMode::kError, true);
        } else if (command == "sleep" || command == "sleeping") {
            cli_control_override_ = true;
            SetCliMode(CliMode::kSleeping, true);
        } else {
            return false;
        }
        return true;
    }

    void StartCliControlServer() {
        if (cli_control_started_) {
            return;
        }
        if (xTaskCreate(&CustomLcdDisplay::OnCliControlTask, "moji_cli_udp", 4096, this, 4, nullptr) == pdPASS) {
            cli_control_started_ = true;
        } else {
            ESP_LOGE(TAG, "Failed to start CLI control UDP task");
        }
    }

    void RunCliControlServer() {
        vTaskDelay(pdMS_TO_TICKS(3000));

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Failed to create CLI control UDP socket");
            vTaskDelete(nullptr);
            return;
        }

        int reuse = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in listen_addr = {};
        listen_addr.sin_family = AF_INET;
        listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        listen_addr.sin_port = htons(kCliControlPort);

        if (bind(sock, reinterpret_cast<sockaddr*>(&listen_addr), sizeof(listen_addr)) < 0) {
            ESP_LOGE(TAG, "Failed to bind CLI control UDP port %d", kCliControlPort);
            close(sock);
            vTaskDelete(nullptr);
            return;
        }

        ESP_LOGI(TAG, "CLI control UDP server listening on port %d", kCliControlPort);

        while (true) {
            char buffer[64] = {};
            sockaddr_in source_addr = {};
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                reinterpret_cast<sockaddr*>(&source_addr), &socklen);
            if (len <= 0) {
                continue;
            }

            auto command = NormalizeCommand(buffer, len);
            bool accepted = ApplyCliControlCommand(command);
            ESP_LOGI(TAG, "CLI control command '%s': %s", command.c_str(), accepted ? "accepted" : "ignored");
        }
    }

    void RenderLocked(const char* face, const char* status, lv_color_t border_color = lv_color_hex(kCliForegroundColor)) {
        if (cli_emoji_label_ != nullptr) {
            lv_label_set_text(cli_emoji_label_, face);
            lv_obj_remove_flag(cli_emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (cli_status_label_ != nullptr) {
            lv_label_set_text(cli_status_label_, status);
            lv_obj_remove_flag(cli_status_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (notification_label_ != nullptr) {
            lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (cli_frame_ != nullptr) {
            lv_obj_set_style_border_color(cli_frame_, border_color, 0);
        }
    }

    void SetCliMode(CliMode mode, bool force = false) {
        if (!setup_ui_called_) {
            cli_mode_ = mode;
            return;
        }
        if (!force && cli_mode_ == mode) {
            return;
        }

        cli_mode_ = mode;
        animation_frame_ = 0;

        DisplayLockGuard lock(this);
        switch (cli_mode_) {
            case CliMode::kBooting:
                RenderLocked("(^_^)", "Booting");
                break;
            case CliMode::kReady:
                RenderLocked("(o_o)", "Codex Ready");
                break;
            case CliMode::kThinking:
                RenderLocked("(o.o)", "Thinking");
                break;
            case CliMode::kListening:
                RenderLocked("(o_o)", "Codex Listening");
                break;
            case CliMode::kWorking:
                RenderLocked("(o_O)", "Codex Working");
                break;
            case CliMode::kCodexWorking:
                RenderLocked("(｡◕‿◕｡)", "Codex Working");
                break;
            case CliMode::kTesting:
                RenderLocked("(O_O)", "Testing");
                break;
            case CliMode::kDone:
                RenderLocked("(^_^)", "Task Complete");
                break;
            case CliMode::kError:
                RenderLocked("(T_T)", "Error");
                break;
            case CliMode::kSleeping:
                RenderLocked("(-_-)", "Sleeping");
                break;
        }
    }

    void AnimateCliFace() {
        if (!setup_ui_called_ || cli_emoji_label_ == nullptr) {
            return;
        }

        const char* face = nullptr;
        switch (cli_mode_) {
            case CliMode::kBooting: {
                static const char* faces[] = {"(^_^)", "(^.^)"};
                face = faces[animation_frame_ % 2];
                break;
            }
            case CliMode::kThinking: {
                static const char* faces[] = {"(o.o)", "(o_O)", "(O_o)"};
                face = faces[animation_frame_ % 3];
                break;
            }
            case CliMode::kListening: {
                static const char* faces[] = {"(o_o)", "(O_O)"};
                face = faces[animation_frame_ % 2];
                break;
            }
            case CliMode::kWorking: {
                static const char* faces[] = {"(o_O)", "(O_o)", "(o_o)"};
                face = faces[animation_frame_ % 3];
                break;
            }
            case CliMode::kCodexWorking:
                face = "(｡◕‿◕｡)";
                break;
            case CliMode::kTesting: {
                static const char* faces[] = {"(O_O)", "(o_o)"};
                face = faces[animation_frame_ % 2];
                break;
            }
            case CliMode::kReady:
                face = (animation_frame_ % 12 == 0) ? "(-_-)" : "(o_o)";
                break;
            case CliMode::kDone:
                if (done_frames_left_ > 0 && --done_frames_left_ == 0) {
                    cli_control_override_ = false;
                    SetCliMode(CliMode::kReady, true);
                    return;
                }
                face = (animation_frame_ % 2 == 0) ? "(^_^)" : "(^o^)";
                break;
            case CliMode::kError: {
                static const char* faces[] = {"(T_T)", "(x_x)"};
                face = faces[animation_frame_ % 2];
                break;
            }
            case CliMode::kSleeping: {
                static const char* faces[] = {"(-_-)", "(-.-)"};
                face = faces[animation_frame_ % 2];
                break;
            }
            default:
                break;
        }

        if (face != nullptr) {
            DisplayLockGuard lock(this);
            lv_label_set_text(cli_emoji_label_, face);
        }
        animation_frame_++;
    }

    void ApplyStateStatus() {
        StartCliControlServer();

        if (cli_control_override_) {
            return;
        }

        auto state = Application::GetInstance().GetDeviceState();

        switch (state) {
            case kDeviceStateStarting:
                SetCliMode(CliMode::kBooting);
                break;
            case kDeviceStateWifiConfiguring:
            case kDeviceStateActivating:
                SetCliMode(CliMode::kThinking);
                break;
            case kDeviceStateConnecting:
                had_active_session_ = true;
                SetCliMode(CliMode::kThinking);
                break;
            case kDeviceStateListening:
                had_active_session_ = true;
                SetCliMode(CliMode::kListening);
                break;
            case kDeviceStateSpeaking:
                had_active_session_ = true;
                SetCliMode(CliMode::kWorking);
                break;
            case kDeviceStateAudioTesting:
                SetCliMode(CliMode::kTesting);
                break;
            case kDeviceStateUpgrading:
                SetCliMode(CliMode::kWorking);
                break;
            case kDeviceStateFatalError:
                SetCliMode(CliMode::kError, true);
                break;
            case kDeviceStateIdle:
                if (had_active_session_) {
                    had_active_session_ = false;
                    done_frames_left_ = 8;
                    SetCliMode(CliMode::kDone, true);
                } else {
                    SetCliMode(CliMode::kReady);
                }
                break;
            case kDeviceStateUnknown:
            default:
                SetCliMode(CliMode::kReady);
                break;
        }
    }

public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                    esp_lcd_panel_handle_t panel_handle,
                    int width,
                    int height,
                    int offset_x,
                    int offset_y,
                    bool mirror_x,
                    bool mirror_y,
                    bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
        esp_timer_create_args_t timer_args = {
            .callback = &CustomLcdDisplay::OnCliTimer,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "moji_cli_face",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &cli_timer_));
    }

    virtual ~CustomLcdDisplay() {
        if (cli_timer_ != nullptr) {
            esp_timer_stop(cli_timer_);
            esp_timer_delete(cli_timer_);
        }
    }

    virtual void SetupUI() override {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
            return;
        }

        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        auto screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_hex(kCliBackgroundColor), 0);

        if (container_ != nullptr) {
            lv_obj_set_style_bg_color(container_, lv_color_hex(kCliBackgroundColor), 0);
            lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
        }
        if (top_bar_ != nullptr) {
            lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        if (content_ != nullptr) {
            lv_obj_add_flag(content_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_image_ != nullptr) {
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }

        cli_frame_ = lv_obj_create(screen);
        lv_obj_set_size(cli_frame_, LV_HOR_RES - 44, LV_VER_RES - 44);
        lv_obj_center(cli_frame_);
        lv_obj_set_style_radius(cli_frame_, 0, 0);
        lv_obj_set_style_bg_opa(cli_frame_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(cli_frame_, 2, 0);
        lv_obj_set_style_border_color(cli_frame_, lv_color_hex(kCliForegroundColor), 0);
        lv_obj_set_style_pad_all(cli_frame_, 0, 0);
        lv_obj_clear_flag(cli_frame_, LV_OBJ_FLAG_SCROLLABLE);

        cli_emoji_label_ = lv_label_create(screen);
        lv_obj_set_style_text_font(cli_emoji_label_, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(cli_emoji_label_, lv_color_hex(kCliForegroundColor), 0);
        lv_obj_set_style_text_align(cli_emoji_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(cli_emoji_label_, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(cli_emoji_label_, LV_HOR_RES - 56);
        lv_obj_align(cli_emoji_label_, LV_ALIGN_CENTER, 0, -34);

        if (status_bar_ != nullptr) {
            lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
            lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES * 0.18, 0);
            lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES * 0.18, 0);
            lv_obj_align(status_bar_, LV_ALIGN_CENTER, 0, 22);
            lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        }

        cli_status_label_ = lv_label_create(screen);
        lv_obj_set_style_text_font(cli_status_label_, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(cli_status_label_, lv_color_hex(kCliForegroundColor), 0);
        lv_obj_set_style_text_align(cli_status_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(cli_status_label_, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(cli_status_label_, LV_HOR_RES - 70);
        lv_obj_align(cli_status_label_, LV_ALIGN_CENTER, 0, 22);

        if (notification_label_ != nullptr) {
            lv_obj_set_style_text_font(notification_label_, &BUILTIN_TEXT_FONT, 0);
            lv_obj_set_style_text_color(notification_label_, lv_color_hex(kCliForegroundColor), 0);
            lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(notification_label_, LV_HOR_RES - 74);
        }
        if (bottom_bar_ != nullptr) {
            lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(bottom_bar_, 0, 0);
            lv_obj_set_style_pad_left(bottom_bar_, LV_HOR_RES * 0.18, 0);
            lv_obj_set_style_pad_right(bottom_bar_, LV_HOR_RES * 0.18, 0);
            lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -34);
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }

        cli_message_label_ = lv_label_create(screen);
        lv_obj_set_style_text_font(cli_message_label_, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(cli_message_label_, lv_color_hex(kCliForegroundColor), 0);
        lv_obj_set_style_text_align(cli_message_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(cli_message_label_, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(cli_message_label_, LV_HOR_RES - 82);
        lv_obj_align(cli_message_label_, LV_ALIGN_BOTTOM_MID, 0, -34);
        lv_obj_add_flag(cli_message_label_, LV_OBJ_FLAG_HIDDEN);

        RenderLocked("(^_^)", "Booting");
        ESP_ERROR_CHECK(esp_timer_start_periodic(cli_timer_, 450 * 1000));
    }

    virtual void SetStatus(const char* status) override {
        if (Contains(status, "error") || Contains(status, "failed") || Contains(status, "fail")) {
            cli_control_override_ = false;
            SetCliMode(CliMode::kError, true);
            return;
        }
        ApplyStateStatus();
    }

    virtual void SetEmotion(const char* emotion) override {
        if (cli_control_override_) {
            return;
        }

        if (Contains(emotion, "xmark") || Contains(emotion, "exclamation") || Contains(emotion, "error")) {
            SetCliMode(CliMode::kError, true);
        } else if (Contains(emotion, "sleepy")) {
            SetCliMode(CliMode::kSleeping, true);
        } else if (Contains(emotion, "thinking")) {
            SetCliMode(CliMode::kThinking, true);
        } else if (Contains(emotion, "happy") || Contains(emotion, "laughing")) {
            done_frames_left_ = 8;
            SetCliMode(CliMode::kDone, true);
        } else if (Contains(emotion, "neutral") || Contains(emotion, "microchip") || Contains(emotion, "link")) {
            ApplyStateStatus();
        }
    }

    virtual void SetChatMessage(const char* role, const char* content) override {
        DisplayLockGuard lock(this);
        bool show_system_message = role != nullptr && std::strcmp(role, "system") == 0 && content != nullptr && content[0] != '\0';
        if (cli_message_label_ != nullptr) {
            lv_label_set_text(cli_message_label_, show_system_message ? content : "");
            if (show_system_message) {
                lv_obj_remove_flag(cli_message_label_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(cli_message_label_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    virtual void ClearChatMessages() override {
        SetChatMessage("system", "");
    }

    virtual void SetPowerSaveMode(bool on) override {
        SetChatMessage("system", "");
        SetCliMode(on ? CliMode::kSleeping : CliMode::kReady, true);
    }
};

class MovecallMojiESP32S3 : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Button boot_button_;
    Display* display_;

    void InitializeCodecI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize SPI bus");
        spi_bus_config_t buscfg = GC9A01_PANEL_BUS_SPI_CONFIG(DISPLAY_SPI_SCLK_PIN, DISPLAY_SPI_MOSI_PIN,
                                    DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeGc9a01Display() {
        ESP_LOGI(TAG, "Init GC9A01 display");

        ESP_LOGI(TAG, "Install panel IO");
        esp_lcd_panel_io_handle_t io_handle = NULL;
        esp_lcd_panel_io_spi_config_t io_config = GC9A01_PANEL_IO_SPI_CONFIG(DISPLAY_SPI_CS_PIN, DISPLAY_SPI_DC_PIN, NULL, NULL);
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io_handle));

        ESP_LOGI(TAG, "Install GC9A01 panel driver");
        esp_lcd_panel_handle_t panel_handle = NULL;
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_SPI_RESET_PIN;
        panel_config.rgb_endian = LCD_RGB_ENDIAN_BGR;
        panel_config.bits_per_pixel = 16;

        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

        display_ = new CustomLcdDisplay(io_handle, panel_handle,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                    DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    MovecallMojiESP32S3() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeCodecI2c();
        InitializeSpi();
        InitializeGc9a01Display();
        InitializeButtons();
        GetBacklight()->RestoreBrightness();
    }

    virtual Led* GetLed() override {
        static SingleLed led_strip(BUILTIN_LED_GPIO);
        return &led_strip;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }
};

DECLARE_BOARD(MovecallMojiESP32S3);
