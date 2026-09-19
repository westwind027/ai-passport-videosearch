#include "application.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "display/lcd_display.h"
#include "mcp_server.h"
#include "mqtt_protocol.h"
#include "search/player_control_client.h"
#include "search/search_query.h"
#include "search/search_url.h"
#include "settings.h"
#include "system_info.h"
#include "text_glyph_payload.h"
#include "websocket_protocol.h"
#include "wifi_board.h"

#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

#define TAG "Application"

namespace {

constexpr size_t kSettingsItemCount = 4;
// Keep the public firmware free of a developer LAN address. Users can set the
// actual endpoint from Settings -> Service URL.
constexpr char kDefaultSearchServiceUrl[] = "http://video-search.local:8000";
constexpr uint32_t kActivationAudioDrainTimeoutMs = 20000;
constexpr uint32_t kActivationAudioCancelTimeoutMs = 1000;

constexpr const char* kLowerTextInputKeys[] = {
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l",   "m",   "n", "o",
    "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "ABC", "123", "<", "GO",
};
constexpr const char* kUpperTextInputKeys[] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L",   "M",   "N", "O",
    "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "abc", "123", "<", "GO",
};
constexpr const char* kSymbolTextInputKeys[] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "-", "_", ".", "@",   "#",   "$", "%",
    "&", "*", "!", "+", "=", "?", "/", ":", ";", "(", ")", "[", "]", "abc", "ABC", "<", "GO",
};

std::vector<std::string> GetTextInputKeys(TextInputPage page) {
    const char* const* keys = kLowerTextInputKeys;
    std::size_t count = std::size(kLowerTextInputKeys);
    if (page == TextInputPage::Upper) {
        keys = kUpperTextInputKeys;
        count = std::size(kUpperTextInputKeys);
    } else if (page == TextInputPage::Symbols) {
        keys = kSymbolTextInputKeys;
        count = std::size(kSymbolTextInputKeys);
    }

    std::vector<std::string> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        result.emplace_back(keys[index]);
    }
    return result;
}

}  // namespace

