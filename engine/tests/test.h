// Minimal self-contained test harness (no external dependency).
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace hsynth_test {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

std::vector<TestCase>& registry();
extern int g_failures;
extern const char* g_current;

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

}  // namespace hsynth_test

#define HS_CAT2(a, b) a##b
#define HS_CAT(a, b) HS_CAT2(a, b)
#define TEST(name)                                                                              \
    static void name();                                                                         \
    static hsynth_test::Registrar HS_CAT(reg_, name)(#name, &name);                             \
    static void name()

#define CHECK(cond)                                                                             \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                      \
            ++hsynth_test::g_failures;                                                          \
        }                                                                                       \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                   \
    do {                                                                                        \
        const double va_ = (double)(a), vb_ = (double)(b);                                     \
        if (!(std::fabs(va_ - vb_) <= (double)(tol))) {                                         \
            std::printf("  FAIL %s:%d: %s=%g vs %s=%g (tol %g)\n", __FILE__, __LINE__, #a, va_, \
                        #b, vb_, (double)(tol));                                                \
            ++hsynth_test::g_failures;                                                          \
        }                                                                                       \
    } while (0)

#define INFO(...) std::printf("  info: " __VA_ARGS__)
