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

    // ---- room for MaxCars cars ([Traffic] MaxCars, 40 by default) ----
    //  The game keeps its cars in UTL::FixedVector tables (vtable, mBegin, mCapacity, mSize, then the
    //  storage inline): the 10 vehicle lists at 0x92CD18 (VEHICLE_LIST 0..9, 0x60 apart, 20 each), five more
    //  per-car tables of 20 (0x90D660, 0x913DB0, 0x913E10, 0x913E70, 0x913ED0, 0x920E48) and three of 25
    //  (0x913C50, 0x913CC4, 0x913D38). Found with the 'fixedlists' test command (sizes follow the car count).
    //  Every access in the exe goes through the header (mBegin / mCapacity / mSize - a scan of the exe for
    //  addresses inside their inline storage found none), and a push past mCapacity writes over the next
    //  table. So each table gets a buffer of MaxCars entries ([Traffic] MaxCars, +5 for the lists of 25; mBegin / mCapacity repointed, the cars in it
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
        uint32_t big = (uint32_t)CopyCarCap() > t.cap ? (uint32_t)CopyCarCap() + (t.cap - 20) : t.cap;   // MaxCars (+5 for the lists of 25)
        if (g_tableBuf[i] && begin == (uint32_t)(uintptr_t)g_tableBuf[i] && cap == big) return true;     // already ours
        if (begin != t.at + 0x10 && cap >= big && size <= cap && begin) return true;                  // grown on its own (growable lists)
        if (!begin && !cap) return false;   // not set up yet (0x90D660 at start-up): the next frames try again
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
            if (grownNow) LOG("Budget: %d car table(s) moved to bigger buffers (%d of %d ready)", grownNow, ok, kTableCount);
            if (all != g_tablesBig) {
                g_tablesBig = all;
                LOG("Budget: room for %d cars %s", CopyCarCap(), all ? "ready (the other player's copies may use it)" : "NOT ready - the 20-car limit stays");
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

    // ---- address space ----
    //  speed.exe is 32-bit and not Large Address Aware: 2 GB for everything. 40 cars take ~430 MB private
    //  memory / ~740 MB of address space (measured, two games on the laptop); each extra car ~1-2.5 MB.
    //  Above kVaTight MB in use the room above 20 cars is not used (the game's own 20 stay), logged.
    constexpr uint32_t kVaWarn = 1400, kVaTight = 1600;
    uint32_t AddressSpaceUsedMB() {
        MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms);
        if (!GlobalMemoryStatusEx(&ms)) return 0;
        return (uint32_t)((ms.ullTotalVirtual - ms.ullAvailVirtual) / (1024 * 1024));
    }
    bool AddressSpaceTight() {
        static DWORD at = 0; static bool tight = false;
        DWORD now = GetTickCount();
        if (now - at < 2000) return tight;
        at = now;
        uint32_t mb = AddressSpaceUsedMB();
        bool was = tight; tight = mb >= kVaTight;
        static bool warned = false;
        if (mb >= kVaWarn && !warned) { warned = true; LOG("Budget: the game uses %u MB of its 2048 MB address space (32-bit) - getting full", mb); }
        if (tight != was) LOG("Budget: %u MB of 2048 MB address space in use - room above 20 cars %s", mb, tight ? "NOT used now (memory)" : "used again");
        return tight;
    }

    // which cars an eviction took (names, ours / a copy / a cop), for the log - noted before the check, as
    // an evicted car is freed by then
    struct CarNote { void* iv; char name[16]; bool copy, cop; };
    void SnapCars(CarNote* out, int& n) {
        n = 0; void** b = nullptr; int cnt = ListSize(VL_ALL, &b);
        for (int i = 0; i < cnt && n < 128; ++i) if (void* t = ListGet(VL_ALL, i)) {
            CarNote& c = out[n++]; c.iv = t;
            strncpy_s(c.name, VehName(t), _TRUNCATE);
            c.copy = IsMirrorCar(t); c.cop = ListContainsOne(VL_AICOPS, t);
        }
    }
    void SnapCarsSafe(CarNote* out, int& n) { __try { SnapCars(out, n); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; } }
    void LogEvictedSafe(const CarNote* was, int nWas) {
        __try {
            void** b = nullptr; int cnt = ListSize(VL_ALL, &b);
            char buf[400] = ""; size_t L = 0;
            for (int i = 0; i < nWas && L < 360; ++i) {
                bool still = false;
                for (int j = 0; j < cnt; ++j) if (ListGet(VL_ALL, j) == was[i].iv) { still = true; break; }
                if (!still) L += sprintf_s(buf + L, sizeof(buf) - L, " %s(%s%s)", was[i].name, was[i].copy ? "copy" : "ours", was[i].cop ? ", cop" : "");
            }
            uint32_t pool = 0; Mem::Read(PoolFreeAddr, pool);
            uint32_t used = ((uint32_t(__cdecl*)())0x683C80)();
            LOG("Budget: evicted:%s - car models %u KB of %u KB", buf[0] ? buf : " (none found)", used / 1024, pool / 1024);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    bool __cdecl hk_BudgetCheck(void* cache, void* request) {
        int before = CarCount(), asked = 0;
        CarNote was[128]; int nWas = 0;
        SnapCarsSafe(was, nWas);
        if (request) Mem::Read((uintptr_t)request + 8, asked);
        // the other player's copies: up to 40. The game's own cars: their 20 next to the copies in the world
        // (20 + copies, at most 40), so its traffic and cops are as many as in the plain game.
        bool roomy = g_tablesBig && g_limitPatchOk && !AddressSpaceTight();
        if (roomy) {
            int copies = 0;
            if (!g_copySpawn) for (void* k : g_keepCars) if (k && ListContains(-1, k)) ++copies;
            int lim = g_copySpawn ? CopyCarCap() : kGameCarLimit + copies;
            SetCountLimit(lim > CopyCarCap() ? CopyCarCap() : lim);
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
            LogEvictedSafe(was, nWas);
        }
        return ok;
    }
}

// every 30 s while cars were evicted or refused: one summary line
// ---- the car loader's 40 ride slots ----
//  Past 40 cars the game stopped at an assert (int 3 at 0x759FB1, then a jump to 0x42652EE0): every car in the
//  world holds one slot of CarLoadedRideInfoSlotPool (*0x9B0A0C, 0x6D4 bytes a slot), made with 40 slots by
//  0x73A350 (push 28h at 0x73A394), and CarLoader 0x75BB40 waits for a free one - unloading unused ones - and
//  asserts when none is left. The pool can grow (the game's own add-slots routine 0x465DE0(pool, n), as its
//  other car pools do), so it gets MaxCars + 40 slots (~1.7 KB each); the create byte is raised as well in case
//  the pools are made again. Total slots: pool +0x2C.
namespace {
    constexpr uintptr_t RidePoolPtr = 0x9B0A0C, RidePoolCreateCount = 0x73A395, PoolAddSlots = 0x465DE0;
    int RideSlotsWanted() { int n = CopyCarCap() + 40; return n < 40 ? 40 : n; }
    void RidePoolTick() {
        static bool patched = false;
        if (!patched) {
            patched = true;
            uint8_t cur = 0x28, want = (uint8_t)(RideSlotsWanted() > 127 ? 127 : RideSlotsWanted());
            bool ok = Mem::PatchBytes(RidePoolCreateCount, &cur, &want, 1);
            LOG("Budget: car ride slots made with %u %s", (unsigned)want, ok ? "(when the game makes the pool)" : "- NOT patched (byte differs)");
        }
        static void* grownPool = nullptr;
        __try {
            void* pool = nullptr; int total = 0;
            if (!Mem::Read(RidePoolPtr, pool) || !pool || pool == grownPool) return;
            if (!Mem::Read((uintptr_t)pool + 0x2C, total) || total <= 0 || total > 10000) return;
            int want = RideSlotsWanted();
            if (total < want) ((void(__thiscall*)(void*, int))PoolAddSlots)(pool, want - total);
            int now = 0; Mem::Read((uintptr_t)pool + 0x2C, now);
            grownPool = pool;
            LOG("Budget: car ride slots %d -> %d (one per car in the world)", total, now);
        } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Budget: exception growing the car ride slots"); grownPool = (void*)1; }
    }
}

static void FvGrowLog();
void Game::BudgetTick() {
    GrowCarTables();
    RidePoolTick();
    FvGrowLog();   // every frame: a few reads (a table set back by its own code is grown again at once)
    DWORD now = GetTickCount();
    if (!g_budgetSince) g_budgetSince = now;
    if (now - g_budgetSince < 30000) return;
    if (g_budget.evictions || g_budget.refused || g_budgetWaits)
        LOG("Budget: last 30 s - %d car spawn(s), %d over 20 cars, %d car(s) evicted, %d refused, %d wait(s) of the other player's cars for room; up to %d cars in the world, %d now; %u MB of 2048 MB address space in use",
            g_budget.checks, g_budget.overCount, g_budget.evictions, g_budget.refused, g_budgetWaits, g_budget.peak, CarCount(), AddressSpaceUsedMB());
    g_budgetWaits = 0;
    g_budget = BudgetStats{};
    g_budgetSince = now;
}

// ---- every UTL::FixedVector may grow ----
//  The game's fixed-size lists (UTL::FixedVector<T, N>: vtable, mBegin, mCapacity, mSize, storage inline at +0x10)
//  get their room from virtual calls: slot 1 AllocVectorSpace(n, align) returns the inline storage whatever n is,
//  slot 2 FreeVectorSpace(p, n) does nothing, slot 3 GetGrowSize(n) returns N. reserve(n) (e.g. 0x674060) still
//  sets mCapacity = n - so a list of N that gets more than N entries writes past its storage. With more than 20
//  cars that is what crashed: the budget's eviction list (a stack FixedVector of 20, 0x687956) and the race start
//  (0x765305, a call to 0). Every FixedVector type in the exe, whatever its size (120 types; with MaxCars above 40 its lists of 40 overran too) (found by its vtable: slot 1
//  "lea eax,[ecx+10h] / ret 8", slots 3 / 4 "mov eax,N / ret") now grows: up to N entries the inline storage as
//  before, more than that a heap buffer (slot 3 asks for twice N at least), freed by slot 2. Nothing writes past
//  a list's storage any more, in any mode. The element size isn't in the vtable: the heap buffer gives every
//  entry kFvMaxElem bytes (more than any list of 20 here holds per entry).
namespace {
    constexpr uint32_t kFvMaxElem = 512;
    HANDLE g_fvHeap = nullptr;
    CRITICAL_SECTION g_fvCs;
    struct FvBlock { void* aligned; void* raw; };
    FvBlock g_fvBlocks[1024] = {};
    volatile LONG g_fvGrown = 0, g_fvLive = 0, g_fvFailed = 0;
    struct FvEvent { uint32_t vt, n, cap; bool ok; };
    FvEvent g_fvEvents[32]; volatile LONG g_fvEventN = 0;   // ring of the latest growths (logged by FvGrowLog)
    void FvNote(void* self, uint32_t n, uint32_t cap, bool ok) {
        LONG i = InterlockedIncrement(&g_fvEventN) - 1;
        FvEvent& e = g_fvEvents[i & 31];
        e.vt = *(const uint32_t*)self; e.n = n; e.cap = cap; e.ok = ok;
        if (!ok) InterlockedIncrement(&g_fvFailed);
    }

    void* FvHeapAlloc(uint32_t n) {
        if (!g_fvHeap || n > 100000) return nullptr;
        void* raw = HeapAlloc(g_fvHeap, HEAP_ZERO_MEMORY, (SIZE_T)n * kFvMaxElem + 16);
        if (!raw) return nullptr;
        void* al = (void*)(((uintptr_t)raw + 15) & ~(uintptr_t)15);
        EnterCriticalSection(&g_fvCs);
        bool kept = false;
        for (auto& b : g_fvBlocks) if (!b.aligned) { b.aligned = al; b.raw = raw; kept = true; break; }
        LeaveCriticalSection(&g_fvCs);
        if (!kept) { HeapFree(g_fvHeap, 0, raw); return nullptr; }
        InterlockedIncrement(&g_fvGrown); InterlockedIncrement(&g_fvLive);
        return al;
    }
    bool FvHeapFree(void* p) {
        void* raw = nullptr;
        EnterCriticalSection(&g_fvCs);
        for (auto& b : g_fvBlocks) if (b.aligned == p && p) { raw = b.raw; b.aligned = b.raw = nullptr; break; }
        LeaveCriticalSection(&g_fvCs);
        if (!raw) return false;
        HeapFree(g_fvHeap, 0, raw); InterlockedDecrement(&g_fvLive);
        return true;
    }
    // each list type's own size, by its vtable (filled once at start-up, read-only after)
    struct FvType { uint32_t vt, cap; };
    FvType g_fvTypes[256]; int g_fvTypeN = 0;
    uint32_t FvCapOf(const void* self) {
        uint32_t vt = *(const uint32_t*)self;
        for (int i = 0; i < g_fvTypeN; ++i) if (g_fvTypes[i].vt == vt) return g_fvTypes[i].cap;
        return 0;
    }
    // __thiscall (this in ecx, callee pops) as __fastcall with the edx slot unused
    void* __fastcall FvAlloc(void* self, void*, uint32_t n, uint32_t) {
        uint32_t cap = FvCapOf(self);
        if (n <= cap || !cap) return (uint8_t*)self + 0x10;
        void* p = FvHeapAlloc(n);
        FvNote(self, n, cap, p != nullptr);
        return p ? p : (uint8_t*)self + 0x10;          // (no memory: the old behaviour - logged as NOT grown)
    }
    void __fastcall FvFree(void*, void*, void* p, uint32_t) { FvHeapFree(p); }   // inline / not ours: nothing
    uint32_t __fastcall FvGrowSize(void* self, void*, uint32_t n) {
        uint32_t cap = FvCapOf(self);
        return n <= cap ? cap : (n <= 2 * cap ? 2 * cap : n + cap);
    }
    bool CodeIs(uintptr_t fn, const uint8_t* bytes, size_t len) { return Mem::IsReadable((const void*)fn, len) && !memcmp((const void*)fn, bytes, len); }
    bool ImmRet(uintptr_t fn, uint32_t& imm, bool ret4) {   // mov eax,imm32 / ret (4)
        uint8_t b[8];
        if (!Mem::IsReadable((const void*)fn, 8)) return false;
        memcpy(b, (const void*)fn, 8);
        if (b[0] != 0xB8) return false;
        memcpy(&imm, b + 1, 4);
        return ret4 ? (b[5] == 0xC2 && b[6] == 0x04 && b[7] == 0x00) : b[5] == 0xC3;
    }
}
static void FvGrowLog() {   // when a fixed list grew past its size: once per change
    // every growth of a game list past its own size (which list by its vtable, to how many entries); at most
    // 8 lines a second, the rest counted
    static LONG done = 0; static DWORD secAt = 0; static int inSec = 0, skipped = 0;
    LONG upTo = g_fvEventN;
    if (upTo - done > 32) { skipped += upTo - done - 32; done = upTo - 32; }
    for (; done < upTo; ++done) {
        DWORD now = GetTickCount();
        if (now - secAt > 1000) { secAt = now; inSec = 0; }
        if (++inSec > 8) { ++skipped; continue; }
        const FvEvent& e = g_fvEvents[done & 31];
        if (e.ok) LOG("Budget: game list %08X (made for %u) grew to %u entries - %ld grown so far, %ld on the heap now",
                      e.vt, e.cap, e.n, (long)g_fvGrown, (long)g_fvLive);
        else LOG("Budget: game list %08X (made for %u) could NOT grow to %u entries (no memory / too many) - it may overrun", e.vt, e.cap, e.n);
    }
    if (skipped && inSec <= 8) { LOG("Budget: %d more list growth(s) not logged (too many at once)", skipped); skipped = 0; }
}
static void InstallGrowableFixedVectors() {
    InitializeCriticalSection(&g_fvCs);
    g_fvHeap = HeapCreate(0, 0, 0);
    static const uint8_t kAllocInline[6] = { 0x8D, 0x41, 0x10, 0xC2, 0x08, 0x00 };   // lea eax,[ecx+10h] / ret 8
    int n20 = 0, n25 = 0, other = 0;
    __try {
        auto* base = (const uint8_t*)GetModuleHandleA(nullptr);
        auto* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        uintptr_t textLo = 0, textHi = 0;
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) { textLo = (uintptr_t)base + sec[i].VirtualAddress; textHi = textLo + sec[i].Misc.VirtualSize; break; }
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (sec[i].Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE)) continue;   // vtables: read-only data
            uintptr_t p = (uintptr_t)base + sec[i].VirtualAddress, end = p + sec[i].Misc.VirtualSize - 24;
            for (; p < end; p += 4) {
                const uint32_t* vt = (const uint32_t*)p;
                bool code = true;
                for (int k = 1; k <= 4; ++k) if (vt[k] < textLo || vt[k] >= textHi) { code = false; break; }
                if (!code || !CodeIs(vt[1], kAllocInline, 6)) continue;
                uint32_t cap = 0, cap2 = 0;
                if (!ImmRet(vt[3], cap, true) || !ImmRet(vt[4], cap2, false) || cap != cap2) continue;
                if (!cap || cap > 100000 || g_fvTypeN >= 256) { ++other; continue; }
                g_fvTypes[g_fvTypeN++] = { (uint32_t)p, cap };
                if (cap == 20) ++n20; else if (cap == 25) ++n25; else ++other;
                void *a = (void*)&FvAlloc, *f = (void*)&FvFree, *g = (void*)&FvGrowSize;
                Mem::PatchPointer((void*)(p + 4), a);
                Mem::PatchPointer((void*)(p + 8), f);
                Mem::PatchPointer((void*)(p + 12), g);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Budget: exception while making the fixed lists growable"); }
    LOG("Budget: the game's fixed lists made growable: %d types (%d of 20, %d of 25, %d of other sizes)", g_fvTypeN, n20, n25, g_fvTypeN - n20 - n25);
}

static void InstallBudgetLog() {
    const uint8_t cmp[3] = { 0x83, 0xF8, 0x14 }, lea[3] = { 0x8D, 0x50, 0xEC };   // cmp eax,0x14 / lea edx,[eax-0x14]
    g_limitPatchOk = !memcmp((void*)(LimitCmpImm - 2), cmp, 3) && !memcmp((void*)(LimitLeaImm - 2), lea, 3);
    LOG("Budget: count limit %s", g_limitPatchOk ? "found (20, [Traffic] MaxCars for the other player's copies once the tables are grown)" : "NOT found (code differs) - 20 for all");
    InstallGrowableFixedVectors();
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
// ---- the car model memory pool ----
//  At boot (0x664A24) the game reads *0x8F7EF0 (KB), shifts it to bytes (<<10), sizes its main heap as
//  0x7EF4000 + that and gives it to the "Cars" pool (CarLoader::SetMemoryPoolSize 0x75C550): 50000 KB, made
//  for 20 cars. A second reader (0x666EC1, SetMemoryPoolSize at 0x666EEA) takes *0x8F7EF4 instead in one
//  mode (split screen); that one is left alone. With more cars - the
//  partner's traffic adds its own models - a new model often didn't fit and the budget evicted cars that
//  were in use (cops vanished and came back). Set before the game reads it (DllMain): +32 MB with MaxCars > 20.
void Game::InstallCarPoolSize() {
    constexpr uintptr_t kCarsPoolKB = 0x8F7EF0;
    constexpr int32_t kStock = 50000, kExtraKB = 32 * 1024;
    int32_t kb = 0;
    if (!Mem::Read(kCarsPoolKB, kb) || kb != kStock) { LOG("Budget: car model pool not resized (%d KB, not the 1.3 default)", kb); return; }
    if (g_cfg.maxCars <= 20) return;
    int32_t want = kStock + kExtraKB;
    LOG("Budget: car model pool %d KB -> %d KB %s", kStock, want, Mem::Write(kCarsPoolKB, want) ? "" : "(write FAILED)");
}
