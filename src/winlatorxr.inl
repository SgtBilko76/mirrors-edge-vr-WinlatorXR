// ================================================================ WinlatorXR (standalone Quest)
//
// Compiled only into the WinlatorXR flavour (MEVR_WINLATORXR, built by build_linux.py). The
// desktop build never sees this file.
//
// WinlatorXR runs Windows games through Wine, Box64 and DXVK on the headset itself. There is no
// OpenXR runtime inside the container. Instead it speaks XrAPI (https://winlatorxr.github.io/xrapi.html):
//
//   - head, controller and button state arrives as one text line per frame on UDP 127.0.0.1:7872
//   - the game answers on UDP 7278 with "L_HAPTICS R_HAPTICS MODE_VR MODE_3D FOVX FOVY"
//   - WinlatorXR shows the game's own window: in side-by-side mode the left half to the left eye
//   - pixel (0,0) of the screen carries the HMD_SYNC value of the pose the frame was drawn with,
//     as red; for alternate-eye frames blue says which eye (0 left, 255 right)
//
// The mod already renders both eyes side by side into its backbuffer, so the picture needs no
// work at all - WinlatorXR only needs to be told how to read it. What the mod cannot do without is
// an OpenXR runtime to ask for poses and input. So this file IS that runtime: the subset of the
// OpenXR API the mod calls, implemented in-process on top of XrAPI, with the same semantics. The
// thousands of lines of game logic above and below keep calling xrLocateSpace and xrSyncActions
// exactly as they do on a PC and do not know the difference.
//
// What is deliberately not implemented: swapchains and frame submission. WinlatorXR has no
// compositor layers; the frame is the window. Present takes the WinlatorXR path instead
// (winlatorxr_frame.inl) and those functions report unsupported if anything ever reaches them.
//
// Controller poses: XrAPI sends one orientation per controller, which the CoD4 port measured to be
// an OpenXR aim pose rolled 180 degrees about its pointing axis. That is undone for the aim pose;
// the grip pose is the corrected aim pitched up by WinlatorXrGripPitch (default 40 degrees - the
// mod's pistol calibration of 40 degrees wrist-down was tuned against a grip about that far above
// the pointing ray). Tracked hands are optional in the VR menu; the joypad layout works either way.

#pragma comment(lib, "ws2_32.lib")

