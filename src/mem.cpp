#include "mem.h"
#include <cstring>

bool Mem::SafeRead(const void* src, void* dst, size_t len) {
    __try {
        memcpy(dst, src, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Mem::SafeWrite(void* dst, const void* src, size_t len) {
    __try {
        memcpy(dst, src, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Mem::PatchBytes(uintptr_t at, const void* expect, const void* bytes, size_t len) {
    if (memcmp((const void*)at, expect, len) != 0) return false;
    DWORD old;
    if (!VirtualProtect((void*)at, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)at, bytes, len);
    VirtualProtect((void*)at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, len);
    return true;
}

uintptr_t Mem::PatchCall(uintptr_t at, void* to) {
    uint8_t* p = (uint8_t*)at;
    uintptr_t oldTarget = 0;
    if (p[0] == 0xE8 || p[0] == 0xE9) oldTarget = at + 5 + *(int32_t*)(p + 1);
    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    uint8_t op = (p[0] == 0xE9) ? 0xE9 : 0xE8;
    p[0] = op;
    *(int32_t*)(p + 1) = (int32_t)((uintptr_t)to - (at + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return oldTarget;
}
