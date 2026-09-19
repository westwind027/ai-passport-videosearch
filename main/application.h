#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"
#include "display.h"
#include "notify/notify_player.h"
#include "ota.h"
#include "protocol.h"
#include "search/search_session.h"
#include "search/video_search_client.h"
#include "wifi_board.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE (1 << 0)
#define MAIN_EVENT_SEND_AUDIO (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED (1 << 2)
#define MAIN_EVENT_VAD_CHANGE (1 << 3)
#define MAIN_EVENT_ERROR (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE (1 << 5)
#define MAIN_EVENT_CLOCK_TICK (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT (1 << 9)
#define MAIN_EVENT_START_LISTENING (1 << 10)
#define MAIN_EVENT_STOP_LISTENING (1 << 11)
#define MAIN_EVENT_STATE_CHANGED (1 << 12)
#define MAIN_EVENT_PLAYBACK_DRAINED (1 << 13)
#define MAIN_EVENT_ENTER_SETTINGS (1 << 14)
#define MAIN_EVENT_BUTTON_UP (1 << 15)
#define MAIN_EVENT_BUTTON_DOWN (1 << 16)
#define MAIN_EVENT_WIFI_SETUP (1 << 17)
#define MAIN_EVENT_BUTTON_UP_LONG (1 << 18)
#define MAIN_EVENT_BUTTON_DOWN_LONG (1 << 19)

enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }

    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "",
               const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Handle a long OK press (event-based, thread-safe).
     * Opens the root settings menu from home and returns to the parent page
     * from every menu page.
     */
    void EnterSettings();

    /**
     * Show the device-side Wi-Fi picker when no credentials are available.
     */
    void EnterWifiSetup();

    /**
     * Handle the board's up/down buttons in the main task.
     */
    void HandleUpButton();
    void HandleDownButton();
    void HandleUpButtonLong();
    void HandleDownButtonLong();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }

    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    NotifyPlayer notify_player_;
    uint32_t notification_playback_id_ = 0;
    std::unique_ptr<Ota> ota_;

    std::function<void(const std::string&)> mcp_broadcast_callback_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ =
        false;  // Flag to play popup sound after state changes to listening
    bool pending_listening_start_ =
        false;  // Waiting for playback to drain before starting listening (auto mode)
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;

    enum class UiMode {
        Home,
        Settings,
        WifiList,
        TextInput,
    };

    enum class TextInputTarget {
        WifiPassword,
        SearchService,
    };
    UiMode ui_mode_ = UiMode::Home;
    size_t settings_index_ = 0;
    bool settings_volume_mode_ = false;
    std::vector<WifiScanResult> wifi_networks_;
    std::size_t wifi_network_index_ = 0;
    bool wifi_scan_in_progress_ = false;
    std::string wifi_selected_ssid_;
    std::string text_input_value_;
    // Scheme kept aside while editing a service address so the value box
    // shows only the host:port part. Re-attached on submit.
    std::string text_input_scheme_;
    TextInputTarget text_input_target_ = TextInputTarget::WifiPassword;
    TextInputPage text_input_page_ = TextInputPage::Lower;
    std::size_t text_input_index_ = 0;
    bool text_input_replace_existing_ = false;
    bool wifi_connection_pending_ = false;
    size_t search_result_index_ = 0;
    uint32_t search_image_generation_ = 0;
    std::string search_service_url_;
    SearchSession search_session_;
    VideoSearchClient video_search_client_;

    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleEnterSettingsEvent();
    void HandleWifiSetupEvent();
    void HandleUpButtonEvent(bool fast = false);
    void HandleDownButtonEvent(bool fast = false);
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void BeginWakeWordInvoke(const std::string& wake_word);
    void ContinueWakeWordInvoke(const std::string& wake_word);
    void StartListeningAudio();
    void ConfigureWakeWordForListening();
    void StartNotification(std::string audio_url, std::vector<NotifySubtitle> subtitles);
    void StopNotification();
    void HandleNotificationFinished(uint32_t playback_id, bool success);
    void HandleSearchText(const std::string& text);
    void HandleSearchCompleted(uint32_t request_id, bool success, const SearchResponse* response,
                               std::string error);
    void HandleSearchImageStreamCompleted(uint32_t request_id, uint32_t generation, bool success,
                                          size_t width, size_t height, std::string error);
    void ShowSearchResult(size_t index);
    void RenderSettingsMenu();
    void RenderVolumeMenu();
    void RenderWifiList();
    void RenderTextInput(const char* status = nullptr);
    void HandleWifiListConfirm();
    void HandleTextInputKey();
    void CommitTextInput();
    void HandleSettingsConfirm();
    void AdjustVolume(int delta);
    void ClearSearchImage();

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;

    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};

class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() { vTaskPrioritySet(NULL, original_priority_); }

private:
    BaseType_t original_priority_;
};

#endif  // _APPLICATION_H_