namespace wxr {

static const u_short kPortIn = 7872, kPortInFallback = 7873, kPortOut = 7278;
static const char* const kDir        = "Z:\\tmp\\xr";
static const char* const kSystemFile = "Z:\\tmp\\xr\\system";

// Field order of the T/F token, exactly as transmitted.
enum Button {
    LGrip, LMenu, LStick, LStickLeft, LStickRight, LStickUp, LStickDown, LTrigger, LX, LY,
    RA, RB, RGrip, RStick, RStickLeft, RStickRight, RStickUp, RStickDown, RTrigger, kButtons
};

struct Packet {
    bool  valid = false;
    float lq[4] = {0,0,0,1}, lStick[2] = {}, lp[3] = {};
    float rq[4] = {0,0,0,1}, rStick[2] = {}, rp[3] = {};
    float hq[4] = {0,0,0,1}, hp[3] = {};
    float ipd = 0.063f, fovX = 0, fovY = 0;     // metres, degrees (full angles)
    int   sync = 0;
    bool  button[kButtons] = {};
    bool  hasAltitude = false;
    float altitude = 0;
    // WinlatorXR's own display state, appended as a T/F token by current builds.
    bool  hasMode = false, immersive = false, sideBySide = false;
};

static SRWLOCK  g_lock = SRWLOCK_INIT;
static Packet   g_latest;                  // written by the receive thread
static Packet   g_frame;                   // latched once per game frame; what the runtime reports
static long     g_packets = 0, g_rejected = 0;
static HANDLE   g_arrived = nullptr;       // auto-reset, signalled per packet
static HANDLE   g_thread = nullptr;
static SOCKET   g_in = INVALID_SOCKET, g_out = INVALID_SOCKET;
static volatile LONG g_stop = 0;
static bool     g_started = false;
static int      g_screenW = 0, g_screenH = 0;
static char     g_product[96] = "";
static char     g_lastBad[160] = "";

static bool ParseFloat(const char* s, float* out)
{
    char* end = nullptr;
    const float v = strtof(s, &end);
    if (end == s || *end || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

// The documented order, with or without the client label current builds put in front.
static bool Parse(char* text, Packet* out)
{
    char* ctx = nullptr;
    const char* tok[64]; int n = 0;
    for (char* t = strtok_s(text, " \t\r\n", &ctx); t && n < 64; t = strtok_s(nullptr, " \t\r\n", &ctx))
        tok[n++] = t;
    int i = 0;
    float probe;
    if (n && !ParseFloat(tok[0], &probe)) ++i;
    if (n < i + 28 + 2) return false;
    float f[28];
    for (int k = 0; k < 28; ++k) if (!ParseFloat(tok[i++], &f[k])) return false;
    float sync;
    if (!ParseFloat(tok[i++], &sync)) return false;
    const char* b = tok[i++];
    if (strlen(b) < kButtons || strspn(b, "TF") != strlen(b)) return false;

    Packet p;
    memcpy(p.lq, f + 0, 16);  memcpy(p.lStick, f + 4, 8);  memcpy(p.lp, f + 6, 12);
    memcpy(p.rq, f + 9, 16);  memcpy(p.rStick, f + 13, 8); memcpy(p.rp, f + 15, 12);
    memcpy(p.hq, f + 18, 16); memcpy(p.hp, f + 22, 12);
    p.ipd  = f[25] > 1.0f ? f[25] * 0.001f : f[25];    // tolerate a sender that reports mm
    p.fovX = f[26];
    p.fovY = f[27];
    p.sync = (int)lroundf(sync);
    for (int k = 0; k < kButtons; ++k) p.button[k] = b[k] == 'T';
    // XrAPI 0.5 appends the head's height above the floor and two grip quaternions.
    if (i < n && ParseFloat(tok[i], &probe)) { p.hasAltitude = true; p.altitude = probe; }
    // Some builds append the two flags to the button token, others send them as the last token.
    if (strlen(b) >= kButtons + 2) { p.hasMode = true; p.immersive = b[kButtons] == 'T'; p.sideBySide = b[kButtons + 1] == 'T'; }
    {
        const char* last = tok[n - 1];
        if (last != b && strlen(last) >= 2 && strlen(last) <= 4 && strspn(last, "TF") == strlen(last)) {
            p.hasMode = true; p.immersive = last[0] == 'T'; p.sideBySide = last[1] == 'T';
        }
    }
    const float hl = p.hq[0]*p.hq[0] + p.hq[1]*p.hq[1] + p.hq[2]*p.hq[2] + p.hq[3]*p.hq[3];
    if (!(hl > 0.25f && hl < 4.0f)) return false;
    const float inv = 1.0f / sqrtf(hl);
    for (float& c : p.hq) c *= inv;
    p.valid = true;
    *out = p;
    return true;
}

static unsigned __stdcall ReceiveThread(void*)
{
    char buffer[2048];
    while (!g_stop) {
        const int got = recvfrom(g_in, buffer, sizeof(buffer) - 1, 0, nullptr, nullptr);
        if (got <= 0) continue;            // the receive timeout lets the stop flag be seen
        buffer[got] = 0;
        char copy[2048];
        memcpy(copy, buffer, (size_t)got + 1);
        Packet p;
        if (Parse(buffer, &p)) {
            AcquireSRWLockExclusive(&g_lock);
            g_latest = p;
            ++g_packets;
            ReleaseSRWLockExclusive(&g_lock);
            SetEvent(g_arrived);
        } else {
            AcquireSRWLockExclusive(&g_lock);
            if (++g_rejected <= 3) strncpy_s(g_lastBad, copy, _TRUNCATE);
            ReleaseSRWLockExclusive(&g_lock);
        }
    }
    return 0;
}

static bool WriteFileText(const char* path, const char* text)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") || !f) return false;
    const size_t len = strlen(text);
    const bool ok = fwrite(text, 1, len, f) == len;
    fclose(f);
    return ok;
}

// Z:\tmp\xr\system: manufacturer, product, Android version, patch level, screen "WxH".
static void ReadSystemFile()
{
    FILE* f = nullptr;
    if (fopen_s(&f, kSystemFile, "rb") || !f) return;
    char line[160]; int row = 0; char manufacturer[48] = "";
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (row == 0) strncpy_s(manufacturer, line, _TRUNCATE);
        if (row == 1) sprintf_s(g_product, "%s %s", manufacturer, line);
        int w = 0, h = 0;
        if (sscanf_s(line, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) { g_screenW = w; g_screenH = h; }
        ++row;
    }
    fclose(f);
}

static bool Present()
{
    const DWORD a = GetFileAttributesA(kSystemFile);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static SOCKET Bind(u_short port)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) { closesocket(s); return INVALID_SOCKET; }
    const DWORD timeoutMs = 250;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
    return s;
}

static void Send(float lHaptic, float rHaptic, int modeVr, int mode3d, float fovX, float fovY)
{
    if (g_out == INVALID_SOCKET) return;
    // Locale-independent: printf would write a decimal comma under some Wine locales.
    char msg[96];
    auto fixed = [](char* at, size_t cap, float v) {
        if (!std::isfinite(v)) v = 0;
        const long milli = lroundf(fabsf(v) * 1000.0f);
        sprintf_s(at, cap, "%s%ld.%03ld", v < 0 ? "-" : "", milli / 1000, milli % 1000);
    };
    char a[24], b[24], c[24], d[24];
    fixed(a, sizeof(a), lHaptic); fixed(b, sizeof(b), rHaptic);
    fixed(c, sizeof(c), fovX);    fixed(d, sizeof(d), fovY);
    sprintf_s(msg, "%s %s %d %d %s %s", a, b, modeVr, mode3d, c, d);
    sockaddr_in t{};
    t.sin_family = AF_INET;
    t.sin_port = htons(kPortOut);
    t.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(g_out, msg, (int)strlen(msg), 0, (sockaddr*)&t, sizeof(t));
}

static bool Start()
{
    if (g_started) return true;
    if (!Present()) {
        Log("[wxr] %s not found - not running inside a WinlatorXR container with the XR API enabled",
            kSystemFile);
        return false;
    }
    ReadSystemFile();
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { Log("[wxr] WSAStartup failed"); return false; }
    g_in = Bind(kPortIn);
    if (g_in == INVALID_SOCKET) g_in = Bind(kPortInFallback);
    g_out = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_in == INVALID_SOCKET || g_out == INVALID_SOCKET) {
        Log("[wxr] could not bind UDP 7872/7873 (error %d)", WSAGetLastError());
        return false;
    }
    char version[16] = "0.5";
    if (const char* v = getenv("MEVR_WINLATORXR_API_VERSION")) strncpy_s(version, v, _TRUNCATE);
    CreateDirectoryA("Z:\\tmp", nullptr);
    CreateDirectoryA(kDir, nullptr);
    const bool wroteVersion = WriteFileText("Z:\\tmp\\xr\\version", version);
    const bool wroteVr      = WriteFileText("Z:\\tmp\\xr\\vr", "VR");
    g_arrived = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g_thread = (HANDLE)_beginthreadex(nullptr, 0, ReceiveThread, nullptr, 0, nullptr);
    if (!g_thread) { Log("[wxr] receive thread failed to start"); return false; }
    g_started = true;
    // Flat until the first stereo frame: intros and the front end are 2D.
    Send(0, 0, 2, 0, 0, 0);
    Log("*** [wxr] XrAPI %s requested%s%s; headset '%s', virtual screen %dx%d",
        version, wroteVersion ? "" : " (version file NOT written)",
        wroteVr ? "" : " (vr marker NOT written)", g_product, g_screenW, g_screenH);
    return true;
}

