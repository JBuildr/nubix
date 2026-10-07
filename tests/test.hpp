// Nubix — tiny self-contained unit test framework (host only).
//
//   XC_TEST(suite, name) { XC_CHECK(cond); XC_CHECK_EQ(a, b); XC_REQUIRE(cond); }
//
// Run: xc-tests [suite ...]   (no args = all suites). Exit code = number of failed tests.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace xctest {

struct Failure {};  // thrown by XC_REQUIRE to abort the current test

struct TestCase {
    const char* suite;
    const char* name;
    std::function<void()> fn;
};

std::vector<TestCase>& registry();
// Record a failed check for the currently running test.
void fail(const char* file, int line, const std::string& msg);

struct Registrar {
    Registrar(const char* suite, const char* name, std::function<void()> fn) {
        registry().push_back({suite, name, std::move(fn)});
    }
};

template <typename T>
std::string show(const T& v) {
    std::ostringstream os;
    os << v;
    return os.str();
}
inline std::string show(const std::string& v) { return "\"" + v + "\""; }
inline std::string show(const char* v) { return v ? "\"" + std::string(v) + "\"" : "(null)"; }
inline std::string show(unsigned char v) { return std::to_string(static_cast<unsigned>(v)); }
inline std::string show(signed char v) { return std::to_string(static_cast<int>(v)); }
inline std::string show(bool v) { return v ? "true" : "false"; }

}  // namespace xctest

#define XC_CAT2(a, b) a##b
#define XC_CAT(a, b) XC_CAT2(a, b)

#define XC_TEST(suite, name)                                                                     \
    static void XC_CAT(xc_test_##suite##_, name)();                                              \
    static ::xctest::Registrar XC_CAT(xc_reg_##suite##_, name)(#suite, #name,                   \
                                                               &XC_CAT(xc_test_##suite##_, name)); \
    static void XC_CAT(xc_test_##suite##_, name)()

#define XC_CHECK(cond)                                                   \
    do {                                                                 \
        if (!(cond)) ::xctest::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define XC_REQUIRE(cond)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ::xctest::fail(__FILE__, __LINE__, "REQUIRE(" #cond ")");        \
            throw ::xctest::Failure{};                                       \
        }                                                                    \
    } while (0)

#define XC_CHECK_EQ(a, b)                                                                                  \
    do {                                                                                                   \
        const auto& xc_a_ = (a);                                                                           \
        const auto& xc_b_ = (b);                                                                           \
        if (!(xc_a_ == xc_b_))                                                                             \
            ::xctest::fail(__FILE__, __LINE__,                                                             \
                           "CHECK_EQ(" #a ", " #b "): " + ::xctest::show(xc_a_) + " != " + ::xctest::show(xc_b_)); \
    } while (0)
