#pragma once
#include <windows.h>
#include <cstdint>

// Memory helpers. Reads go through SEH so a wrong pointer logs an error instead of crashing the game.
namespace Mem {
    // Replace a pointer-sized value (vtable / IAT slot), returning the old one. The slot is data: it is
    // made read/write only (executable as well only if its page already was).
    inline void* PatchPointer(void* slot, void* value) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(slot, &mbi, sizeof(mbi))) return nullptr;
        const DWORD execBits = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        DWORD old;
        if (!VirtualProtect(slot, sizeof(void*), (mbi.Protect & execBits) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old)) return nullptr;
        void* prev = *(void**)slot;
        *(void**)slot = value;
        VirtualProtect(slot, sizeof(void*), old, &old);
        return prev;
    }

    inline bool IsReadable(const void* p, size_t len) {
        if (!p) return false;
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        return (uintptr_t)p + len <= end;
    }

    // Copy bytes with SEH protection. Returns false on access violation.
    bool SafeRead(const void* src, void* dst, size_t len);
    bool SafeWrite(void* dst, const void* src, size_t len);

    template <typename T>
    inline bool Read(uintptr_t addr, T& out) { return SafeRead((const void*)addr, &out, sizeof(T)); }

    template <typename T>
    inline bool Write(uintptr_t addr, const T& v) { return SafeWrite((void*)addr, &v, sizeof(T)); }

    // Write a 5-byte relative CALL/JMP at 'at' targeting 'to'. Returns the old call target for CALLs.
    uintptr_t PatchCall(uintptr_t at, void* to);

    // Overwrite code bytes only if they currently equal `expect` (same length). Returns true if patched.
    bool PatchBytes(uintptr_t at, const void* expect, const void* bytes, size_t len);
}
