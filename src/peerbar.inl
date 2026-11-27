// ---- the other player's pursuit bar: a copy of the game's own BUSTED / EVADE meter, right above ours ----
// (speed.exe 1.3; HUD_SingleRace.fng, 640x480 units, origin at the screen centre, +y down.)
// The meter is drawn by PursuitBoard (Update 0x57AEE0; BustedMeter 0x568EB0 is an empty stub) with FEng
// objects of the HUD package, all under one parent group (0xAB9C1D4D):
//   0x7B422BA3  the chase meter: label panel y 161..193 (BUSTED / EVADE), the strip at y 196 (5 high)
//       0xE0E0169B BUSTED far (54 wide, grows left)    0x8EEEBD33 BUSTED near (48, grows left)
//       0x47A4E2A9 the middle mark (script only)       0x8E3653A6 EVADE near (48, grows right)
//       0x30F39A6F EVADE far (54, grows right)         0x988B8325 the track (216)
//       0x8C20A763 / 0x6A144066 the backup timer panel and text (below the strip)
//   0x84A226EC  the cooldown meter: the strip at y 196, its panel BELOW it (y 199..231)
//       0xCF817638 the bar (216, grows right)  0xA1EB7E6E the track
//       0x5C2A4F20 / 0x60B45523 / 0x38074901 the HIDDEN panel, text and icon (above the strip)
//   0xE0B1430B  the cop-count row (y 133..164), right above the meter
// The fill is the WIDTH of a segment (FEngSetSize 0x5155E0), then the fixed edge is put back
// (FEngSetBottomRight 0x524E20 / FEngSetTopLeft 0x524CC0) - the game's own steps at 0x57B2B0..0x57B612.
//
// The copy: FEGroup::Clone (vtable slot 1, 0x5C49C0; the game does the same for the credits lines at
// 0x533207) copies a group with everything in it. What the copy constructor (0x5C31A0) leaves out:
//   - the object's data (+0x2C, size +0x30) is zeroed: copied here;
//   - +0x58 (the cached render object) is not initialised: zeroed here (a crash at draw / unload otherwise).
// And so the copy never talks to the real HUD: its name hashes are cleared (FEngFindObject finds the
// originals), its message responses get an id nobody sends, and its scripts lose their events
// (FEEventList::SetCount 0x5B5350) - the originals' scripts tell the cop row to show and play sounds.
// The copies are added to the same parent (FEGroup::AddObject 0x5B5420), so the package updates, draws and
// frees them with the rest. Render contexts are handed out per frame (FEngine::RenderGroup 0x5BB9C0).
//
// Position: every group script carries its own position (track "7" = data +0x1C), so a plain write to the
// data is undone by the scripts. FEObject::SetPosition (0x5B8190, thiscall(const float[3], bool relative))
// shifts the data AND that track in every script of the object - used for the copies (up by the offset)
// and, while the partner bar shows, for the cop row and the HIDDEN icon (put back when it hides).
// Shown / hidden with the groups' own fade scripts 0x5079C8F8 / 0x33113AC, as PursuitBoard does.
namespace {
namespace PB {
    constexpr uint32_t kChase = 0x7B422BA3, kCool = 0x84A226EC, kRow = 0xE0B1430B, kIcon = 0x38074901;
    constexpr uint32_t kBustFar = 0xE0E0169B, kBustNear = 0x8EEEBD33, kMid = 0x47A4E2A9, kEvadeNear = 0x8E3653A6,
                       kEvadeFar = 0x30F39A6F, kTrack = 0x988B8325, kBackupPanel = 0x8C20A763, kBackupText = 0x6A144066,
                       kCoolBar = 0xCF817638, kCoolTrack = 0xA1EB7E6E, kHiddenPanel = 0x5C2A4F20, kHiddenText = 0x60B45523;
    // script names (FEHashUpper): INIT, SHOW, HIDE, PULSE, ALERT; the two fades are not in the exe by name
    constexpr uint32_t sInit = 0x1744B3, sShow = 0x1CA7C0, sHide = 0x16A259, sPulse = 0x3826A28, sAlert = 0x26DED57,
                       sFadeIn = 0x5079C8F8, sFadeOut = 0x33113AC, sFlipIn = 0x13F51124;
    constexpr uintptr_t kFEGroupVtbl = 0x8A2E24, kFEGroupClone = 0x5C49C0;
    // how far up a copy sits: the chase meter's own height (panel + strip), or - for the cooldown copy, whose
    // panel hangs below its strip - the whole height when our own meter has something above its strip
    constexpr float kUpNear = 38.f, kUpFar = 70.f;
    const char* const kPkgName = "HUD_SingleRace.fng";