Application::Application() : notify_player_(audio_service_) {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {.callback =
                                                    [](void* arg) {
                                                        Application* app = (Application*)arg;
                                                        xEventGroupSetBits(app->event_group_,
                                                                           MAIN_EVENT_CLOCK_TICK);
                                                    },
                                                .arg = this,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "clock_timer",
                                                .skip_unhandled_events = true};
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    notify_player_.Stop();
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) { return state_machine_.TransitionTo(state); }

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();

    // Apply the assets partition right after SetupUI so the full CJK text
    // font is active before the first frame. Without this the pre-connection
    // pages (Wi-Fi picker, settings menu) render with the built-in basic
    // font and drop every character outside its small Latin charset. Apply
    // must stay after SetupUI because it rebinds widget fonts through
    // SetTheme(), which dereferences the widgets it created.
    auto& assets = Assets::GetInstance();
    if (assets.partition_valid()) {
        assets.Apply();
    }
    // Use the same reset path as every return to the home page. This also
    // restores the shared header/body/footer visibility if a board-specific
    // startup callback touched the display before the application loop began.
    display->ResetSearchContent();
    display->SetStatus(Lang::Strings::STANDBY);
    video_search_client_.InitializeWorkspace();

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    Settings audio_settings("audio", false);
    codec->SetOutputVolume(std::clamp<int>(
        static_cast<int>(audio_settings.GetInt("volume", codec->output_volume())), 0, 100));
    audio_service_.Initialize(codec);
    audio_service_.Start();

    // The search service is intentionally independent from the XiaoZhi
    // activation server. It is configured through the device settings page.
    Settings search_settings("search", false);
    search_service_url_ = search_settings.GetString("server_url", kDefaultSearchServiceUrl);
    if (search_service_url_.empty()) {
        search_service_url_ = kDefaultSearchServiceUrl;
    }
    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    callbacks.on_playback_drained = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
    };
    callbacks.on_playback_progress = [this](uint32_t playback_id, uint32_t media_position_ms) {
        notify_player_.OnPlaybackProgress(playback_id, media_position_ms);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();

        switch (event) {
            case NetworkEvent::Scanning:
                // Keep the search home shell visible during automatic startup.
                // The interactive Wi-Fi page owns its own status text.
                if (ui_mode_ != UiMode::WifiList && ui_mode_ != UiMode::TextInput) {
                    display->SetStatus(Lang::Strings::SCANNING_WIFI);
                }
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // Do not put the SSID over the search body. The signal icon
                    // in the shared header is the only network detail needed.
                    display->SetStatus(Lang::Strings::CONNECTING);
                }
                break;
            }
            case NetworkEvent::Connected: {
                display->SetStatus(Lang::Strings::ACTIVATION);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // The standalone application provisions Wi-Fi on the device.
                // Do not expose the legacy XiaoZhi web configuration page.
                display->SetStatus("选择 Wi-Fi");
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "warning",
                      Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO | MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE | MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED | MAIN_EVENT_NETWORK_DISCONNECTED | MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING | MAIN_EVENT_STOP_LISTENING | MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED | MAIN_EVENT_PLAYBACK_DRAINED | MAIN_EVENT_ENTER_SETTINGS |
        MAIN_EVENT_BUTTON_UP | MAIN_EVENT_BUTTON_DOWN | MAIN_EVENT_WIFI_SETUP |
        MAIN_EVENT_BUTTON_UP_LONG | MAIN_EVENT_BUTTON_DOWN_LONG;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            if (GetDeviceState() == kDeviceStateNotifying) {
                StopNotification();
            }
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_PLAYBACK_DRAINED) {
            if (audio_service_.IsPlaybackIdle()) {
                notify_player_.OnPlaybackDrained();
            }
            // Deferred listening start (auto mode): the playback queue has
            // drained, so it is now safe to enable voice processing.
            if (pending_listening_start_ && GetDeviceState() == kDeviceStateListening &&
                audio_service_.IsPlaybackIdle()) {
                pending_listening_start_ = false;
                StartListeningAudio();
            }
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_ENTER_SETTINGS) {
            HandleEnterSettingsEvent();
        }

        if (bits & MAIN_EVENT_WIFI_SETUP) {
            HandleWifiSetupEvent();
        }

        if (bits & MAIN_EVENT_BUTTON_UP_LONG) {
            HandleUpButtonEvent(true);
        } else if (bits & MAIN_EVENT_BUTTON_UP) {
            HandleUpButtonEvent();
        }

        if (bits & MAIN_EVENT_BUTTON_DOWN_LONG) {
            HandleDownButtonEvent(true);
        } else if (bits & MAIN_EVENT_BUTTON_DOWN) {
            HandleDownButtonEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    // Drop the remaining packets. Leaving them in the queue would
                    // stall the Opus codec task (it waits for queue space), which in
                    // turn deadlocks the whole audio input pipeline, as no new
                    // MAIN_EVENT_SEND_AUDIO event would ever be triggered again.
                    while (audio_service_.PopPacketFromSendQueue())
                        ;
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();

            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
                // SystemInfo::PrintTaskList();
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        if (ui_mode_ == UiMode::WifiList || ui_mode_ == UiMode::TextInput) {
            if (auto* wifi_board = dynamic_cast<WifiBoard*>(&Board::GetInstance())) {
                wifi_board->StopInteractiveWifiScan();
            }
            ui_mode_ = UiMode::Home;
            wifi_connection_pending_ = false;
            Board::GetInstance().GetDisplay()->HideSettingsMenu();
        }
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate(
            [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                app->ActivationTask();
                app->activation_task_handle_ = nullptr;
                vTaskDelete(NULL);
            },
            "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateNotifying) {
        StopNotification();
    }
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(Lang::Strings::STANDBY);
    display->ResetSearchContent();

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }

    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_download", Lang::Sounds::OGG_UPGRADE);

        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetSearchContent("", Lang::Strings::PLEASE_WAIT);

        bool success =
            assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                Schedule([display, message = std::string(buffer)]() {
                    display->SetSearchContent("", message.c_str());
                });
            });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetSearchContent("", "");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10;  // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            int error_message_length =
                snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err,
                         ota_->GetCheckVersionUrl().c_str());
            if (error_message_length < 0 ||
                error_message_length >= static_cast<int>(sizeof(error_message))) {
                snprintf(error_message, sizeof(error_message), "code=%d", err);
            }

            char buffer[320];
            int alert_message_length =
                snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED,
                         retry_delay, error_message);
            if (alert_message_length < 0 ||
                alert_message_length >= static_cast<int>(sizeof(buffer))) {
                snprintf(buffer, sizeof(buffer), "code=%d", err);
            }
            Alert(Lang::Strings::ERROR, buffer, "cloud_off", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay,
                     retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2;  // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10;  // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return;  // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ESP_ERR_NO_MEM;
            try {
                err = ota_->Activate();
            } catch (const std::bad_alloc&) {
                // A failed C++ allocation must follow the normal retry path.
                // Letting it escape from this task calls std::terminate and
                // reboots the device in the middle of pairing.
                ESP_LOGE(TAG, "Activation request ran out of memory");
            }
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() { DismissAlert(); });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });

    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });

    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG,
                     "Server sample rate %d does not match device output sample rate %d, "
                     "resampling may cause distortion",
                     protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });

    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            if (ui_mode_ == UiMode::Settings || ui_mode_ == UiMode::WifiList ||
                ui_mode_ == UiMode::TextInput ||
                search_session_.state() != SearchSessionState::Idle) {
                if (GetDeviceState() != kDeviceStateIdle) {
                    SetDeviceState(kDeviceStateIdle);
                }
                return;
            }
            SetDeviceState(kDeviceStateIdle);
        });
    });

    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Incoming JSON message has no type");
            return;
        }
        if (strcmp(type->valuestring, "notify") == 0) {
            auto audio_url = cJSON_GetObjectItem(root, "audio_url");
            if (!cJSON_IsString(audio_url) || audio_url->valuestring[0] == '\0') {
                ESP_LOGW(TAG, "Notify message requires audio_url");
                return;
            }

            std::vector<NotifySubtitle> subtitles;
            auto subtitles_json = cJSON_GetObjectItem(root, "subtitles");
            if (subtitles_json != nullptr && !cJSON_IsArray(subtitles_json)) {
                ESP_LOGW(TAG, "Notify subtitles must be an array");
                return;
            }
            if (cJSON_IsArray(subtitles_json)) {
                cJSON* item = nullptr;
                cJSON_ArrayForEach (item, subtitles_json) {
                    auto start_ms = cJSON_GetObjectItem(item, "start_ms");
                    auto text = cJSON_GetObjectItem(item, "text");
                    if (!cJSON_IsNumber(start_ms) || start_ms->valuedouble < 0 ||
                        start_ms->valuedouble > std::numeric_limits<uint32_t>::max() ||
                        !cJSON_IsString(text)) {
                        ESP_LOGW(TAG, "Ignoring invalid notify subtitle");
                        continue;
                    }
                    subtitles.push_back({.start_ms = static_cast<uint32_t>(start_ms->valuedouble),
                                         .text = text->valuestring});
                }
            }

            Schedule([this, url = std::string(audio_url->valuestring),
                      subtitles = std::move(subtitles)]() mutable {
                StartNotification(std::move(url), std::move(subtitles));
            });
        } else if (strcmp(type->valuestring, "tts") == 0) {
            // This application is a recognizer front-end, not a XiaoZhi
            // conversation client. STT is consumed below; TTS state and
            // sentence messages must not reopen the old dialogue page.
            return;
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                std::vector<TextGlyph> glyphs;
                uint8_t bpp = 0;
                if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                    glyphs.clear();
                }
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring),
                          glyphs = std::move(glyphs), bpp, this]() {
                    if (search_session_.state() == SearchSessionState::Searching ||
                        search_session_.state() == SearchSessionState::Results ||
                        search_session_.state() == SearchSessionState::Error) {
                        return;
                    }
                    const std::string query = NormalizeSearchQuery(message);
                    display->AddTextGlyphs(glyphs, bpp);
                    display->SetSearchContent(query.c_str(), "识别完成，正在搜索...");
                    if (search_session_.state() == SearchSessionState::Listening) {
                        HandleSearchText(query);
                    }
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            // LLM replies belong to the removed XiaoZhi dialogue UI.
            return;
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() { Reboot(); });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring,
                      Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            // Custom XiaoZhi messages are deliberately not rendered by the
            // standalone search application.
            ESP_LOGI(TAG, "Ignoring custom XiaoZhi message");
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });

    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{
        {digit_sound{'0', Lang::Sounds::OGG_0}, digit_sound{'1', Lang::Sounds::OGG_1},
         digit_sound{'2', Lang::Sounds::OGG_2}, digit_sound{'3', Lang::Sounds::OGG_3},
         digit_sound{'4', Lang::Sounds::OGG_4}, digit_sound{'5', Lang::Sounds::OGG_5},
         digit_sound{'6', Lang::Sounds::OGG_6}, digit_sound{'7', Lang::Sounds::OGG_7},
         digit_sound{'8', Lang::Sounds::OGG_8}, digit_sound{'9', Lang::Sounds::OGG_9}}};

    // The prompt and digits transiently consume about 9 KiB for Opus PCM and
    // resampling. Drain them before the activation TLS handshake starts so
    // the two internal-SRAM peaks cannot overlap.
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
                               [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }

    if (!audio_service_.WaitForPlaybackIdle(kActivationAudioDrainTimeoutMs)) {
        ESP_LOGW(TAG, "Activation prompt did not drain in time; cancelling audio");
        audio_service_.ResetDecoder();
        audio_service_.WaitForPlaybackIdle(kActivationAudioCancelTimeoutMs);
    }
    if (!audio_service_.StopOutputIfIdle()) {
        ESP_LOGW(TAG, "Activation prompt output is still active");
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion,
                        const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    // Alerts use the fixed search page's detail area. This keeps activation
    // and error text visible without bringing back the old chat bubble.
    display->SetSearchContent("", message != nullptr ? message : "");
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->ResetSearchContent();
    }
}

