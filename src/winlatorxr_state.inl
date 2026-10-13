// WinlatorXR: redundant D3D9 state filter and cheap current-shader tracking.
//
// Headset profile 2026-10-13, in game, per frame on the render thread: SetSamplerState 12,600
// calls / 7.0 ms, SetRenderState 6,500 / 4.0 ms, SetTexture 2,100 / 1.7 ms - every one a call
// into DXVK, which is x86 code under Box64 (~0.5 us each), for only ~670 draws. UE3 re-sends
// state it has already set. This layer sits on the device vtable in front of DXVK, remembers
// what was last set, and drops calls that would change nothing.
//
// It also tracks the bound vertex and pixel shader, so the mod's per-draw shader checks no
// longer ask DXVK (GetVertexShader / GetPixelShader + Release) several times per draw.
//
// Correctness: every cached value is forgotten (and re-learned from the next real call, or for
// shaders from one Get call) whenever anything could change device state behind its back:
// Reset, state-block recording (Begin/EndStateBlock, during which nothing is filtered), and
// IDirect3DStateBlock9::Apply, which is hooked on the state-block vtable the first time a
// state block is created. WinlatorXrStateFilter = off removes the filter entirely.

static bool g_wxrStateFilter = true;
// WinlatorXrStateFilterMask: 1 render states, 2 sampler states, 4 textures (default 7). Lets the
// three parts be tested one at a time on the headset.
static int  g_wxrStateFilterMask = 7;
// The cache is only valid for one thread's view of the device. Headset 2026-10-13: with the
// filter on, the game froze while loading the main menu (fine on desktop); the suspected cause
// is device calls from a second thread (loading movie). Only the thread that presents is
// filtered; any other thread's calls pass through untouched and wipe the cache, and are logged.
static volatile DWORD g_wxrSCThread = 0;
static volatile LONG  g_wxrSCForeign = 0;
static volatile DWORD g_wxrSCForeignTid = 0;

struct WxrStateCache {
    DWORD rs[256];        bool rsKnown[256];
    DWORD samp[20][16];   bool sampKnown[20][16];
    IDirect3DBaseTexture9* tex[20]; bool texKnown[20];
    IDirect3DVertexShader9* vs; bool vsKnown;
    IDirect3DPixelShader9*  ps; bool psKnown;
    bool recording;
    long skipped[3], passed[3];   // render state, sampler, texture
};
static WxrStateCache g_wxrSC{};

static void WxrStateInvalidate()
{
    memset(g_wxrSC.rsKnown, 0, sizeof(g_wxrSC.rsKnown));
    memset(g_wxrSC.sampKnown, 0, sizeof(g_wxrSC.sampKnown));
    memset(g_wxrSC.texKnown, 0, sizeof(g_wxrSC.texKnown));
    g_wxrSC.vsKnown = g_wxrSC.psKnown = false;
}

// True when this call may use the cache. A call from any other thread invalidates it.
static inline bool WxrSCMine()
{
    const DWORD me = GetCurrentThreadId();
    if (me == g_wxrSCThread) return true;
    if (g_wxrSCThread) { InterlockedIncrement(&g_wxrSCForeign); g_wxrSCForeignTid = me; }
    WxrStateInvalidate();
    return false;
}

static inline int WxrSamplerIndex(DWORD stage)
{
    if (stage < 16) return (int)stage;
    if (stage >= D3DVERTEXTEXTURESAMPLER0 && stage <= D3DVERTEXTEXTURESAMPLER3) return 16 + (int)(stage - D3DVERTEXTEXTURESAMPLER0);
    return -1;
}

typedef HRESULT (STDMETHODCALLTYPE* PFN_SC_SetRS)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT (STDMETHODCALLTYPE* PFN_SC_SetSamp)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
typedef HRESULT (STDMETHODCALLTYPE* PFN_SC_SetTex)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_SC_SetVS)(IDirect3DDevice9*, IDirect3DVertexShader9*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_SC_SetPS)(IDirect3DDevice9*, IDirect3DPixelShader9*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_SC_Reset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_SC_CreateSB)(IDirect3DDevice9*, D3DSTATEBLOCKTYPE, IDirect3DStateBlock9**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_SC_BeginSB)(IDirect3DDevice9*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_SC_EndSB)(IDirect3DDevice9*, IDirect3DStateBlock9**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_SC_SBApply)(IDirect3DStateBlock9*);
static PFN_SC_SetRS   g_scSetRS = nullptr;
static PFN_SC_SetSamp g_scSetSamp = nullptr;
static PFN_SC_SetTex  g_scSetTex = nullptr;
static PFN_SC_SetVS   g_scSetVS = nullptr;
static PFN_SC_SetPS   g_scSetPS = nullptr;
static PFN_SC_Reset   g_scReset = nullptr;
static PFN_SC_CreateSB g_scCreateSB = nullptr;
static PFN_SC_BeginSB g_scBeginSB = nullptr;
static PFN_SC_EndSB   g_scEndSB = nullptr;
static PFN_SC_SBApply g_scSBApply = nullptr;

