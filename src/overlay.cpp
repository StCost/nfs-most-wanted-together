#include <intrin.h>
#include "overlay.h"
#include "log.h"
#include "mem.h"
#include "config.h"
#include "input.h"
#include "together.h"
#include "game.h"
#include <cstdint>
#include <algorithm>

// speed.exe v1.3: import address table slot for d3d9!Direct3DCreate9
static constexpr uintptr_t IAT_Direct3DCreate9 = 0x8904A8;

namespace {
    using Direct3DCreate9_t = IDirect3D9* (WINAPI*)(UINT);
    using CreateDevice_t    = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
    using Present_t         = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    using Reset_t           = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

    Direct3DCreate9_t o_Direct3DCreate9 = nullptr;
    CreateDevice_t    o_CreateDevice = nullptr;
    Present_t         o_Present = nullptr;
    uint32_t          g_frames = 0;
    Reset_t           o_Reset = nullptr;

    IDirect3DDevice9* g_dev = nullptr;
    Overlay::FrameCallback g_cb = nullptr;

    // ---- text rendering (GDI -> managed texture) ----
    constexpr int TEX_W = 1024, TEX_H = 256;
    IDirect3DTexture9* g_tex = nullptr;
    std::string g_text, g_texText;
    int  g_textW = 0, g_textH = 0;
    bool g_visible = true;
    std::string g_shotPath;
    bool g_shotOverlay = false;   // test command "shot+": the frame as shown, with our overlay / page     // test command: save the next frame (without our overlay) as a BMP

