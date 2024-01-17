#include <iostream>
#include "generator.hpp"

auto range(int i, int e) COZ_BEG(demo::generator<int>, (i, e)) {
    for (; i != e; ++i) {
        COZ_YIELD(i);
    }
}
COZ_END

int main() {
    for (const auto i : range(0, 10)) {
        std::cout << i << ',';
    }
}