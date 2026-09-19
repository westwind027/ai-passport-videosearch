#pragma once

#include <string>

// Removes punctuation and whitespace that speech recognition commonly adds
// at the end of a search phrase, while preserving punctuation inside it.
std::string NormalizeSearchQuery(const std::string& text);
