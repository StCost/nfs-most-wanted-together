#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>

// A .dds file's header, enough to put it into a Direct3D 9 texture: 32-bit A8R8G8B8 or DXT1/3/5,
// with or without mip levels. No D3D here (tested on its own).
namespace DdsUtil {

constexpr uint32_t FMT_A8R8G8B8 = 21;                 // D3DFMT_A8R8G8B8
constexpr uint32_t FMT_DXT1 = 0x31545844, FMT_DXT3 = 0x33545844, FMT_DXT5 = 0x35545844;   // the FourCCs

struct Info {
    uint32_t width = 0, height = 0, mips = 1;
    uint32_t format = 0;                              // a D3DFORMAT value
    size_t   dataOffset = 128;                        // "DDS " + the 124-byte header
};

inline bool Compressed(uint32_t f) { return f == FMT_DXT1 || f == FMT_DXT3 || f == FMT_DXT5; }

// bytes of one row (of 4-pixel blocks for DXT) and how many rows, for one level
inline uint32_t RowBytes(uint32_t f, uint32_t w) {
    if (Compressed(f)) return ((w + 3) / 4) * (f == FMT_DXT1 ? 8u : 16u);
    return w * 4;
}
inline uint32_t Rows(uint32_t f, uint32_t h) { return Compressed(f) ? (h + 3) / 4 : h; }
inline size_t LevelBytes(uint32_t f, uint32_t w, uint32_t h) { return (size_t)RowBytes(f, w) * Rows(f, h); }
inline uint32_t Mip(uint32_t v, uint32_t level) { v >>= level; return v ? v : 1; }

inline bool Parse(const uint8_t* p, size_t n, Info& out, std::string& err) {
    auto u32 = [&](size_t o) { uint32_t v; memcpy(&v, p + o, 4); return v; };
    if (!p || n < 128 || memcmp(p, "DDS ", 4) != 0 || u32(4) != 124) { err = "not a .dds file"; return false; }
    Info i;
    i.height = u32(12); i.width = u32(16);
    uint32_t flags = u32(8), mips = u32(28);
    i.mips = (flags & 0x20000) && mips ? mips : 1;    // DDSD_MIPMAPCOUNT
    uint32_t pfFlags = u32(80), fourCC = u32(84), bits = u32(88);
    if (pfFlags & 4) {                                // DDPF_FOURCC
        if (fourCC == 0x30315844) { err = "DX10 .dds files aren't supported - save as DXT1/3/5 or 32-bit ARGB"; return false; }
        if (!Compressed(fourCC)) { err = "unsupported compression - use DXT1/3/5 or 32-bit ARGB"; return false; }
        i.format = fourCC;
    } else if ((pfFlags & 0x40) && bits == 32 && u32(92) == 0x00FF0000 && u32(96) == 0x0000FF00 && u32(100) == 0x000000FF
               && ((pfFlags & 1) ? u32(104) == 0xFF000000 : true)) {
        i.format = FMT_A8R8G8B8;                      // (no alpha mask: opaque, still 32 bits per pixel)
    } else { err = "unsupported pixel format - use DXT1/3/5 or 32-bit ARGB"; return false; }
    if (!i.width || !i.height || i.width > 4096 || i.height > 4096 || i.mips > 13) { err = "bad size"; return false; }
    size_t need = i.dataOffset;
    for (uint32_t l = 0; l < i.mips; ++l) need += LevelBytes(i.format, Mip(i.width, l), Mip(i.height, l));
    if (n < need) { err = "the file is cut short"; return false; }
    out = i;
    return true;
}

}  // namespace DdsUtil