void Application::ToggleChatState() { xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT); }

void Application::EnterSettings() { xEventGroupSetBits(event_group_, MAIN_EVENT_ENTER_SETTINGS); }

void Application::EnterWifiSetup() { xEventGroupSetBits(event_group_, MAIN_EVENT_WIFI_SETUP); }

void Application::HandleUpButton() { xEventGroupSetBits(event_group_, MAIN_EVENT_BUTTON_UP); }

void Application::HandleDownButton() { xEventGroupSetBits(event_group_, MAIN_EVENT_BUTTON_DOWN); }

void Application::HandleUpButtonLong() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_BUTTON_UP_LONG);
}

void Application::HandleDownButtonLong() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_BUTTON_DOWN_LONG);
}

void Application::HandleWifiSetupEvent() {
    auto& board = Board::GetInstance();
    auto* wifi_board = dynamic_cast<WifiBoard*>(&board);
    auto display = board.GetDisplay();
    if (wifi_board == nullptr) {
        display->ShowNotification("当前板卡不支持设备端配网");
        return;
    }

    auto state = GetDeviceState();
    if (state == kDeviceStateIdle || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking || state == kDeviceStateNotifying) {
        video_search_client_.Cancel();
        search_session_.Reset();
        ++search_image_generation_;
        ClearSearchImage();
        ResetProtocol();
    }

    ui_mode_ = UiMode::WifiList;
    settings_volume_mode_ = false;
    wifi_networks_.clear();
    wifi_network_index_ = 0;
    wifi_scan_in_progress_ = true;
    wifi_connection_pending_ = false;
    display->HideSettingsMenu();
    display->SetSearchContent("", "");
    display->SetStatus("选择 Wi-Fi");
    if (GetDeviceState() != kDeviceStateWifiConfiguring) {
        SetDeviceState(kDeviceStateWifiConfiguring);
    }
    RenderWifiList();

    wifi_board->StartInteractiveWifiScan([this](std::vector<WifiScanResult> networks) {
        Schedule([this, networks = std::move(networks)]() mutable {
            if (ui_mode_ != UiMode::WifiList) {
                return;
            }
            if (networks.size() > 7) {
                networks.resize(7);
            }
            wifi_networks_ = std::move(networks);
            wifi_network_index_ = 0;
            wifi_scan_in_progress_ = false;
            RenderWifiList();
        });
    });
}

void Application::RenderWifiList() {
    auto display = Board::GetInstance().GetDisplay();
    std::vector<std::string> items;
    items.reserve(wifi_networks_.size() + 1);
    for (const auto& network : wifi_networks_) {
        std::string item = network.secure ? "[锁] " : "[开] ";
        item += network.ssid;
        item += "  ";
        item += std::to_string(network.rssi);
        item += "dBm";
        items.push_back(std::move(item));
    }
    if (items.empty()) {
        items.push_back(wifi_scan_in_progress_ ? "正在扫描附近 Wi-Fi..." : "未发现可用 Wi-Fi");
    }

    const std::size_t selected =
        wifi_networks_.empty() ? 0 : std::min(wifi_network_index_, wifi_networks_.size() - 1);
    // The shared status bar already identifies this page. Do not draw a
    // second "选择 Wi-Fi" title inside the list body.
    display->ShowSettingsMenu(items, selected, nullptr, "上/下选择  OK进入  长按返回");
}