    struct State {
        void* pkg = nullptr;
        void* chase = nullptr; void* cool = nullptr; void* row = nullptr; void* icon = nullptr;   // the game's own
        void* ownHidden = nullptr;                              // the game's own HIDDEN panel (shown: we are in a hiding spot)
        void* parChase = nullptr; void* parCool = nullptr;      // their parent groups (null: the package's own list)
        void* cChase = nullptr; void* cCool = nullptr;          // the copies
        void* seg[5] = {};                                      // in cChase: BUSTED far, near, the middle, EVADE near, far
        void* coolBar = nullptr;                                // in cCool
        void* hide[6] = {}; int hideN = 0;                      // parts of the copies that never show (backup timer, HIDDEN)
        float wFar = 54.f, wNear = 48.f, wCool = 216.f;         // the full widths
        float upChase = 0, upCool = 0, lift = 0;                // applied now: the copies' offsets, the cop row's lift
        int objects = 0;
        bool ready = false;
        void* deadPkg = nullptr; void* deadChase = nullptr;     // cloning failed for this package: not tried again
        bool forced = false; int forceKind = 0, forceValue = 0; // test: DebugPeerBar
    } g;

    inline uint8_t* B(void* o) { return (uint8_t*)o; }
    inline int   Type(void* o)  { return *(int*)(B(o) + 0x18); }
    inline uint32_t Hash(void* o) { return *(uint32_t*)(B(o) + 0x10); }
    inline uint8_t* Data(void* o) { return *(uint8_t**)(B(o) + 0x2C); }
    inline void* Next(void* o)  { return *(void**)(B(o) + 4); }
    inline void* FirstChild(void* grp) { return *(void**)(B(grp) + 0x64); }
    inline int   ChildCount(void* grp) { return *(int*)(B(grp) + 0x60); }
    inline bool  Sane(void* o, size_t len = 0x60) { return (uintptr_t)o >= 0x10000 && Mem::IsReadable(o, len); }

    void* Package() {
        void* fe = nullptr;
        if (!Mem::Read(0x91CADC, fe) || !fe) return nullptr;
        return ((void*(__thiscall*)(void*, const char*))0x516AF0)(fe, kPkgName);          // cFEng::FindPackage
    }
    void* Find(void* pkg, uint32_t hash, int type) {
        void* o = ((void*(__thiscall*)(void*, uint32_t))0x5B8550)(pkg, hash);             // FEPackage::FindObjectByHash
        return (o && Sane(o) && Type(o) == type && Sane(Data(o), 0x44)) ? o : nullptr;
    }
    bool InList(void* first, void* what) {
        int n = 0;
        for (void* o = first; o && n < 600; o = Next(o), ++n) if (o == what) return true;   // (under __try: no per-node VirtualQuery)
        return false;
    }
    bool IsChild(void* pkg, void* parent, void* what) {
        return InList(parent ? FirstChild(parent) : *(void**)(B(pkg) + 0x4C), what);
    }
    // the group that holds `what` (found = true, *parent null when it is in the package's own list)
    bool FindParent(void* first, void* holder, void* what, void** parent, int depth) {
        int n = 0;
        for (void* o = first; o && n < 600; o = Next(o), ++n) {
            if (!Sane(o)) return false;
            if (o == what) { *parent = holder; return true; }
            if (Type(o) == 5 && depth < 10 && FindParent(FirstChild(o), o, what, parent, depth + 1)) return true;
        }
        return false;
    }

