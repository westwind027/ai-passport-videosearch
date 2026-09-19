#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_event.h>
#include <esp_timer.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct WifiScanResult {
    std::string ssid;
    int rssi = 0;
    bool secure = false;
};

using WifiScanCallback = std::function<void(std::vector<WifiScanResult>)>;

class WifiBoard : public Board {
protected:
    esp_timer_handle_t connect_timer_ = nullptr;
    bool in_config_mode_ = false;
    NetworkEventCallback network_event_callback_ = nullptr;

    virtual std::string GetBoardJson() override;

    /**
     * Handle network event (called from WiFi manager callbacks)
     * @param event The network event type
     * @param data Additional data (e.g., SSID for Connecting/Connected events)
     */
    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");

    /**
     * Start WiFi connection attempt
     */
    void TryWifiConnect();

    /**
     * WiFi connection timeout callback
     */
    static void OnWifiConnectTimeout(void* arg);

public:
    WifiBoard();
    virtual ~WifiBoard();
    
    virtual std::string GetBoardType() override;
    
    /**
     * Start network connection asynchronously
     * This function returns immediately. Network events are notified through the callback set by SetNetworkEventCallback().
     */
    virtual void StartNetwork() override;
    
    virtual NetworkInterface* GetNetwork() override;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override;
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;
    
    /**
     * Enter WiFi configuration mode (thread-safe, can be called from any task)
     */
    void EnterWifiConfigMode();

    /**
     * Start the device-side Wi-Fi picker. Results are delivered from the Wi-Fi
     * event task; callers should marshal UI work back to their main task.
     */
    void StartInteractiveWifiScan(WifiScanCallback callback);

    /**
     * Save the selected credentials and restart station mode so the normal
     * WifiManager callbacks continue to drive activation.
     */
    void ConnectInteractiveWifi(const std::string& ssid, const std::string& password);

    /** Stop delivering results to the device-side picker. */
    void StopInteractiveWifiScan();
    
    /**
     * Check if in WiFi config mode
     */
    bool IsInWifiConfigMode() const;

private:
    static void OnWifiScanDone(void* arg, esp_event_base_t event_base, int32_t event_id,
                               void* event_data);
    void HandleWifiScanDone();

    esp_event_handler_instance_t wifi_scan_handler_ = nullptr;
    WifiScanCallback interactive_wifi_scan_callback_;
    std::mutex interactive_wifi_mutex_;
};

#endif // WIFI_BOARD_H
