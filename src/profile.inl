// Included from game.cpp (after story.inl).
// ---------------------------------------------------------------------------
// The name we go by
//  Every player plays their own career profile and goes by its alias: in the hello to the other game,
//  the standings, the menu's profile box and the notices.
// ---------------------------------------------------------------------------
namespace {
    // the loaded career profile's alias as the game has it (the name comes first in the UserProfile)
    std::string LoadedProfileAlias() {
        uint8_t* fedb = nullptr; uint8_t* up = nullptr; char raw[33] = {};
        if (!Mem::Read(0x91CF90, fedb) || !fedb || !fedb[3] || !Mem::Read((uintptr_t)fedb + 0x10, up) || !up) return "";
        if (!Mem::SafeRead(up, raw, 32)) return "";
        raw[32] = 0;
        return raw;
    }
}

// The name we go by is our career profile's alias (TogetherUtil::PlayerName): checked twice a second, so
// a profile loaded or switched in the menus is what the other player sees next (the hello repeats it).
void Game::PlayerNameTick() {
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 500) return;
    last = now;
    std::string loaded = g_ok ? LoadedProfileAlias() : std::string();
    std::string n = TogetherUtil::PlayerName(loaded, g_cfg.lastProfile, g_cfg.legacyName);
    if (n == g_cfg.playerName) return;
    LOG("Player: we go by %s (%s)", n.c_str(), !TogetherUtil::CleanPlayerName(loaded).empty() ? "our career profile"
        : !TogetherUtil::CleanPlayerName(g_cfg.lastProfile).empty() ? "the profile loaded last" : "the old [Player] Name, until a profile is loaded");
    g_cfg.playerName = n;
}
