#include <chrono>
#include <cstdio>
#include <cstring>

#include "test.h"

namespace hsynth_test {
std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
int g_failures = 0;
const char* g_current = "";
}  // namespace hsynth_test

int main(int argc, char** argv) {
    using namespace hsynth_test;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, failedTests = 0;
    const auto start = std::chrono::steady_clock::now();
    for (auto& t : registry()) {
        if (filter && !std::strstr(t.name, filter)) continue;
        const int before = g_failures;
        g_current = t.name;
        const auto t0 = std::chrono::steady_clock::now();
        std::printf("[ RUN  ] %s\n", t.name);
        t.fn();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const bool ok = g_failures == before;
        if (!ok) ++failedTests;
        std::printf("[ %s ] %s (%.1f ms)\n", ok ? " OK " : "FAIL", t.name, ms);
        ++run;
    }
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("\n%d tests, %d failed, %d failed checks, %.2f s\n", run, failedTests, g_failures, total);
    return g_failures == 0 ? 0 : 1;
}