    bool ScriptIs(void* o, uint32_t hash) { return ((bool(__cdecl*)(void*, uint32_t))0x514DA0)(o, hash); }          // FEngIsScriptSet
    void SetScript(void* o, uint32_t hash) { ((void(__cdecl*)(void*, uint32_t, bool))0x514D10)(o, hash, true); }    // FEngSetScript (from its start)
    void Want(void* o, uint32_t hash) { if (o && !ScriptIs(o, hash)) SetScript(o, hash); }
    void Dirty(void* o) { *(uint32_t*)(B(o) + 0x1C) |= 0x400000; }
    void SetPos(void* o, const float* v, bool relative) {
        ((void(__thiscall*)(void*, const float*, bool))0x5B8190)(o, v, relative);         // FEObject::SetPosition
    }
    void MoveBy(void* o, float dy) {
        if (!o || dy == 0.f) return;
        float v[3] = { 0.f, dy, 0.f };
        SetPos(o, v, true);
    }
    void HideNow(void* o) {              // gone at once, whatever the script state: the HIDE script and alpha 0
        if (!o) return;
        Want(o, sHide);
        *(int*)(Data(o) + 0x0C) = 0;
        Dirty(o);
    }

    // the two trees have the same shape and data sizes (nothing is written)
    bool SameShape(void* a, void* b, int depth, int* count) {
        if (!Sane(a) || !Sane(b) || Type(a) != Type(b)) return false;
        uint32_t sa = *(uint32_t*)(B(a) + 0x30), sb = *(uint32_t*)(B(b) + 0x30);
        if (sa != sb || sa < 0x44 || sa > 0x400 || !Sane(Data(a), sa) || !Sane(Data(b), sb)) return false;
        if (++*count > 400) return false;
        if (Type(a) != 5) return true;
        if (depth > 8 || ChildCount(a) != ChildCount(b)) return false;
        void* ca = FirstChild(a); void* cb = FirstChild(b);
        for (int i = 0, n = ChildCount(a); i < n; ++i, ca = Next(ca), cb = Next(cb)) {
            if (!ca || !cb || !SameShape(ca, cb, depth + 1, count)) return false;
        }
        return true;
    }
    void Note(void* orig, void* copy) {
        switch (Hash(orig)) {
        case kBustFar: g.seg[0] = copy; break;      case kBustNear: g.seg[1] = copy; break;
        case kMid: g.seg[2] = copy; break;          case kEvadeNear: g.seg[3] = copy; break;
        case kEvadeFar: g.seg[4] = copy; break;     case kCoolBar: g.coolBar = copy; break;
        case kTrack: { float w = *(float*)(Data(orig) + 0x38); if (w > 20.f && w < 2000.f) { g.wFar = w * 0.25f; g.wNear = w * (48.f / 216.f); } break; }
        case kCoolTrack: { float w = *(float*)(Data(orig) + 0x38); if (w > 20.f && w < 2000.f) g.wCool = w; break; }
        case kBackupPanel: case kBackupText: case kHiddenPanel: case kHiddenText: case kIcon:
            if (g.hideN < 6) g.hide[g.hideN++] = copy;
            break;
        }
    }
    // make the copy a full, silent twin of the original (SameShape checked first)
    void Finish(void* orig, void* copy) {
        memcpy(Data(copy), Data(orig), *(uint32_t*)(B(orig) + 0x30));
        *(void**)(B(copy) + 0x58) = nullptr;                 // no cached render object yet
        Note(orig, copy);
        *(uint32_t*)(B(copy) + 0x10) = 0;                    // found by nobody
        int n = 0;
        for (void* r = *(void**)(B(copy) + 0x3C); r && n < 64 && Sane(r, 0x18); r = Next(r), ++n)
            *(uint32_t*)(B(r) + 0x0C) = 0xDEAD0000u + n;      // a message nobody sends
        n = 0;
        for (void* s = *(void**)(B(copy) + 0x4C); s && n < 64 && Sane(s, 0x34); s = Next(s), ++n)
            if (*(int*)(B(s) + 0x24)) ((void(__thiscall*)(void*, int))0x5B5350)(B(s) + 0x24, 0);   // FEEventList::SetCount(0)
        Dirty(copy);
        if (Type(orig) != 5) return;
        void* co = FirstChild(orig); void* cc = FirstChild(copy);
        for (int i = 0, c = ChildCount(orig); i < c && co && cc; ++i, co = Next(co), cc = Next(cc)) Finish(co, cc);
    }
    // the place the group's scripts give it (the base key of the position track: +0x18 in a 0x38-byte track
    // whose data offset byte, +7, is 7) - our own group may be mid-flip when it is copied
    bool ScriptPos(void* o, float out[3]) {
        int n = 0;
        for (void* s = *(void**)(B(o) + 0x4C); s && n < 64 && Sane(s, 0x34); s = Next(s), ++n) {
            int tracks = *(uint8_t*)(B(s) + 0x1C); uint8_t* t = *(uint8_t**)(B(s) + 0x20);
            if (!t || !Sane(t, (size_t)tracks * 0x38)) continue;
            for (int i = 0; i < tracks; ++i, t += 0x38)
                if ((int8_t)t[7] == 7 && t[1] == 0x0C) { memcpy(out, t + 0x18, 12); return true; }
        }
        return false;
    }
    void* CloneGroup(void* grp) {
        void** vt = *(void***)grp;
        if ((uintptr_t)vt != kFEGroupVtbl || (uintptr_t)vt[1] != kFEGroupClone) return nullptr;
        return ((void*(__thiscall*)(void*, bool))vt[1])(grp, false);
    }
    void Add(void* pkg, void* parent, void* obj) {
        if (parent) ((void(__thiscall*)(void*, void*))0x5B5420)(parent, obj);           // FEGroup::AddObject
        else ((void(__thiscall*)(void*, void*))0x50DB90)(pkg, obj);                     // FEPackage::AddObject
    }
    // a fresh copy: at its scripts' own place, full size, hidden
    void Settle(void* copy) {
        float p[3];
        if (ScriptPos(copy, p)) SetPos(copy, p, false);
        float* size = (float*)(Data(copy) + 0x38);
        size[0] = size[1] = size[2] = 1.f;
        HideNow(copy);
    }

