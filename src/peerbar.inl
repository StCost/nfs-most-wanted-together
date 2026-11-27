// ---- the other player's pursuit bar: a copy of the game's own BUSTED / EVADE strip, stacked under ours ----
// (speed.exe 1.3; HUD_SingleRace.fng, 640x480 units, origin at the screen centre, +y down.)
// The meter is drawn by PursuitBoard (Update 0x57AEE0; BustedMeter 0x568EB0 is an empty stub) with FEng
// objects of the HUD package, all under one parent group (0xAB9C1D4D):
//   0x7B422BA3  the chase meter: label panel y 161..193 (BUSTED / EVADE), the strip at y 196 (5 high)
//       0xE0E0169B BUSTED far (54 wide, grows left)    0x8EEEBD33 BUSTED near (48, grows left)
//       0x47A4E2A9 the middle mark (script only)       0x8E3653A6 EVADE near (48, grows right)
//       0x30F39A6F EVADE far (54, grows right)         0x988B8325 the track (216)
//       0x8C20A763 / 0x6A144066 the backup timer panel and text (below the strip)
//       0xBAE10529 the label panel, 0x6BF5C681 / 0x0B3DF9DF the words BUSTED / EVADE (above the strip)
// Only the chase meter is copied: the cooldown is shared, so our own cooldown meter is the partner's too.
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
// shifts the data AND that track in every script of the object - used to put the copy under our strip.
// Shown / hidden with the groups' own fade scripts 0x5079C8F8 / 0x33113AC, as PursuitBoard does.
namespace {
namespace PB {
    constexpr uint32_t kChase = 0x7B422BA3;
    constexpr uint32_t kBustFar = 0xE0E0169B, kBustNear = 0x8EEEBD33, kMid = 0x47A4E2A9, kEvadeNear = 0x8E3653A6,
                       kEvadeFar = 0x30F39A6F, kTrack = 0x988B8325, kBackupPanel = 0x8C20A763, kBackupText = 0x6A144066;
    // script names (FEHashUpper): INIT, SHOW, HIDE, PULSE, ALERT; the two fades are not in the exe by name
    constexpr uint32_t sInit = 0x1744B3, sShow = 0x1CA7C0, sHide = 0x16A259, sPulse = 0x3826A28, sAlert = 0x26DED57,
                       sFadeIn = 0x5079C8F8, sFadeOut = 0x33113AC, sFlipIn = 0x13F51124;
    constexpr uintptr_t kFEGroupVtbl = 0x8A2E24, kFEGroupClone = 0x5C49C0;
    // Stacked under ours: only the moving strip (and its ticks), kStripDown below our strip; the label panel
    // stays ours alone. Our backup timer stays put; it fits under both strips. With no chase meter of our own
    // up, the copy shows whole, in our meter's place.
    constexpr float kStripDown = 8.f;
    // a copy that failed (the HUD mid-load, a race HUD, an exception) is tried again this much later - never given
    // up for good: the same addresses come back with every race (a pursuit from a race kept no strip after it)
    constexpr DWORD kRetryMs = 10000;
    constexpr int kMaxTries = 3;            // then not for this package any more (a failure that repeats isn't transient)
    // the shown value follows the partner's at this rate (1/s; the packets come ~every 50 ms, in steps)
    constexpr float kFollowRate = 12.f;
    constexpr uint32_t kLabelPanel = 0xBAE10529, kLabelBusted = 0x6BF5C681, kLabelEvade = 0x0B3DF9DF;
    const char* const kPkgName = "HUD_SingleRace.fng";

    struct State {
        void* pkg = nullptr;
        void* chase = nullptr;                                  // the game's own chase meter
        void* parChase = nullptr;                               // its parent group (null: the package's own list)
        void* cChase = nullptr;                                 // the copy
        void* seg[5] = {};                                      // in cChase: BUSTED far, near, the middle, EVADE near, far
        void* hide[2] = {}; int hideN = 0;                      // parts of the copy that never show (the backup timer)
        void* lbl[3] = {}; int lblN = 0;                        // in cChase: the label panel and its two words
        float wFar = 54.f, wNear = 48.f;                        // the full widths
        float upChase = 0;                                      // applied now: the copy's offset (up)
        float shownV = 0; DWORD shownAt = 0;                    // the value on the strip now (follows the partner's)
        int objects = 0;
        bool ready = false;
        void* deadPkg = nullptr; void* deadChase = nullptr;     // cloning failed for this package: tried again kRetryMs later
        DWORD deadAt = 0; int deadTries = 0;                    // failures in a row for this package
        void* recopiedPkg = nullptr; void* recopiedChase = nullptr;   // copied a second time here once already
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
        case kEvadeFar: g.seg[4] = copy; break;
        case kTrack: { float w = *(float*)(Data(orig) + 0x38); if (w > 20.f && w < 2000.f) { g.wFar = w * 0.25f; g.wNear = w * (48.f / 216.f); } break; }
        case kLabelPanel: case kLabelBusted: case kLabelEvade:
            if (g.lblN < 3) g.lbl[g.lblN++] = copy;
            break;
        case kBackupPanel: case kBackupText:
            if (g.hideN < 2) g.hide[g.hideN++] = copy;
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
        g.deadPkg = keep.deadPkg; g.deadChase = keep.deadChase; g.deadAt = keep.deadAt; g.deadTries = keep.deadTries;
        g.recopiedPkg = keep.recopiedPkg; g.recopiedChase = keep.recopiedChase;
    }

