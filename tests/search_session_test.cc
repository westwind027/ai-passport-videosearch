#include "search_session.h"

#include <cassert>
#include <iostream>

int main() {
    SearchSession session;
    assert(session.state() == SearchSessionState::Idle);
    assert(session.StartListening());

    const uint32_t first_request = 11;
    assert(session.StartSearch("第一句", first_request));
    assert(session.state() == SearchSessionState::Searching);

    SearchResponse first_response;
    first_response.query = "第一句";
    SearchResult first_result;
    first_result.title = "第一条";
    first_response.results.push_back(first_result);
    assert(session.SetResults(first_request, std::move(first_response)));
    assert(session.state() == SearchSessionState::Results);
    assert(session.response().results.size() == 1);

    assert(session.StartListening());
    assert(session.StartSearch("第二句", 12));
    SearchResponse stale_response;
    stale_response.query = "旧请求";
    assert(!session.SetResults(first_request, std::move(stale_response)));
    assert(session.state() == SearchSessionState::Searching);
    assert(session.query() == "第二句");

    assert(session.SetError(12, "服务不可用"));
    assert(session.state() == SearchSessionState::Error);
    assert(session.error() == "服务不可用");

    session.Reset();
    assert(session.state() == SearchSessionState::Idle);
    assert(session.response().results.empty());

    std::cout << "search_session_test: PASS\n";
}