    void Forget() {             // the package is gone: nothing of ours is left to touch
        State keep = g;
        g = State{}; g.forced = keep.forced; g.forceKind = keep.forceKind; g.forceValue = keep.forceValue;
        g.deadPkg = keep.deadPkg; g.deadChase = keep.deadChase;
    }

    const char* Setup(void* pkg) {
        Forget();
        g.pkg = pkg;
        g.chase = Find(pkg, kChase, 5);
        if (pkg == g.deadPkg && g.chase == g.deadChase) return nullptr;      // failed before, said so then
        g.cool = Find(pkg, kCool, 5);
        if (!g.chase || !g.cool) return "the meter's groups are not in the HUD (another HUD?)";
        g.row = Find(pkg, kRow, 5); g.icon = Find(pkg, kIcon, 1); g.ownHidden = Find(pkg, kHiddenPanel, 1);
        void* first = *(void**)(B(pkg) + 0x4C);
        if (!FindParent(first, nullptr, g.chase, &g.parChase, 0) || !FindParent(first, nullptr, g.cool, &g.parCool, 0))
            return "the meter's groups are not in the package's tree";
        void* a = CloneGroup(g.chase);
        if (!a) return "the chase group is not a plain FEGroup (or out of memory)";
        void* b = CloneGroup(g.cool);
        int count = 0;
        // (a copy that cannot be finished is left alone, unattached: a few hundred bytes, once per HUD)
        if (!b) return "the cooldown group is not a plain FEGroup (or out of memory)";
        if (!SameShape(g.chase, a, 0, &count) || !SameShape(g.cool, b, 0, &count)) return "the copy does not match the original";
        Finish(g.chase, a); Finish(g.cool, b);
        if (!g.seg[0] || !g.seg[1] || !g.seg[2] || !g.seg[3] || !g.seg[4] || !g.coolBar) {
            // attached all the same (the package frees them), but never shown
            Settle(a); Settle(b); Add(pkg, g.parChase, a); Add(pkg, g.parCool, b);
            return "the meter's segments are not in the groups";
        }
        Settle(a); Settle(b);
        for (int i = 0; i < g.hideN; ++i) HideNow(g.hide[i]);
        Add(pkg, g.parChase, a); Add(pkg, g.parCool, b);
        g.cChase = a; g.cCool = b; g.objects = count; g.ready = true;
        return nullptr;
    }

