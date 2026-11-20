// Included from game.cpp (after pursuit.inl).
// ---------------------------------------------------------------------------
// World streaming around the other player
//  A car only keeps full physics while world collision is loaded under it (PVehicle "off world" flag,
//  0x680840); without it the AI switches the car to the cheap road-following mode (floats, no
//  collisions). The track streamer (0x9AA9D0) loads the world around up to 2 positions - the second
//  is the split-screen player's and stays empty in single player. Every frame 0x47C880 (called at
//  0x480C16) clears both and predicts position 0 from the camera; we then also predict position 1
//  from the other player's car, so the world (and full physics for the cars there) exists on both.
//  PredictStreamingPosition 0x7238D0: thiscall(streamer, int slot, bVector3* pos, bVector2* vel,
//  bVector2* dir, bool), ret 0x14. bVector = { z, -x, y } of the sim (UMath) vector.
// ---------------------------------------------------------------------------
namespace {
    constexpr uintptr_t StreamCallSite = 0x480C16, StreamUpdateFn = 0x47C880, PredictStreamFn = 0x7238D0;
    bool  g_streamPeer = false;   // set every frame by the game thread: peer car exists, connected
    bool  g_streamQuiet = false;  // a story movie starts / plays over the world here (story.inl): only
                                  // our own area, like single player - the movie takes its buffers from the
                                  // streamer's pool (0x517470 -> 0x723A00) once the streamer is idle (0x58F1E0)
    float g_streamPos[3], g_streamVel[2];
    DWORD g_streamHoldUntil = 0;  // test 'tp': keep the world loaded around g_holdPos until then
    float g_holdPos[3];

    void __cdecl hk_StreamUpdate(float dT) {
        ((void(__cdecl*)(float))StreamUpdateFn)(dT);
        bool hold = g_streamHoldUntil && (int)(GetTickCount() - g_streamHoldUntil) < 0;
        if ((!g_streamPeer && !hold) || g_streamQuiet) return;
        __try {
            char* gm = nullptr; char* rs = nullptr;
            Mem::Read(0x91E00C, gm); Mem::Read(0x91E000, rs);
            if (gm && gm[0x2B4]) return;          // the original skips while these are set (loading / NIS)
            if (rs && rs[0x19A4]) return;
            float dir[2] = { 0, 0 }, still[2] = { 0, 0 };
            ((void(__thiscall*)(void*, int, float*, float*, float*, bool))PredictStreamFn)
                ((void*)0x9AA9D0, 1, hold ? g_holdPos : g_streamPos, hold ? still : g_streamVel, dir, true);
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_streamPeer = false; }
    }
}

bool Game::InstallStreamHook() {
    bool ok = PatchCallChecked(StreamCallSite, StreamUpdateFn, (void*)&hk_StreamUpdate);
    LOG("Streaming: world around the other player %s", ok ? "hooked" : "NOT hooked (call site differs)");
    return ok;
}

// Test 'tp': load the world around a sim position for ms milliseconds (uses the second streaming slot).
static void StreamHoldAt(const float* p, DWORD ms) {
    g_holdPos[0] = p[2]; g_holdPos[1] = -p[0]; g_holdPos[2] = p[1];
    g_streamHoldUntil = GetTickCount() + ms;
}
// Called every frame from the ghost update: where the other player's car is (or none).
static void SetStreamPeer(void* rb) {
    if (!rb || !g_cfg.streamAroundPeer) { g_streamPeer = false; return; }
    const float* p = RB_GetVec(rb, RB_GetPosition);
    const float* v = RB_GetVec(rb, RB_GetLinearVelocity);
    g_streamPos[0] = p[2]; g_streamPos[1] = -p[0]; g_streamPos[2] = p[1];
    g_streamVel[0] = v[2]; g_streamVel[1] = -v[0];
    g_streamPeer = true;
}