static HRESULT STDMETHODCALLTYPE WxrSC_SetRS(IDirect3DDevice9* d, D3DRENDERSTATETYPE s, DWORD v)
{
    if ((g_wxrStateFilterMask & 1) && WxrSCMine() && !g_wxrSC.recording && (unsigned)s < 256) {
        if (g_wxrSC.rsKnown[s] && g_wxrSC.rs[s] == v) { ++g_wxrSC.skipped[0]; return D3D_OK; }
        const HRESULT hr = g_scSetRS(d, s, v);
        if (SUCCEEDED(hr)) { g_wxrSC.rs[s] = v; g_wxrSC.rsKnown[s] = true; }
        ++g_wxrSC.passed[0];
        return hr;
    }
    return g_scSetRS(d, s, v);
}
static HRESULT STDMETHODCALLTYPE WxrSC_SetSamp(IDirect3DDevice9* d, DWORD stage, D3DSAMPLERSTATETYPE t, DWORD v)
{
    const int i = WxrSamplerIndex(stage);
    if ((g_wxrStateFilterMask & 2) && WxrSCMine() && !g_wxrSC.recording && i >= 0 && (unsigned)t < 16) {
        if (g_wxrSC.sampKnown[i][t] && g_wxrSC.samp[i][t] == v) { ++g_wxrSC.skipped[1]; return D3D_OK; }
        const HRESULT hr = g_scSetSamp(d, stage, t, v);
        if (SUCCEEDED(hr)) { g_wxrSC.samp[i][t] = v; g_wxrSC.sampKnown[i][t] = true; }
        ++g_wxrSC.passed[1];
        return hr;
    }
    return g_scSetSamp(d, stage, t, v);
}
static HRESULT STDMETHODCALLTYPE WxrSC_SetTex(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* t)
{
    const int i = WxrSamplerIndex(stage);
    // Skipping a re-bind of the same texture is safe: the device already holds a reference to
    // the bound texture, so it cannot have been freed and its address reused meanwhile.
    if ((g_wxrStateFilterMask & 4) && WxrSCMine() && !g_wxrSC.recording && i >= 0) {
        if (g_wxrSC.texKnown[i] && g_wxrSC.tex[i] == t) { ++g_wxrSC.skipped[2]; return D3D_OK; }
        const HRESULT hr = g_scSetTex(d, stage, t);
        if (SUCCEEDED(hr)) { g_wxrSC.tex[i] = t; g_wxrSC.texKnown[i] = true; } else g_wxrSC.texKnown[i] = false;
        ++g_wxrSC.passed[2];
        return hr;
    }
    return g_scSetTex(d, stage, t);
}
// Shaders are tracked, never filtered.
static HRESULT STDMETHODCALLTYPE WxrSC_SetVS(IDirect3DDevice9* d, IDirect3DVertexShader9* s)
{
    const HRESULT hr = g_scSetVS(d, s);
    if (WxrSCMine() && !g_wxrSC.recording && SUCCEEDED(hr)) { g_wxrSC.vs = s; g_wxrSC.vsKnown = true; } else g_wxrSC.vsKnown = false;
    return hr;
}
static HRESULT STDMETHODCALLTYPE WxrSC_SetPS(IDirect3DDevice9* d, IDirect3DPixelShader9* s)
{
    const HRESULT hr = g_scSetPS(d, s);
    if (WxrSCMine() && !g_wxrSC.recording && SUCCEEDED(hr)) { g_wxrSC.ps = s; g_wxrSC.psKnown = true; } else g_wxrSC.psKnown = false;
    return hr;
}
static HRESULT STDMETHODCALLTYPE WxrSC_SBApply(IDirect3DStateBlock9* sb)
{
    const HRESULT hr = g_scSBApply(sb);
    WxrStateInvalidate();
    return hr;
}
static void WxrSC_PatchStateBlock(IDirect3DStateBlock9* sb)
{
    if (g_scSBApply || !sb) return;
    g_scSBApply = (PFN_SC_SBApply)PatchVTable(sb, 5, (void*)&WxrSC_SBApply);   // QI, AddRef, Release, GetDevice, Capture, Apply
    Log("[state] IDirect3DStateBlock9::Apply hooked - every Apply resets the state filter");
}
static HRESULT STDMETHODCALLTYPE WxrSC_Reset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp)
{
    const HRESULT hr = g_scReset(d, pp);
    WxrStateInvalidate();
    return hr;
}
static HRESULT STDMETHODCALLTYPE WxrSC_CreateSB(IDirect3DDevice9* d, D3DSTATEBLOCKTYPE t, IDirect3DStateBlock9** out)
{
    const HRESULT hr = g_scCreateSB(d, t, out);
    if (SUCCEEDED(hr) && out) WxrSC_PatchStateBlock(*out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WxrSC_BeginSB(IDirect3DDevice9* d)
{
    const HRESULT hr = g_scBeginSB(d);
    if (SUCCEEDED(hr)) { g_wxrSC.recording = true; WxrStateInvalidate(); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE WxrSC_EndSB(IDirect3DDevice9* d, IDirect3DStateBlock9** out)
{
    const HRESULT hr = g_scEndSB(d, out);
    g_wxrSC.recording = false;
    WxrStateInvalidate();
    if (SUCCEEDED(hr) && out) WxrSC_PatchStateBlock(*out);
    return hr;
}

// The bound shaders without asking DXVK when the tracker knows them; no reference is added.
static IDirect3DVertexShader9* WxrCurrentVS(IDirect3DDevice9* dev)
{
    const bool mine = GetCurrentThreadId() == g_wxrSCThread;
    if (g_wxrStateFilter && g_scSetVS && mine && g_wxrSC.vsKnown) return g_wxrSC.vs;
    IDirect3DVertexShader9* s = nullptr;
    if (FAILED(dev->GetVertexShader(&s)) || !s) return nullptr;
    s->Release();                       // the device keeps its own reference while bound
    if (g_wxrStateFilter && g_scSetVS && mine && !g_wxrSC.recording) { g_wxrSC.vs = s; g_wxrSC.vsKnown = true; }
    return s;
}
static IDirect3DPixelShader9* WxrCurrentPS(IDirect3DDevice9* dev)
{
    const bool mine = GetCurrentThreadId() == g_wxrSCThread;
    if (g_wxrStateFilter && g_scSetPS && mine && g_wxrSC.psKnown) return g_wxrSC.ps;
    IDirect3DPixelShader9* s = nullptr;
    if (FAILED(dev->GetPixelShader(&s)) || !s) return nullptr;
    s->Release();
    if (g_wxrStateFilter && g_scSetPS && mine && !g_wxrSC.recording) { g_wxrSC.ps = s; g_wxrSC.psKnown = true; }
    return s;
}

static void WxrInstallStateFilter(IDirect3DDevice9* dev)
{
    if (!g_wxrStateFilter) { Log("[state] redundant-state filter off (WinlatorXrStateFilter = off)"); return; }
    WxrStateInvalidate();
    // The mod already hooks Reset (16); the wrapper goes in front of its hook.
    g_scReset     = (PFN_SC_Reset)   PatchVTable(dev, 16, (void*)&WxrSC_Reset);
    g_scCreateSB  = (PFN_SC_CreateSB)PatchVTable(dev, 59, (void*)&WxrSC_CreateSB);
    g_scBeginSB   = (PFN_SC_BeginSB) PatchVTable(dev, 60, (void*)&WxrSC_BeginSB);
    g_scEndSB     = (PFN_SC_EndSB)   PatchVTable(dev, 61, (void*)&WxrSC_EndSB);
    g_scSetRS     = (PFN_SC_SetRS)   PatchVTable(dev, 57, (void*)&WxrSC_SetRS);
    g_scSetTex    = (PFN_SC_SetTex)  PatchVTable(dev, 65, (void*)&WxrSC_SetTex);
    g_scSetSamp   = (PFN_SC_SetSamp) PatchVTable(dev, 69, (void*)&WxrSC_SetSamp);
    g_scSetVS     = (PFN_SC_SetVS)   PatchVTable(dev, 92, (void*)&WxrSC_SetVS);
    g_scSetPS     = (PFN_SC_SetPS)   PatchVTable(dev, 107, (void*)&WxrSC_SetPS);
    Log("[state] redundant-state filter installed (render, sampler, texture; shader tracking)");
}

// Called from Present: the presenting thread owns the cache. When it changes (the engine
// recreates its rendering thread on a level load) the cache starts over.
static void WxrStateOwnerTick()
{
    const DWORD me = GetCurrentThreadId();
    if (me != g_wxrSCThread) {
        Log("[state] filter now follows the presenting thread %lu (was %lu)", (unsigned long)me, (unsigned long)g_wxrSCThread);
        g_wxrSCThread = me;
        WxrStateInvalidate();
    }
}

static void WxrStateReport(long frames)
{
    if (!g_wxrStateFilter || frames <= 0) return;
    static const char* const kNames[3] = { "render states", "sampler states", "textures" };
    char line[300]; int at = 0;
    for (int i = 0; i < 3; ++i) {
        const long tot = g_wxrSC.skipped[i] + g_wxrSC.passed[i];
        at += sprintf_s(line + at, sizeof(line) - at, "%s%s %ld/f dropped of %ld/f (%.0f%%)", i ? ", " : "",
                        kNames[i], g_wxrSC.skipped[i] / frames, tot / frames, tot ? 100.0 * g_wxrSC.skipped[i] / tot : 0.0);
        g_wxrSC.skipped[i] = g_wxrSC.passed[i] = 0;
    }
    Log("[state] redundant calls kept from DXVK: %s", line);
    const LONG foreign = InterlockedExchange(&g_wxrSCForeign, 0);
    if (foreign) Log("[state] %ld state calls came from another thread (last %lu) - passed through, cache reset",
                     foreign, (unsigned long)g_wxrSCForeignTid);
}