    // our copies are still the ones in this package (it is unloaded and loaded again with every race / restart,
    // often at the same addresses)
    bool StillValid(void* pkg) {
        return g.ready && pkg == g.pkg && Find(pkg, kChase, 5) == g.chase &&
               IsChild(pkg, g.parChase, g.cChase) && IsChild(pkg, g.parCool, g.cCool) &&
               Hash(g.cChase) == 0 && Hash(g.cCool) == 0;     // still our silent copies (a reload at the same address: not)
    }

    void SetWidthRight(void* o, float w) {       // the right edge stays (BUSTED side), as at 0x57B2B0
        float x = 0, y = 0, sx = 0, sy = 0, nx = 0, ny = 0;
        ((void(__cdecl*)(void*, float*, float*))0x515140)(o, &x, &y);                    // FEngGetBottomRight
        ((void(__cdecl*)(void*, float*, float*))0x515520)(o, &sx, &sy);                  // FEngGetSize
        if (sx == w) return;
        ((void(__cdecl*)(void*, float, float))0x5155E0)(o, w, sy);                       // FEngSetSize
        ((void(__cdecl*)(void*, float*, float*))0x515140)(o, &nx, &ny);
        ((void(__cdecl*)(void*, float, float))0x524E20)(o, x, ny);                       // FEngSetBottomRight
    }
    void SetWidthLeft(void* o, float w) {        // the left edge stays (EVADE side, the cooldown bar), as at 0x57B4CE
        float x = 0, y = 0, sx = 0, sy = 0, nx = 0, ny = 0;
        ((void(__cdecl*)(void*, float*, float*))0x514FD0)(o, &x, &y);                    // FEngGetTopLeft
        ((void(__cdecl*)(void*, float*, float*))0x515520)(o, &sx, &sy);
        if (sx == w) return;
        ((void(__cdecl*)(void*, float, float))0x5155E0)(o, w, sy);
        ((void(__cdecl*)(void*, float*, float*))0x514FD0)(o, &nx, &ny);
        ((void(__cdecl*)(void*, float, float))0x524CC0)(o, x, ny);                       // FEngSetTopLeft
    }
    // the game's own maths (PursuitBoard::Update): v +1 busted .. -1 evaded
    void FillChase(float v) {
        SharedPursuit::ChaseFill f = SharedPursuit::ChaseMeterFill(v);
        SetWidthRight(g.seg[0], f.bustFar * g.wFar);
        SetWidthRight(g.seg[1], f.bustNear * g.wNear);
        SetWidthLeft(g.seg[3], f.evadeNear * g.wNear);
        SetWidthLeft(g.seg[4], f.evadeFar * g.wFar);
        Want(g.seg[0], f.bustAlert ? sAlert : sInit);
        Want(g.seg[4], f.evadeAlert ? sAlert : sInit);
        Want(g.seg[2], f.midPulse ? sPulse : sShow);
    }
    void ShowGroup(void* grp, bool show) {
        if (show) { if (!ScriptIs(grp, sFadeIn)) SetScript(grp, sFadeIn); }
        else if (ScriptIs(grp, sFadeIn)) SetScript(grp, sFadeOut);
    }
    void MoveTo(void* o, float& applied, float want) {
        if (applied == want) return;
        MoveBy(o, applied - want);          // up = -y
        applied = want;
    }

    void Unlift() {             // the cop row and the HIDDEN icon back where the game has them
        if (g.lift != 0.f) { MoveBy(g.row, g.lift); MoveBy(g.icon, g.lift); g.lift = 0.f; }
    }