static void Stop()
{
    if (!g_started) return;
    Send(0, 0, 0, 0, 0, 0);                 // hand the headset back to the flat desktop
    DeleteFileA("Z:\\tmp\\xr\\vr");
    InterlockedExchange(&g_stop, 1);
    WaitForSingleObject(g_thread, 1000);
    g_started = false;
}

static bool WaitFirstPacket(DWORD timeoutMs)
{
    const DWORD until = GetTickCount() + timeoutMs;
    for (;;) {
        AcquireSRWLockShared(&g_lock);
        const bool have = g_latest.valid;
        ReleaseSRWLockShared(&g_lock);
        if (have) return true;
        const DWORD now = GetTickCount();
        if ((LONG)(until - now) <= 0) return false;
        WaitForSingleObject(g_arrived, until - now);
    }
}

// Once per game frame. Waits a little for a pose newer than the one the last frame used, which
// paces the game to the headset the way xrWaitFrame does on a PC, and then makes it the pose every
// runtime call reports until the next frame. One pose per frame is what keeps the sync stamp
// honest: the picture, the stamp and the head tracking all describe the same sample.
static void LatchFrame(DWORD waitMs)
{
    AcquireSRWLockShared(&g_lock);
    const bool fresh = g_latest.valid && g_latest.sync != g_frame.sync;
    ReleaseSRWLockShared(&g_lock);
    if (!fresh && waitMs) WaitForSingleObject(g_arrived, waitMs);
    AcquireSRWLockExclusive(&g_lock);
    g_frame = g_latest;
    ReleaseSRWLockExclusive(&g_lock);
}

static Packet Frame()
{
    AcquireSRWLockShared(&g_lock);
    const Packet p = g_frame;
    ReleaseSRWLockShared(&g_lock);
    return p;
}

// ---- the OpenXR subset ------------------------------------------------------------------------

static XrTime NowNs()
{
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (XrTime)((double)c.QuadPart * 1.0e9 / (double)f.QuadPart);
}

enum SpaceKind { SpaceLocal, SpaceView, SpaceAction };
struct Space  { bool live; SpaceKind kind; uint32_t action; XrPosef offset; };
// A source is one control on one hand: 0..8 per hand (see SourceFromPath), +16 for the right.
struct Action { XrActionType type; std::vector<int> sources; float prevF; float prevX, prevY; bool prevB; };

