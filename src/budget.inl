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
            LOG("Budget: a new car was REFUSED - %d car(s) in the world + %d new (limit 20), car models %u KB of %u KB%s",
                before, asked, used / 1024, pool / 1024, cache ? "" : " (no vehicle cache to make room)");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG("Budget: a new car was REFUSED - %d car(s) in the world + %d new (limit 20)", before, asked);
        }
    }

    bool __cdecl hk_BudgetCheck(void* cache, void* request) {
        int before = CarCount(), asked = 0;
        if (request) Mem::Read((uintptr_t)request + 8, asked);
        bool ok = ((bool(__cdecl*)(void*, void*))BudgetFn)(cache, request);
        int after = CarCount();
        ++g_budget.checks;
        if (before + asked > 20) ++g_budget.overCount;
        if (after < before) g_budget.evictions += before - after;
        if (before > g_budget.peak) g_budget.peak = before;
        if (!ok) {
            ++g_budget.refused;
            DWORD now = GetTickCount();
            if (now - g_budgetLastRefusalLog > 2000) { g_budgetLastRefusalLog = now; LogRefusalSafe(before, asked, cache); }
        } else if (after < before) {
            LOG("Budget: %d car(s) evicted to make room for a new one (%d in the world before, limit 20)", before - after, before);
        }
        return ok;
    }
}

// every 30 s while cars were evicted or refused: one summary line
void Game::BudgetTick() {
    DWORD now = GetTickCount();
    if (!g_budgetSince) g_budgetSince = now;
    if (now - g_budgetSince < 30000) return;
    if (g_budget.evictions || g_budget.refused || g_budgetWaits)
        LOG("Budget: last 30 s - %d car spawn(s), %d over the 20-car limit, %d car(s) evicted, %d refused, %d wait(s) of the other player's cars for room; up to %d cars in the world, %d now",
            g_budget.checks, g_budget.overCount, g_budget.evictions, g_budget.refused, g_budgetWaits, g_budget.peak, CarCount());
    g_budgetWaits = 0;
    g_budget = BudgetStats{};
    g_budgetSince = now;
}

static void InstallBudgetLog() {
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