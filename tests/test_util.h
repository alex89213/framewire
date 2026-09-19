/*
 * Description: Small assertion helpers for the unit tests, kept here so the
 *   project does not pull in a test framework dependency.
 * Author: Alex Wu
 * Dependencies:
 * Usage:
 */

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

namespace fwtest {

inline int g_checks = 0;
inline int g_failures = 0;
inline int g_case_start_failures = 0;
inline const char* g_current_case = "";

inline void BeginCase(const char* name) {
  g_current_case = name;
  // remember the failure count on entry, so one bad case does not mark every
  // case after it as failed too
  g_case_start_failures = g_failures;
  std::printf("  %-44s", name);
  std::fflush(stdout);
}

inline void EndCase() {
  std::printf("%s\n", g_failures == g_case_start_failures ? "ok" : "FAILED");
}

inline void Report(bool condition, const char* expression, const char* file, int line) {
  ++g_checks;
  if (condition) return;
  ++g_failures;
  std::printf("\n    FAIL %s:%d in %s\n      %s\n", file, line, g_current_case, expression);
}

inline void ReportEq(long long got, long long want, const char* expression, const char* file,
                     int line) {
  ++g_checks;
  if (got == want) return;
  ++g_failures;
  std::printf("\n    FAIL %s:%d in %s\n      %s\n      got %lld, want %lld\n", file, line,
              g_current_case, expression, got, want);
}

inline void ReportNear(double got, double want, double tolerance, const char* expression,
                       const char* file, int line) {
  ++g_checks;
  const double diff = got > want ? got - want : want - got;
  if (diff <= tolerance) return;
  ++g_failures;
  std::printf("\n    FAIL %s:%d in %s\n      %s\n      got %g, want %g +- %g\n", file, line,
              g_current_case, expression, got, want, tolerance);
}

inline int Finish(const char* suite) {
  if (g_failures == 0) {
    std::printf("%s: %d checks passed\n", suite, g_checks);
    return 0;
  }
  std::printf("%s: %d of %d checks FAILED\n", suite, g_failures, g_checks);
  return 1;
}

}  // namespace fwtest

#define CHECK(cond) ::fwtest::Report((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(got, want) \
  ::fwtest::ReportEq(static_cast<long long>(got), static_cast<long long>(want), #got " == " #want, \
                     __FILE__, __LINE__)
#define CHECK_NEAR(got, want, tol) \
  ::fwtest::ReportNear((got), (want), (tol), #got " ~= " #want, __FILE__, __LINE__)
#define TEST_CASE(name)                                        \
  for (bool _fwtest_once = (::fwtest::BeginCase(name), true);  \
       _fwtest_once; _fwtest_once = (::fwtest::EndCase(), false))
