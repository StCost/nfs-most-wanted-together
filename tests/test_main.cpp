// Test runner entry point. See mwt_test.h.
#include <winsock2.h>
#include <windows.h>
#include <crtdbg.h>
#include <cstdlib>
#include <rtcapi.h>
#include "mwt_test.h"

namespace {
    // Runs one test; a crash (access violation etc.) is reported as a failure instead of a dialog.
    bool RunGuarded(void (*fn)(), unsigned long* code) {
        __try { fn(); return true; }
        __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void OnInvalidParameter(const wchar_t* expr, const wchar_t* func, const wchar_t*, unsigned, uintptr_t) {
        // a "secure CRT" function refused its arguments (e.g. sprintf_s into a buffer that is too small)
        printf("\n    FATAL: invalid parameter in CRT call (%ls in %ls)\n", expr ? expr : L"?", func ? func : L"?");
        fflush(stdout);
        _exit(3);
    }

#ifdef __MSVC_RUNTIME_CHECKS
    int __cdecl OnRtcError(int, const wchar_t* file, int line, const wchar_t*, const wchar_t* fmt, ...) {
        printf("\n    FATAL: run-time check failed at %ls(%d): %ls\n", file ? file : L"?", line, fmt ? fmt : L"?");
        fflush(stdout);
        _exit(3);
    }
#endif
}

int main(int argc, char** argv) {
    // never block on a dialog box (local runs and CI alike): report to stderr and fail
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    for (int t : { _CRT_WARN, _CRT_ERROR, _CRT_ASSERT }) {
        _CrtSetReportMode(t, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(t, _CRTDBG_FILE_STDERR);
    }
    _set_invalid_parameter_handler(&OnInvalidParameter);
#ifdef __MSVC_RUNTIME_CHECKS
    _RTC_SetErrorFuncW(&OnRtcError);
#endif
    setvbuf(stdout, nullptr, _IONBF, 0);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { printf("WSAStartup failed\n"); return 2; }

    const char* filter = nullptr;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--list")) list = true;
        else filter = argv[i];
    }

    int run = 0, failed = 0;
    for (const mwt::TestCase& t : mwt::Registry()) {
        if (filter && !strstr(t.name, filter)) continue;
        if (list) { printf("%s\n", t.name); continue; }
        ++run;
        mwt::Failures() = 0;
        printf("[ RUN  ] %s\n", t.name);
        unsigned long code = 0;
        if (!RunGuarded(t.fn, &code)) {
            printf("    FAILED: crashed with exception %08lX\n", code);
            ++mwt::Failures();
        }
        if (mwt::Failures()) { ++failed; printf("[ FAIL ] %s (%d failed check(s))\n", t.name, mwt::Failures()); }
        else printf("[  OK  ] %s\n", t.name);
    }
    if (list) return 0;

#ifdef _DEBUG
    if (!_CrtCheckMemory()) { printf("FAILED: debug heap reports corruption\n"); ++failed; }
#endif
    WSACleanup();
    printf("\n%d test(s) run, %d failed, %lld checks\n", run, failed, mwt::Checks());
    if (run == 0) { printf("no tests matched\n"); return 2; }
    return failed ? 1 : 0;
}
