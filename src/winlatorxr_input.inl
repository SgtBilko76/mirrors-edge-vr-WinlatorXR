// ================================================================ WinlatorXR: gamepad only
//
// WinlatorXR turns the Touch controllers into desktop keyboard and mouse events as well as XrAPI
// state (the CoD4 port had to ignore them too). Reaching the game, those keys fired actions on top
// of the mod's synthesised Xbox 360 pad. Mirror's Edge is built for a pad, so on the headset the
// game is given the pad and nothing else: its keyboard and mouse are silenced on all three routes
// it reads them by -
//
//   - window messages (UE3's keyboard): dropped in the subclassed window procedure
//   - DirectInput 8 (mouse and keyboard devices): the system keyboard and mouse report nothing
//   - GetAsyncKeyState / GetKeyState: "not pressed" when called from the game's own code
//
// The mod's own hotkeys (Page Up recentre, hold Pause to exit) still read the real keyboard.
// WinlatorXrKeyboardMouse = on in mevr.ini turns all of this off.

#include <dinput.h>

// g_wxrBlockKeyboardMouse is defined with the forward declarations in d3d9.cpp.

static uintptr_t g_wxrExeLo = 0, g_wxrExeHi = 0;
static bool WxrCalledFromGame(void* returnAddress)
{
    const uintptr_t a = (uintptr_t)returnAddress;
    return a >= g_wxrExeLo && a < g_wxrExeHi;
}

