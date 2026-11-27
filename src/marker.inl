// Included from game.cpp (after hud.inl).
// ---------------------------------------------------------------------------
// Friend marker ([UI] FriendMarker): the game's thin chevron (texture ARROW_SKINNIER, in GLOBALB.BUN,
// always loaded) in the friend's paint colour, see-through, over their car - or at the edge of the
// screen pointing at them when they're off screen or behind. overlay.cpp draws it; this projects.
//  GetTextureInfo(hash, 0, 0)   0x503400 cdecl -> TextureInfo* (+0x24 name hash, +0 PlatInfo*,
//                                PlatInfo +0x18 = IDirect3DTexture9*: TextureInfoPlatInterface::Lock 0x6BED80)
//  eViews[EVIEW_PLAYER1]        0x919650: +0 PlatInfo* (view-projection 4x4 at +0x80, row vectors - what
//                                eViewPlatInterface::GetScreenPosition 0x6BE940 multiplies by), +8 Active
//  Render space = (game.z, -game.x, game.y) (inverse of ReadCamera's swizzle in traffic.inl).
// ---------------------------------------------------------------------------
namespace {
    constexpr uint32_t kChevronHash = 0x1A6D3D2C;      // "ARROW_SKINNIER" (FEng-style hash, h*33+c from ~0)
    constexpr float kMarkerHeight = 1.5f;              // over the car's centre, metres

    void* ChevronTexture() {
        void* ti = ((void*(__cdecl*)(uint32_t, int, int))0x503400)(kChevronHash, 0, 0);
        uint32_t nameHash = 0; void* plat = nullptr; void* tex = nullptr; void* vt = nullptr;
        if (!ti || !Mem::Read((uintptr_t)ti + 0x24, nameHash) || nameHash != kChevronHash) return nullptr;
        if (!Mem::Read((uintptr_t)ti, plat) || !plat || !Mem::Read((uintptr_t)plat + 0x18, tex) || !tex) return nullptr;
        if (!Mem::Read((uintptr_t)tex, vt) || !Mem::IsReadable(vt, 4)) return nullptr;
        return tex;
    }

    const char* MarkerTickImpl(bool linked, Overlay::Marker& m) {
        if (!g_cfg.friendMarker) return "FriendMarker=0";
        if (!linked || !g_coopLinked) return "not linked";
        if (Game::GameFlowState() != 6 || !LocalVehicle()) return "not in the world";
        if (NisActive()) return "a cutscene";
        if (g_sp.lastStatus == PS_BUSTED) return "we are busted";
        if (g_hudHidden) return "the HUD is hidden (F1)";
        if (!FEScreens::IsPushed("HUD_SingleRace.fng") && !FEScreens::IsPushed("HUD.fng")) return "no HUD";
        if (FEScreens::AnyCloseableOverlay() || FEScreens::IsPushed("Pause_Main.fng") || FEScreens::IsPushed("WorldMapMain.fng")) return "a menu is open";
        void* grb = RigidBodyOf(g_ghost);
        const float* gp = grb ? RB_GetVec(grb, RB_GetPosition) : nullptr;
        if (!gp || !NetUtil::FiniteWithin(gp, 3, NetUtil::MAX_POS)) return "no car of theirs";
        uint8_t active = 0; uintptr_t plat = 0; float M[16];
        if (!Mem::Read(0x919658, active) || !active || !Mem::Read(0x919650, plat) || !plat ||
            !Mem::SafeRead((void*)(plat + 0x80), M, sizeof(M)) || !NetUtil::FiniteWithin(M, 16, 1e7f)) return "no camera";
        const float r[4] = { gp[2], -gp[0], gp[1] + kMarkerHeight, 1.f };
        float c[4];
        for (int j = 0; j < 4; ++j) c[j] = r[0] * M[j] + r[1] * M[4 + j] + r[2] * M[8 + j] + r[3] * M[12 + j];
        float camDist = -1.f;                                           // unknown: no hiding up close
        if (void* lrb = RigidBodyOf(LocalVehicle())) {
            if (const float* lp = RB_GetVec(lrb, RB_GetPosition)) {
                float dx = gp[0] - lp[0], dy = gp[1] - lp[1], dz = gp[2] - lp[2];
                camDist = sqrtf(dx * dx + dy * dy + dz * dz);
            }
        }
        static bool closeBy = false;                                       // on top of each other (e.g. the same spawn): none
        closeBy = camDist >= 0.f && camDist < (closeBy ? 8.f : 5.f);
        if (closeBy) return "right next to us";
        m.scale = camDist <= 60.f ? 1.f : camDist >= 400.f ? 0.7f : 1.f - 0.3f * (camDist - 60.f) / 340.f;
        m.color = Game::PeerBlipColor();
        m.tex = ChevronTexture();
        if (c[3] > 0.05f) {
            float nx = c[0] / c[3], ny = c[1] / c[3];
            float sx = (nx + 1) * 0.5f, sy = (1 - ny) * 0.5f;
            if (sx >= 0.03f && sx <= 0.97f && sy >= 0.06f && sy <= 0.97f) {
                m.onScreen = true; m.x = sx; m.y = sy; m.show = true;
                return nullptr;
            }
        }
        // off screen: (c0, c1) keeps the true side even behind the camera (only the divide by w flips it)
        m.dirX = c[0]; m.dirY = -c[1];
        if (c[3] <= 0.05f) {                                  // behind: along the bottom or the sides
            float len = sqrtf(m.dirX * m.dirX + m.dirY * m.dirY);
            m.dirY = fabsf(m.dirY) + 0.35f * (len > 1e-3f ? len : 1.f);
        }
        m.show = true;
        return nullptr;
    }
}

static const char* MarkerTickSafe(bool linked, Overlay::Marker* m) {
    __try { return MarkerTickImpl(linked, *m); } __except (EXCEPTION_EXECUTE_HANDLER) { return "exception"; }
}

void Game::MarkerTick(bool linked) {
    static std::string lastWhy = "-";
    Overlay::Marker m;
    const char* why = g_ok ? MarkerTickSafe(linked, &m) : "game not hooked";
    if (why) m = Overlay::Marker{};
    Overlay::SetMarker(m);
    std::string w = why ? why : "";
    if (w != lastWhy) {
        if (!why) LOG("Marker: shown (chevron texture %s)", m.tex ? "found" : "missing - a drawn one");
        else if (lastWhy.empty()) LOG("Marker: hidden - %s", why);
        lastWhy = w;
    }
}