    void SaveBackBuffer(IDirect3DDevice9* dev, const std::string& path) {
        IDirect3DSurface9* bb = nullptr; IDirect3DSurface9* sys = nullptr;
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
        D3DSURFACE_DESC d; bb->GetDesc(&d);
        IDirect3DSurface9* src = bb; IDirect3DSurface9* rt = nullptr;
        if (d.MultiSampleType != D3DMULTISAMPLE_NONE) {   // resolve first
            if (SUCCEEDED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr))) {
                dev->StretchRect(bb, nullptr, rt, nullptr, D3DTEXF_NONE); src = rt;
            }
        }
        if (SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
            SUCCEEDED(dev->GetRenderTargetData(src, sys))) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                if (FILE* f = fopen(path.c_str(), "wb")) {   // 32-bit BMP, top-down (X8R8G8B8 / A8R8G8B8)
                    BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
                    ih.biSize = sizeof(ih); ih.biWidth = d.Width; ih.biHeight = -(LONG)d.Height; ih.biPlanes = 1; ih.biBitCount = 32;
                    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
                    fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
                    for (UINT y = 0; y < d.Height; ++y) fwrite((uint8_t*)lr.pBits + y * lr.Pitch, 4, d.Width, f);
                    fclose(f);
                    LOG("Screenshot saved: %s (%ux%u)", path.c_str(), d.Width, d.Height);
                }
                sys->UnlockRect();
            }
        }
        if (sys) sys->Release();
        if (rt) rt->Release();
        bb->Release();
    }   // a notification is up: draw even when the overlay is hidden (F8)
    // Initialised at DLL load (static constructor), before anything can call SetText.
    struct TextLock {
        CRITICAL_SECTION cs;
        TextLock() { InitializeCriticalSection(&cs); }
    } g_textLock;
    #define g_textCs (g_textLock.cs)

    bool RebuildTexture(IDirect3DDevice9* dev) {
        if (!g_tex) {
            if (FAILED(dev->CreateTexture(TEX_W, TEX_H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex, nullptr))) {
                g_tex = nullptr;
                return false;
            }
        }

        HDC dc = CreateCompatibleDC(nullptr);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = TEX_W;
        bi.bmiHeader.biHeight = -TEX_H; // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (!bmp || !bits) {   // out of GDI handles / memory: skip this update instead of writing to null
            if (bmp) DeleteObject(bmp);
            if (dc) DeleteDC(dc);
            return false;
        }
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        HFONT font = CreateFontA(18, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
        HGDIOBJ oldFont = SelectObject(dc, font);

        memset(bits, 0, TEX_W * TEX_H * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT rc{ 6, 4, TEX_W - 6, TEX_H - 4 };
        DrawTextA(dc, g_texText.c_str(), -1, &rc, DT_LEFT | DT_TOP | DT_NOPREFIX);
        RECT calc{ 0, 0, TEX_W - 12, TEX_H - 8 };
        DrawTextA(dc, g_texText.c_str(), -1, &calc, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_CALCRECT);
        g_textW = (std::min)((int)calc.right + 12, TEX_W);
        g_textH = (std::min)((int)calc.bottom + 8, TEX_H);
        GdiFlush();

        D3DLOCKED_RECT lr;
        if (SUCCEEDED(g_tex->LockRect(0, &lr, nullptr, 0))) {
            const uint32_t* src = (const uint32_t*)bits;
            for (int y = 0; y < TEX_H; ++y) {
                uint32_t* dst = (uint32_t*)((uint8_t*)lr.pBits + y * lr.Pitch);
                for (int x = 0; x < TEX_W; ++x) {
                    uint32_t c = src[y * TEX_W + x] & 0xFF; // white text, use one channel as alpha
                    dst[x] = (c << 24) | 0x00FFFFFF;
                }
            }
            g_tex->UnlockRect(0);
        }

        SelectObject(dc, oldFont);
        SelectObject(dc, oldBmp);
        DeleteObject(font);
        DeleteObject(bmp);
        DeleteDC(dc);
        return true;
    }

    struct Vtx { float x, y, z, rhw; DWORD color; float u, v; };

    // Fixed-function states for drawing pre-transformed, textured quads on top of the frame.
    void SetupStates(IDirect3DDevice9* dev) {
        dev->SetVertexShader(nullptr);
        dev->SetPixelShader(nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    }

    void DrawOverlay(IDirect3DDevice9* dev) {
        std::string text;
        EnterCriticalSection(&g_textCs);
        text = g_text;
        LeaveCriticalSection(&g_textCs);
        if (text.empty()) return;

        if (!g_tex || text != g_texText) {
            g_texText = text;
            if (!RebuildTexture(dev)) return;
        }

        IDirect3DStateBlock9* sb = nullptr;
        if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) return;

        SetupStates(dev);

        const float x0 = 10.f, y0 = 10.f;
        const float w = (float)g_textW, h = (float)g_textH;
        const float u1 = w / TEX_W, v1 = h / TEX_H;

        // background panel (texture disabled -> diffuse only)
        dev->SetTexture(0, nullptr);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        const DWORD bg = 0xA0000000;
        Vtx quad[4] = {
            { x0 - .5f,     y0 - .5f,     0, 1, bg, 0, 0 },
            { x0 + w - .5f, y0 - .5f,     0, 1, bg, 0, 0 },
            { x0 - .5f,     y0 + h - .5f, 0, 1, bg, 0, 0 },
            { x0 + w - .5f, y0 + h - .5f, 0, 1, bg, 0, 0 },
        };
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vtx));

        // text
        dev->SetTexture(0, g_tex);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        const DWORD fg = 0xFFFFFFFF;
        Vtx tq[4] = {
            { x0 - .5f,     y0 - .5f,     0, 1, fg, 0,  0  },
            { x0 + w - .5f, y0 - .5f,     0, 1, fg, u1, 0  },
            { x0 - .5f,     y0 + h - .5f, 0, 1, fg, 0,  v1 },
            { x0 + w - .5f, y0 + h - .5f, 0, 1, fg, u1, v1 },
        };
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, tq, sizeof(Vtx));

        sb->Apply();
        sb->Release();
    }


    // ---- the other player's pursuit bar (pursuit.inl sets it) ----
    Overlay::PeerBar g_peerBar;
    constexpr int LBL_W = 512, LBL_H = 48;
    IDirect3DTexture9* g_lblTex = nullptr;
    std::string g_lblText;
    int g_lblW = 0, g_lblH = 0;

    bool RebuildLabel(IDirect3DDevice9* dev, const std::string& text) {
        if (!g_lblTex && FAILED(dev->CreateTexture(LBL_W, LBL_H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_lblTex, nullptr))) {
            g_lblTex = nullptr; return false;
        }
        HDC dc = CreateCompatibleDC(nullptr);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = LBL_W; bi.bmiHeader.biHeight = -LBL_H;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) { if (bmp) DeleteObject(bmp); DeleteDC(dc); return false; }
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        HFONT font = CreateFontA(-34, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
        HGDIOBJ oldFont = SelectObject(dc, font);
        memset(bits, 0, LBL_W * LBL_H * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT rc{ 0, 0, LBL_W, LBL_H };
        DrawTextA(dc, text.c_str(), -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        RECT calc{ 0, 0, LBL_W, LBL_H };
        DrawTextA(dc, text.c_str(), -1, &calc, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
        g_lblW = (std::min)((int)calc.right + 2, LBL_W); g_lblH = LBL_H;
        GdiFlush();
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(g_lblTex->LockRect(0, &lr, nullptr, 0))) {
            const uint32_t* src = (const uint32_t*)bits;
            for (int y = 0; y < LBL_H; ++y) {
                uint32_t* dst = (uint32_t*)((uint8_t*)lr.pBits + y * lr.Pitch);
                for (int x = 0; x < LBL_W; ++x) dst[x] = ((src[y * LBL_W + x] & 0xFF) << 24) | 0x00FFFFFF;
            }
            g_lblTex->UnlockRect(0);
        }
        SelectObject(dc, oldFont); SelectObject(dc, oldBmp);
        DeleteObject(font); DeleteObject(bmp); DeleteDC(dc);
        g_lblText = text;
        return true;
    }

    void FillQuad(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1, DWORD c) {
        if (!(x1 > x0) || !(y1 > y0)) return;
        Vtx q[4] = { { x0 - .5f, y0 - .5f, 0, 1, c, 0, 0 }, { x1 - .5f, y0 - .5f, 0, 1, c, 0, 0 },
                     { x0 - .5f, y1 - .5f, 0, 1, c, 0, 0 }, { x1 - .5f, y1 - .5f, 0, 1, c, 0, 0 } };
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx));
    }

    void DrawPeerBar(IDirect3DDevice9* dev, UINT bbW, UINT bbH) {
        Overlay::PeerBar b;
        EnterCriticalSection(&g_textCs);
        b = g_peerBar;
        LeaveCriticalSection(&g_textCs);
        if (b.kind <= 0 || !bbW || !bbH) return;
        std::string label = b.name.empty() ? std::string("Partner") : b.name;
        if (b.kind == 2) label += "  -  COOLDOWN";
        else if (b.value >= 100) label += "  -  EVADE";
        else if (b.value <= -60) label += "  -  BUSTED?";
        if (!g_lblTex || label != g_lblText) { if (!RebuildLabel(dev, label)) return; }

        IDirect3DStateBlock9* sb = nullptr;
        if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) return;
        SetupStates(dev);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        const float H = (float)bbH, W = (float)bbW;
        const float bw = b.width * H, ph = 0.046f * H;
        const float left = (float)(int)(W / 2 - bw / 2), right = left + bw;
        const float top = (float)(int)(b.y * H - ph / 2), bottom = top + ph;
        const float pad = 0.006f * H;
        const float lblH = 0.022f * H, trackTop = top + pad + lblH + 0.003f * H, trackBot = bottom - pad;

        dev->SetTexture(0, nullptr);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        FillQuad(dev, left, top, right, bottom, 0xA0000000);                             // panel
        const float tl = left + pad, tr = right - pad, mid = (float)(int)((tl + tr) / 2), half = (tr - tl) / 2;
        FillQuad(dev, tl, trackTop, tr, trackBot, 0x50FFFFFF);                            // track
        float v = (float)(b.value < -100 ? -100 : b.value > 100 ? 100 : b.value) / 100.f;
        if (b.kind == 2) {
            FillQuad(dev, tl, trackTop, tl + (tr - tl) * v, trackBot, 0xE03C8CF0);       // cooldown: blue, left to right
        } else {
            if (v < 0) FillQuad(dev, mid + half * v, trackTop, mid, trackBot, 0xE0E03030);   // busted: red, to the left
            if (v > 0) FillQuad(dev, mid, trackTop, mid + half * v, trackBot, 0xE030D060);   // evade: green, to the right
            FillQuad(dev, mid - 1, trackTop - 0.003f * H, mid + 1, trackBot + 0.003f * H, 0xC0FFFFFF);   // the middle
            float mx = mid + half * v;
            FillQuad(dev, mx - 2, trackTop - 0.004f * H, mx + 2, trackBot + 0.004f * H, 0xFFFFFFFF);   // marker
        }
        // label: the other player's name (and the state)
        float scale = lblH / (float)g_lblH;
        float lw = g_lblW * scale, lx = tl, ly = top + pad;
        float u1 = (float)g_lblW / LBL_W;
        dev->SetTexture(0, g_lblTex);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        const DWORD fg = 0xF0FFFFFF;
        Vtx tq[4] = { { lx - .5f, ly - .5f, 0, 1, fg, 0, 0 }, { lx + lw - .5f, ly - .5f, 0, 1, fg, u1, 0 },
                      { lx - .5f, ly + lblH - .5f, 0, 1, fg, 0, 1 }, { lx + lw - .5f, ly + lblH - .5f, 0, 1, fg, u1, 1 } };
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, tq, sizeof(Vtx));
        sb->Apply();
        sb->Release();
    }

    // ---- the Together settings page (together.cpp paints it, we put it on screen) ----
    constexpr int TG_W = 1024, TG_H = 1024;
    IDirect3DTexture9* g_tgTex = nullptr;
    int g_tgW = 0, g_tgH = 0;

    void DrawTogether(IDirect3DDevice9* dev, UINT bbW, UINT bbH) {
        if (!Together::Visible()) return;
        if (!g_tgTex) {
            if (FAILED(dev->CreateTexture(TG_W, TG_H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tgTex, nullptr))) { g_tgTex = nullptr; return; }
            g_tgW = g_tgH = 0;
        }
        if (Together::Dirty() || !g_tgW) {
            D3DLOCKED_RECT lr;
            if (FAILED(g_tgTex->LockRect(0, &lr, nullptr, 0))) return;
            int w = 0, h = 0;
            if (!Together::Paint((uint32_t*)lr.pBits, lr.Pitch / 4, TG_W, TG_H, &w, &h)) w = h = 0;
            g_tgTex->UnlockRect(0);
            g_tgW = w; g_tgH = h;
        }
        if (!g_tgW || !g_tgH) return;

        IDirect3DStateBlock9* sb = nullptr;
        if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) return;
        SetupStates(dev);
        float w = (float)g_tgW, h = (float)g_tgH, x0, y0;
        if (Together::Centered()) {     // the page: centred, scaled down to fit small back buffers
            float fit = std::min((bbW - 16.f) / w, (bbH - 16.f) / h);
            float sc = std::min(1.f, fit);
            if (bbH >= 1400) sc = std::min(fit, bbH * 0.7f / h);   // and up on very big screens
            if (sc != 1.f) { dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR); }
            w *= sc; h *= sc;
            x0 = (float)(int)((bbW - w) / 2); y0 = (float)(int)((bbH - h) / 2);
            // dim the game behind it
            dev->SetTexture(0, nullptr);
            dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
            const DWORD dim = 0x90000000; const float W = (float)bbW, H = (float)bbH;
            Vtx q[4] = { { -.5f, -.5f, 0, 1, dim, 0, 0 }, { W - .5f, -.5f, 0, 1, dim, 0, 0 },
                         { -.5f, H - .5f, 0, 1, dim, 0, 0 }, { W - .5f, H - .5f, 0, 1, dim, 0, 0 } };
            dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx));
        } else {                        // the hint: bottom left
            x0 = 12.f; y0 = (float)bbH - h - 12.f;
        }
        const float u1 = (float)g_tgW / TG_W, v1 = (float)g_tgH / TG_H;
        dev->SetTexture(0, g_tgTex);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        const DWORD fg = 0xFFFFFFFF;
        Vtx tq[4] = {
            { x0 - .5f,     y0 - .5f,     0, 1, fg, 0,  0  },
            { x0 + w - .5f, y0 - .5f,     0, 1, fg, u1, 0  },
            { x0 - .5f,     y0 + h - .5f, 0, 1, fg, 0,  v1 },
            { x0 + w - .5f, y0 + h - .5f, 0, 1, fg, u1, v1 },
        };
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, tq, sizeof(Vtx));
        sb->Apply();
        sb->Release();
    }

    // Draw onto the back buffer right before the frame is presented, so it is always on top and
    // always on screen, no matter which offscreen targets the game used during the frame.
    void DrawOnBackBuffer(IDirect3DDevice9* dev) {
        IDirect3DSurface9 *oldRT = nullptr, *oldDS = nullptr, *bb = nullptr;
        D3DVIEWPORT9 oldVp;
        dev->GetViewport(&oldVp);
        if (FAILED(dev->GetRenderTarget(0, &oldRT))) oldRT = nullptr;
        if (FAILED(dev->GetDepthStencilSurface(&oldDS))) oldDS = nullptr;
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) {
            if (oldRT) oldRT->Release();
            if (oldDS) oldDS->Release();
            return;
        }
        dev->SetRenderTarget(0, bb);       // also resets the viewport to the full back buffer
        dev->SetDepthStencilSurface(nullptr);

        D3DSURFACE_DESC bd{}; bb->GetDesc(&bd);
        if (SUCCEEDED(dev->BeginScene())) {
            DrawPeerBar(dev, bd.Width, bd.Height);
            if (g_visible) DrawOverlay(dev);
            DrawTogether(dev, bd.Width, bd.Height);
            dev->EndScene();
        }

        if (g_frames == 1 || g_frames % 3600 == 0) {
            D3DSURFACE_DESC d{}; bb->GetDesc(&d);
            LOG("Overlay: frame %u, back buffer %ux%u, text %s", g_frames, d.Width, d.Height, g_texText.empty() ? "empty" : "set");
        }

        dev->SetRenderTarget(0, oldRT ? oldRT : bb);
        dev->SetDepthStencilSurface(oldDS);
        dev->SetViewport(&oldVp);
        if (oldRT) oldRT->Release();
        if (oldDS) oldDS->Release();
        bb->Release();
    }

    Present_t g_realPresent = nullptr; // first (d3d9.dll) Present we ever saw
    Reset_t   g_realReset = nullptr;
    bool      g_inPresent = false;

    // ---- hide the Windows cursor over the game (the game draws its own) ----
    // In windowed / borderless mode the system arrow stays visible on top of the game's cursor.
    HWND    g_gameWnd = nullptr;
    WNDPROC g_origWndProc = nullptr;
    bool    g_wndUnicode = false;

    void (*g_onClose)() = nullptr;

    LRESULT CALLBACK CursorWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
        // Alt+F4 / X button / game quit: let the other player know right away.
        if ((msg == WM_CLOSE || msg == WM_DESTROY || msg == WM_ENDSESSION || (msg == WM_SYSCOMMAND && (w & 0xFFF0) == SC_CLOSE)) && g_onClose) {
            static int logged = 0;
            if (logged++ < 4) LOG("Window: closing message %04X (w %08X l %08X) from %p", msg, (unsigned)w, (unsigned)l, _ReturnAddress());
            if (msg == WM_DESTROY) Game::LogStack("Window: WM_DESTROY");   // who destroyed it (the exe: its exit flag was set)
            g_onClose();
        }
        // Our Together page reads the keyboard itself. Its typed characters must not reach the game as
        // well: in the main menu a 'q' / 'Q' opens the game's "Quit?" dialog (0x62799A4C, exitutil.h),
        // which the next Enter on our page would answer.
        if (msg == WM_CHAR && Together::IsOpen()) return 0;
        // A device was plugged in / removed (USB or Bluetooth pad): the hot-plug check runs on the next frames.
        if (msg == WM_DEVICECHANGE) Pad::OnDeviceChange();
        // Hide only while the game is the active window; a background game window can still get
        // WM_SETCURSOR when the pointer passes over it, and must not hide the user's cursor then.
        if (msg == WM_SETCURSOR && LOWORD(l) == HTCLIENT && g_cfg.hideCursor && GetForegroundWindow() == h) {
            SetCursor(nullptr);
            return TRUE;
        }
        // Losing focus (alt-tab, another window clicked): put the normal arrow back right away.
        if ((msg == WM_ACTIVATEAPP && !w) || msg == WM_KILLFOCUS || (msg == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE))
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return g_wndUnicode ? CallWindowProcW(g_origWndProc, h, msg, w, l)
                            : CallWindowProcA(g_origWndProc, h, msg, w, l);
    }

    void UpdateCursor(IDirect3DDevice9* dev) {
        if (!g_gameWnd) {
            D3DDEVICE_CREATION_PARAMETERS cp{};
            if (FAILED(dev->GetCreationParameters(&cp)) || !cp.hFocusWindow) return;
            g_gameWnd = cp.hFocusWindow;
            g_wndUnicode = IsWindowUnicode(g_gameWnd) != FALSE;
            g_origWndProc = (WNDPROC)(g_wndUnicode
                ? SetWindowLongPtrW(g_gameWnd, GWLP_WNDPROC, (LONG_PTR)&CursorWndProc)
                : SetWindowLongPtrA(g_gameWnd, GWLP_WNDPROC, (LONG_PTR)&CursorWndProc));
            LOG("Cursor: subclassed game window %p (orig proc %p)", g_gameWnd, g_origWndProc);
        }
        if (!g_cfg.hideCursor) return;
        // WM_SETCURSOR only arrives when the mouse moves; also clear it while the game is focused
        // and the pointer is inside its client area (e.g. right after alt-tabbing back).
        // Only when the game is foreground AND the pointer is over the game's client area AND no
        // other window sits under the pointer (e.g. an always-on-top window above the game).
        if (GetForegroundWindow() != g_gameWnd) return;
        POINT pt; RECT rc;
        if (!GetCursorPos(&pt) || WindowFromPoint(pt) != g_gameWnd) return;
        POINT cp = pt;
        if (ScreenToClient(g_gameWnd, &cp) && GetClientRect(g_gameWnd, &rc) && PtInRect(&rc, cp))
            SetCursor(nullptr);
    }

    // test command "stack": the game's return addresses on the stack of the next Present call (which loop
    // is drawing this frame, e.g. a loading screen that waits): values in the exe's code that follow a call
    volatile LONG g_stackReq = 0;
    void LogStackOnce() {
        char here = 0;
        const uint8_t* sp = (const uint8_t*)&here;
        const uint8_t* top = (const uint8_t*)((NT_TIB*)NtCurrentTeb())->StackBase;
        char line[1024]; int n = 0, found = 0;
        n += sprintf_s(line + n, sizeof(line) - n, "Stack:");
        for (const uint8_t* p = (const uint8_t*)((uintptr_t)sp & ~3u); p + 4 <= top && found < 48; p += 4) {
            uint32_t a = *(const uint32_t*)p;
            if (a < 0x401005 || a > 0x8A0000) continue;
            const uint8_t* c = (const uint8_t*)a;
            bool afterCall = c[-5] == 0xE8 || c[-6] == 0xFF || c[-2] == 0xFF || c[-3] == 0xFF;
            if (!afterCall) continue;
            n += sprintf_s(line + n, sizeof(line) - n, " %06X@%X", a, (unsigned)(p - sp));
            ++found;
            if (n > (int)sizeof(line) - 24) break;
        }
        LOG("%s", line);
    }

    HRESULT STDMETHODCALLTYPE hk_Present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) {
        if (g_inPresent) return g_realPresent(dev, src, dst, wnd, dirty); // someone chained back into us
        g_inPresent = true;
        UpdateCursor(dev);
        if (InterlockedExchange(&g_stackReq, 0)) { __try { LogStackOnce(); } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Stack: walk failed"); } }
        IDirect3DDevice9* gameDev = nullptr;
        Mem::Read(0x982BDC, gameDev);
        if (gameDev && gameDev != g_dev) {
            LOG("Game device changed %p -> %p", g_dev, gameDev);
            if (g_tgTex) { g_tgTex->Release(); g_tgTex = nullptr; }
            g_dev = gameDev;
        }
        if (dev == g_dev) {
            ++g_frames;
            if (g_cb) g_cb(dev);
            if (!g_shotPath.empty() && !g_shotOverlay) { SaveBackBuffer(dev, g_shotPath); g_shotPath.clear(); }   // before our overlay
            if (g_visible || Together::Visible()) DrawOnBackBuffer(dev);
            if (!g_shotPath.empty() && g_shotOverlay) { SaveBackBuffer(dev, g_shotPath); g_shotPath.clear(); }
        }
        HRESULT hr = o_Present(dev, src, dst, wnd, dirty);
        g_inPresent = false;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hk_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
        // our texture is D3DPOOL_MANAGED and state blocks are per-frame, so nothing to release
        HRESULT hr = o_Reset(dev, pp);
        LOG("Device Reset -> 0x%08X", (unsigned)hr);
        return hr;
    }

    void HookDevice(IDirect3DDevice9* dev) {
        void** vt = *(void***)dev;
        if (vt[17] != (void*)&hk_Present) {
            void* prevP = Mem::PatchPointer(&vt[17], (void*)&hk_Present);
            if (prevP != (void*)&hk_Present) o_Present = (Present_t)prevP;
            if (!g_realPresent) g_realPresent = o_Present;
            LOG("Hooked Present in vtable %p (prev %p)", vt, prevP);
        }
        if (vt[16] != (void*)&hk_Reset) {
            void* prevR = Mem::PatchPointer(&vt[16], (void*)&hk_Reset);
            if (prevR != (void*)&hk_Reset) o_Reset = (Reset_t)prevR;
            if (!g_realReset) g_realReset = o_Reset;
            LOG("Hooked Reset in vtable %p (prev %p)", vt, prevR);
        }
    }

    HRESULT STDMETHODCALLTYPE hk_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags,
                                              D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
        HRESULT hr = o_CreateDevice(d3d, adapter, type, wnd, flags, pp, out);
        LOG("CreateDevice -> 0x%08X (%ux%u windowed=%d)", (unsigned)hr,
            pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->Windowed : -1);
        if (SUCCEEDED(hr) && out && *out) {
            if (g_tex) { g_tex->Release(); g_tex = nullptr; } // new device: drop old textures
            if (g_tgTex) { g_tgTex->Release(); g_tgTex = nullptr; }
            g_dev = *out;
            HookDevice(g_dev);
        }
        return hr;
    }

    IDirect3D9* WINAPI hk_Direct3DCreate9(UINT sdk) {
        IDirect3D9* d3d = o_Direct3DCreate9(sdk);
        LOG("Direct3DCreate9(%u) -> %p", sdk, d3d);
        if (d3d && !o_CreateDevice) {
            void** vt = *(void***)d3d;
            o_CreateDevice = (CreateDevice_t)Mem::PatchPointer(&vt[16], (void*)&hk_CreateDevice);
        }
        return d3d;
    }
}

