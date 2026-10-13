// ================================================================ WinlatorXR: the frame
//
// The sibling of SubmitTestQuad for the WinlatorXR flavour. There is nothing to copy and nothing
// to submit: the backbuffer the game is about to present already IS the headset image, because
// draw duplication put the two eyes side by side in it. What is left is to tell WinlatorXR how to
// read it, and to put on it what an OpenXR compositor would otherwise have layered on top.
//
//   1. decide what this frame is - side-by-side stereo, one eye of alternate-eye, or a flat
//      menu/loading picture - with the same per-frame duplication test the desktop path uses
//   2. draw the VR settings panel into it, once per eye
//   3. stamp the HMD_SYNC of the pose it was rendered with into the top-left corner
//   4. reply with the mode and the FOV it was rendered at
//   5. take the next pose, and drive head tracking and input from it, as the desktop loop does
//
// Called from Present, before the real Present, in place of CaptureFrame + SubmitTestQuad.

static IDirect3DTexture9*    g_wxrScreenTex = nullptr;   // the flat frame, for the virtual screen
static UINT                  g_wxrScreenW = 0, g_wxrScreenH = 0;
static IDirect3DTexture9*    g_wxrMenuTex = nullptr;
static IDirect3DStateBlock9* g_wxrMenuSB  = nullptr;
static bool                  g_wxrMenuTexStale = true;
static DWORD                 g_wxrSyncWaitMs = 20;

// DEFAULT-pool resources must not outlive a Reset on a plain D3D9 device.
static void WxrReleaseDeviceResources()
{
    if (g_wxrMenuTex) { g_wxrMenuTex->Release(); g_wxrMenuTex = nullptr; }
    if (g_wxrScreenTex) { g_wxrScreenTex->Release(); g_wxrScreenTex = nullptr; }
    g_wxrScreenW = g_wxrScreenH = 0;
    if (g_wxrMenuSB)  { g_wxrMenuSB->Release();  g_wxrMenuSB  = nullptr; }
    g_wxrMenuTexStale = true;
}

// ---- the window ----
//
// WinlatorXR samples the X SCREEN at (0,0), not the backbuffer. Public builds give a windowed game
// a title bar and an offset, and then the stamp is never seen and the side-by-side pair is shown
// flat, as two copies. So the window is made borderless and pinned to the screen.
//
// Not from here: Present runs on UE3's render thread, and restyling a window owned by another
// thread sends it messages and waits - while that thread may be waiting on this one. The CoD4 port
// measured exactly that deadlock (0.9 fps, black). The window is subclassed instead, and the work is
// POSTED to it, which never blocks; its own thread does the restyle when it next pumps messages.
static HWND    g_wxrWindow = nullptr;
static WNDPROC g_wxrPrevProc = nullptr;
static bool    g_wxrWindowUnicode = false;
static const UINT kWxrPinMessage = WM_APP + 0x4D45;

static bool WxrWindowPlaced(HWND w)
{
    RECT r{};
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    return GetWindowRect(w, &r) && r.left == 0 && r.top == 0 &&
           r.right - r.left == sw && r.bottom - r.top == sh &&
           !(GetWindowLongPtrA(w, GWL_STYLE) & (WS_CAPTION | WS_THICKFRAME));
}

static LRESULT CALLBACK WxrWindowProc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (WxrSwallowInputMessage(msg)) return 0;          // gamepad only, see winlatorxr_input.inl
    if (msg == kWxrPinMessage) {
        RECT before{};
        GetWindowRect(w, &before);
        const LONG_PTR style = GetWindowLongPtrA(w, GWL_STYLE);
        SetWindowLongPtrA(w, GWL_STYLE, (style & ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME |
                                                   WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP);
        const LONG_PTR ex = GetWindowLongPtrA(w, GWL_EXSTYLE);
        SetWindowLongPtrA(w, GWL_EXSTYLE, ex & ~(WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME |
                                                 WS_EX_STATICEDGE));
        SetWindowPos(w, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                     SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        Log("[wxr] window pinned borderless to 0,0 %dx%d (was %ld,%ld %ldx%ld)",
            GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), before.left, before.top,
            before.right - before.left, before.bottom - before.top);
        return 0;
    }
    return g_wxrWindowUnicode ? CallWindowProcW(g_wxrPrevProc, w, msg, wp, lp)
                              : CallWindowProcA(g_wxrPrevProc, w, msg, wp, lp);
}

static void WxrKeepWindowPinned(IDirect3DDevice9* dev, long frame)
{
    static int attempts = 0;
    if (!g_wxrWindow) {
        D3DDEVICE_CREATION_PARAMETERS cp{};
        if (FAILED(dev->GetCreationParameters(&cp)) || !cp.hFocusWindow) return;
        g_wxrWindow = cp.hFocusWindow;
        g_wxrWindowUnicode = IsWindowUnicode(g_wxrWindow) != FALSE;
        g_wxrPrevProc = (WNDPROC)(g_wxrWindowUnicode
            ? SetWindowLongPtrW(g_wxrWindow, GWLP_WNDPROC, (LONG_PTR)&WxrWindowProc)
            : SetWindowLongPtrA(g_wxrWindow, GWLP_WNDPROC, (LONG_PTR)&WxrWindowProc));
        if (!g_wxrPrevProc) { Log("[wxr] could not subclass the game window - not pinning it"); attempts = 1000; }
    }
    // Twice a second is plenty, and a window manager that keeps undoing it gets thirty tries.
    if (attempts >= 30 || (frame % 30) != 0 || WxrWindowPlaced(g_wxrWindow)) return;
    if (++attempts == 30) Log("[wxr] the window keeps leaving 0,0 - giving up on pinning it");
    PostMessageA(g_wxrWindow, kWxrPinMessage, 0, 0);
}

