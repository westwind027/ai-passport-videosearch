#include "search_url.h"

#include <cassert>
#include <iostream>

int main() {
    assert(ResolveSearchUrl("http://192.168.1.20:8000", "/v1/search") ==
           "http://192.168.1.20:8000/v1/search");
    assert(ResolveSearchUrl("http://192.168.1.20:8000/", "/v1/media/a/frames/a.jpg") ==
           "http://192.168.1.20:8000/v1/media/a/frames/a.jpg");
    assert(ResolveSearchUrl("http://192.168.1.20:8000", "https://cdn.example.com/frame.jpg") ==
           "https://cdn.example.com/frame.jpg");
    assert(!IsSupportedSearchUrl("ftp://cdn.example.com/frame.jpg"));
    assert(!IsSupportedSearchUrl("/v1/media/frame.jpg"));
    assert(IsSupportedSearchUrl("http://192.168.1.20:8000/v1/search"));
    assert(IsSupportedSearchUrl("https://search.example.com/v1/search"));
    assert(BuildSearchRequestUrl("http://192.168.1.20:8000/", "男人") ==
           "http://192.168.1.20:8000/v1/search?q=%E7%94%B7%E4%BA%BA&limit=12&mode=scene");
    assert(BuildSearchRequestUrl("http://192.168.1.20:8000", "a b&c") ==
           "http://192.168.1.20:8000/v1/search?q=a%20b%26c&limit=12&mode=scene");
    assert(BuildSearchRequestUrl("http://192.168.1.20:8000", "a b&c", 6) ==
           "http://192.168.1.20:8000/v1/search?q=a%20b%26c&limit=6&mode=scene");
    assert(BuildPlayerControlUrl("http://192.168.1.20:8000/") ==
           "http://192.168.1.20:8000/v1/player/control");
    assert(BuildPlayerControlUrl("http://192.168.1.20:8000/api") ==
           "http://192.168.1.20:8000/api/v1/player/control");
    assert(BuildPlayerControlUrl("not-a-url").empty());
    assert(BuildSearchImageUrl("http://192.168.1.20:8000/static/frames/a/frame.jpg",
                               SearchImageVariant::Tiny) ==
           "http://192.168.1.20:8000/static/frames/a/frame.jpg?size=tiny&width=80&height=60");
    assert(BuildSearchImageUrl("http://192.168.1.20:8000/static/frames/a/frame.jpg?token=x",
                               SearchImageVariant::Small) ==
           "http://192.168.1.20:8000/static/frames/a/frame.jpg?token=x&size=small");
    assert(BuildSearchImageUrl(
               "http://192.168.1.20:8000/static/frames/a/frame.jpg?size=small&width=640#frame",
               SearchImageVariant::Tiny) ==
           "http://192.168.1.20:8000/static/frames/a/frame.jpg?size=tiny&width=80&height=60#frame");

    std::cout << "search_url_test: PASS\n";
}