void Application::RenderTextInput(const char* status) {
    auto display = Board::GetInstance().GetDisplay();
    const bool wifi_password = text_input_target_ == TextInputTarget::WifiPassword;
    const char* title = wifi_password ? "网络设置" : "服务地址配置";
    const char* context = wifi_password ? wifi_selected_ssid_.c_str() : nullptr;
    const std::string input_status = status != nullptr ? status : "上/下移动  长按跨行";

    auto keys = GetTextInputKeys(text_input_page_);
    if (!keys.empty()) {
        text_input_index_ %= keys.size();
    } else {
        text_input_index_ = 0;
    }
    display->ShowTextInput(title, text_input_value_.c_str(), wifi_password, keys, text_input_index_,
                           text_input_page_, input_status.c_str(), "OK选择  GO提交  长按返回",
                           context);
}

void Application::HandleWifiListConfirm() {
    if (wifi_networks_.empty()) {
        return;
    }

    wifi_network_index_ = std::min(wifi_network_index_, wifi_networks_.size() - 1);
    wifi_selected_ssid_ = wifi_networks_[wifi_network_index_].ssid;
    text_input_value_.clear();
    text_input_target_ = TextInputTarget::WifiPassword;
    text_input_page_ = TextInputPage::Lower;
    text_input_index_ = 0;
    text_input_replace_existing_ = false;
    wifi_connection_pending_ = false;
    ui_mode_ = UiMode::TextInput;
    Board::GetInstance().GetDisplay()->SetStatus("输入 Wi-Fi 密码");
    RenderTextInput();
}

void Application::HandleTextInputKey() {
    if (wifi_connection_pending_) {
        return;
    }

    auto keys = GetTextInputKeys(text_input_page_);
    if (keys.empty()) {
        return;
    }
    text_input_index_ %= keys.size();
    const std::string key = keys[text_input_index_];

    if (key == "ABC") {
        text_input_page_ = TextInputPage::Upper;
        text_input_index_ = 0;
    } else if (key == "abc") {
        text_input_page_ = TextInputPage::Lower;
        text_input_index_ = 0;
    } else if (key == "123") {
        text_input_page_ = TextInputPage::Symbols;
        text_input_index_ = 0;
    } else if (key == "<") {
        if (!text_input_value_.empty()) {
            text_input_value_.pop_back();
            text_input_replace_existing_ = false;
        } else if (text_input_target_ == TextInputTarget::WifiPassword) {
            ui_mode_ = UiMode::WifiList;
            RenderWifiList();
            return;
        }
    } else if (key == "GO") {
        CommitTextInput();
        return;
    } else {
        const std::size_t maximum = text_input_target_ == TextInputTarget::WifiPassword ? 63 : 160;
        if (text_input_value_.size() + key.size() <= maximum) {
            if (text_input_replace_existing_) {
                text_input_value_.clear();
                text_input_replace_existing_ = false;
            }
            text_input_value_ += key;
        }
    }
    RenderTextInput();
}

void Application::CommitTextInput() {
    if (wifi_connection_pending_) {
        return;
    }

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (text_input_target_ == TextInputTarget::WifiPassword) {
        if (wifi_selected_ssid_.empty()) {
            return;
        }
        bool secure = true;
        for (const auto& network : wifi_networks_) {
            if (network.ssid == wifi_selected_ssid_) {
                secure = network.secure;
                break;
            }
        }
        if (secure && text_input_value_.empty()) {
            RenderTextInput("请输入 Wi-Fi 密码");
            return;
        }

        auto* wifi_board = dynamic_cast<WifiBoard*>(&board);
        if (wifi_board == nullptr) {
            RenderTextInput("当前板卡不支持配网");
            return;
        }
        wifi_connection_pending_ = true;
        RenderTextInput("正在连接，请稍候...");
        wifi_board->ConnectInteractiveWifi(wifi_selected_ssid_, text_input_value_);
        return;
    }

    // The editor keeps the scheme out of the value; re-attach it before
    // validation and storage.
    std::string server_url = text_input_scheme_ + text_input_value_;
    if (!IsSupportedSearchUrl(server_url)) {
        RenderTextInput("地址必须以 http:// 或 https:// 开头");
        return;
    }

    Settings settings("search", true);
    settings.SetString("server_url", server_url);
    search_service_url_ = server_url;
    ui_mode_ = UiMode::Settings;
    text_input_replace_existing_ = false;
    display->ShowNotification("服务地址已保存");
    RenderSettingsMenu();
}

void Application::HandleEnterSettingsEvent() {
    auto state = GetDeviceState();
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    if (ui_mode_ == UiMode::WifiList) {
        // Wi-Fi is a child page of settings. Long OK always goes back to the
        // settings list; the legacy hotspot/web configuration page is not a
        // part of this application anymore.
        if (auto* wifi_board = dynamic_cast<WifiBoard*>(&board)) {
            wifi_board->StopInteractiveWifiScan();
        }
        ui_mode_ = UiMode::Settings;
        settings_volume_mode_ = false;
        wifi_scan_in_progress_ = false;
        wifi_connection_pending_ = false;
        RenderSettingsMenu();
        return;
    }
    if (ui_mode_ == UiMode::TextInput) {
        // Text input is also a child page. GO is the explicit save/submit
        // action; long OK cancels the edit and returns to its owner.
        if (text_input_target_ == TextInputTarget::WifiPassword) {
            ui_mode_ = UiMode::WifiList;
            wifi_connection_pending_ = false;
            RenderWifiList();
        } else {
            ui_mode_ = UiMode::Settings;
            text_input_replace_existing_ = false;
            RenderSettingsMenu();
        }
        return;
    }

    if (ui_mode_ == UiMode::Settings) {
        if (settings_volume_mode_) {
            settings_volume_mode_ = false;
            RenderSettingsMenu();
            return;
        }
        ui_mode_ = UiMode::Home;
        display->HideSettingsMenu();
        search_session_.Reset();
        ++search_image_generation_;
        ClearSearchImage();
        display->ClearChatMessages();
        display->ResetSearchContent();
        display->SetStatus(Lang::Strings::STANDBY);
        return;
    }

    // A device without credentials, or one waiting for Wi-Fi after a
    // connection timeout, enters the same on-device picker as the settings
    // menu. There is no web/AP configuration fallback in this app.
    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        EnterWifiSetup();
        return;
    }
    if (state == kDeviceStateActivating || state == kDeviceStateUpgrading) {
        return;
    }

    video_search_client_.Cancel();
    search_session_.Reset();
    ++search_image_generation_;
    ClearSearchImage();

    ui_mode_ = UiMode::Settings;
    settings_index_ = 0;
    settings_volume_mode_ = false;

    if (state == kDeviceStateNotifying) {
        StopNotification();
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetDeviceState(kDeviceStateIdle);
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
            protocol_->CloseAudioChannel();
        }
        SetDeviceState(kDeviceStateIdle);
    }

    RenderSettingsMenu();
}