static std::vector<std::string> g_paths;
static std::vector<Space>       g_spaces;
static std::vector<Action>      g_actions;
static std::deque<XrSessionState> g_events;
static SRWLOCK g_rtLock = SRWLOCK_INIT;     // the render thread locates spaces too

enum Source { SrcStick, SrcStickClick, SrcTrigger, SrcSqueeze, SrcPrimary, SrcSecondary, SrcMenu, SrcPose, SrcAim };

static const uint64_t kInstance = 0x1001, kSession = 0x2001, kActionSet = 0x3001;
static const XrSystemId kSystem = 0x4001;
static const uint64_t kSpaceTag = 0x5000000000ull, kActionTag = 0x6000000000ull;

static int SourceFromPath(const std::string& p)
{
    const bool right = p.compare(0, 17, "/user/hand/right/") == 0;
    if (!right && p.compare(0, 16, "/user/hand/left/") != 0) return -1;
    const std::string c = p.substr(right ? 17 : 16);
    int s = -1;
    if (c == "input/thumbstick")                                    s = SrcStick;
    else if (c == "input/thumbstick/click")                         s = SrcStickClick;
    else if (c == "input/trigger/value" || c == "input/trigger/click") s = SrcTrigger;
    else if (c == "input/squeeze/value" || c == "input/squeeze/click") s = SrcSqueeze;
    else if (c == "input/a/click" || c == "input/x/click")          s = SrcPrimary;
    else if (c == "input/b/click" || c == "input/y/click")          s = SrcSecondary;
    else if (c == "input/menu/click")                               s = SrcMenu;
    else if (c == "input/grip/pose")                                s = SrcPose;
    else if (c == "input/aim/pose")                                 s = SrcAim;
    if (s < 0) return -1;
    // The Touch layout puts A/B on the right hand and X/Y on the left; reject a mismatch.
    if ((c == "input/a/click" || c == "input/b/click") && !right) return -1;
    if ((c == "input/x/click" || c == "input/y/click") && right) return -1;
    if (c == "input/menu/click" && right) return -1;   // the right menu button is the system's
    return s + (right ? 16 : 0);
}

static float SourceValue(const Packet& p, int src, float* x, float* y)
{
    const bool right = src >= 16;
    const int s = src & 15;
    const float* stick = right ? p.rStick : p.lStick;
    if (x) *x = 0;
    if (y) *y = 0;
    switch (s) {
        case SrcStick:      if (x) *x = stick[0]; if (y) *y = stick[1];
                            return sqrtf(stick[0]*stick[0] + stick[1]*stick[1]);
        case SrcStickClick: return p.button[right ? RStick : LStick] ? 1.0f : 0.0f;
        case SrcTrigger:    return p.button[right ? RTrigger : LTrigger] ? 1.0f : 0.0f;
        case SrcSqueeze:    return p.button[right ? RGrip : LGrip] ? 1.0f : 0.0f;
        case SrcPrimary:    return p.button[right ? RA : LX] ? 1.0f : 0.0f;
        case SrcSecondary:  return p.button[right ? RB : LY] ? 1.0f : 0.0f;
        case SrcMenu:       return p.button[LMenu] ? 1.0f : 0.0f;
        default:            return 0.0f;
    }
}

static Action* FindAction(XrAction a)
{
    const uint64_t v = (uint64_t)a;
    if ((v & ~0xFFFFFFFFull) != kActionTag) return nullptr;
    const uint32_t i = (uint32_t)v;
    return i < g_actions.size() ? &g_actions[i] : nullptr;
}

static XrQuaternionf QMul(const XrQuaternionf& a, const XrQuaternionf& b)
{
    return { a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
             a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
             a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
             a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z };
}
static XrVector3f QRot(const XrQuaternionf& q, const XrVector3f& v)
{
    const XrQuaternionf p{ v.x, v.y, v.z, 0 }, c{ -q.x, -q.y, -q.z, q.w };
    const XrQuaternionf r = QMul(QMul(q, p), c);
    return { r.x, r.y, r.z };
}
static XrPosef Compose(const XrPosef& a, const XrPosef& b)
{
    XrPosef r;
    r.orientation = QMul(a.orientation, b.orientation);
    const XrVector3f t = QRot(a.orientation, b.position);
    r.position = { a.position.x + t.x, a.position.y + t.y, a.position.z + t.z };
    return r;
}
static XrPosef Invert(const XrPosef& a)
{
    XrPosef r;
    r.orientation = { -a.orientation.x, -a.orientation.y, -a.orientation.z, a.orientation.w };
    const XrVector3f t = QRot(r.orientation, a.position);
    r.position = { -t.x, -t.y, -t.z };
    return r;
}

static XrPosef HeadPose(const Packet& p)
{
    XrPosef h;
    h.orientation = { p.hq[0], p.hq[1], p.hq[2], p.hq[3] };
    h.position = { p.hp[0], p.hp[1], p.hp[2] };
    return h;
}