// ---- the VR settings panel ----
//
// Head-locked like the desktop quad layer: the same pose, distance and size, projected into each
// eye with the frustum WinlatorXR is told the frame was rendered at, so it lands where the OpenXR
// quad would have.
struct WxrMenuVertex { float x, y, z, rhw, u, v; };

static void WxrDrawMenu(IDirect3DDevice9* dev, IDirect3DSurface9* bb, const D3DSURFACE_DESC& d,
                        int layout, int aerEye, float ipd, float tanX, float tanY)
{
    if (!VrMenuTickVisible()) return;
    if (g_menuPixels.size() != (size_t)kMenuW * kMenuH) {
        g_menuPixels.resize((size_t)kMenuW * kMenuH);
        g_menuLastImage.clear();
    }
    if (!g_wxrMenuTex) {
        if (FAILED(dev->CreateTexture(kMenuW, kMenuH, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                                      D3DPOOL_DEFAULT, &g_wxrMenuTex, nullptr))) {
            g_wxrMenuTex = nullptr;
            return;
        }
        g_wxrMenuTexStale = true;
    }
    if (VrMenuRasterizeIfChanged() || g_wxrMenuTexStale) {
        D3DLOCKED_RECT lr{};
        if (FAILED(g_wxrMenuTex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) return;
        for (int y = 0; y < kMenuH; ++y)
            memcpy((BYTE*)lr.pBits + (size_t)y * lr.Pitch, &g_menuPixels[(size_t)y * kMenuW], kMenuW * 4);
        g_wxrMenuTex->UnlockRect(0);
        g_wxrMenuTexStale = false;
    }
    if (!g_wxrMenuSB && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_wxrMenuSB))) { g_wxrMenuSB = nullptr; return; }
    if (FAILED(g_wxrMenuSB->Capture())) return;

    // The desktop layer's geometry, in metres, head-locked.
    float cx, cy, dist, sx, sy, u1, v1;
    if (g_menuOpen) { cx = 0; cy = -0.10f; dist = g_menuDistance; sx = g_menuSize; sy = g_menuSize * kMenuH / kMenuW; u1 = 1; v1 = 1; }
    else            { cx = 0.45f; cy = -0.4f; dist = 1.5f; sx = 0.22f; sy = 0.065f; u1 = 300.0f / kMenuW; v1 = 85.0f / kMenuH; }

    IDirect3DSurface9* oldRT = nullptr;
    dev->GetRenderTarget(0, &oldRT);
    dev->SetRenderTarget(0, bb);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetTexture(0, g_wxrMenuTex);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);

    const int passes = layout == 1 ? 2 : 1;
    for (int pass = 0; pass < passes; ++pass) {
        // layout 0: flat picture - no eye; 1: side by side; 2: alternate-eye, aerEye's frame
        const int eye = layout == 1 ? pass : (layout == 2 ? aerEye : -1);
        const float vpX = layout == 1 ? (pass ? d.Width * 0.5f : 0.0f) : 0.0f;
        const float vpW = layout == 1 ? d.Width * 0.5f : (float)d.Width;
        const float vpH = (float)d.Height;
        D3DVIEWPORT9 vp{ (DWORD)vpX, 0, (DWORD)vpW, (DWORD)vpH, 0.0f, 1.0f };
        dev->SetViewport(&vp);
        const float ex = eye < 0 ? 0.0f : (eye == 0 ? -0.5f : 0.5f) * ipd;
        auto px = [&](float x) { return vpX + (0.5f + 0.5f * (x - ex) / (dist * tanX)) * vpW - 0.5f; };
        auto py = [&](float y) { return (0.5f - 0.5f * y / (dist * tanY)) * vpH - 0.5f; };
        const float l = px(cx - sx * 0.5f), r = px(cx + sx * 0.5f);
        const float t = py(cy + sy * 0.5f), b = py(cy - sy * 0.5f);
        const WxrMenuVertex q[4] = { { l, t, 0, 1, 0, 0 }, { r, t, 0, 1, u1, 0 },
                                     { l, b, 0, 1, 0, v1 }, { r, b, 0, 1, u1, v1 } };
        if (g_origDrawPrimUP) g_origDrawPrimUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(WxrMenuVertex));
        else dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(WxrMenuVertex));
    }

    dev->SetTexture(0, nullptr);
    g_wxrMenuSB->Apply();
    if (oldRT) { dev->SetRenderTarget(0, oldRT); oldRT->Release(); }
}

// ---- frame dump, for checking the picture without a headset view ----
//
// Create mevr_shot.request beside d3d9.dll (adb push works on the headset) and the next frame
// is written to mevr_shot.bmp there, exactly as WinlatorXR receives it, stamp included. The
// request is deleted once served. Polled twice a second, so it costs nothing otherwise.
static void WxrServeFrameDump(IDirect3DDevice9* dev, IDirect3DSurface9* bb, const D3DSURFACE_DESC& d)
{
    wchar_t dir[MAX_PATH] = L"";
    if (!g_selfModule || !GetModuleFileNameW(g_selfModule, dir, MAX_PATH)) return;
    if (wchar_t* slash = wcsrchr(dir, L'\\')) *slash = 0;
    wchar_t request[MAX_PATH], shot[MAX_PATH];
    swprintf_s(request, L"%s\\mevr_shot.request", dir);
    swprintf_s(shot, L"%s\\mevr_shot.bmp", dir);
    if (GetFileAttributesW(request) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileW(request);
    IDirect3DSurface9* sys = nullptr;
    if (FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr))) return;
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(dev->GetRenderTargetData(bb, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        FILE* f = nullptr;
        if (!_wfopen_s(&f, shot, L"wb") && f) {
            const uint32_t row = d.Width * 4, size = row * d.Height;
            BITMAPFILEHEADER fh{ 0x4D42, (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + size), 0, 0,
                                 (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)) };
            BITMAPINFOHEADER ih{ sizeof(BITMAPINFOHEADER), (LONG)d.Width, -(LONG)d.Height, 1, 32, BI_RGB, size };
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&ih, sizeof(ih), 1, f);
            for (UINT y = 0; y < d.Height; ++y) fwrite((BYTE*)lr.pBits + (size_t)y * lr.Pitch, row, 1, f);
            fclose(f);
            Log("[wxr] frame %ux%u dumped to mevr_shot.bmp", d.Width, d.Height);
        }
        sys->UnlockRect();
    }
    sys->Release();
}