void Application::HandleUpButtonEvent(bool fast) {
    if (ui_mode_ == UiMode::WifiList) {
        if (fast) {
            return;
        }
        if (!wifi_networks_.empty()) {
            wifi_network_index_ =
                (wifi_network_index_ + wifi_networks_.size() - 1) % wifi_networks_.size();
            RenderWifiList();
        }
        return;
    }
    if (ui_mode_ == UiMode::TextInput) {
        const auto keys = GetTextInputKeys(text_input_page_);
        if (!keys.empty()) {
            const std::size_t step = fast ? std::min<std::size_t>(6, keys.size()) : 1;
            text_input_index_ = (text_input_index_ + keys.size() - step) % keys.size();
            RenderTextInput();
        }
        return;
    }
    if (ui_mode_ == UiMode::Settings) {
        if (fast) {
            return;
        }
        if (settings_volume_mode_) {
            AdjustVolume(5);
        } else {
            settings_index_ = (settings_index_ + kSettingsItemCount - 1) % kSettingsItemCount;
            RenderSettingsMenu();
        }
        return;
    }

    if (fast) {
        // Long-press UP on a full-screen result asks the backend web player
        // to open the current media in fullscreen. Match the web player's
        // own click-to-play time selection (openPlayer): start at the
        // preview frame's exact timestamp and fall back to the scene start
        // only when the service did not provide one.
        if (search_session_.state() == SearchSessionState::Results &&
            !search_session_.response().results.empty()) {
            const auto& result = search_session_.response().results[search_result_index_];
            if (!result.media_id.empty()) {
                const double play_time =
                    result.preview_time >= 0.0 ? result.preview_time : result.start;
                Board::GetInstance().GetDisplay()->SetStatus("远程播放...");
                if (!PlayerControlClient::Play(video_search_client_, search_service_url_,
                                               result.media_id.c_str(), play_time)) {
                    Board::GetInstance().GetDisplay()->SetStatus("设备忙，请稍后再试");
                }
            }
        }
        return;
    }

    if (search_session_.state() == SearchSessionState::Results &&
        !search_session_.response().results.empty()) {
        const size_t count = search_session_.response().results.size();
        const size_t previous = search_result_index_ == 0 ? count - 1 : search_result_index_ - 1;
        ShowSearchResult(previous);
    } else if (GetDeviceState() == kDeviceStateIdle) {
        AdjustVolume(5);
    }
}

void Application::HandleDownButtonEvent(bool fast) {
    if (ui_mode_ == UiMode::WifiList) {
        if (fast) {
            return;
        }
        if (!wifi_networks_.empty()) {
            wifi_network_index_ = (wifi_network_index_ + 1) % wifi_networks_.size();
            RenderWifiList();
        }
        return;
    }
    if (ui_mode_ == UiMode::TextInput) {
        const auto keys = GetTextInputKeys(text_input_page_);
        if (!keys.empty()) {
            const std::size_t step = fast ? std::min<std::size_t>(6, keys.size()) : 1;
            text_input_index_ = (text_input_index_ + step) % keys.size();
            RenderTextInput();
        }
        return;
    }
    if (ui_mode_ == UiMode::Settings) {
        if (fast) {
            return;
        }
        if (settings_volume_mode_) {
            AdjustVolume(-5);
        } else {
            settings_index_ = (settings_index_ + 1) % kSettingsItemCount;
            RenderSettingsMenu();
        }
        return;
    }

    if (fast) {
        // Long-press DOWN on a full-screen result closes the backend web
        // player popup and exits its fullscreen.
        if (search_session_.state() == SearchSessionState::Results &&
            !search_session_.response().results.empty()) {
            Board::GetInstance().GetDisplay()->SetStatus("关闭远程播放...");
            if (!PlayerControlClient::Close(video_search_client_, search_service_url_)) {
                Board::GetInstance().GetDisplay()->SetStatus("设备忙，请稍后再试");
            }
        }
        return;
    }

    if (search_session_.state() == SearchSessionState::Results &&
        !search_session_.response().results.empty()) {
        const size_t count = search_session_.response().results.size();
        ShowSearchResult((search_result_index_ + 1) % count);
    } else if (GetDeviceState() == kDeviceStateIdle) {
        AdjustVolume(-5);
    }
}