static float g_gripPitchDeg = 40.0f;

// One controller's grip or aim pose in XrAPI's tracking frame.
static bool HandPose(const Packet& p, int src, XrPosef* out)
{
    const bool right = src >= 16;
    const float* q = right ? p.rq : p.lq;
    const float* pos = right ? p.rp : p.lp;
    const float l = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
    if (!p.valid || !(l > 0.25f && l < 4.0f)) return false;
    const float inv = 1.0f / sqrtf(l);
    XrQuaternionf o{ q[0] * inv, q[1] * inv, q[2] * inv, q[3] * inv };
    o = QMul(o, { 0.0f, 0.0f, 1.0f, 0.0f });             // undo XrAPI's 180 degree roll
    if ((src & 15) == SrcPose && g_gripPitchDeg != 0.0f) {
        const float h = 0.5f * g_gripPitchDeg * 0.01745329252f;
        o = QMul(o, { sinf(h), 0.0f, 0.0f, cosf(h) });   // +X pitch: the grip's -Z tilts upward
    }
    out->orientation = o;
    out->position = { pos[0], pos[1], pos[2] };
    return true;
}

static int PoseSource(uint32_t action)
{
    if (action >= g_actions.size()) return -1;
    for (int src : g_actions[action].sources)
        if ((src & 15) == SrcPose || (src & 15) == SrcAim) return src;
    return -1;
}

// The pose of a space in XrAPI's tracking frame. False for a pose that is not being tracked.
static bool SpaceWorld(uint64_t handle, const Packet& p, XrPosef* out)
{
    if ((handle & ~0xFFFFFFFFull) != kSpaceTag) return false;
    const uint32_t i = (uint32_t)handle;
    if (i >= g_spaces.size() || !g_spaces[i].live) return false;
    const Space& s = g_spaces[i];
    if (s.kind == SpaceLocal) { *out = s.offset; return true; }
    if (s.kind == SpaceView)  { if (!p.valid) return false; *out = Compose(HeadPose(p), s.offset); return true; }
    const int src = PoseSource(s.action);
    XrPosef hand;
    if (src < 0 || !HandPose(p, src, &hand)) return false;
    *out = Compose(hand, s.offset);
    return true;
}

static float SafeIpd(const Packet& p)
{
    return (p.ipd >= 0.045f && p.ipd <= 0.085f) ? p.ipd : 0.063f;
}

} // namespace wxr

// ---- entry points, with the loader's exact signatures -----------------------------------------

#define WXR_API extern "C" XRAPI_ATTR XrResult XRAPI_CALL

WXR_API xrCreateInstance(const XrInstanceCreateInfo*, XrInstance* instance)
{
    if (!wxr::Start()) return XR_ERROR_RUNTIME_UNAVAILABLE;
    *instance = (XrInstance)wxr::kInstance;
    return XR_SUCCESS;
}

WXR_API xrGetInstanceProperties(XrInstance, XrInstanceProperties* p)
{
    strcpy_s(p->runtimeName, "WinlatorXR XrAPI (in-process OpenXR subset)");
    p->runtimeVersion = XR_MAKE_VERSION(0, 5, 0);
    return XR_SUCCESS;
}

WXR_API xrGetSystem(XrInstance, const XrSystemGetInfo*, XrSystemId* system)
{
    // Tracking is what makes this a headset. WinlatorXR sends packets only once it has seen the
    // version file, so a container without the XR API enabled is reported here, not later.
    // Not a reason to give up, though: on a slow start WinlatorXR's first packet was seen to
    // arrive after a 15 s wait had already failed the whole run. The session starts either way
    // and every pose reads as untracked until tracking arrives; the frame loop logs that moment.
    if (wxr::WaitFirstPacket(3000)) {
        wxr::LatchFrame(0);
        const wxr::Packet p = wxr::Frame();
        Log("*** [wxr] first packet: FOV %.1f x %.1f deg, IPD %.1f mm, sync %d%s",
            p.fovX, p.fovY, p.ipd * 1000.0f, p.sync, p.hasAltitude ? ", floor height present" : "");
    } else {
        Log("[wxr] no XrAPI packet yet - starting anyway; VR engages when tracking arrives"
            " (check that the XR API is enabled for this container if it never does)");
    }
    *system = wxr::kSystem;
    return XR_SUCCESS;
}

