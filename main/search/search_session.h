#pragma once

#include "search_payload.h"

#include <cassert>
#include <cstdint>
#include <string>

enum class SearchSessionState {
    Idle,
    Listening,
    Searching,
    Results,
    Error,
};

class SearchSession {
public:
    SearchSessionState state() const { return state_; }
    const std::string& query() const { return query_; }
    const SearchResponse& response() const {
        assert(response_ != nullptr);
        return *response_;
    }
    const std::string& error() const { return error_; }
    uint32_t request_id() const { return request_id_; }

    bool StartListening();
    bool StartSearch(const std::string& query, uint32_t request_id);
    bool SetResults(uint32_t request_id, const SearchResponse* response);
    bool SetError(uint32_t request_id, std::string error);
    void Reset();

private:
    SearchSessionState state_ = SearchSessionState::Idle;
    uint32_t request_id_ = 0;
    std::string query_;
    const SearchResponse* response_ = nullptr;
    std::string error_;
};