void Application::HandleSettingsConfirm() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    // Volume is a child page of the settings list. A short OK press confirms
    // the current value and returns to that list; a long OK press uses the
    // same parent-navigation path in HandleEnterSettingsEvent().
    if (settings_volume_mode_) {
        settings_volume_mode_ = false;
        RenderSettingsMenu();
        return;
    }

    switch (settings_index_) {
        case 0:
            settings_volume_mode_ = true;
            RenderVolumeMenu();
            break;
        case 1: {
            settings_volume_mode_ = false;
            display->HideSettingsMenu();
            EnterWifiSetup();
            break;
        }
        case 2: {
            settings_volume_mode_ = false;
            text_input_target_ = TextInputTarget::SearchService;
            text_input_value_ =
                search_service_url_.empty() ? kDefaultSearchServiceUrl : search_service_url_;
            // Edit the address without its scheme: the value box fits a
            // single line and backspace never eats into the scheme. The
            // scheme is remembered and re-attached when the input commits.
            text_input_scheme_ = "http://";
            for (const char* prefix : {"https://", "http://"}) {
                if (text_input_value_.rfind(prefix, 0) == 0) {
                    text_input_scheme_ = prefix;
                    text_input_value_.erase(0, std::strlen(prefix));
                    break;
                }
            }
            text_input_page_ = TextInputPage::Lower;
            text_input_index_ = 0;
            text_input_replace_existing_ = true;
            wifi_connection_pending_ = false;
            ui_mode_ = UiMode::TextInput;
            RenderTextInput();
            break;
        }
        case 3:
        default:
            ui_mode_ = UiMode::Home;
            settings_volume_mode_ = false;
            display->HideSettingsMenu();
            search_session_.Reset();
            ++search_image_generation_;
            ClearSearchImage();
            display->ClearChatMessages();
            display->ResetSearchContent();
            display->SetStatus(Lang::Strings::STANDBY);
            break;
    }
}

void Application::RenderSettingsMenu() {
    auto display = Board::GetInstance().GetDisplay();
    auto codec = Board::GetInstance().GetAudioCodec();
    const int volume = codec == nullptr ? 0 : codec->output_volume();

    const std::vector<std::string> items = {
        "音量 " + std::to_string(volume),
        "网络配置",
        "服务地址配置",
        "返回主页面",
    };

    display->ClearChatMessages();
    display->SetStatus("设置");
    // "设置" is already rendered by the shared status bar. The list body
    // contains only the selectable rows, so the page title is not duplicated.
    display->ShowSettingsMenu(items, settings_index_, nullptr, "OK进入  长按返回");
}

void Application::RenderVolumeMenu() {
    auto display = Board::GetInstance().GetDisplay();
    auto codec = Board::GetInstance().GetAudioCodec();
    const int volume = codec == nullptr ? 0 : codec->output_volume();
    const std::vector<std::string> items = {"音量 " + std::to_string(volume)};

    display->ClearChatMessages();
    display->SetStatus("音量");
    display->ShowSettingsMenu(items, 0, "音量设置", "上/下调节  OK确认  长按返回设置");
}

void Application::AdjustVolume(int delta) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }

    const int volume = std::clamp(codec->output_volume() + delta, 0, 100);
    codec->SetOutputVolume(volume);
    Settings settings("audio", true);
    settings.SetInt("volume", volume);

    if (ui_mode_ == UiMode::Settings) {
        if (settings_volume_mode_) {
            RenderVolumeMenu();
        } else {
            RenderSettingsMenu();
        }
    } else {
        Board::GetInstance().GetDisplay()->ShowNotification(Lang::Strings::VOLUME +
                                                            std::to_string(volume));
    }
}

void Application::ClearSearchImage() {
    auto* lcd_display = dynamic_cast<LcdDisplay*>(Board::GetInstance().GetDisplay());
    if (lcd_display != nullptr) {
        lcd_display->SetSearchResultImage(nullptr);
    }
}

void Application::HandleSearchText(const std::string& text) {
    const std::string query = NormalizeSearchQuery(text);
    if (query.empty() || search_session_.state() != SearchSessionState::Listening) {
        return;
    }

    auto display = Board::GetInstance().GetDisplay();
    // Release the XiaoZhi audio-channel resources before creating the HTTP
    // search task. On the C3, keeping the UDP/TLS channel and opening the
    // search connection at the same time can exhaust the heap. The resulting
    // allocation failure used to escape from the MQTT task and call abort().
    if (protocol_) {
        protocol_->SendStopListening();
        protocol_->SendAbortSpeaking(kAbortReasonNone);
        protocol_->CloseAudioChannel();
    }
    // Search is now independent of XiaoZhi audio. Stop the audio processor
    // before allocating the HTTP client, and keep WakeNet disabled while the
    // search session owns the limited C3 heap. The idle state callback runs
    // asynchronously, so doing this here closes the allocation race.
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(false);
    audio_service_.ReleaseWakeWordResources();
    // The idle power timer would keep the codec input open for another 25s;
    // close it now so the search/image phase starts with the input chain's
    // memory already released.
    audio_service_.RequestInputStop();
    ESP_LOGI(TAG, "Search start: heap free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));

    if (search_service_url_.empty()) {
        search_session_.StartSearch(query, 1);
        search_session_.SetError(1, "未配置服务地址");
        display->SetStatus("搜索失败");
        display->SetSearchContent(query.c_str(), "请先在设置中配置服务地址");
    } else {
        const uint32_t request_id = video_search_client_.Search(
            search_service_url_, query,
            [this](uint32_t id, bool success, const SearchResponse* response, std::string error) {
                Schedule([this, id, success, response, error = std::move(error)]() mutable {
                    HandleSearchCompleted(id, success, response, std::move(error));
                });
            });
        if (request_id == 0 || !search_session_.StartSearch(query, request_id)) {
            video_search_client_.Cancel();
            search_session_.StartSearch(query, request_id == 0 ? 1 : request_id);
            search_session_.SetError(request_id == 0 ? 1 : request_id, "搜索请求无法启动");
            display->SetStatus("搜索失败");
            display->SetSearchContent(query.c_str(), "搜索请求无法启动");
        } else {
            display->SetStatus("搜索中");
            display->SetSearchContent(query.c_str(), "正在搜索...");
        }
    }

    // STT is the terminal event for this interaction. Keep the device idle
    // while the independent search request is running.
    SetDeviceState(kDeviceStateIdle);
}

void Application::HandleSearchCompleted(uint32_t request_id, bool success,
                                        const SearchResponse* response, std::string error) {
    if (!success) {
        if (!search_session_.SetError(request_id, std::move(error))) {
            return;
        }
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus("搜索失败");
        display->SetSearchContent(search_session_.query().c_str(), search_session_.error().c_str());
        return;
    }

    if (!search_session_.SetResults(request_id, response)) {
        return;
    }

    search_result_index_ = 0;
    if (search_session_.response().results.empty()) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus("搜索完成");
        display->SetSearchContent(search_session_.query().c_str(), "没有找到相关视频");
        return;
    }
    ShowSearchResult(0);
}