WXR_API xrEnumerateViewConfigurationViews(XrInstance, XrSystemId, XrViewConfigurationType,
                                          uint32_t capacity, uint32_t* count, XrViewConfigurationView* views)
{
    *count = 2;
    if (!capacity) return XR_SUCCESS;
    if (capacity < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    // The "headset size" the mod builds its automatic resolution from: one window half. The screen
    // WinlatorXR splits is the X screen the container runs at, and the game must render exactly
    // that. It is taken from Wine's desktop mode, not from Z:\tmp\xr\system: on a Quest 3 that
    // file reported 840x880 for a container set to a 16:9 screen.
    uint32_t w = 960, h = 1080;
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_REGISTRY_SETTINGS, &dm) && dm.dmPelsWidth >= 1024 && dm.dmPelsHeight >= 576) {
        w = dm.dmPelsWidth / 2; h = dm.dmPelsHeight;
    } else if (wxr::g_screenW >= 1024 && wxr::g_screenH >= 576) {
        w = (uint32_t)wxr::g_screenW / 2; h = (uint32_t)wxr::g_screenH;
    }
    // Alternate-eye shows the whole screen to each eye. The engine still renders 16:9 (the mod
    // asks for twice this width, 16:9), so the width is chosen for the vertical detail: 16:9 at
    // 80% of the screen height, stretched to the screen by the window. 1792x1624 -> 2310x1299.
    if (g_wxrAlternateEye && h >= 576) w = (uint32_t)(h * 0.8f * 16.0f / 9.0f) / 2;
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("[wxr] screen: desktop mode %lux%lu, current %dx%d, XrAPI system file %dx%d -> eye %ux%u",
            dm.dmPelsWidth, dm.dmPelsHeight, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
            wxr::g_screenW, wxr::g_screenH, w, h);
    }
    for (uint32_t i = 0; i < 2; ++i) {
        views[i].recommendedImageRectWidth  = views[i].maxImageRectWidth  = w;
        views[i].recommendedImageRectHeight = views[i].maxImageRectHeight = h;
        views[i].recommendedSwapchainSampleCount = views[i].maxSwapchainSampleCount = 1;
    }
    return XR_SUCCESS;
}

WXR_API xrGetInstanceProcAddr(XrInstance, const char*, PFN_xrVoidFunction* function)
{
    *function = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

WXR_API xrCreateSession(XrInstance, const XrSessionCreateInfo*, XrSession* session)
{
    Log("[wxr] controller grip pose = aim pitched up %.0f deg (WinlatorXrGripPitch)", wxr::g_gripPitchDeg);
    *session = (XrSession)wxr::kSession;
    wxr::g_events.push_back(XR_SESSION_STATE_READY);
    return XR_SUCCESS;
}

WXR_API xrBeginSession(XrSession, const XrSessionBeginInfo*)
{
    wxr::g_events.push_back(XR_SESSION_STATE_SYNCHRONIZED);
    wxr::g_events.push_back(XR_SESSION_STATE_VISIBLE);
    wxr::g_events.push_back(XR_SESSION_STATE_FOCUSED);
    return XR_SUCCESS;
}

WXR_API xrEndSession(XrSession)
{
    wxr::Stop();
    return XR_SUCCESS;
}

WXR_API xrPollEvent(XrInstance, XrEventDataBuffer* buffer)
{
    if (wxr::g_events.empty()) return XR_EVENT_UNAVAILABLE;
    auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(buffer);
    *e = { XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED };
    e->session = (XrSession)wxr::kSession;
    e->state = wxr::g_events.front();
    e->time = wxr::NowNs();
    wxr::g_events.pop_front();
    return XR_SUCCESS;
}

WXR_API xrCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo* info, XrSpace* space)
{
    wxr::Space s{ true, wxr::SpaceLocal, 0, info->poseInReferenceSpace };
    if (info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW) s.kind = wxr::SpaceView;
    // LOCAL and STAGE are both XrAPI's own tracking frame. The mod recentres with offsets of its
    // own, so the origin WinlatorXR captured at container start is as good as any.
    AcquireSRWLockExclusive(&wxr::g_rtLock);
    wxr::g_spaces.push_back(s);
    *space = (XrSpace)(wxr::kSpaceTag | (uint64_t)(wxr::g_spaces.size() - 1));
    ReleaseSRWLockExclusive(&wxr::g_rtLock);
    return XR_SUCCESS;
}

WXR_API xrCreateActionSpace(XrSession, const XrActionSpaceCreateInfo* info, XrSpace* space)
{
    wxr::Space s{ true, wxr::SpaceAction, (uint32_t)(uint64_t)info->action, info->poseInActionSpace };
    AcquireSRWLockExclusive(&wxr::g_rtLock);
    wxr::g_spaces.push_back(s);
    *space = (XrSpace)(wxr::kSpaceTag | (uint64_t)(wxr::g_spaces.size() - 1));
    ReleaseSRWLockExclusive(&wxr::g_rtLock);
    return XR_SUCCESS;
}