// ---- menus, loading and videos on a screen fixed in the room ----
//
// WinlatorXR's own screen mode (MODE_VR 2) is head-locked: the picture follows every head turn,
// which is what players reported. As the CoD4 port does, a flat frame is instead copied, the
// window cleared, and the copy drawn into both eye halves as a screen 2 m in front of where the
// player was looking when it appeared. It stays there until the next one, or a recentre.
// The screen is curved: a cylinder section centred on where the player stood, so every column is
// the same distance away and the edges of a wide menu do not fall off into the distance.
struct WxrScreenAnchor { bool valid = false; float cx, cy, cz, rx, rz; LONG recenter = -1; };
static WxrScreenAnchor g_wxrAnchor;
static const float kWxrScreenDistance = 2.0f, kWxrScreenWidth = 2.8f;

static void WxrAnchorScreen(const wxr::Packet& p)
{
    const XrQuaternionf q{ p.hq[0], p.hq[1], p.hq[2], p.hq[3] };
    const XrVector3f f = wxr::QRot(q, { 0, 0, -1 });
    float fx = f.x, fz = f.z;
    const float l = sqrtf(fx * fx + fz * fz);
    if (l < 0.1f) { fx = 0; fz = -1; } else { fx /= l; fz /= l; }
    g_wxrAnchor.cx = p.hp[0] + fx * kWxrScreenDistance;
    g_wxrAnchor.cy = p.hp[1];
    g_wxrAnchor.cz = p.hp[2] + fz * kWxrScreenDistance;
    g_wxrAnchor.rx = -fz; g_wxrAnchor.rz = fx;            // the screen's right, horizontal
    g_wxrAnchor.valid = true;
    g_wxrAnchor.recenter = g_motionRecenterSerial;
}

