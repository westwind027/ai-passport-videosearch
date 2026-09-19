#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_event.h>
#include <esp_network.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <algorithm>
#include <utility>

#include <material_symbols.h>
#include <wifi_manager.h>
#include <wifi_station.h>
#include <ssid_manager.h>
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
#include "blufi.h"
#endif

static const char *TAG = "WifiBoard";

// Connection timeout in seconds
static constexpr int CONNECT_TIMEOUT_SEC = 60;

WifiBoard::WifiBoard() {
    // Create connection timeout timer
    esp_timer_create_args_t timer_args = {
        .callback = OnWifiConnectTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_connect_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&timer_args, &connect_timer_);
}

WifiBoard::~WifiBoard() {
    if (wifi_scan_handler_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                               wifi_scan_handler_);
        wifi_scan_handler_ = nullptr;
    }
    if (connect_timer_) {
        esp_timer_stop(connect_timer_);
        esp_timer_delete(connect_timer_);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::StartNetwork() {
    auto& wifi_manager = WifiManager::GetInstance();

    // Initialize WiFi manager
    WifiManagerConfig config;
    config.ssid_prefix = "Xiaozhi";
    config.language = Lang::CODE;
    // The standalone application does not expose the legacy configuration
    // portal. Wi-Fi and the search service are configured on the device.
    config.show_ota_config = false;
    config.show_sleep_config = false;

    // Set a DHCP hostname so the router shows a friendly name instead of "espressif".
    // Uses the same "<prefix>-<last 2 MAC bytes>" scheme as the config AP SSID.
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char hostname[32];
        snprintf(hostname, sizeof(hostname), "%s-%02X%02X", config.ssid_prefix.c_str(), mac[4], mac[5]);
        config.station_hostname = hostname;
    }
    wifi_manager.Initialize(config);

    // Keep one lightweight scan observer for the device-side Wi-Fi picker.
    // WifiManager still owns station/AP lifecycle and all connection events.
    if (wifi_scan_handler_ == nullptr) {
        const esp_err_t ret = esp_event_handler_instance_register(
            WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &WifiBoard::OnWifiScanDone, this,
            &wifi_scan_handler_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register WiFi scan observer: %s", esp_err_to_name(ret));
        }
    }

    // Set unified event callback - forward to NetworkEvent with SSID data
    wifi_manager.SetEventCallback([this](WifiEvent event, const std::string& data) {
        switch (event) {
            case WifiEvent::Scanning:
                OnNetworkEvent(NetworkEvent::Scanning);
                break;
            case WifiEvent::Connecting:
                OnNetworkEvent(NetworkEvent::Connecting, data);
                break;
            case WifiEvent::Connected:
                OnNetworkEvent(NetworkEvent::Connected, data);
                break;
            case WifiEvent::Disconnected:
                OnNetworkEvent(NetworkEvent::Disconnected);
                break;
            case WifiEvent::ConfigModeEnter:
                OnNetworkEvent(NetworkEvent::WifiConfigModeEnter);
                break;
            case WifiEvent::ConfigModeExit:
                OnNetworkEvent(NetworkEvent::WifiConfigModeExit);
                break;
        }
    });

    // Try to connect or enter config mode
    TryWifiConnect();
}

void WifiBoard::TryWifiConnect() {
    auto& ssid_manager = SsidManager::GetInstance();
    bool have_ssid = !ssid_manager.GetSsidList().empty();

    if (have_ssid) {
        // Start connection attempt with timeout
        ESP_LOGI(TAG, "Starting WiFi connection attempt");
        esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
        WifiManager::GetInstance().StartStation();
    } else {
        // No SSID configured: let the device-side picker show nearby networks.
        ESP_LOGI(TAG, "No WiFi credentials, opening device-side WiFi picker");
        Application::GetInstance().EnterWifiSetup();
    }
}