void Application::ShowSearchResult(size_t index) {
    if (search_session_.state() != SearchSessionState::Results ||
        index >= search_session_.response().results.size()) {
        return;
    }

    search_result_index_ = index;
    const auto& result = search_session_.response().results[index];
    auto display = Board::GetInstance().GetDisplay();
    const char* title = result.title.empty() ? result.media_id.c_str() : result.title.c_str();
    std::string message = std::to_string(index + 1) + "/" +
                          std::to_string(search_session_.response().results.size()) + " " + title;
    if (!result.caption.empty()) {
        message += '\n';
        message += result.caption.c_str();
    }
    display->SetSearchContent(search_session_.query().c_str(), message.c_str());
    video_search_client_.Cancel();
    auto* lcd_display = dynamic_cast<LcdDisplay*>(display);
    uint32_t image_epoch = 0;
    if (lcd_display != nullptr) {
        image_epoch = lcd_display->BeginSearchImageTransition();
    }

    const uint32_t generation = ++search_image_generation_;
    if (result.image_url.empty()) {
        display->SetStatus("搜索完成");
        return;
    }

    // No separate low-memory thumbnail pipeline: every result streams the
    // small variant straight onto the panel in full-screen mode. This keeps
    // one image path and fits the C3's ~26 KiB free heap budget.
    display->SetStatus("图片加载中");
    if (lcd_display == nullptr ||
        video_search_client_.LoadImageBlocks(
            search_session_.request_id(), result.image_url.c_str(), SearchImageVariant::Small,
            [lcd_display, image_epoch](uint32_t, const Rgb565ImageBlock& block) {
                return lcd_display->DrawSearchResultImageBlock(block, image_epoch);
            },
            [this, generation](uint32_t request_id, bool success, size_t width, size_t height,
                               std::string error) {
                Schedule([this, request_id, generation, success, width, height,
                          error = std::move(error)]() mutable {
                    HandleSearchImageStreamCompleted(request_id, generation, success, width, height,
                                                     std::move(error));
                });
            }) == 0) {
        display->SetStatus("图片加载失败");
    }
}

void Application::HandleSearchImageStreamCompleted(uint32_t request_id, uint32_t generation,
                                                   bool success, size_t width, size_t height,
                                                   std::string error) {
    if (generation != search_image_generation_ || request_id != search_session_.request_id() ||
        search_session_.state() != SearchSessionState::Results) {
        return;
    }

    auto display = Board::GetInstance().GetDisplay();
    auto* lcd_display = dynamic_cast<LcdDisplay*>(display);
    if (lcd_display != nullptr) {
        lcd_display->EndSearchResultImageStream(success);
    }
    if (!success) {
        display->SetStatus("图片加载失败");
        if (!error.empty()) {
            display->SetSearchContent(search_session_.query().c_str(), error.c_str());
        }
        return;
    }

    ESP_LOGI(TAG, "Streamed search image: %ux%u", static_cast<unsigned>(width),
             static_cast<unsigned>(height));
    display->SetStatus("搜索完成");
}

void Application::StartListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING); }

void Application::StopListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING); }