    void MarkDead(void* pkg, void* chase) {
        g.deadTries = (pkg == g.deadPkg && chase == g.deadChase) ? g.deadTries + 1 : 1;
        g.deadPkg = pkg; g.deadChase = chase; g.deadAt = GetTickCount();
    }
    const char* Setup(void* pkg) {
        Forget();
        g.pkg = pkg;
        g.chase = Find(pkg, kChase, 5);
        if (pkg == g.deadPkg && g.chase == g.deadChase && (g.deadTries >= kMaxTries || GetTickCount() - g.deadAt < kRetryMs))
            return nullptr;                         // failed just now (or too often here), said so then
        if (!g.chase) return "the meter's group is not in the HUD (another HUD?)";
        if (!FindParent(*(void**)(B(pkg) + 0x4C), nullptr, g.chase, &g.parChase, 0)) return "the meter's group is not in the package's tree";
        void* a = CloneGroup(g.chase);
        if (!a) return "the chase group is not a plain FEGroup (or out of memory)";
        int count = 0;
        // (a copy that cannot be finished is left alone, unattached: a few hundred bytes, once per HUD)
        if (!SameShape(g.chase, a, 0, &count)) return "the copy does not match the original";
        Finish(g.chase, a);
        const char* bad = !g.seg[0] || !g.seg[1] || !g.seg[2] || !g.seg[3] || !g.seg[4] ? "the meter's segments are not in the group"
                        : g.lblN != 3 ? "the meter's label panel is not in the group" : nullptr;
        Settle(a);
        for (int i = 0; i < g.hideN; ++i) HideNow(g.hide[i]);
        Add(pkg, g.parChase, a);                    // (the package frees it; a bad copy is never shown)
        if (bad) return bad;
        g.cChase = a; g.objects = count; g.ready = true;
        return nullptr;
    }

