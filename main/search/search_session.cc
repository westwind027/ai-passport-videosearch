#include "search_session.h"

bool SearchSession::StartListening() {
    if (state_ != SearchSessionState::Idle && state_ != SearchSessionState::Results &&
        state_ != SearchSessionState::Error) {
        return false;
    }

    state_ = SearchSessionState::Listening;
    request_id_ = 0;
    query_.clear();
    response_ = nullptr;
    error_.clear();
    return true;
}

bool SearchSession::StartSearch(const std::string& query, uint32_t request_id) {
    if (state_ != SearchSessionState::Listening || query.empty() || request_id == 0) {
        return false;
    }

    state_ = SearchSessionState::Searching;
    request_id_ = request_id;
    query_ = query;
    response_ = nullptr;
    error_.clear();
    return true;
}

bool SearchSession::SetResults(uint32_t request_id, const SearchResponse* response) {
    if (state_ != SearchSessionState::Searching || request_id != request_id_ ||
        response == nullptr) {
        return false;
    }

    response_ = response;
    error_.clear();
    state_ = SearchSessionState::Results;
    return true;
}

bool SearchSession::SetError(uint32_t request_id, std::string error) {
    if (state_ != SearchSessionState::Searching || request_id != request_id_) {
        return false;
    }

    error_ = std::move(error);
    response_ = nullptr;
    state_ = SearchSessionState::Error;
    return true;
}

void SearchSession::Reset() {
    state_ = SearchSessionState::Idle;
    request_id_ = 0;
    query_.clear();
    response_ = nullptr;
    error_.clear();
}