// Background check (4x per second): something (d3d9 runtime / another mod) restores the original
// Present pointer during startup, so we re-hook when that happens. Logs hook health once a minute and re-hooks if another mod or the game
// replaced our vtable entries or switched to a new device.
static DWORD WINAPI Watchdog(LPVOID) {
    uint32_t lastFrames = 0;
    for (int tick = 0;; ++tick) {
        Sleep(250);
        IDirect3DDevice9* gameDev = nullptr;
        Mem::Read(0x982BDC, gameDev);
        void** vt = nullptr;
        if (gameDev) Mem::Read((uintptr_t)gameDev, vt);
        void* slot17 = nullptr;
        if (vt) Mem::Read((uintptr_t)&vt[17], slot17);
        bool hooked = slot17 == (void*)&hk_Present;
        if (!hooked || tick % 240 == 0) // on re-hook, and once a minute
            LOG("watchdog: frames %u (+%u)  gameDev %p  ourDev %p  vt %p  Present slot %p %s",
                g_frames, g_frames - lastFrames, gameDev, g_dev, vt, slot17, hooked ? "(ours)" : "(NOT OURS)");
        lastFrames = g_frames;
        if (gameDev && vt && !hooked && o_Present) {
            LOG("watchdog: re-hooking device %p", gameDev);
            HookDevice(gameDev);
        }
    }
    return 0;
}