    // our copies are still the ones in this package (it is unloaded and loaded again with every race / restart,
    // often at the same addresses)
    bool StillValid(void* pkg) {
        return g.ready && pkg == g.pkg && Find(pkg, kChase, 5) == g.chase &&
               IsChild(pkg, g.parChase, g.cChase) && Hash(g.cChase) == 0;     // still our silent copy (a reload at the same address: not)
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
    void SetWidthLeft(void* o, float w) {        // the left edge stays (EVADE side), as at 0x57B4CE
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

    // The copy lives in the game's pursuit group: when the game hides that whole group (a race HUD:
    // FEngSetInvisible 0x514C70 marks every child, ours too) and later shows only its own meter, ours stayed
    // invisible for good (a pursuit from a race kept no strip after it). Shown: our copy and all in it are
    // made visible again every frame (the same flag the game uses, +0x1C bit 0); the labels are hidden after.
    void MakeVisible(void* o, int depth) {
        if (!o || depth > 8) return;
        uint32_t& f = *(uint32_t*)(B(o) + 0x1C);
        if (f & 1) f = (f & ~1u) | 0x2400000;
        if (Type(o) != 5) return;
        void* c = FirstChild(o);
        for (int i = 0, n = ChildCount(o); i < n && c && i < 64; ++i, c = Next(c)) MakeVisible(c, depth + 1);
    }
    // the copy's label panel and words: the object's own invisible flag (+0x1C bit 0, as FEngSetInvisible /
    // FEngSetVisible 0x514C70 / 0x514CC0 set it), every frame - a parent shown again would show them too
    void ShowLabels(bool show) {
        for (int i = 0; i < g.lblN; ++i) {
            uint32_t& f = *(uint32_t*)(B(g.lbl[i]) + 0x1C);
            uint32_t want = show ? ((f & ~1u) | 0x2400000) : (f | 0x2400001);
            if (f != want) f = want;
        }
    }

    // kind 0 hides, 1 their chase (value -100 busted .. +100 evade). own: our own meter, 0 none, 1 the chase,
    // 2 the cooldown, 3 the chase with the backup timer. Returns: 1 shown, 0 hidden, -1 no copy (why: once per package)
    int Apply(int kind, int value, int own, const char** why) {
        *why = nullptr;
        void* pkg = Package();
        if (!pkg || !Sane(pkg, 0x54)) { Forget(); return kind ? -1 : 0; }
        if (g.ready && StillValid(pkg)) { g.recopiedPkg = g.recopiedChase = nullptr; }   // a later reload may copy again
        else if (g.ready) {
            // the HUD was loaded again (often at the same address): copy again, but at most twice in a row here
            bool samePlace = pkg == g.pkg && Find(pkg, kChase, 5) == g.chase;
            void* chase = g.chase;
            bool again = samePlace && g.recopiedPkg == pkg && g.recopiedChase == chase;
            Forget();
            if (again) {
                MarkDead(pkg, chase);
                *why = "the copies went missing again";
                return -1;
            }
            if (samePlace) { g.recopiedPkg = pkg; g.recopiedChase = chase; }
        }
        if (!g.ready) {
            if (!kind) return 0;                    // copied when first needed
            const char* err = Setup(pkg);
            if (!g.ready) {
                if (err) { MarkDead(pkg, g.chase); *why = err; }
                return -1;
            }
            g.deadPkg = g.deadChase = nullptr; g.deadTries = 0;   // a good copy: earlier failures were passing
            LOG("Pursuit: partner bar - cloned the game's meter (%d objects, copy %p, segments %.0f / %.0f)",
                g.objects, g.cChase, g.wFar, g.wNear);
        }
        bool ownUp = own == 1 || own == 3;          // our chase meter up: only the strip, under ours
        // the game keeps our chase meter hidden (a race hides the police HUD: FEngSetInvisible, +0x1C bit 0): none
        bool gameHides = ownUp && (*(uint32_t*)(B(g.chase) + 0x1C) & 1);
        bool show = kind == 1 && own != 2 && !gameHides;   // with our cooldown up: none (the cooldown is shared)
        ShowGroup(g.cChase, show);
        if (!show) { g.shownAt = 0; return 0; }
        MoveTo(g.cChase, g.upChase, ownUp ? -kStripDown : 0.f);
        MakeVisible(g.cChase, 0);
        for (int i = 0; i < g.hideN; ++i) HideNow(g.hide[i]);     // (made visible above: the backup timer stays hidden)
        ShowLabels(!ownUp);
        DWORD now = GetTickCount();
        float target = SharedPursuit::MeterV(value);
        // between packets it eases; a real step (seen / unseen, as on their own meter) is shown at once
        bool step = !g.shownAt || fabsf(target - g.shownV) > 0.3f;
        g.shownV = step ? target : SharedPursuit::FollowValue(g.shownV, target, (now - g.shownAt) / 1000.f, kFollowRate);
        g.shownAt = now;
        FillChase(g.shownV);
        return 1;
    }
    void CleanUp() {
        if (!g.ready) return;
        HideNow(g.cChase);
    }
    void CleanUpGuarded() {
        __try { CleanUp(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    int ApplyGuarded(int kind, int value, int own, const char** why) {
        __try {
            return Apply(kind, value, own, why);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // whatever it was: the copy is hidden if that still works, and this package's is not touched again
            void* pkg = g.pkg; void* chase = g.chase;
            CleanUpGuarded();
            Forget();
            MarkDead(pkg, chase);
            *why = "exception";
            return -1;
        }
    }
}
}

// kind 0 hides. True while the copy shows the bar.
static bool PeerBarNativeSet(int kind, int value, int own) {
    const char* why = nullptr;
    int r = PB::ApplyGuarded(kind, value, own, &why);
    if (why) LOG("Pursuit: partner bar - not shown, the game's meter could not be cloned: %s", why);
    return r == 1;
}
static bool PeerBarForced(int* kind, int* value) {
    if (!PB::g.forced) return false;
    *kind = PB::g.forceKind; *value = PB::g.forceValue;
    return true;
}

namespace Game {
    // test: "peerbar chase <-100..100>" | "peerbar off" - the partner strip with a fixed value, no partner needed
    std::string DebugPeerBar(const char* args) {
        std::string a = args ? args : "";
        int v = 0;
        if (sscanf_s(a.c_str(), "chase %d", &v) == 1) { PB::g.forced = true; PB::g.forceKind = 1; PB::g.forceValue = (int)SharedPursuit::Clamp((float)v, -100.f, 100.f); }
        else if (a == "off") { PB::g.forced = false; return "peerbar: back to the other player's own bar"; }
        else return "peerbar chase <-100..100> | peerbar off";
        char buf[96];
        sprintf_s(buf, "peerbar: forced chase %d", PB::g.forceValue);
        return buf;
    }
}
