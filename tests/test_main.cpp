// Nubix — unit test runner.
// Copyright (C) 2026 Nubix contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <cstring>
#include <exception>
#include <set>

#include "test.hpp"

namespace xctest {

namespace {
int g_currentFailures = 0;
}

std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

void fail(const char* file, int line, const std::string& msg) {
    ++g_currentFailures;
    std::fprintf(stderr, "    %s:%d: %s\n", file, line, msg.c_str());
}

}  // namespace xctest

int main(int argc, char** argv) {
    std::set<std::string> suites;
    for (int i = 1; i < argc; ++i) suites.insert(argv[i]);
    if (suites.count("--list")) {
        for (const auto& t : xctest::registry()) std::printf("%s.%s\n", t.suite, t.name);
        return 0;
    }

    int run = 0, failed = 0;
    for (const auto& t : xctest::registry()) {
        if (!suites.empty() && !suites.count(t.suite)) continue;
        ++run;
        xctest::g_currentFailures = 0;
        std::printf("[ RUN  ] %s.%s\n", t.suite, t.name);
        try {
            t.fn();
        } catch (const xctest::Failure&) {
        } catch (const std::exception& e) {
            xctest::fail(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what());
        } catch (...) {
            xctest::fail(__FILE__, __LINE__, "unexpected non-std exception");
        }
        if (xctest::g_currentFailures) {
            ++failed;
            std::printf("[ FAIL ] %s.%s\n", t.suite, t.name);
        } else {
            std::printf("[  OK  ] %s.%s\n", t.suite, t.name);
        }
    }
    std::printf("%d tests, %d failed\n", run, failed);
    if (run == 0) {
        std::fprintf(stderr, "no tests matched\n");
        return 1;
    }
    return failed > 255 ? 255 : failed;
}
