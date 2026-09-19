#include "search_payload.h"

#include <cassert>
#include <iostream>

int main() {
    const char* payload = R"json({
        "query": "夕阳下骑车",
        "results": [
            {
                "media_id": "movie-1",
                "title": "第一部电影",
                "score": 0.8123,
                "image_url": "https://cdn.example.com/movie-1.jpg",
                "preview_time": 2259.0,
                "start": 2276.0,
                "end": 2290.0,
                "scene": {"start": 12.5, "end": 18.0}
            },
            {
                "media_id": "movie-2",
                "title": "第二部电影",
                "score": 0.71,
                "scene": {"preview": "/v1/media/movie-2/frames/frame.jpg", "caption": "镜头描述",
                           "preview_time": 45.5}
            }
        ]
    })json";

    SearchResponse response;
    std::string error;
    assert(ParseSearchResponse(payload, "http://192.168.1.20:8000", response, error));
    assert(error.empty());
    assert(response.query == "夕阳下骑车");
    assert(response.results.size() == 2);
    assert(response.results[0].title == "第一部电影");
    assert(response.results[0].image_url == "https://cdn.example.com/movie-1.jpg");
    assert(response.results[0].score == 0.8123F);
    assert(response.results[0].start == 2276.0);
    // The top-level preview_time wins over the nested scene one; it is what
    // the web player seeks to when clicking a result.
    assert(response.results[0].preview_time == 2259.0);
    assert(response.results[1].preview_time == 45.5);
    assert(response.results[1].image_url ==
           "http://192.168.1.20:8000/v1/media/movie-2/frames/frame.jpg");
    assert(response.results[1].caption == "镜头描述");

    SearchResponse limited_response;
    assert(ParseSearchResponse(payload, "http://192.168.1.20:8000", limited_response, error, 1));
    assert(limited_response.results.size() == 1);

    SearchResponse empty_response;
    assert(!ParseSearchResponse(R"json({"results":{}})json", "http://localhost:8000",
                                empty_response, error));
    assert(!error.empty());

    // A response without preview_time leaves the field negative so callers
    // fall back to the scene start, matching the web player.
    SearchResponse no_preview_response;
    assert(ParseSearchResponse(R"json({"results":[{"media_id":"m","start":9.0}]})json",
                               "http://localhost:8000", no_preview_response, error));
    assert(no_preview_response.results.size() == 1);
    assert(no_preview_response.results[0].preview_time < 0.0);
    assert(no_preview_response.results[0].start == 9.0);

    std::cout << "search_payload_test: PASS\n";
}
