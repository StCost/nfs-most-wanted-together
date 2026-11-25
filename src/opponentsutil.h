#pragma once
// Fixed AI opponents ([Coop] FixedOpponents): every start and restart of an event gets the same AI racers -
// the same names, cars and tuning - also on a new save. Rules that don't need the game (tests\test_opponents.cpp).
//  - names: GRacerInfo::ChooseRandomName draws from the game's random generator; for that one call the
//    generator is seeded from the event and the racer's slot (NameSeed), so both games pick the same names;
//  - cars and tuning: the game's scripts pick them at random. The first time an event is built here its
//    opponents (model + parts record, in build order) go into a book kept next to the plugin; every later
//    build of that event builds the same ones. Joining the other player's event still builds THEIR
//    opponents (the roster they send), and those then go into our book, so the two books agree.
#include <cstdint>
#include <cstring>
#include <vector>

namespace Opponents {
    constexpr int kRecordBytes = 0x198;        // the game's car parts record (FECustomizationRecord)
    constexpr int kMaxPerEvent = 8;
    constexpr int kMaxEvents = 512;
    constexpr uint32_t kFileMagic = 0x4F54574D; // "MWTO"
    constexpr uint32_t kFileVersion = 1;

    // A seed for the game's random generator (bRandom: never 0 - its xorshift would stay at 0 there)
    inline uint32_t NameSeed(uint32_t eventHash, int slot) {
        uint32_t x = eventHash ^ (0x9E3779B9u * (uint32_t)(slot + 1));
        x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
        return x ? x : 0x1D872B41u;
    }

    struct Entry { uint32_t key = 0; bool hasCustom = false; uint8_t custom[kRecordBytes] = {}; };
    struct Event { uint32_t hash = 0; int n = 0; Entry e[kMaxPerEvent]; };

    class Book {
    public:
        // the entry for opponent `index` of this event, or null
        const Entry* Get(uint32_t hash, int index) const {
            const Event* ev = Find(hash);
            return (ev && index >= 0 && index < ev->n) ? &ev->e[index] : nullptr;
        }
        int Count(uint32_t hash) const { const Event* ev = Find(hash); return ev ? ev->n : 0; }
        // remember opponent `index` (only the next one in order: a gap means the build order differed).
        // True if the book changed.
        bool Put(uint32_t hash, int index, uint32_t key, const void* custom) {
            if (!hash || !key || index < 0 || index >= kMaxPerEvent) return false;
            Event* ev = Find(hash);
            if (!ev) {
                if (index != 0) return false;
                if ((int)events_.size() >= kMaxEvents) events_.erase(events_.begin());   // the oldest one goes
                events_.push_back(Event{});
                ev = &events_.back();
                ev->hash = hash;
            }
            if (index < ev->n) return false;            // already there: the book wins
            if (index != ev->n) return false;
            Entry& e = ev->e[ev->n++];
            e.key = key;
            e.hasCustom = custom != nullptr;
            if (custom) memcpy(e.custom, custom, kRecordBytes); else memset(e.custom, 0, kRecordBytes);
            return true;
        }
        void Forget(uint32_t hash) {
            for (size_t i = 0; i < events_.size(); ++i) if (events_[i].hash == hash) { events_.erase(events_.begin() + i); return; }
        }
        size_t Events() const { return events_.size(); }

        std::vector<uint8_t> Save() const {
            std::vector<uint8_t> out;
            auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(v >> (8 * i))); };
            put32(kFileMagic); put32(kFileVersion); put32((uint32_t)events_.size());
            for (const Event& ev : events_) {
                put32(ev.hash); out.push_back((uint8_t)ev.n);
                for (int i = 0; i < ev.n; ++i) {
                    put32(ev.e[i].key); out.push_back(ev.e[i].hasCustom ? 1 : 0);
                    out.insert(out.end(), ev.e[i].custom, ev.e[i].custom + kRecordBytes);
                }
            }
            return out;
        }
        // false (book left empty) on a damaged or foreign file
        bool Load(const uint8_t* p, size_t len) {
            events_.clear();
            size_t at = 0;
            auto get32 = [&](uint32_t& v) { if (at > len || len - at < 4) return false; v = p[at] | (p[at + 1] << 8) | (p[at + 2] << 16) | ((uint32_t)p[at + 3] << 24); at += 4; return true; };
            uint32_t magic = 0, ver = 0, count = 0;
            if (!p || !get32(magic) || !get32(ver) || !get32(count) || magic != kFileMagic || ver != kFileVersion || count > (uint32_t)kMaxEvents) return false;
            std::vector<Event> evs;
            for (uint32_t k = 0; k < count; ++k) {
                Event ev{};
                if (!get32(ev.hash) || at >= len) { return false; }
                ev.n = p[at++];
                if (ev.n > kMaxPerEvent || !ev.hash) return false;
                for (int i = 0; i < ev.n; ++i) {
                    if (!get32(ev.e[i].key) || len - at < (size_t)kRecordBytes + 1) return false;
                    ev.e[i].hasCustom = p[at++] != 0;
                    memcpy(ev.e[i].custom, p + at, kRecordBytes); at += kRecordBytes;
                }
                evs.push_back(ev);
            }
            events_ = evs;
            return true;
        }
    private:
        const Event* Find(uint32_t hash) const { for (const Event& ev : events_) if (ev.hash == hash) return &ev; return nullptr; }
        Event* Find(uint32_t hash) { for (Event& ev : events_) if (ev.hash == hash) return &ev; return nullptr; }
        std::vector<Event> events_;
    };

    // A new build of the event's racers: the first racer after a pause, or another event
    constexpr uint32_t kNewBuildGapMs = 3000;
    inline bool NewBuild(uint32_t lastHash, uint32_t hash, uint32_t sinceLastMs) {
        return hash != lastHash || sinceLastMs >= kNewBuildGapMs;
    }
}