void Overlay::RequestStack() { InterlockedExchange(&g_stackReq, 1); }
bool Overlay::InstallHooks() {
    void** slot = (void**)IAT_Direct3DCreate9;
    if (!Mem::IsReadable(slot, 4)) { LOG("IAT slot not readable - wrong exe version?"); return false; }
    o_Direct3DCreate9 = (Direct3DCreate9_t)Mem::PatchPointer(slot, (void*)&hk_Direct3DCreate9);
    LOG("Hooked Direct3DCreate9 IAT (orig %p)", o_Direct3DCreate9);
    if (HANDLE th = CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr)) CloseHandle(th);
    return o_Direct3DCreate9 != nullptr;
}

void Overlay::SetFrameCallback(FrameCallback cb) { g_cb = cb; }
void Overlay::SetPeerBar(const PeerBar& b) {
    EnterCriticalSection(&g_textCs);
    g_peerBar = b;
    LeaveCriticalSection(&g_textCs);
}

void Overlay::SetText(const std::string& text) {
    EnterCriticalSection(&g_textCs);
    g_text = text;
    LeaveCriticalSection(&g_textCs);
}

void Overlay::SetVisible(bool v) { g_visible = v; }
void Overlay::Screenshot(const std::string& path, bool withOverlay) { g_shotOverlay = withOverlay; g_shotPath = path; }
bool Overlay::IsVisible() { return g_visible; }
IDirect3DDevice9* Overlay::Device() { return g_dev; }
HWND Overlay::GameWindow() { return g_gameWnd; }
void Overlay::SetCloseCallback(void (*cb)()) { g_onClose = cb; }
