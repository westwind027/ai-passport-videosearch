#include "player_control_client.h"

#include "search_url.h"

#include <cJSON.h>
#include <esp_log.h>

#include <functional>
#include <string>

namespace {

constexpr char kTag[] = "PlayerCtrl";

std::string BuildControlJson(const std::function<void(cJSON* object)>& populate) {
    cJSON* body = cJSON_CreateObject();
    if (body == nullptr) {
        return {};
    }
    populate(body);
    char* printed = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (printed == nullptr) {
        return {};
    }
    std::string json = printed;
    cJSON_free(printed);
    return json;
}

}  // namespace

bool PlayerControlClient::Play(VideoSearchClient& client, const std::string& service_url,
                               const std::string& media_id, double time) {
    const std::string url = BuildPlayerControlUrl(service_url);
    if (url.empty() || media_id.empty()) {
        return false;
    }

    // The play payload follows the backend contract and seeks to the scene
    // timestamp of the displayed result.
    const std::string json = BuildControlJson([&](cJSON* body) {
        cJSON_AddStringToObject(body, "media_id", media_id.c_str());
        cJSON_AddNumberToObject(body, "time", time);
        cJSON_AddBoolToObject(body, "fullscreen", true);
        cJSON_AddBoolToObject(body, "autoplay", true);
    });
    if (json.empty()) {
        return false;
    }

    return client.SendPlayerControl(url, json, [](bool success, std::string error) {
        if (success) {
            ESP_LOGI(kTag, "remote play accepted");
        } else {
            ESP_LOGE(kTag, "remote play failed: %s", error.c_str());
        }
    });
}

bool PlayerControlClient::Close(VideoSearchClient& client, const std::string& service_url) {
    const std::string url = BuildPlayerControlUrl(service_url);
    if (url.empty()) {
        return false;
    }

    const std::string json = BuildControlJson(
        [&](cJSON* body) { cJSON_AddStringToObject(body, "action", "close"); });
    if (json.empty()) {
        return false;
    }

    return client.SendPlayerControl(url, json, [](bool success, std::string error) {
        if (success) {
            ESP_LOGI(kTag, "remote play close accepted");
        } else {
            ESP_LOGE(kTag, "remote play close failed: %s", error.c_str());
        }
    });
}