void Application::HandleToggleChatEvent() {
    if (ui_mode_ == UiMode::Settings) {
        HandleSettingsConfirm();
        return;
    }
    if (ui_mode_ == UiMode::WifiList) {
        HandleWifiListConfirm();
        return;
    }
    if (ui_mode_ == UiMode::TextInput) {
        HandleTextInputKey();
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateNotifying) {
        StopNotification();
        state = kDeviceStateIdle;
    }

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (search_session_.state() == SearchSessionState::Searching) {
            video_search_client_.Cancel();
            search_session_.Reset();
        }
        if (!search_session_.StartListening()) {
            return;
        }
        ++search_image_generation_;
        ClearSearchImage();

        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        search_session_.Reset();
        ++search_image_generation_;
        ClearSearchImage();
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error)
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateNotifying) {
        StopNotification();
        state = kDeviceStateIdle;
    }

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (search_session_.state() == SearchSessionState::Results ||
            search_session_.state() == SearchSessionState::Error) {
            search_session_.Reset();
            ++search_image_generation_;
            ClearSearchImage();
        }
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() { ContinueOpenAudioChannel(kListeningModeManualStop); });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateNotifying) {
        StopNotification();
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        search_session_.Reset();
        ++search_image_generation_;
        ClearSearchImage();
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        BeginWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateNotifying) {
        StopNotification();
        BeginWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue())
            ;

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::BeginWakeWordInvoke(const std::string& wake_word) {
    // Must run in the main task with the device in idle state
    audio_service_.EncodeWakeWord();

    // Always pass through the connecting state, even if the audio channel is
    // already opened. ContinueWakeWordInvoke() rejects any other state, so
    // skipping this transition would silently drop the wake word invocation.
    if (!SetDeviceState(kDeviceStateConnecting)) {
        // Wake word detection was stopped by the detection itself; restore it
        // so the device does not become unresponsive to wake words.
        audio_service_.EnableWakeWordDetection(true);
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        // Schedule to let the state change be processed first (UI update),
        // then continue with OpenAudioChannel which may block for ~1 second
        Schedule([this, wake_word]() { ContinueWakeWordInvoke(wake_word); });
        return;
    }
    // Channel already opened, continue directly
    ContinueWakeWordInvoke(wake_word);
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error), and
            // wake word detection is re-enabled by the idle state handler.
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;
    // Any state change invalidates a pending deferred listening start;
    // the Listening case below re-arms it when needed.
    pending_listening_start_ = false;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle: {
            if (ui_mode_ == UiMode::Settings) {
                display->SetStatus("设置");
            } else if (ui_mode_ == UiMode::TextInput) {
                display->SetStatus("编辑中");
            } else if (ui_mode_ == UiMode::WifiList) {
                display->SetStatus("选择 Wi-Fi");
            } else if (search_session_.state() == SearchSessionState::Searching) {
                display->SetStatus("搜索中");
            } else if (search_session_.state() == SearchSessionState::Results) {
                display->SetStatus("搜索完成");
            } else if (search_session_.state() == SearchSessionState::Error) {
                display->SetStatus("搜索失败");
            } else {
                display->SetStatus(Lang::Strings::STANDBY);
                display->ClearChatMessages();  // Clear messages first
                display->ResetSearchContent();
            }
            audio_service_.EnableVoiceProcessing(false);
            const bool search_active = search_session_.state() == SearchSessionState::Searching ||
                                       search_session_.state() == SearchSessionState::Results ||
                                       search_session_.state() == SearchSessionState::Error;
            if (search_active) {
                audio_service_.EnableWakeWordDetection(false);
                audio_service_.ReleaseWakeWordResources();
            } else {
                audio_service_.EnableWakeWordDetection(true);
            }
            break;
        }
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for the playback queue to drain before enabling
                // voice processing. This prevents audio truncation when STOP arrives
                // late due to network jitter. Instead of blocking the main loop here,
                // defer the start until MAIN_EVENT_PLAYBACK_DRAINED arrives.
                if (listening_mode_ == kListeningModeAutoStop && !audio_service_.IsPlaybackIdle()) {
                    pending_listening_start_ = true;
                } else {
                    StartListeningAudio();
                }
            } else {
                ConfigureWakeWordForListening();
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateNotifying:
            display->SetStatus(Lang::Strings::SPEAKING);
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            if (ui_mode_ == UiMode::WifiList) {
                display->SetStatus("选择 Wi-Fi");
            } else if (ui_mode_ == UiMode::TextInput) {
                display->SetStatus("输入 Wi-Fi 密码");
            }
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::StartListeningAudio() {
    // Runs in the main loop, either directly from HandleStateChangedEvent or
    // deferred via MAIN_EVENT_PLAYBACK_DRAINED once the playback queue drains.
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }

    // Send the start listening command
    protocol_->SendStartListening(listening_mode_);
    audio_service_.EnableVoiceProcessing(true);

    ConfigureWakeWordForListening();

    // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
    if (play_popup_on_listening_) {
        play_popup_on_listening_ = false;
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
    }
}

void Application::ConfigureWakeWordForListening() {
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
    // Enable wake word detection in listening mode (configured via Kconfig)
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
    // Disable wake word detection in listening mode
    audio_service_.EnableWakeWordDetection(false);
#endif
}

void Application::StartNotification(std::string audio_url, std::vector<NotifySubtitle> subtitles) {
    if (GetDeviceState() != kDeviceStateIdle || notify_player_.IsBusy()) {
        ESP_LOGW(TAG, "Ignoring notify message while device is busy");
        return;
    }

    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
    audio_service_.ReleaseWakeWordResources();
    while (audio_service_.PopPacketFromSendQueue()) {
        // Discard microphone audio left over from a previous conversation.
    }

    if (!SetDeviceState(kDeviceStateNotifying)) {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        return;
    }

    audio_service_.ResetDecoder();
    uint32_t playback_id = ++notification_playback_id_;
    if (playback_id == 0) {
        playback_id = ++notification_playback_id_;
    }
    audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);

    bool started = notify_player_.Start(
        std::move(audio_url), std::move(subtitles), playback_id,
        [](uint32_t, const std::string&) {},
        [this](uint32_t id, bool success) {
            Schedule([this, id, success]() { HandleNotificationFinished(id, success); });
        });

    if (!started) {
        ESP_LOGE(TAG, "Failed to start notification playback");
        StopNotification();
    }
}

void Application::StopNotification() {
    notify_player_.Stop();
    audio_service_.ResetDecoder();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    if (GetDeviceState() == kDeviceStateNotifying) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleNotificationFinished(uint32_t playback_id, bool success) {
    if (GetDeviceState() != kDeviceStateNotifying || notification_playback_id_ != playback_id) {
        return;
    }
    ESP_LOGI(TAG, "Notification playback %lu %s", static_cast<unsigned long>(playback_id),
             success ? "completed" : "failed");
    StopNotification();
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    if (GetDeviceState() == kDeviceStateNotifying) {
        StopNotification();
    }
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    if (GetDeviceState() == kDeviceStateNotifying) {
        StopNotification();
    }

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download",
          Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetSearchContent("", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetSearchContent("", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG,
                 "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start();                              // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);  // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "cancel",
              Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetSearchContent("", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));  // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateIdle) {
        // May be called from outside the main task (e.g. board button
        // callbacks), so schedule the invocation instead of running it here
        Schedule([this, wake_word]() {
            if (GetDeviceState() == kDeviceStateIdle) {
                BeginWakeWordInvoke(wake_word);
            }
        });
    } else if (state == kDeviceStateNotifying) {
        Schedule([this, wake_word]() {
            if (GetDeviceState() == kDeviceStateNotifying) {
                StopNotification();
                BeginWakeWordInvoke(wake_word);
            }
        });
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() { AbortSpeaking(kAbortReasonNone); });
    } else if (state == kDeviceStateListening) {
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback) {
    mcp_broadcast_callback_ = std::move(callback);
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
        if (mcp_broadcast_callback_) {
            mcp_broadcast_callback_(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
            case kAecOff:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
                break;
            case kAecOnServerSide:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
            case kAecOnDeviceSide:
                audio_service_.EnableDeviceAec(true);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) { audio_service_.PlaySound(sound); }

void Application::ResetProtocol() {
    Schedule([this]() {
        if (GetDeviceState() == kDeviceStateNotifying) {
            StopNotification();
        }
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}