WXR_API xrDestroySpace(XrSpace space)
{
    const uint64_t v = (uint64_t)space;
    AcquireSRWLockExclusive(&wxr::g_rtLock);
    if ((v & ~0xFFFFFFFFull) == wxr::kSpaceTag && (uint32_t)v < wxr::g_spaces.size())
        wxr::g_spaces[(uint32_t)v].live = false;
    ReleaseSRWLockExclusive(&wxr::g_rtLock);
    return XR_SUCCESS;
}

WXR_API xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime, XrSpaceLocation* location)
{
    location->locationFlags = 0;
    location->pose = { {0, 0, 0, 1}, {0, 0, 0} };
    const wxr::Packet p = wxr::Frame();
    XrPosef s, b;
    AcquireSRWLockShared(&wxr::g_rtLock);
    const bool ok = wxr::SpaceWorld((uint64_t)space, p, &s) && wxr::SpaceWorld((uint64_t)baseSpace, p, &b);
    ReleaseSRWLockShared(&wxr::g_rtLock);
    if (!ok) return XR_SUCCESS;            // valid call, untracked pose
    location->pose = wxr::Compose(wxr::Invert(b), s);
    location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                              XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    return XR_SUCCESS;
}

WXR_API xrLocateViews(XrSession, const XrViewLocateInfo* info, XrViewState* state,
                      uint32_t capacity, uint32_t* count, XrView* views)
{
    *count = 2;
    state->viewStateFlags = 0;
    if (capacity < 2) return capacity ? XR_ERROR_SIZE_INSUFFICIENT : XR_SUCCESS;
    const wxr::Packet p = wxr::Frame();
    XrPosef base;
    AcquireSRWLockShared(&wxr::g_rtLock);
    const bool ok = p.valid && p.fovX > 1.0f && p.fovY > 1.0f && wxr::SpaceWorld((uint64_t)info->space, p, &base);
    ReleaseSRWLockShared(&wxr::g_rtLock);
    if (!ok) return XR_SUCCESS;
    const XrPosef head = wxr::Compose(wxr::Invert(base), wxr::HeadPose(p));
    const float halfX = p.fovX * 0.5f * 0.01745329252f, halfY = p.fovY * 0.5f * 0.01745329252f;
    const float ipd = wxr::SafeIpd(p);
    for (int e = 0; e < 2; ++e) {
        XrPosef eye{ {0, 0, 0, 1}, { (e == 0 ? -0.5f : 0.5f) * ipd, 0, 0 } };
        views[e].pose = wxr::Compose(head, eye);
        views[e].fov = { -halfX, halfX, halfY, -halfY };
    }
    state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                            XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    return XR_SUCCESS;
}

WXR_API xrStringToPath(XrInstance, const char* text, XrPath* path)
{
    for (size_t i = 0; i < wxr::g_paths.size(); ++i)
        if (wxr::g_paths[i] == text) { *path = (XrPath)(i + 1); return XR_SUCCESS; }
    wxr::g_paths.push_back(text);
    *path = (XrPath)wxr::g_paths.size();
    return XR_SUCCESS;
}

WXR_API xrCreateActionSet(XrInstance, const XrActionSetCreateInfo*, XrActionSet* set)
{
    *set = (XrActionSet)wxr::kActionSet;
    return XR_SUCCESS;
}

WXR_API xrCreateAction(XrActionSet, const XrActionCreateInfo* info, XrAction* action)
{
    wxr::g_actions.push_back({ info->actionType, {}, 0, 0, 0, false });
    *action = (XrAction)(wxr::kActionTag | (uint64_t)(wxr::g_actions.size() - 1));
    return XR_SUCCESS;
}

WXR_API xrSuggestInteractionProfileBindings(XrInstance, const XrInteractionProfileSuggestedBinding* s)
{
    // XrAPI reports Touch-style controls on every headset WinlatorXR supports, so the Touch
    // suggestions are the bindings; any other profile is accepted and ignored.
    const XrPath profile = s->interactionProfile;
    if (!profile || profile > wxr::g_paths.size() ||
        wxr::g_paths[(size_t)profile - 1] != "/interaction_profiles/oculus/touch_controller")
        return XR_SUCCESS;
    for (uint32_t i = 0; i < s->countSuggestedBindings; ++i) {
        const XrActionSuggestedBinding& b = s->suggestedBindings[i];
        wxr::Action* a = wxr::FindAction(b.action);
        if (!a || !b.binding || b.binding > wxr::g_paths.size()) return XR_ERROR_PATH_INVALID;
        const int src = wxr::SourceFromPath(wxr::g_paths[(size_t)b.binding - 1]);
        if (src < 0) {
            Log("[wxr] no XrAPI control for %s - left unbound", wxr::g_paths[(size_t)b.binding - 1].c_str());
            continue;
        }
        a->sources.push_back(src);
    }
    return XR_SUCCESS;
}

WXR_API xrAttachSessionActionSets(XrSession, const XrSessionActionSetsAttachInfo*)
{
    return XR_SUCCESS;
}