void WifiBoard::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    switch (event) {
        case NetworkEvent::Connected:
            // Stop timeout timer
            esp_timer_stop(connect_timer_);
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
            // make sure blufi resources has been released
            Blufi::GetInstance().deinit();
#endif
            in_config_mode_ = false;
            ESP_LOGI(TAG, "Connected to WiFi: %s", data.c_str());
            break;
        case NetworkEvent::Scanning:
            ESP_LOGI(TAG, "WiFi scanning");
            break;
        case NetworkEvent::Connecting:
            ESP_LOGI(TAG, "WiFi connecting to %s", data.c_str());
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGW(TAG, "WiFi disconnected");
            break;
        case NetworkEvent::WifiConfigModeEnter:
            ESP_LOGI(TAG, "WiFi config mode entered");
            in_config_mode_ = true;
            break;
        case NetworkEvent::WifiConfigModeExit:
            ESP_LOGI(TAG, "WiFi config mode exited");
            in_config_mode_ = false;
            // Try to connect with the new credentials
            TryWifiConnect();
            break;
        default:
            break;
    }

    // Notify external callback if set
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void WifiBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void WifiBoard::OnWifiConnectTimeout(void* arg) {
    auto* board = static_cast<WifiBoard*>(arg);
    ESP_LOGW(TAG, "WiFi connection timeout, opening device-side WiFi picker");

    board->StopInteractiveWifiScan();
    WifiManager::GetInstance().StopStation();
    Application::GetInstance().EnterWifiSetup();
}

void WifiBoard::StartInteractiveWifiScan(WifiScanCallback callback) {
    {
        std::lock_guard<std::mutex> lock(interactive_wifi_mutex_);
        interactive_wifi_scan_callback_ = std::move(callback);
    }

    auto& wifi_manager = WifiManager::GetInstance();
    if (wifi_manager.IsConfigMode()) {
        wifi_manager.StopConfigAp();
    }

    // StartStation is idempotent. With no saved credentials it still creates
    // the STA interface and scans, which is exactly what the picker needs.
    wifi_manager.StartStation();
    const esp_err_t ret = esp_wifi_scan_start(nullptr, false);
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_STATE) {
        ESP_LOGW(TAG, "Failed to start interactive WiFi scan: %s", esp_err_to_name(ret));
    }
}

void WifiBoard::ConnectInteractiveWifi(const std::string& ssid, const std::string& password) {
    if (ssid.empty()) {
        return;
    }

    StopInteractiveWifiScan();
    esp_timer_stop(connect_timer_);

    // The device-side picker is an explicit choice. Make it the primary saved
    // network so WifiStation cannot reconnect to an older network instead.
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.Clear();
    ssid_manager.AddSsid(ssid, password);

    WifiManager::GetInstance().StopConfigAp();
    WifiManager::GetInstance().StopStation();
    esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
    WifiManager::GetInstance().StartStation();
}

void WifiBoard::StopInteractiveWifiScan() {
    std::lock_guard<std::mutex> lock(interactive_wifi_mutex_);
    interactive_wifi_scan_callback_ = nullptr;
}

void WifiBoard::OnWifiScanDone(void* arg, esp_event_base_t event_base, int32_t event_id,
                               void* event_data) {
    (void)event_base;
    (void)event_id;
    (void)event_data;
    static_cast<WifiBoard*>(arg)->HandleWifiScanDone();
}

void WifiBoard::HandleWifiScanDone() {
    WifiScanCallback callback;
    {
        std::lock_guard<std::mutex> lock(interactive_wifi_mutex_);
        callback = interactive_wifi_scan_callback_;
    }
    if (!callback) {
        return;
    }

    uint16_t ap_count = 0;
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK || ap_count == 0) {
        callback({});
        return;
    }

    std::vector<wifi_ap_record_t> records(ap_count);
    if (esp_wifi_scan_get_ap_records(&ap_count, records.data()) != ESP_OK) {
        callback({});
        return;
    }
    records.resize(ap_count);
    std::sort(records.begin(), records.end(), [](const wifi_ap_record_t& left,
                                                 const wifi_ap_record_t& right) {
        return left.rssi > right.rssi;
    });

    std::vector<WifiScanResult> results;
    results.reserve(records.size());
    for (const auto& record : records) {
        const char* ssid = reinterpret_cast<const char*>(record.ssid);
        if (ssid[0] == '\0') {
            continue;
        }
        const auto duplicate = std::find_if(
            results.begin(), results.end(), [ssid](const WifiScanResult& item) {
                return item.ssid == ssid;
            });
        if (duplicate != results.end()) {
            continue;
        }
        results.push_back({ssid, record.rssi, record.authmode != WIFI_AUTH_OPEN});
    }
    callback(std::move(results));
}

