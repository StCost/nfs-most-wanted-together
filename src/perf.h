#pragma once
// Performance diagnostics ([Debug] Perf=1). Measures the game's frame rate (time between our frame
// callbacks, i.e. between Present calls) and how long each part of the mod takes per frame.
// Every 5 s: one log line, and a short summary for the overlay.
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include "log.h"

namespace Perf {
    enum Section { Menus, Net, Ghost, Traffic, Pursuit, Rules, Overlay, Marker, Count };
    inline const char* Name(int s) {
        static const char* n[] = { "menus", "net", "ghost", "traffic", "pursuit", "rules", "overlay", "marker" };
        return n[s];
    }

    struct State {
        bool on = false;
        LARGE_INTEGER qpf{}, lastFrame{}, windowStart{};
        double secMs[Count] = {}, secMax[Count] = {};
        double frameMs[4096]; int frames = 0;
        double modMs = 0;
        std::string summary;
    };
    inline State& S() { static State s; return s; }
    inline double Ms(LARGE_INTEGER a, LARGE_INTEGER b) { return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)S().qpf.QuadPart; }

    inline void Enable(bool on) { S().on = on; QueryPerformanceFrequency(&S().qpf); }

    // Call at the start of every frame callback.
    inline void FrameStart() {
        State& s = S();
        if (!s.on) return;
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        if (s.lastFrame.QuadPart) {
            double ft = Ms(s.lastFrame, now);
            if (s.frames < 4096) s.frameMs[s.frames++] = ft;
        } else s.windowStart = now;
        s.lastFrame = now;
        double win = Ms(s.windowStart, now);
        if (win < 5000 || s.frames < 2) return;

        // report the last 5 s
        int n = s.frames;
        double sum = 0, mx = 0;
        for (int i = 0; i < n; ++i) { sum += s.frameMs[i]; mx = std::max(mx, s.frameMs[i]); }
        std::sort(s.frameMs, s.frameMs + n);
        double p99 = s.frameMs[std::min(n - 1, (int)(n * 0.99))];
        double fps = n * 1000.0 / win;
        char line[512]; int len = 0;
        len += sprintf_s(line + len, sizeof(line) - len, "Perf: %.0f fps (1%% low %.0f, worst frame %.1f ms, avg %.2f ms) | mod %.2f ms/frame:",
                         fps, p99 > 0 ? 1000.0 / p99 : 0, mx, sum / n, s.modMs / n);
        for (int i = 0; i < Count; ++i)
            len += sprintf_s(line + len, sizeof(line) - len, " %s %.2f(max %.1f)", Name(i), s.secMs[i] / n, s.secMax[i]);
        LOG("%s", line);
        char sb[256];
        sprintf_s(sb, "Perf: %.0f fps, 1%% low %.0f, worst %.1f ms | mod %.2f ms/frame\n", fps, p99 > 0 ? 1000.0 / p99 : 0, mx, s.modMs / n);
        s.summary = sb;
        s.frames = 0; s.modMs = 0;
        for (int i = 0; i < Count; ++i) s.secMs[i] = s.secMax[i] = 0;
        s.windowStart = now;
    }

    // Times one section: Perf::Scope p(Perf::Net);
    struct Scope {
        int sec; LARGE_INTEGER t0;
        explicit Scope(int s) : sec(s) { if (S().on) QueryPerformanceCounter(&t0); }
        ~Scope() {
            State& s = S();
            if (!s.on) return;
            LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
            double ms = Ms(t0, t1);
            s.secMs[sec] += ms; s.modMs += ms;
            if (ms > s.secMax[sec]) s.secMax[sec] = ms;
        }
    };
    inline const std::string& Summary() { return S().summary; }
}
