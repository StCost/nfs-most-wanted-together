#pragma once
// Why the game quit (tests\test_exit.cpp). speed.exe v1.3 quits through one flag, ExitTheGameFlag
// (0x9257EC): the main loop (main 0x6665FC / 0x66663A) leaves when it is set, and
// __DeInitializeEverything (0x6DFA80, the only caller of DestroyWindow) then destroys the window -
// the WM_DESTROY our window hook logs. Nothing else in the exe calls DestroyWindow, so a WM_DESTROY
// without a WM_CLOSE before it means one of these writers set the flag:
//   0x5A4C87 MenuScreen::BaseNotify, message 0xBE34E004: Yes in the main menu's "Quit?" dialog. The
//            dialog (ShowTwoButtons at 0x5A491B) comes up on 0x62799A4C, which the game queues for a
//            'q' / 'Q' character (WM_CHAR, 0x56C910 / KeyboardEditString::NotifyChar 0x5A8738) in the
//            front end (flow 3) while no text is being edited.
//   0x55BE9B the online patch / login screen (0xA8DE4CD2 item).
//   0x57E667 DiscErrorPC.fng (boot flow, only without the disc): a button other than Retry.
//   0x6C2206 the message pump (0x6C21A0): msg.message == WM_QUIT after PeekMessage(PM_NOREMOVE).
//            The pump ignores PeekMessage's result: with no message waiting the MSG on its stack is
//            left as it was, so whatever an earlier call left in that stack slot is read as the
//            message - a stale 0x12 there quits the game like a real WM_QUIT.
//   0x6DB7D4 the window procedure (0x6DB6C0): WM_CLOSE (Alt+F4, the X button).
// The window procedure answers WM_DESTROY with PostQuitMessage (0x6DB7BB), the exe's only one.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ExitUtil {
    constexpr uintptr_t ExitFlag = 0x9257EC;
    constexpr uint32_t  WmQuit = 0x12;
    constexpr uintptr_t PumpPeekReturn = 0x6C21B8;   // return address of the pump's PeekMessage(PM_NOREMOVE)

    struct Site { uintptr_t at; const char* name; };
    inline const Site* Sites(int* count) {
        static const Site k[] = {
            { 0x5A4C87, "the main menu's Quit dialog (Yes)" },
            { 0x55BE9B, "the online login screen" },
            { 0x57E667, "the disc error screen" },
            { 0x6C2206, "WM_QUIT in the message pump" },
            { 0x6DB7D4, "WM_CLOSE (Alt+F4 / the window's X)" },
        };
        if (count) *count = (int)(sizeof(k) / sizeof(k[0]));
        return k;
    }
    inline const char* SiteName(int index) {
        int n = 0; const Site* s = Sites(&n);
        return (index >= 0 && index < n) ? s[index].name : "an unknown place";
    }
    // the instruction every writer uses: mov dword ptr [0x9257EC], 1
    inline const uint8_t* SetFlagBytes() {
        static const uint8_t b[10] = { 0xC7, 0x05, 0xEC, 0x57, 0x92, 0x00, 0x01, 0x00, 0x00, 0x00 };
        return b;
    }

    // The pump's PeekMessage found nothing, but the MSG it reads still says WM_QUIT: stale stack data,
    // not a quit request. Only for the pump's own call (other callers check the result).
    inline bool IsStaleQuit(bool gotMessage, uint32_t message, uintptr_t returnAddress) {
        return !gotMessage && message == WmQuit && returnAddress == PumpPeekReturn;
    }

    // A return address (a stack value right after a call instruction): E8 rel32, or FF /2 in any of its
    // lengths (call reg, [reg], [reg+disp8], [sib], [disp32], [reg+disp32]). `before` holds the 7 bytes in
    // front of the address (before[6] is the byte right before it).
    inline bool LooksAfterCall(const uint8_t before[7]) {
        if (before[2] == 0xE8) return true;                                         // call rel32
        auto call = [](uint8_t op, uint8_t modrm) { return op == 0xFF && (modrm & 0x38) == 0x10; };
        auto mod = [](uint8_t m) { return m >> 6; };
        auto rm = [](uint8_t m) { return m & 7; };
        uint8_t m;
        m = before[6]; if (call(before[5], m) && (mod(m) == 3 || (mod(m) == 0 && rm(m) != 4 && rm(m) != 5))) return true;   // 2 bytes
        m = before[5]; if (call(before[4], m) && ((mod(m) == 1 && rm(m) != 4) || (mod(m) == 0 && rm(m) == 4))) return true;  // 3 bytes
        m = before[4]; if (call(before[3], m) && mod(m) == 1 && rm(m) == 4) return true;                                       // 4 bytes
        m = before[2]; if (call(before[1], m) && ((mod(m) == 0 && rm(m) == 5) || (mod(m) == 2 && rm(m) != 4))) return true;   // 6 bytes
        m = before[1]; if (call(before[0], m) && mod(m) == 2 && rm(m) == 4) return true;                                       // 7 bytes
        return false;
    }

    // "speed.exe+2C21A0" style, or the plain address for the exe (its addresses are the ones in the docs).
    inline void FormatAddress(char* out, size_t n, const char* module, uintptr_t base, uintptr_t addr, bool isExe) {
        if (!out || !n) return;
        if (isExe || !module || !module[0]) { snprintf(out, n, "%08X", (unsigned)addr); return; }
        snprintf(out, n, "%s+%X", module, (unsigned)(addr - base));
    }
    // the file name of a module path
    inline const char* BaseName(const char* path) {
        if (!path) return "";
        const char* b = path;
        for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') b = p + 1;
        return b;
    }

    // May an invitation from the partner act here? Not while the game still boots (no profile loaded
    // yet, or the boot screens are up); everywhere else the event code decides as before (the world,
    // loading, and the main menu a quick-race join quit to).
    inline bool RemoteEventCanAct(int flow, bool bootScreensUp, bool profileLoaded) {
        if (!profileLoaded && flow <= 3) return false;
        if (flow == 3 && bootScreensUp) return false;
        return true;
    }

    // "<name> is in an event" while we're in the front end: once per event of theirs.
    struct BusyNotice { uint32_t toldEvent = 0; };
    inline bool PeerBusyInMenuTick(BusyNotice& b, bool connected, int ourFlow, bool peerValid, int peerFlow,
                                   uint32_t peerEvent, bool peerInRace) {
        bool busy = connected && ourFlow == 3 && peerValid && peerFlow == 6 && peerEvent != 0 && peerInRace;
        if (!connected || (peerValid && peerEvent == 0)) b.toldEvent = 0;   // they left it: say it again next time
        if (!busy || b.toldEvent == peerEvent) return false;
        b.toldEvent = peerEvent;
        return true;
    }
}