void WifiBoard::EnterWifiConfigMode() {
    ESP_LOGI(TAG, "EnterWifiConfigMode redirected to device-side WiFi picker");
    // Keep this compatibility entry point for board code, but never start the
    // old AP/web portal. Application marshals the UI and protocol cleanup to
    // its main task.
    Application::GetInstance().EnterWifiSetup();
}

bool WifiBoard::IsInWifiConfigMode() const {
    return WifiManager::GetInstance().IsConfigMode();
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    auto& wifi = WifiManager::GetInstance();

    if (wifi.IsConfigMode()) {
        return MATERIAL_SYMBOLS_WIFI;
    }
    if (!wifi.IsConnected()) {
        return MATERIAL_SYMBOLS_WIFI_OFF;
    }

    int rssi = wifi.GetRssi();
    if (rssi >= -65) {
        return MATERIAL_SYMBOLS_WIFI;
    } else if (rssi >= -75) {
        return MATERIAL_SYMBOLS_WIFI_2_BAR;
    }
    return MATERIAL_SYMBOLS_WIFI_1_BAR;
}

std::string WifiBoard::GetBoardJson() {
    auto& wifi = WifiManager::GetInstance();
    std::string json = R"({"type":")" + std::string(BOARD_TYPE) + R"(",)";
    json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";
    json += R"("manufacturer":")" + std::string(BOARD_MANUFACTURER) + R"(",)";

    if (!wifi.IsConfigMode()) {
        json += R"("ssid":")" + wifi.GetSsid() + R"(",)";
        json += R"("rssi":)" + std::to_string(wifi.GetRssi()) + R"(,)";
        json += R"("channel":)" + std::to_string(wifi.GetChannel()) + R"(,)";
        json += R"("ip":")" + wifi.GetIpAddress() + R"(",)";
    }

    json += R"("mac":")" + SystemInfo::GetMacAddress() + R"("})";
    return json;
}

void WifiBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    WifiPowerSaveLevel wifi_level;
    switch (level) {
        case PowerSaveLevel::LOW_POWER:
            wifi_level = WifiPowerSaveLevel::LOW_POWER;
            break;
        case PowerSaveLevel::BALANCED:
            wifi_level = WifiPowerSaveLevel::BALANCED;
            break;
        case PowerSaveLevel::PERFORMANCE:
        default:
            wifi_level = WifiPowerSaveLevel::PERFORMANCE;
            break;
    }
    WifiManager::GetInstance().SetPowerSaveLevel(wifi_level);
}

std::string WifiBoard::GetDeviceStatusJson() {
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    if (auto codec = board.GetAudioCodec()) {
        cJSON_AddNumberToObject(audio_speaker, "volume", codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen
    auto screen = cJSON_CreateObject();
    if (auto backlight = board.GetBacklight()) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    if (auto display = board.GetDisplay(); display && display->height() > 64) {
        if (auto theme = display->GetTheme()) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        auto battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto& wifi = WifiManager::GetInstance();
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi.GetSsid().c_str());
    int rssi = wifi.GetRssi();
    const char* signal = rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak");
    cJSON_AddStringToObject(network, "signal", signal);
    cJSON_AddItemToObject(root, "network", network);

    // Chip temperature
    float temp = 0.0f;
    if (board.GetTemperature(temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}
