// SPDX-License-Identifier: GPL-2.0-or-later
//
// A deliberately tiny test harness: TEST registers, CHECK records. Test
// files define their tests in an anonymous namespace; unit_tests.cpp runs
// them all.
#pragma once

#include <cstdio>
#include <vector>

namespace knobs::test {

struct TestCase {
  const char* name;
  void (*run)();
};

inline std::vector<TestCase>& Tests() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(const char* name, void (*run)()) { Tests().push_back({name, run}); }
};

inline int g_failures = 0;

}  // namespace knobs::test

#define TEST(name)                                            \
  void name();                                                \
  const ::knobs::test::Registrar name##_registrar(#name, &name); \
  void name()

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s(%d): CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      ++::knobs::test::g_failures;                                                        \
    }                                                                                     \
  } while (false)
