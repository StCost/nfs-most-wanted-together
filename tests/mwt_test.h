#pragma once
// Tiny self-contained test runner (no dependencies).
//
//   TEST(name) { CHECK(x); CHECK_EQ(a, b); CHECK_NEAR(a, b, eps); CHECK_STR(a, b); REQUIRE(x); }
//
// tests.exe [filter] [--list]   runs every test whose name contains `filter`; exit code 0 = all passed.
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

namespace mwt {
    struct TestCase { const char* name; const char* file; void (*fn)(); };
    inline std::vector<TestCase>& Registry() { static std::vector<TestCase> r; return r; }
    struct Registrar { Registrar(const char* n, const char* f, void (*fn)()) { Registry().push_back({ n, f, fn }); } };

    inline int& Failures() { static int n = 0; return n; }   // failed checks in the running test
    inline long long& Checks() { static long long n = 0; return n; }

    inline void Fail(const char* file, int line, const char* fmt, ...) {
        if (++Failures() > 20) return;                         // don't flood the output
        const char* base = strrchr(file, '\\');
        if (!base) base = strrchr(file, '/');
        printf("    FAILED %s(%d): ", base ? base + 1 : file, line);
        va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
        printf("\n");
    }
}

#define TEST(name) \
    static void name(); \
    static mwt::Registrar mwt_reg_##name(#name, __FILE__, &name); \
    static void name()

#define CHECK(cond) \
    do { ++mwt::Checks(); if (!(cond)) mwt::Fail(__FILE__, __LINE__, "CHECK(%s)", #cond); } while (0)

// stops the test (use when later checks would crash)
#define REQUIRE(cond) \
    do { ++mwt::Checks(); if (!(cond)) { mwt::Fail(__FILE__, __LINE__, "REQUIRE(%s)", #cond); return; } } while (0)

#define CHECK_EQ(a, b) \
    do { ++mwt::Checks(); long long mwt_a = (long long)(a), mwt_b = (long long)(b); \
         if (mwt_a != mwt_b) mwt::Fail(__FILE__, __LINE__, "CHECK_EQ(%s, %s): %lld != %lld", #a, #b, mwt_a, mwt_b); } while (0)

#define CHECK_NEAR(a, b, eps) \
    do { ++mwt::Checks(); double mwt_a = (double)(a), mwt_b = (double)(b); \
         if (!(fabs(mwt_a - mwt_b) <= (eps))) mwt::Fail(__FILE__, __LINE__, "CHECK_NEAR(%s, %s): %g vs %g", #a, #b, mwt_a, mwt_b); } while (0)

#define CHECK_STR(a, b) \
    do { ++mwt::Checks(); std::string mwt_a = (a), mwt_b = (b); \
         if (mwt_a != mwt_b) mwt::Fail(__FILE__, __LINE__, "CHECK_STR(%s, %s): \"%s\" != \"%s\"", #a, #b, mwt_a.c_str(), mwt_b.c_str()); } while (0)