// Window messages: called from WxrWindowProc before the engine's own procedure.
static bool WxrSwallowInputMessage(UINT msg)
{
    if (!g_wxrBlockKeyboardMouse) return false;
    return (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
           (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
           msg == WM_INPUT || msg == WM_MOUSEHOVER || msg == WM_MOUSELEAVE;
}

// ---- key state, for the game's own callers only ----
static SHORT (WINAPI *g_origGetAsyncKeyState)(int) = nullptr;
static SHORT (WINAPI *g_origGetKeyState)(int) = nullptr;
static SHORT WINAPI WxrGetAsyncKeyState(int key)
{
    if (g_wxrBlockKeyboardMouse && WxrCalledFromGame(_ReturnAddress())) return 0;
    return g_origGetAsyncKeyState(key);
}
static SHORT WINAPI WxrGetKeyState(int key)
{
    if (g_wxrBlockKeyboardMouse && WxrCalledFromGame(_ReturnAddress())) return 0;
    return g_origGetKeyState(key);
}

// ---- DirectInput: the system keyboard and mouse devices go quiet ----
//
// All devices share one vtable, so the hooks look the device up in the short list of the ones
// created for GUID_SysKeyboard / GUID_SysMouse. A joystick, if any, is left alone.
static const GUID kWxrSysKeyboard = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID kWxrSysMouse    = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static void* g_wxrSilenced[8] = {};
static volatile LONG g_wxrSilencedCount = 0;

static bool WxrIsSilenced(void* device)
{
    const LONG n = g_wxrSilencedCount;
    for (LONG i = 0; i < n && i < 8; ++i) if (g_wxrSilenced[i] == device) return true;
    return false;
}

typedef HRESULT (STDMETHODCALLTYPE *PFN_DIGetState)(IDirectInputDevice8W*, DWORD, LPVOID);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DIGetData)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DICreateDevice)(IDirectInput8W*, REFGUID, LPDIRECTINPUTDEVICE8W*, LPUNKNOWN);
typedef HRESULT (WINAPI *PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static PFN_DIGetState         g_origDIGetState = nullptr;
static PFN_DIGetData          g_origDIGetData = nullptr;
static PFN_DICreateDevice     g_origDICreateDevice = nullptr;
static PFN_DirectInput8Create g_origDirectInput8Create = nullptr;

static HRESULT STDMETHODCALLTYPE WxrDIGetState(IDirectInputDevice8W* self, DWORD size, LPVOID data)
{
    const HRESULT hr = g_origDIGetState(self, size, data);
    if (g_wxrBlockKeyboardMouse && SUCCEEDED(hr) && data && WxrIsSilenced(self)) memset(data, 0, size);
    return hr;
}
static HRESULT STDMETHODCALLTYPE WxrDIGetData(IDirectInputDevice8W* self, DWORD size, LPDIDEVICEOBJECTDATA data,
                                              LPDWORD inOut, DWORD flags)
{
    const HRESULT hr = g_origDIGetData(self, size, data, inOut, flags);
    if (g_wxrBlockKeyboardMouse && SUCCEEDED(hr) && inOut && WxrIsSilenced(self)) *inOut = 0;
    return hr;
}
static HRESULT STDMETHODCALLTYPE WxrDICreateDevice(IDirectInput8W* self, REFGUID guid, LPDIRECTINPUTDEVICE8W* out,
                                                   LPUNKNOWN outer)
{
    const HRESULT hr = g_origDICreateDevice(self, guid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        const bool keyboard = IsEqualGUID(guid, kWxrSysKeyboard), mouse = IsEqualGUID(guid, kWxrSysMouse);
        if (keyboard || mouse) {
            const LONG slot = InterlockedIncrement(&g_wxrSilencedCount) - 1;
            if (slot < 8) g_wxrSilenced[slot] = *out;
            if (!g_origDIGetState) {
                // GetDeviceState is slot 9 and GetDeviceData slot 10 of IDirectInputDevice8.
                g_origDIGetState = (PFN_DIGetState)PatchVTable(*out, 9, (void*)&WxrDIGetState);
                g_origDIGetData  = (PFN_DIGetData)PatchVTable(*out, 10, (void*)&WxrDIGetData);
            }
            Log("[wxr] DirectInput %s silenced (gamepad only)", keyboard ? "keyboard" : "mouse");
        }
    }
    return hr;
}
static HRESULT WINAPI WxrDirectInput8Create(HINSTANCE inst, DWORD version, REFIID iid, LPVOID* out, LPUNKNOWN outer)
{
    const HRESULT hr = g_origDirectInput8Create(inst, version, iid, out, outer);
    if (SUCCEEDED(hr) && out && *out && !g_origDICreateDevice)
        g_origDICreateDevice = (PFN_DICreateDevice)PatchVTable(*out, 3, (void*)&WxrDICreateDevice);   // CreateDevice
    return hr;
}

// From DllMain, after the settings are read: everything the game imports is mapped by then.
static void WxrInstallInputBlock()
{
    if (!g_wxrBlockKeyboardMouse) { Log("[wxr] keyboard and mouse reach the game (WinlatorXrKeyboardMouse = on)"); return; }
    const HMODULE exe = GetModuleHandleW(nullptr);
    const auto* dos = (const IMAGE_DOS_HEADER*)exe;
    const auto* nt = (const IMAGE_NT_HEADERS*)((const BYTE*)exe + dos->e_lfanew);
    g_wxrExeLo = (uintptr_t)exe;
    g_wxrExeHi = g_wxrExeLo + nt->OptionalHeader.SizeOfImage;
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) { Log("[wxr] input block: MinHook unavailable"); return; }
    int hooked = 0;
    auto hook = [&](const wchar_t* module, const char* name, void* detour, void** original) {
        HMODULE m = GetModuleHandleW(module);
        void* target = m ? (void*)GetProcAddress(m, name) : nullptr;
        if (target && MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK) ++hooked;
        else Log("[wxr] input block: %s not hooked", name);
    };
    hook(L"user32.dll", "GetAsyncKeyState", (void*)&WxrGetAsyncKeyState, (void**)&g_origGetAsyncKeyState);
    hook(L"user32.dll", "GetKeyState", (void*)&WxrGetKeyState, (void**)&g_origGetKeyState);
    // A static import of the game, so mapped already; no LoadLibrary under the loader lock.
    hook(L"dinput8.dll", "DirectInput8Create", (void*)&WxrDirectInput8Create, (void**)&g_origDirectInput8Create);
    Log("*** [wxr] gamepad only: the game's keyboard and mouse are silenced (%d hooks; window input dropped)", hooked);
}
