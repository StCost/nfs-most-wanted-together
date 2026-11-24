#pragma once
// Which original functions belong to which COM vtable, for hooks that patch the slots of more than one
// vtable. The game's DirectInput devices don't always share one: on some PCs the keyboard has its own
// (the first device, a mouse, had another), so a hook made only on the first device's vtable never saw
// the keyboard - Enter then reached the menus as Accept and the Together screen played its exit.
#include <cstddef>

namespace HookUtil {

template <class Orig, int N = 8>
struct VtableOriginals {
    void* vt[N] = {};
    Orig  orig[N] = {};
    int   count = 0;

    // The original for this vtable, or nullptr if it was never hooked.
    Orig Find(void* table) const {
        if (!table) return nullptr;
        for (int i = 0; i < count; ++i) if (vt[i] == table) return orig[i];
        return nullptr;
    }
    bool Has(void* table) const { return Find(table) != nullptr; }
    // Remember the original of a newly hooked vtable. False if it is already known (hook it once),
    // the original is missing (or is the hook itself), or there is no room left.
    bool Add(void* table, Orig original, Orig hook) {
        if (!table || !original || original == hook || Has(table) || count >= N) return false;
        vt[count] = table; orig[count] = original; ++count;
        return true;
    }
};

}  // namespace HookUtil
