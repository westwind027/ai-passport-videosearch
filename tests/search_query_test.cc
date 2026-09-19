#include "search_query.h"

#include <cassert>
#include <iostream>

int main() {
    assert(NormalizeSearchQuery("男人。") == "男人");
    assert(NormalizeSearchQuery("男人。\n") == "男人");
    assert(NormalizeSearchQuery("  男人。  ") == "男人");
    assert(NormalizeSearchQuery("男人，女人") == "男人，女人");
    assert(NormalizeSearchQuery("？！") == "");

    std::cout << "search_query_test: PASS\n";
}
