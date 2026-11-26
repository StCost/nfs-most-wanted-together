// Included from game.cpp (after traffic.inl).
// ---------------------------------------------------------------------------
// The game's car budget, logged
//  PVehicle::Construct (call at 0x689B07) asks 0x687710 cdecl(IVehicleCache* cache, request*) -> bool
//  whether a new car fits. Two limits, both checked there:
//   - count: cars in the world (*0x92CD24) + the new ones (request +8) must stay <= 20 (cmp at 0x687817)
//   - memory: the CarLoader models in use (0x683C80) + the new models must fit the pool (0x739F60:
//     *0x9B2C64, less 512 KB for some loads)
//  Over either one, it asks the listed vehicle caches (0x92FDA4) which cars may go, deletes those
//  (OnRemovedVehicleCache, then the car's destructor) and returns true; with no cache, or nothing it may
//  delete, it returns false and the new car isn't built. We log both: cars evicted to make room, and
//  cars refused, with the count and memory at that moment - so a full budget shows up in the log.
// ---------------------------------------------------------------------------
namespace {
    constexpr uintptr_t BudgetCall = 0x689B07, BudgetFn = 0x687710;
    constexpr uintptr_t CarCountAddr = 0x92CD24, PoolFreeAddr = 0x9B2C64;
    struct BudgetStats { int checks, overCount, evictions, refused, peak; };
    BudgetStats g_budget{};
    DWORD g_budgetSince = 0, g_budgetLastRefusalLog = 0;

    int CarCount() { int n = 0; Mem::Read(CarCountAddr, n); return n; }

    void LogRefusalSafe(int before, int asked, void* cache) {
        __try {
            uint32_t pool = 0; Mem::Read(PoolFreeAddr, pool);
            uint32_t used = ((uint32_t(__cdecl*)())0x683C80)();
            LOG("Budget: a new car was REFUSED - %d car(s) in the world + %d new (limit 20, %d for copies), car models %u KB of %u KB%s",
                before, asked, CopyCarLimit(), used / 1024, pool / 1024, cache ? "" : " (no vehicle cache to make room)");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG("Budget: a new car was REFUSED - %d car(s) in the world + %d new (limit 20)", before, asked);
        }
    }

    // ---- room for 40 cars ----
    //  The game keeps its cars in UTL::FixedVector tables (vtable, mBegin, mCapacity, mSize, then the
    //  storage inline): the 10 vehicle lists at 0x92CD18 (VEHICLE_LIST 0..9, 0x60 apart, 20 each), five more
    //  per-car tables of 20 (0x90D660, 0x913DB0, 0x913E10, 0x913E70, 0x913ED0, 0x920E48) and three of 25
    //  (0x913C50, 0x913CC4, 0x913D38). Found with the 'fixedlists' test command (sizes follow the car count).
    //  Every access in the exe goes through the header (mBegin / mCapacity / mSize - a scan of the exe for
    //  addresses inside their inline storage found none), and a push past mCapacity writes over the next
    //  table. So each table gets a buffer twice its size (mBegin / mCapacity repointed, the cars in it
    //  copied), on the game's own thread. A table its code set back to the inline storage gets it again.
    //  The budget's count limit (cmp eax,0x14 at 0x687817 / lea edx,[eax-0x14] at 0x68781C) then allows 40
    //  for the other player's copies (SpawnVehicle); the game's own cars keep the 20 - its traffic and cops
    //  stay as many as in the plain game, only the copies of the other player's cars get the room above.
    struct CarTable { uintptr_t at; uint32_t cap; };
    constexpr CarTable kCarTables[] = {
        { 0x92CD18, 20 }, { 0x92CD78, 20 }, { 0x92CDD8, 20 }, { 0x92CE38, 20 }, { 0x92CE98, 20 },
        { 0x92CEF8, 20 }, { 0x92CF58, 20 }, { 0x92CFB8, 20 }, { 0x92D018, 20 }, { 0x92D078, 20 },
        { 0x90D660, 20 }, { 0x913DB0, 20 }, { 0x913E10, 20 }, { 0x913E70, 20 }, { 0x913ED0, 20 }, { 0x920E48, 20 },
        { 0x913C50, 25 }, { 0x913CC4, 25 }, { 0x913D38, 25 } };
    constexpr int kTableCount = sizeof(kCarTables) / sizeof(kCarTables[0]);
    constexpr uint32_t kTableScale = 2;
    void* g_tableBuf[kTableCount] = {};
    bool  g_limitPatchOk = false;       // the budget's 20 is where we expect it
    bool  g_tableWarned[kTableCount] = {};
    constexpr uintptr_t LimitCmpImm = 0x687819, LimitLeaImm = 0x68781E;