WXR_API xrSyncActions(XrSession, const XrActionsSyncInfo*)
{
    return wxr::Frame().valid ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
}

// Current values of an action: the strongest bound control, OpenXR's own combining rule.
static bool WxrActionValue(XrAction action, float* value, float* x, float* y, bool* active)
{
    wxr::Action* a = wxr::FindAction(action);
    if (!a) return false;
    const wxr::Packet p = wxr::Frame();
    *value = 0;
    if (x) *x = 0;
    if (y) *y = 0;
    *active = p.valid && !a->sources.empty();
    if (!*active) return true;
    for (int src : a->sources) {
        float sx, sy;
        const float v = wxr::SourceValue(p, src, &sx, &sy);
        if (v > *value) { *value = v; if (x) *x = sx; if (y) *y = sy; }
    }
    return true;
}

WXR_API xrGetActionStateBoolean(XrSession, const XrActionStateGetInfo* info, XrActionStateBoolean* st)
{
    float v; bool active;
    if (!WxrActionValue(info->action, &v, nullptr, nullptr, &active)) return XR_ERROR_HANDLE_INVALID;
    wxr::Action* a = wxr::FindAction(info->action);
    const bool now = v > 0.5f;
    st->isActive = active;
    st->currentState = now;
    st->changedSinceLastSync = now != a->prevB;
    st->lastChangeTime = 0;
    a->prevB = now;
    return XR_SUCCESS;
}

WXR_API xrGetActionStateFloat(XrSession, const XrActionStateGetInfo* info, XrActionStateFloat* st)
{
    float v; bool active;
    if (!WxrActionValue(info->action, &v, nullptr, nullptr, &active)) return XR_ERROR_HANDLE_INVALID;
    wxr::Action* a = wxr::FindAction(info->action);
    st->isActive = active;
    st->currentState = v;
    st->changedSinceLastSync = v != a->prevF;
    st->lastChangeTime = 0;
    a->prevF = v;
    return XR_SUCCESS;
}

WXR_API xrGetActionStateVector2f(XrSession, const XrActionStateGetInfo* info, XrActionStateVector2f* st)
{
    float v, x, y; bool active;
    if (!WxrActionValue(info->action, &v, &x, &y, &active)) return XR_ERROR_HANDLE_INVALID;
    wxr::Action* a = wxr::FindAction(info->action);
    st->isActive = active;
    st->currentState = { x, y };
    st->changedSinceLastSync = x != a->prevX || y != a->prevY;
    st->lastChangeTime = 0;
    a->prevX = x; a->prevY = y;
    return XR_SUCCESS;
}

WXR_API xrGetActionStatePose(XrSession, const XrActionStateGetInfo* info, XrActionStatePose* st)
{
    const uint64_t v = (uint64_t)info->action;
    const int src = (v & ~0xFFFFFFFFull) == wxr::kActionTag ? wxr::PoseSource((uint32_t)v) : -1;
    XrPosef unused;
    st->isActive = (src >= 0 && wxr::HandPose(wxr::Frame(), src, &unused)) ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

// Never reached: no D3D11 device exists in this flavour, and Present takes the WinlatorXR path.
WXR_API xrEnumerateSwapchainFormats(XrSession, uint32_t, uint32_t* count, int64_t*) { *count = 0; return XR_ERROR_FUNCTION_UNSUPPORTED; }
WXR_API xrCreateSwapchain(XrSession, const XrSwapchainCreateInfo*, XrSwapchain*) { return XR_ERROR_FUNCTION_UNSUPPORTED; }
WXR_API xrDestroySwapchain(XrSwapchain) { return XR_SUCCESS; }
WXR_API xrEnumerateSwapchainImages(XrSwapchain, uint32_t, uint32_t* count, XrSwapchainImageBaseHeader*) { *count = 0; return XR_ERROR_FUNCTION_UNSUPPORTED; }
WXR_API xrAcquireSwapchainImage(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t*) { return XR_ERROR_FUNCTION_UNSUPPORTED; }
WXR_API xrWaitSwapchainImage(XrSwapchain, const XrSwapchainImageWaitInfo*) { return XR_ERROR_FUNCTION_UNSUPPORTED; }
WXR_API xrReleaseSwapchainImage(XrSwapchain, const XrSwapchainImageReleaseInfo*) { return XR_ERROR_FUNCTION_UNSUPPORTED; }
WXR_API xrWaitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState*) { return XR_ERROR_FUNCTION_UNSUPPORTED; }
WXR_API xrBeginFrame(XrSession, const XrFrameBeginInfo*) { return XR_ERROR_FUNCTION_UNSUPPORTED; }
WXR_API xrEndFrame(XrSession, const XrFrameEndInfo*) { return XR_ERROR_FUNCTION_UNSUPPORTED; }

#undef WXR_API