// False when the frame could not be put on the screen; the caller then shows it head-locked.
// onlyEye < 0 draws both halves (side by side); 0 or 1 draws that eye over the whole frame,
// for alternate-eye presentation at full resolution.
static bool WxrDrawVirtualScreen(IDirect3DDevice9* dev, IDirect3DSurface9* bb, const D3DSURFACE_DESC& d,
                                 const wxr::Packet& p, float tanX, float tanY, int onlyEye)
{
    if (!p.valid) return false;
    if (!g_wxrScreenTex || g_wxrScreenW != d.Width || g_wxrScreenH != d.Height) {
        if (g_wxrScreenTex) { g_wxrScreenTex->Release(); g_wxrScreenTex = nullptr; }
        if (FAILED(dev->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT,
                                      &g_wxrScreenTex, nullptr))) { g_wxrScreenTex = nullptr; return false; }
        g_wxrScreenW = d.Width; g_wxrScreenH = d.Height;
    }
    IDirect3DSurface9* copy = nullptr;
    if (FAILED(g_wxrScreenTex->GetSurfaceLevel(0, &copy))) return false;
    const HRESULT hr = g_origStretchRect ? g_origStretchRect(dev, bb, nullptr, copy, nullptr, D3DTEXF_NONE)
                                         : dev->StretchRect(bb, nullptr, copy, nullptr, D3DTEXF_NONE);
    copy->Release();
    if (FAILED(hr)) return false;
    dev->ColorFill(bb, nullptr, D3DCOLOR_XRGB(0, 0, 0));
    if (!g_wxrAnchor.valid || g_wxrAnchor.recenter != g_motionRecenterSerial) WxrAnchorScreen(p);
    if (!g_wxrMenuSB && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_wxrMenuSB))) { g_wxrMenuSB = nullptr; return true; }
    if (FAILED(g_wxrMenuSB->Capture())) return true;

    IDirect3DSurface9* oldRT = nullptr;
    dev->GetRenderTarget(0, &oldRT);
    dev->SetRenderTarget(0, bb);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetTexture(0, g_wxrScreenTex);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);

    // A grid, so a screen partly behind an eye loses only the cells that are, and the rest keeps
    // perspective-correct texturing through rhw. Fine across, for the curve; coarse down, where
    // the cylinder is straight.
    const int kCols = 32, kRows = 4;
    const float sw = kWxrScreenWidth, sh = kWxrScreenWidth * d.Height / d.Width;
    const XrQuaternionf hq{ p.hq[0], p.hq[1], p.hq[2], p.hq[3] };
    const XrQuaternionf inv{ -hq.x, -hq.y, -hq.z, hq.w };
    const float ipd = wxr::SafeIpd(p), eyeW = onlyEye < 0 ? d.Width * 0.5f : (float)d.Width;
    for (int eye = 0; eye < 2; ++eye) {
        if (onlyEye >= 0 && eye != onlyEye) continue;
        const float left = onlyEye < 0 ? eye * eyeW : 0.0f;
        const XrVector3f eo = wxr::QRot(hq, { (eye ? 0.5f : -0.5f) * ipd, 0, 0 });
        const float ex = p.hp[0] + eo.x, ey = p.hp[1] + eo.y, ez = p.hp[2] + eo.z;
        float gx[kRows + 1][kCols + 1], gy[kRows + 1][kCols + 1], gw[kRows + 1][kCols + 1];
        // Cylinder axis: the head position the screen was anchored from, kWxrScreenDistance back
        // from its centre. Forward is the right vector turned a quarter about up.
        const float fx = g_wxrAnchor.rz, fz = -g_wxrAnchor.rx;
        const float ax = g_wxrAnchor.cx - fx * kWxrScreenDistance, az = g_wxrAnchor.cz - fz * kWxrScreenDistance;
        for (int j = 0; j <= kRows; ++j)
            for (int i = 0; i <= kCols; ++i) {
                const float u = (float)i / kCols - 0.5f, v = 0.5f - (float)j / kRows;
                const float theta = u * sw / kWxrScreenDistance;     // arc length = screen width
                const float cs = cosf(theta) * kWxrScreenDistance, sn = sinf(theta) * kWxrScreenDistance;
                const XrVector3f w{ ax + fx * cs + g_wxrAnchor.rx * sn - ex,
                                    g_wxrAnchor.cy + v * sh - ey,
                                    az + fz * cs + g_wxrAnchor.rz * sn - ez };
                const XrVector3f l = wxr::QRot(inv, w);
                const float depth = -l.z;
                gw[j][i] = depth;
                if (depth > 0.05f) {
                    gx[j][i] = left + (0.5f + 0.5f * l.x / (depth * tanX)) * eyeW - 0.5f;
                    gy[j][i] = (0.5f - 0.5f * l.y / (depth * tanY)) * d.Height - 0.5f;
                }
            }
        static WxrMenuVertex tris[kCols * kRows * 6];
        int n = 0;
        for (int j = 0; j < kRows; ++j)
            for (int i = 0; i < kCols; ++i) {
                if (gw[j][i] <= 0.05f || gw[j][i + 1] <= 0.05f || gw[j + 1][i] <= 0.05f || gw[j + 1][i + 1] <= 0.05f) continue;
                const int ci[6][2] = { {j, i}, {j, i + 1}, {j + 1, i + 1}, {j, i}, {j + 1, i + 1}, {j + 1, i} };
                for (const auto& c : ci)
                    tris[n++] = { gx[c[0]][c[1]], gy[c[0]][c[1]], 0.5f, 1.0f / gw[c[0]][c[1]],
                                  (float)c[1] / kCols, (float)c[0] / kRows };
            }
        if (!n) continue;
        const RECT scissor = { (LONG)left, 0, (LONG)(left + eyeW), (LONG)d.Height };
        dev->SetScissorRect(&scissor);
        D3DVIEWPORT9 vp{ 0, 0, d.Width, d.Height, 0.0f, 1.0f };
        dev->SetViewport(&vp);
        if (g_origDrawPrimUP) g_origDrawPrimUP(dev, D3DPT_TRIANGLELIST, n / 3, tris, sizeof(WxrMenuVertex));
        else dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, n / 3, tris, sizeof(WxrMenuVertex));
    }
    dev->SetTexture(0, nullptr);
    g_wxrMenuSB->Apply();
    if (oldRT) { dev->SetRenderTarget(0, oldRT); oldRT->Release(); }
    return true;
}

// ---- 2D while the player is not in control ----
//
// Menus, the pause screen, cutscenes and scripted cameras go to the 2D screen; stereo is only for
// the player's own view. Not in control: no player pawn, the camera's view target is not the
// pawn (cutscenes use a CameraActor), or the game has stayed paused (not by the VR panel).
//
// bCinematicMode is deliberately NOT a reason: the tutorial toggles it during ordinary play and
// the headset flashed into 2D for a second or two at a time. First-person scripted moments keep
// the player's own view, which is comfortable in stereo. A pause must also last a moment, so a
// brief engine pause does not flash the screen either.
static const char* WxrNotInControlReason()
{
    if (!g_playerCtl || !g_playerPawn) return "no player pawn";
    static int pausedRun = 0;
    pausedRun = (!g_menuOpen && MenuPaused() == 1) ? pausedRun + 1 : 0;
    if (pausedRun > 30) return "game paused";
    if (g_offCtlCamera >= 0 && g_offCamViewTarget >= 0) {
        uint32_t cam = 0, target = 0;
        if (SafeU32(g_playerCtl + g_offCtlCamera, &cam) && cam >= 0x10000 &&
            SafeU32(cam + g_offCamViewTarget, &target) && target >= 0x10000 &&
            target != (uint32_t)g_playerPawn) return "camera on a cutscene or scripted view";
    }
    return nullptr;
}

// Enters 2D at once, leaves it only after control has been stable for half a second, so a
// one-frame camera handover at the end of a cutscene does not flash stereo.
static void WxrUpdateFlatMode(long frame)
{
    static int inControlRun = 0;
    static bool stereoWasOn = false;
    const char* reason = WxrNotInControlReason();
    const bool control = reason == nullptr;
    inControlRun = control ? inControlRun + 1 : 0;
    const bool flat = !control || inControlRun < 36;
    if (flat != g_wxrFlat) {
        g_wxrFlat = flat;
        Log("*** [wxr] %s%s at frame %ld", flat ? "2D screen: " : "stereo: the player is in control",
            flat ? (reason ? reason : "settling after a handover") : "", frame);
    }
    if (g_wxrFlat && g_stereoMode == 1) { stereoWasOn = true; g_stereoMode = 0; g_eyeInject = 0.0f; g_renderedEye = -1; }
    else if (!g_wxrFlat && stereoWasOn) { stereoWasOn = false; g_stereoMode = 1; }
}