    // the table at kCarTables[i]: false if it doesn't look like the FixedVector we know (left alone)
    bool GrowTable(int i, bool& grown) {
        grown = false;
        const CarTable& t = kCarTables[i];
        uint32_t vt = 0, begin = 0, cap = 0, size = 0;
        if (!Mem::Read(t.at, vt) || !Mem::Read(t.at + 4, begin) || !Mem::Read(t.at + 8, cap) || !Mem::Read(t.at + 12, size)) return false;
        uint32_t big = t.cap * kTableScale;
        if (g_tableBuf[i] && begin == (uint32_t)(uintptr_t)g_tableBuf[i] && cap == big) return true;     // already ours
        if (begin != t.at + 0x10 || cap != t.cap || size > cap || vt < 0x880000 || vt > 0x8C0000) {
            if (!g_tableWarned[i]) { g_tableWarned[i] = true; LOG("Budget: car table %08X not as expected (begin %08X capacity %u size %u) - left at %u", (unsigned)t.at, begin, cap, size, t.cap); }
            return false;
        }
        if (!g_tableBuf[i]) g_tableBuf[i] = VirtualAlloc(nullptr, big * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);   // never freed
        if (!g_tableBuf[i]) return false;
        memset(g_tableBuf[i], 0, big * 4);
        memcpy(g_tableBuf[i], (const void*)(t.at + 0x10), size * 4);
        uint32_t nb = (uint32_t)(uintptr_t)g_tableBuf[i];
        Mem::Write(t.at + 4, nb);
        Mem::Write(t.at + 8, big);
        grown = true;
        return true;
    }
    void GrowCarTables() {
        __try {
            int ok = 0, grownNow = 0;
            for (int i = 0; i < kTableCount; ++i) { bool g = false; if (GrowTable(i, g)) ++ok; if (g) ++grownNow; }
            bool all = ok == kTableCount;
            if (grownNow) LOG("Budget: %d car table(s) moved to buffers twice their size (%d of %d ready)", grownNow, ok, kTableCount);
            if (all != g_tablesBig) {
                g_tablesBig = all;
                LOG("Budget: room for %d cars %s", kCopyCarLimit, all ? "ready (the other player's copies may use it)" : "NOT ready - the 20-car limit stays");
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_tablesBig = false; }
    }
    void SetCountLimit(int n) {
        uint8_t cur = 0; Mem::Read(LimitCmpImm, cur);
        if (cur == (uint8_t)n) return;
        uint8_t a = cur, na = (uint8_t)n, b = (uint8_t)(-(int)cur), nb = (uint8_t)(-n);
        Mem::PatchBytes(LimitCmpImm, &a, &na, 1);
        Mem::PatchBytes(LimitLeaImm, &b, &nb, 1);
    }

    bool __cdecl hk_BudgetCheck(void* cache, void* request) {
        int before = CarCount(), asked = 0;
        if (request) Mem::Read((uintptr_t)request + 8, asked);
        // the other player's copies: up to 40. The game's own cars: their 20 next to the copies in the world
        // (20 + copies, at most 40), so its traffic and cops are as many as in the plain game.
        bool roomy = g_tablesBig && g_limitPatchOk;
        if (roomy) {
            int copies = 0;
            if (!g_copySpawn) for (void* k : g_keepCars) if (k && ListContains(-1, k)) ++copies;
            int lim = g_copySpawn ? kCopyCarLimit : kGameCarLimit + copies;
            SetCountLimit(lim > kCopyCarLimit ? kCopyCarLimit : lim);
        }
        bool ok = ((bool(__cdecl*)(void*, void*))BudgetFn)(cache, request);
        if (roomy) SetCountLimit(kGameCarLimit);
        int after = CarCount();
        ++g_budget.checks;
        if (before + asked > kGameCarLimit) ++g_budget.overCount;
        if (after < before) g_budget.evictions += before - after;
        if (before > g_budget.peak) g_budget.peak = before;
        if (!ok) {
            ++g_budget.refused;
            DWORD now = GetTickCount();
            if (now - g_budgetLastRefusalLog > 2000) { g_budgetLastRefusalLog = now; LogRefusalSafe(before, asked, cache); }
        } else if (after < before) {
            LOG("Budget: %d car(s) evicted to make room for a new one (%d in the world before, limit 20, %d for copies)", before - after, before, CopyCarLimit());
        }
        return ok;
    }
}

// every 30 s while cars were evicted or refused: one summary line
void Game::BudgetTick() {
    GrowCarTables();   // every frame: a few reads (a table set back by its own code is grown again at once)
    DWORD now = GetTickCount();
    if (!g_budgetSince) g_budgetSince = now;
    if (now - g_budgetSince < 30000) return;
    if (g_budget.evictions || g_budget.refused || g_budgetWaits)
        LOG("Budget: last 30 s - %d car spawn(s), %d over 20 cars, %d car(s) evicted, %d refused, %d wait(s) of the other player's cars for room; up to %d cars in the world, %d now",
            g_budget.checks, g_budget.overCount, g_budget.evictions, g_budget.refused, g_budgetWaits, g_budget.peak, CarCount());
    g_budgetWaits = 0;
    g_budget = BudgetStats{};
    g_budgetSince = now;
}

static void InstallBudgetLog() {
    const uint8_t cmp[3] = { 0x83, 0xF8, 0x14 }, lea[3] = { 0x8D, 0x50, 0xEC };   // cmp eax,0x14 / lea edx,[eax-0x14]
    g_limitPatchOk = !memcmp((void*)(LimitCmpImm - 2), cmp, 3) && !memcmp((void*)(LimitLeaImm - 2), lea, 3);
    LOG("Budget: count limit %s", g_limitPatchOk ? "found (20, 40 for the other player's copies once the tables are grown)" : "NOT found (code differs) - 20 for all");
    // The budget's eviction path (over the count or the memory limit) lists every car in the world (list
    // 0x92C510) into a local UTL::FixedVector of 20 (vtable 0x8AA234, 0x18-byte entries, storage on the
    // stack). With more than 20 cars a push past its capacity wrote over the stack - the car pointer it
    // reloads at 0x687903 - and the game crashed at 0x687956 (seen with 26 cars: 0xC0000409). The full
    // branch (inc edx / push edx / ... grow at 0x6878B9) now restores ebp and skips to the next car:
    // the eviction picks from the first 20 cars listed.
    {
        const uint8_t orig[9] = { 0x42, 0x52, 0x8B, 0x54, 0x24, 0x30, 0x8D, 0x4C, 0x24 };
        const uint8_t patch[9] = { 0x8B, 0xAC, 0x24, 0x44, 0x02, 0x00, 0x00,     // mov ebp,[esp+244h]
                                   0xEB, 0x4D };                                 // jmp 0x68790F (next car)
        bool full = Mem::PatchBytes(0x6878B9, orig, patch, sizeof(patch));
        LOG("Budget: eviction list kept to its 20 %s", full ? "(no stack overrun past 20 cars)" : "NOT patched (bytes differ) - more than 20 cars can crash an eviction");
        if (!full) g_limitPatchOk = false;   // then stay at 20
    }
    bool ok = PatchCallChecked(BudgetCall, BudgetFn, (void*)&hk_BudgetCheck);
    LOG("Budget: car budget %s", ok ? "logged (evictions / refusals)" : "NOT logged (call site differs)");
}

// test command "fixedlists": every UTL::FixedVector in the exe's data (vtable, mBegin == &self + 0x10,
// mCapacity, mSize) - the fixed per-object tables, with their capacity and how full they are now. Shows
// which tables would overflow if the 20-car limit were raised.
static int FixedListsScan() {
    __try {
        auto* base = (const uint8_t*)GetModuleHandleA(nullptr);
        auto* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        int found = 0;
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
            if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
            uintptr_t p = (uintptr_t)base + sec->VirtualAddress, end = p + sec->Misc.VirtualSize - 16;
            for (; p < end; p += 4) {
                uint32_t vt = *(uint32_t*)p, begin = *(uint32_t*)(p + 4), cap = *(uint32_t*)(p + 8), size = *(uint32_t*)(p + 12);
                if (begin != p + 0x10 || cap == 0 || cap > 4096 || size > cap) continue;
                if (vt < 0x880000 || vt > 0x8C0000) continue;            // a vtable in .rdata
                LOG("FixedVector at %08X: vtable %08X capacity %u size %u", (unsigned)p, vt, cap, size);
                ++found;
            }
        }
        return found;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
std::string Game::DebugFixedLists() { int n = FixedListsScan(); return n < 0 ? "scan failed" : "fixed tables: " + std::to_string(n) + " (listed in the log)"; }