#pragma once

#include <string>

#include "video_search_client.h"

// Builds and dispatches playback commands for the search service's
// /v1/player/control API. Requests run on the shared VideoSearchClient
// worker (same task stack, serialized with search and image transfers) so a
// remote command never races the image pipeline for the last free heap
// blocks. Returns false when the shared worker is busy or the request could
// not be queued.
class PlayerControlClient {
public:
    // Asks the backend web player to open the given media in fullscreen with
    // autoplay, seeking to `time` seconds (typically the scene timestamp of
    // the current search result).
    static bool Play(VideoSearchClient& client, const std::string& service_url,
                     const std::string& media_id, double time);

    // Closes the backend web player popup and exits its fullscreen.
    static bool Close(VideoSearchClient& client, const std::string& service_url);
};