#include "winlatorxr_cpu.inl"

// ---- which head pose a frame was really drawn with ----
//
// Head yaw reaches the picture indirectly: the mod writes it into the player controller at
// Present, the game thread builds its camera from it, and the render thread draws that camera
// one or more Presents later - a lag Box64 does not keep constant. The red sync stamp must name
// the pose the picture was drawn with, or WinlatorXR reprojects by the wrong amount and the
// picture doubles while the head turns (reported 2026-10-13: left eye only, on head turns).
// Each Present records the sync it latched and the controller yaw it left; each alternate-eye
// frame's drawn camera yaw (from the view matrix, see the constant hook) is matched against
// them by yaw CHANGE (offsets between controller and camera cancel), per eye, and the stamp
// uses the best-matching lag. WinlatorXrPoseLag = 0..3 forces a lag instead.
struct WxrPoseRec { int sync; int32_t yaw; bool valid; };
static WxrPoseRec g_wxrPoseRing[8];
static long g_wxrPosePushes = 0;
static double g_wxrLagErr[2][4][2];          // eye, lag, sign(+/-) - moving mean |dCam - s*dCtl|
static long g_wxrLagSamples[2] = { 0, 0 };
static int  g_wxrLagChosen[2] = { 0, 0 };

static int g_wxrLagSign[2] = { 1, 1 };

// Called on the render thread at the first eye-offset injection of a frame; the ring is only
// written at Present on the same thread, so it is stable here.
static float WxrYawCorrectionForFrame(int eye)
{
    if (!g_wxrYawCorrection || eye < 0 || eye > 1 || g_wxrPosePushes < 6 || g_wxrLagSamples[eye] < 40)
        return 0.0f;
    const int lag = g_wxrLagChosen[eye];
    if (lag <= 0) return 0.0f;
    const WxrPoseRec& now = g_wxrPoseRing[(g_wxrPosePushes - 1) & 7];
    const WxrPoseRec& used = g_wxrPoseRing[(g_wxrPosePushes - 1 - lag) & 7];
    if (!now.valid || !used.valid) return 0.0f;
    float d = (float)(now.yaw - used.yaw);
    while (d > 32768.0f) d -= 65536.0f;
    while (d < -32768.0f) d += 65536.0f;
    float rad = d * (3.14159265f / 32768.0f) * (float)g_wxrLagSign[eye];
    const float kMax = 20.0f * (3.14159265f / 180.0f);
    if (rad > kMax) rad = kMax;
    if (rad < -kMax) rad = -kMax;
    return rad;
}

static int WxrWrapYaw(float d) { while (d > 32768.0f) d -= 65536.0f; while (d < -32768.0f) d += 65536.0f; return (int)d; }

static void WxrPushPose(int sync)
{
    WxrPoseRec r{ sync, 0, false };
    int32_t rot[3];
    if (g_lastPlayerController && g_offActorRotation >= 0 &&
        SafeRead(g_lastPlayerController + g_offActorRotation, rot, sizeof(rot))) { r.yaw = rot[1]; r.valid = true; }
    g_wxrPoseRing[g_wxrPosePushes & 7] = r;
    ++g_wxrPosePushes;
}

// Returns the sync to stamp on an alternate-eye frame of `eye`, updating the measurement.
static int WxrStampSync(int eye, int newestSync, long frame)
{
    static float prevCam = 0.0f;
    static long prevPushes = -10;
    const bool haveCam = g_wxrDrawnYawSet;
    const float cam = g_wxrDrawnYaw;
    g_wxrDrawnYawSet = false;
    if (eye < 0 || eye > 1 || g_wxrPosePushes < 6) return newestSync;

    auto rec = [](int lag) -> const WxrPoseRec& { return g_wxrPoseRing[(g_wxrPosePushes - 1 - lag) & 7]; };
    if (haveCam && prevPushes == g_wxrPosePushes - 1) {
        const int dCam = WxrWrapYaw(cam - prevCam);
        float dCtl[4]; bool ok = true;
        for (int L = 0; L < 4; ++L) {
            const WxrPoseRec& a = rec(L); const WxrPoseRec& b = rec(L + 1);
            if (!a.valid || !b.valid) { ok = false; break; }
            dCtl[L] = (float)WxrWrapYaw((float)(a.yaw - b.yaw));
        }
        // Only while turning: at rest every lag matches.
        if (ok && (abs(dCam) > 40 || fabsf(dCtl[0]) > 40 || fabsf(dCtl[1]) > 40) && abs(dCam) < 6000) {
            for (int L = 0; L < 4; ++L)
                for (int s = 0; s < 2; ++s) {
                    const double e = fabs((double)dCam - (s ? -1.0 : 1.0) * dCtl[L]);
                    double& m = g_wxrLagErr[eye][L][s];
                    m = g_wxrLagSamples[eye] ? m * 0.97 + e * 0.03 : e;
                }
            ++g_wxrLagSamples[eye];
            if (g_wxrLagSamples[eye] >= 40) {
                double best = 1e30; int bestL = 0, bestS = 0;
                for (int L = 0; L < 4; ++L)
                    for (int s = 0; s < 2; ++s)
                        if (g_wxrLagErr[eye][L][s] < best) { best = g_wxrLagErr[eye][L][s]; bestL = L; bestS = s; }
                g_wxrLagChosen[eye] = bestL;
                g_wxrLagSign[eye] = bestS ? -1 : 1;
            }
        }
    }
    prevCam = cam; prevPushes = haveCam ? g_wxrPosePushes : -10;

    if ((frame % 600) == 0) {
        for (int e = 0; e < 2; ++e) {
            auto m = [&](int L) { return (std::min)(g_wxrLagErr[e][L][0], g_wxrLagErr[e][L][1]); };
            Log("[wxr] pose lag %s eye: error lag0 %.0f lag1 %.0f lag2 %.0f lag3 %.0f UU over %ld turning frames"
                " -> drawn %d frame(s) late, %s; stamp lag %d", e ? "RIGHT" : "LEFT", m(0), m(1), m(2), m(3),
                g_wxrLagSamples[e], g_wxrLagChosen[e],
                g_wxrYawCorrection ? (g_wxrLagChosen[e] > 0 ? "yaw corrected" : "no correction needed") : "correction off",
                g_wxrPoseLag >= 0 ? g_wxrPoseLag : g_wxrLagChosen[e]);
        }
    }
    const int lag = g_wxrPoseLag >= 0 ? g_wxrPoseLag : g_wxrLagChosen[eye];
    const WxrPoseRec& r = rec(lag);
    return lag == 0 ? newestSync : r.sync;
}