    // kind 0 hides; 1 chase (value -100 busted .. +100 evade); 2 cooldown (0 .. 100 done)
    // returns: 1 shown, 0 hidden, -1 no copy (why: once per package)
    int Apply(int kind, int value, const char** why) {
        *why = nullptr;
        void* pkg = Package();
        if (!pkg || !Sane(pkg, 0x54)) { Forget(); return kind ? -1 : 0; }
        if (g.ready && !StillValid(pkg)) {
            bool samePlace = pkg == g.pkg && Find(pkg, kChase, 5) == g.chase;
            void* chase = g.chase;
            if (samePlace) Unlift();                // the cop row is still the game's: put it back
            Forget();
            if (samePlace) {                        // our copies went missing from a package we still know: leave it
                g.deadPkg = pkg; g.deadChase = chase;
                *why = "the copies are no longer in the HUD";
                return -1;
            }
        }
        if (!g.ready) {
            if (!kind) return 0;                    // copied when first needed
            const char* err = Setup(pkg);
            if (!g.ready) {
                if (err) { g.deadPkg = pkg; g.deadChase = g.chase; *why = err; }
                return -1;
            }
            LOG("Pursuit: partner bar - cloned the game's meter (%d objects; chase %p, cooldown %p; segments %.0f / %.0f, cooldown %.0f wide)",
                g.objects, g.cChase, g.cCool, g.wFar, g.wNear, g.wCool);
        }
        ShowGroup(g.cChase, kind == 1);
        ShowGroup(g.cCool, kind == 2);
        if (!kind) { Unlift(); return 0; }
        // our own meter: the cooldown one without the HIDDEN panel has nothing above its strip
        bool ownCoolBare = (ScriptIs(g.cool, sFlipIn) || ScriptIs(g.cool, sFadeIn)) && !(g.ownHidden && ScriptIs(g.ownHidden, sFadeIn));
        float up = (kind == 2 && !ownCoolBare) ? kUpFar : kUpNear;
        if (kind == 1) {
            MoveTo(g.cChase, g.upChase, up);
            FillChase(SharedPursuit::MeterV(value));
        } else {
            MoveTo(g.cCool, g.upCool, up);
            SetWidthLeft(g.coolBar, SharedPursuit::CooldownDone(value) * g.wCool);
        }
        if (g.lift != up) {                         // the cop row and the HIDDEN icon move out of the way
            float d = g.lift - up;
            MoveBy(g.row, d); MoveBy(g.icon, d);
            g.lift = up;
        }
        return 1;
    }
    void CleanUp() {
        if (!g.ready) return;
        HideNow(g.cChase); HideNow(g.cCool);
        Unlift();
    }
    void CleanUpGuarded() {
        __try { CleanUp(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    int ApplyGuarded(int kind, int value, const char** why) {
        __try {
            return Apply(kind, value, why);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // whatever it was: the copies are hidden and the cop row put back if that still works, and this
            // package's copy is not touched again
            void* pkg = g.pkg; void* chase = g.chase;
            CleanUpGuarded();
            Forget();
            g.deadPkg = pkg; g.deadChase = chase;
            *why = "exception";
            return -1;
        }
    }
}
}

// kind 0 hides. True while the copy shows the bar.
static bool PeerBarNativeSet(int kind, int value) {
    const char* why = nullptr;
    int r = PB::ApplyGuarded(kind, value, &why);
    if (why) LOG("Pursuit: partner bar - not shown, the game's meter could not be cloned: %s", why);
    return r == 1;
}
static bool PeerBarForced(int* kind, int* value) {
    if (!PB::g.forced) return false;
    *kind = PB::g.forceKind; *value = PB::g.forceValue;
    return true;
}

namespace Game {
    // test: "peerbar chase <-100..100>" | "peerbar cool <0..100>" | "peerbar off" (back to the other player's own)
    // - shows the partner bar with a fixed value, no partner needed (the HUD must be up).
    std::string DebugPeerBar(const char* args) {
        std::string a = args ? args : "";
        int v = 0;
        if (sscanf_s(a.c_str(), "chase %d", &v) == 1) { PB::g.forced = true; PB::g.forceKind = 1; PB::g.forceValue = (int)SharedPursuit::Clamp((float)v, -100.f, 100.f); }
        else if (sscanf_s(a.c_str(), "cool %d", &v) == 1) { PB::g.forced = true; PB::g.forceKind = 2; PB::g.forceValue = (int)SharedPursuit::Clamp((float)v, 0.f, 100.f); }
        else if (a == "off") { PB::g.forced = false; return "peerbar: back to the other player's own bar"; }
        else return "peerbar chase <-100..100> | peerbar cool <0..100> | peerbar off";
        char buf[96];
        sprintf_s(buf, "peerbar: forced %s %d", PB::g.forceKind == 1 ? "chase" : "cooldown", PB::g.forceValue);
        return buf;
    }
}
