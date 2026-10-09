// Host test entry point: one binary, one Register...Tests() per suite.
// No arguments runs everything; arguments run the suites whose name contains
// one of them (`run.cmd config`).

#include <cstring>

#include "unity.h"

void RegisterConfigTests(void);

void setUp(void) {}

void tearDown(void) {}

namespace {

int filter_count = 0;
char **filters = nullptr;

bool Wanted(const char *suite) {
    if (filter_count == 0) {
        return true;
    }
    for (int i = 0; i < filter_count; ++i) {
        if (std::strstr(suite, filters[i]) != nullptr) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char **argv) {
    filter_count = argc - 1;
    filters = argv + 1;

    UNITY_BEGIN();
    if (Wanted("config")) RegisterConfigTests();
    return UNITY_END();
}