static void WxrSubmitFrame(IDirect3DDevice9* dev)
{
    if (!g_xrRunning) return;
    const long n = InterlockedIncrement(&g_xrFrames);
    if (n == 1) {
        if (const char* v = getenv("MEVR_WINLATORXR_SYNC_WAIT_MS")) g_wxrSyncWaitMs = (DWORD)atoi(v);
        Log("*** [wxr] first frame; waiting up to %lu ms per frame for a fresh pose",
            (unsigned long)g_wxrSyncWaitMs);
    }
    WxrKeepWindowPinned(dev, n);
    WxrCpuTick(n);
    WxrUpdateFlatMode(n);

    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC d{};
    if (FAILED(bb->GetDesc(&d))) { bb->Release(); return; }
    // The frame needs no capture here, but everything downstream sizes itself from these.
    g_capW = d.Width; g_capH = d.Height;
    g_haveFrame = true;

    // ---- 1. what this frame is ----
    // As in SubmitTestQuad: a frame whose draws were not duplicated is mono whatever the mode says,
    // and it is held mono for a few frames so the presentation does not flap at a transition.
    const LONG dupThisFrame = InterlockedExchange(&g_dupFrameDraws, 0);
    // The eye this frame's scene was really drawn for (see the constant hook), not the eye the
    // last Present predicted.
    const LONG drawnL = InterlockedExchange(&g_wxrDrawnLeft, 0), drawnR = InterlockedExchange(&g_wxrDrawnRight, 0);
    int drawnEye = g_renderedEye;
    if (drawnL || drawnR) drawnEye = drawnL >= drawnR ? 0 : 1;
    {
        static long mismatched = 0, mixed = 0, counted = 0, uncounted = 0;
        if (drawnL || drawnR) {
            ++counted;
            if (drawnL && drawnR) ++mixed;
            if (g_renderedEye >= 0 && drawnEye != g_renderedEye) ++mismatched;
            if ((counted % 1200) == 0)
                Log("[wxr] alternate-eye: %ld of %ld frames were drawn for the other eye than predicted,"
                    " %ld mixed both; %ld stereo frames had no counted draw and used the prediction",
                    mismatched, counted, mixed, uncounted);
        } else if (g_stereoMode == 1 && !g_simulStereo && g_renderedEye >= 0) {
            ++uncounted;
        }
    }
    static int monoRun = 0;
    if (dupThisFrame >= 4) monoRun = 0; else if (monoRun < 1000) ++monoRun;
    const bool frameIsStereo = monoRun < 10;
    const bool sceneFillsFrame = SceneW() == d.Width && SceneH() == d.Height;
    int layout = 0;                                     // 0 flat, 1 side by side, 2 alternate-eye
    if (g_stereoMode == 1 && g_viewsValid) {
        if (g_simulStereo && frameIsStereo && sceneFillsFrame) layout = 1;
        else if (!g_simulStereo && drawnEye >= 0) layout = 2;
    }

    // The FOV the picture was rendered with - the forced frustum, under the same condition the
    // desktop path submits it under - or 0, which keeps the headset's own.
    float fovX = 0, fovY = 0;
    if (layout && g_fovForce && g_gameFovValid && g_targetHalfFovX > 0.0f) {
        fovX = g_targetHalfFovX * 114.5916f; fovY = g_targetHalfFovY * 114.5916f;
    } else if (layout && g_gameFovValid) {
        fovX = g_gameHalfFovX * 114.5916f; fovY = g_gameHalfFovY * 114.5916f;
    }
    const wxr::Packet drawn = wxr::Frame();             // the pose this picture was drawn with
    {
        static bool tracked = false;
        if (drawn.valid != tracked) {
            tracked = drawn.valid;
            if (tracked) Log("*** [wxr] tracking arrived at frame %ld: FOV %.1f x %.1f deg, IPD %.1f mm",
                             n, drawn.fovX, drawn.fovY, drawn.ipd * 1000.0f);
        }
    }
    const float tanX = tanf(0.5f * 0.01745329252f * (fovX > 1 ? fovX : (drawn.fovX > 1 ? drawn.fovX : 100)));
    const float tanY = tanf(0.5f * 0.01745329252f * (fovY > 1 ? fovY : (drawn.fovY > 1 ? drawn.fovY : 100)));

    // ---- 2. the settings panel ----
    // A flat frame is shown by WinlatorXR on a screen in front of the player; the panel then sits
    // centred on that picture, which is where a head-locked quad would appear as well.
    // The 2D screen goes out alternate-eye: each eye gets the whole window, twice the horizontal
    // detail of a side-by-side half, and a still picture loses nothing at half the refresh.
    static int flatEye = 0;
    bool roomScreen = false;
    // Default (WinlatorXrMenuScreen = flat): the game's own frame goes untouched to WinlatorXR's
    // 2D screen (MODE_VR 2, FOV 0 0) - the full window resolution, the sharpest way to show text.
    if (layout == 0 && g_wxrRoomScreen) { flatEye ^= 1; roomScreen = WxrDrawVirtualScreen(dev, bb, d, drawn, tanX, tanY, flatEye); }
    else g_wxrAnchor.valid = false;          // the next screen anchors where the player looks then
    // On WinlatorXR's 2D screen the whole X screen is shown. The 16:9 menu frame stretched over a
    // taller window would look squashed, so it is presented 1:1 (or fitted), centred, black bars.
    g_wxrPresentDstValid = false;
    if (layout == 0 && !roomScreen && g_wxrWindow) {
        RECT cr{};
        if (GetClientRect(g_wxrWindow, &cr) && cr.right > 0 && cr.bottom > 0) {
            const float cw = (float)cr.right, ch = (float)cr.bottom;
            if (fabsf(cw / ch - (float)d.Width / d.Height) > 0.01f * cw / ch) {
                const float s = (std::min)(cw / d.Width, ch / d.Height);
                const LONG w = (LONG)(d.Width * s + 0.5f), h = (LONG)(d.Height * s + 0.5f);
                g_wxrPresentDst.left = (cr.right - w) / 2; g_wxrPresentDst.top = (cr.bottom - h) / 2;
                g_wxrPresentDst.right = g_wxrPresentDst.left + w; g_wxrPresentDst.bottom = g_wxrPresentDst.top + h;
                g_wxrPresentDstValid = true;
                static LONG loggedW = 0, loggedH = 0;
                if (loggedW != cr.right || loggedH != cr.bottom) {
                    loggedW = cr.right; loggedH = cr.bottom;
                    Log("[wxr] 2D screen: %ux%u frame shown centred at %ld,%ld %ldx%ld in the %ldx%ld window",
                        d.Width, d.Height, g_wxrPresentDst.left, g_wxrPresentDst.top, w, h, cr.right, cr.bottom);
                }
            }
        }
    }
    const int stampEye = roomScreen ? flatEye : drawnEye;
    const int stampSync = layout == 2 ? WxrStampSync(drawnEye, drawn.sync, n) : drawn.sync;
    if (layout != 2) g_wxrDrawnYawSet = false;
    g_wxrYawCorrRad = 0.0f;                  // the next frame's first injection sets its own
    WxrDrawMenu(dev, bb, d, roomScreen ? 2 : layout, stampEye, wxr::SafeIpd(drawn), tanX, tanY);

    // ---- 3. the stamp, last, so nothing drawn above can cover it ----
    // A 10x10 block rather than one pixel survives any scaling between backbuffer and X screen.
    const RECT syncRect = { 0, 0, 10, 10 };
    const HRESULT shr = dev->ColorFill(bb, &syncRect, D3DCOLOR_XRGB(stampSync & 0xFF, 0,
                                       ((layout == 2 || roomScreen) && stampEye == 1) ? 255 : 0));
    // An odd interval, so alternate-eye dumps land on both eyes.
    if ((n % 29) == 0) WxrServeFrameDump(dev, bb, d);
    bb->Release();

    // ---- 4. how to show it ----
    if (layout == 1)      wxr::Send(0, 0, 1, 1, fovX, fovY);
    else if (layout == 2) wxr::Send(0, 0, 1, 2, fovX, fovY);
    else if (roomScreen)  wxr::Send(0, 0, 1, 2, drawn.fovX, drawn.fovY);
    else                  wxr::Send(0, 0, 2, 0, 0, 0);

    {
        static int lastLayout = -1;
        static long changes = 0;
        if (layout != lastLayout) {
            lastLayout = layout;
            if (++changes <= 20 || (changes % 50) == 0)
                Log("*** [wxr] PRESENTING -> %s (change #%ld; stereoMode %d viewsValid %d dup %ld"
                    " simul %d eye %d scene %ux%u frame %ux%u, fov %.1f x %.1f, stamp %s)",
                    layout == 1 ? "side-by-side stereo" : layout == 2 ? "alternate-eye stereo"
                                : roomScreen ? "room-fixed virtual screen" : "WinlatorXR 2D screen",
                    changes, g_stereoMode, g_viewsValid ? 1 : 0, (long)dupThisFrame,
                    g_simulStereo ? 1 : 0, g_renderedEye, SceneW(), SceneH(), d.Width, d.Height,
                    fovX, fovY, SUCCEEDED(shr) ? "ok" : "FAILED");
            if (g_stereoMode == 1 && g_simulStereo && frameIsStereo && !sceneFillsFrame)
                Log("[wxr] the engine letterboxed the scene into %ux%u of %ux%u - side by side needs"
                    " the game resolution to equal Winlator's screen size, 16:9", SceneW(), SceneH(),
                    d.Width, d.Height);
        }
    }

    {
        static int lastMode = -2;
        const int mode = drawn.hasMode ? (drawn.immersive ? 2 : 0) + (drawn.sideBySide ? 1 : 0) : -1;
        if (mode != lastMode) {
            lastMode = mode;
            if (mode < 0) Log("[wxr] WinlatorXR reports no display-mode flags");
            else Log("*** [wxr] WinlatorXR reports immersive %s, side-by-side %s%s", drawn.immersive ? "ON" : "OFF",
                     drawn.sideBySide ? "ON" : "OFF",
                     drawn.immersive ? "" : " - the headset shows the window flat; WinlatorXR's VR toggle is off");
        }
    }

    // ---- 5. the next pose ----
    wxr::LatchFrame(g_wxrSyncWaitMs);
    const XrTime now = wxr::NowNs();
    static XrTime prevNow = 0;
    static double periodNs = 1.0e9 / 72.0;
    if (prevNow && now > prevNow) periodNs = periodNs * 0.95 + (double)(now - prevNow) * 0.05;
    prevNow = now;

    g_viewsValid = false;
    if (g_stereoMode == 1) AdoptLocatedViews(now);

    ApplyHeadTracking(now);
    WxrPushPose(wxr::Frame().sync);          // the pose just latched and the yaw it left behind
    const bool actionsSynced = XrSyncInput(now);
    XrSampleMotionPoses(now, actionsSynced);
    PublishP13PoseSnapshot();
    UpdateMotionRigDiscovery();
    TickPacing();
    ReportStereoGeometry();
    g_predTime = now;
    g_predPeriod = (XrDuration)periodNs;
    g_submittedLayerKind = layout ? 1 : 0;

    // The eye the next frame renders, exactly as the desktop loop chooses it.
    if (g_stereoMode == 1 && !g_simulStereo) {
        g_nextEye = 1 - g_nextEye;
        g_eyeInject = (g_nextEye == 0) ? -1.0f : +1.0f;
        // The eye injected now is the eye of the frame the render thread draws next: the
        // per-draw counter in the constant hook measured 100% agreement with THIS, and 100%
        // disagreement with the desktop's "previous g_nextEye". The old value was only the
        // fallback for frames with no counted world draw (~15% in game), and sent each of
        // them to the wrong eye - the one-eye ghosting reported on 2026-10-13.
        g_renderedEye = g_nextEye;
    } else {
        g_renderedEye = -1;
        g_eyeInject = 0.0f;
    }

    if ((n % 600) == 0) {
        AcquireSRWLockShared(&wxr::g_lock);
        const long packets = wxr::g_packets, rejected = wxr::g_rejected;
        char bad[160];
        strcpy_s(bad, wxr::g_lastBad);
        ReleaseSRWLockShared(&wxr::g_lock);
        Log("[wxr] frame %ld: %.1f fps, %ld packets (%ld rejected%s%s), sync %d, %s",
            n, 1.0e9 / periodNs, packets, rejected, rejected ? ", first: " : "", rejected ? bad : "",
            drawn.sync, layout == 1 ? "side-by-side" : layout == 2 ? "alternate-eye" : "flat");
        const LONG reads = InterlockedExchange(&g_safeReadCalls, 0), faults = InterlockedExchange(&g_safeReadFaults, 0);
        const LONG queries = InterlockedExchange(&g_readCacheQueries, 0);
        Log("[perf] memory reads: %ld per frame (%s), %ld faulted, %ld page checks over 600 frames",
            reads / 600, g_readCache ? "page-checked direct" : g_fastSafeRead ? "direct" : "ReadProcessMemory",
            faults, queries);
        const LONG writes = InterlockedExchange(&g_selfWriteCalls, 0), fallbacks = InterlockedExchange(&g_selfWriteFallbacks, 0);
        Log("[perf] memory writes: %ld per frame (%s), %ld needed WriteProcessMemory over 600 frames",
            writes / 600, g_fastSelfWrite ? "direct" : "WriteProcessMemory", fallbacks);
        // What each call costs on this machine, 1000 of each, on the render thread.
        LARGE_INTEGER qf, t0, t1, t2, t3;
        QueryPerformanceFrequency(&qf);
        uint32_t probe = 0;
        const uintptr_t at = (uintptr_t)&g_xrFrames;
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < 1000; ++i) { SIZE_T got; ReadProcessMemory(GetCurrentProcess(), (LPCVOID)at, &probe, 4, &got); }
        QueryPerformanceCounter(&t1);
        for (int i = 0; i < 1000; ++i) SafeReadFast(at, &probe, 4);
        QueryPerformanceCounter(&t2);
        for (int i = 0; i < 1000; ++i) GetAsyncKeyState(VK_F1 + (i & 7));
        QueryPerformanceCounter(&t3);
        LARGE_INTEGER t4, t5;
        static volatile uint32_t sink = 0;
        for (int i = 0; i < 1000; ++i) { uint32_t v = (uint32_t)i; SIZE_T w; WriteProcessMemory(GetCurrentProcess(), (LPVOID)&sink, &v, 4, &w); }
        QueryPerformanceCounter(&t4);
        for (int i = 0; i < 1000; ++i) (void)NowMs();
        QueryPerformanceCounter(&t5);
        LARGE_INTEGER t6, t7;
        MEMORY_BASIC_INFORMATION qm;
        for (int i = 0; i < 1000; ++i) VirtualQuery((LPCVOID)at, &qm, sizeof(qm));
        QueryPerformanceCounter(&t6);
        for (int i = 0; i < 1000; ++i) SafeReadCached(at, &probe, 4);
        QueryPerformanceCounter(&t7);
        const double us = 1.0e6 / (double)qf.QuadPart / 1000.0;
        Log("[perf] per call: ReadProcessMemory %.2f us, direct read %.3f us, GetAsyncKeyState %.2f us,"
            " WriteProcessMemory %.2f us, NowMs %.2f us, VirtualQuery %.2f us, cached read %.3f us",
            (t1.QuadPart - t0.QuadPart) * us, (t2.QuadPart - t1.QuadPart) * us, (t3.QuadPart - t2.QuadPart) * us,
            (t4.QuadPart - t3.QuadPart) * us, (t5.QuadPart - t4.QuadPart) * us,
            (t6.QuadPart - t5.QuadPart) * us, (t7.QuadPart - t6.QuadPart) * us);
    }
}
