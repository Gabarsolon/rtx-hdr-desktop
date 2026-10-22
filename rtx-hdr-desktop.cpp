// RTX HDR Desktop: mirrors a screen (or one window's area) into a click-through,
// capture-excluded flip-model window, so the NVIDIA driver's RTX HDR - which only
// looks at DX11/DX12/Vulkan swap chains of programs whose profile enables it -
// turns ordinary SDR desktop content into HDR.
//
// Capture: DXGI Desktop Duplication (FP16 scRGB when Windows HDR is on).
// Present: B8G8R8A8 FLIP_DISCARD swap chain through DirectComposition (or HWND).
// The overlay is excluded from capture (WDA_EXCLUDEFROMCAPTURE), so it never
// sees itself, and is WS_EX_TRANSPARENT|WS_EX_LAYERED so clicks go through.
//
// The driver only loads RTX HDR for an opaque, unclipped swap chain whose window
// covers a whole monitor (measured: 2560x1440 loads it; 2560x1400, 2400x1440, a
// premultiplied-alpha chain, a DirectComposition clip and a window region do not).
// So the overlay always spans the monitor, and "one window" mode means: the whole
// screen in HDR while that window is in front, nothing while it isn't.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <share.h>
#include <dwmapi.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <psapi.h>
#include <wrl/client.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

#define APP_NAME L"RTX HDR Desktop"
#define APP_VERSION "1.0.0"
#define CTRL_CLASS L"RtxHdrDesktopControl"
#define OVERLAY_CLASS L"RtxHdrDesktopOverlay"
#define WM_TRAY (WM_APP + 1)

enum { HK_QUIT = 1, HK_PAUSE, HK_FOLLOW };
enum { CMD_STATUS = 100, CMD_PAUSE, CMD_PICK, CMD_SETTINGS, CMD_EXIT, CMD_SCREEN0 = 200 };
#define SETTINGS_CLASS L"RtxHdrDesktopSettings"
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------------------------------------------------------------------
// Logging: %TEMP%\rtx-hdr-desktop.log, plus the parent console when there is one.

static FILE* g_log;
static bool g_console;

static void logf(const char* fmt, ...) {
    char buf[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof buf, fmt, a);
    va_end(a);
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (g_log) {
        fprintf(g_log, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
        fflush(g_log);
    }
    if (g_console) printf("%s\n", buf);
}

static std::string narrow(const wchar_t* w) {
    char buf[512];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof buf, nullptr, nullptr);
    return buf;
}

// ---------------------------------------------------------------------------
// Shaders. Full-screen triangle; the overlay covers the captured monitor, so
// the pixel shader reads the capture 1:1. `area` is the part to show (monitor
// pixels, left/top/right/bottom).
// mode bit 0: source is linear scRGB -> back to the SDR code value. With a
//             calibration (mode bit 4) through the measured inverse curve `inv`,
//             otherwise divide by SDR white and encode sRGB.
// mode bit 1: self-test marker (magenta 8x8 square in the area's corner).
// mode bit 2: test pattern instead of the capture (grey ramp across the area).
// mode bit 3: calibration bands (CAL_STEPS grey steps across the area).
// mode bit 4: `inv` holds a calibration; `scale` = 1 / captured value of white.
// `sharp` > 0: contrast-adaptive sharpening (after AMD FidelityFX CAS, MIT) on
// the SDR picture, before the driver's RTX HDR sees it.

#define CAL_STEPS 64
#define INV_SIZE 1024

static const char kShader[] = R"(
cbuffer C : register(b0) { int4 area; float scale; uint mode; float sharp; float pad; };
Texture2D<float4> src : register(t0);
Texture1D<float> inv : register(t1);
SamplerState lin : register(s0);

float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1);
}

float3 encodeSrgb(float3 c) {
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

// The captured pixel at p, back as the SDR code value the app drew.
float3 sdrAt(int2 p) {
    float3 c = src.Load(int3(clamp(p, area.xy, area.zw - 1), 0)).rgb;
    if (mode & 16) {
        c = saturate(c * scale);
        c = c * (1023.0 / 1024) + 0.5 / 1024;  // texel centres of the 1024-entry curve
        c = float3(inv.SampleLevel(lin, c.r, 0), inv.SampleLevel(lin, c.g, 0), inv.SampleLevel(lin, c.b, 0));
    } else if (mode & 1) {
        c = encodeSrgb(saturate(c * scale));
    }
    return c;
}

float4 ps(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
    bool inside = p.x >= area.x && p.y >= area.y && p.x < area.z && p.y < area.w;
    if ((mode & 2) && inside && p.x < area.x + 8 && p.y < area.y + 8) return float4(1, 0, 1, 1);
    if (mode & 4) return float4(saturate((pos.xxx - area.x) / (area.z - area.x)), 1);
    if (mode & 8) return float4(floor((pos.xxx - area.x) * 64 / (area.z - area.x)) / 63, 1);
    float3 e = sdrAt(p);
    if (sharp <= 0) return float4(e, 1);

    // 3x3 neighbourhood:  a b c / d e f / g h i
    float3 a = sdrAt(p + int2(-1, -1)), b = sdrAt(p + int2(0, -1)), c = sdrAt(p + int2(1, -1));
    float3 d = sdrAt(p + int2(-1, 0)), f = sdrAt(p + int2(1, 0));
    float3 g = sdrAt(p + int2(-1, 1)), h = sdrAt(p + int2(0, 1)), i = sdrAt(p + int2(1, 1));
    float3 mn = min(min(min(d, e), min(f, b)), h);
    float3 mx = max(max(max(d, e), max(f, b)), h);
    mn += min(mn, min(min(a, c), min(g, i)));
    mx += max(mx, max(max(a, c), max(g, i)));
    // Less sharpening where the neighbourhood already has strong contrast.
    float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-5)));
    float3 w = amp * (-1.0 / lerp(8.0, 5.0, sharp));
    float3 o = (b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w);
    return float4(saturate(o), 1);
}
)";

struct Constants {
    RECT area;
    float scale;
    UINT mode;
    float sharp;
    float pad;
};

// ---------------------------------------------------------------------------
// Display enumeration.

struct Screen {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput6> output;
    DXGI_OUTPUT_DESC1 desc;
    DXGI_ADAPTER_DESC1 adapterDesc;
};

static std::vector<Screen> enumScreens() {
    std::vector<Screen> list;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return list;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        DXGI_ADAPTER_DESC1 ad;
        adapter->GetDesc1(&ad);
        ComPtr<IDXGIOutput> out;
        for (UINT o = 0; adapter->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; o++) {
            Screen s;
            s.adapter = adapter;
            s.adapterDesc = ad;
            if (FAILED(out.As(&s.output))) continue;
            s.output->GetDesc1(&s.desc);
            if (!s.desc.AttachedToDesktop) continue;
            list.push_back(s);
        }
    }
    return list;
}

static RECT screenRect(const Screen& s) { return s.desc.DesktopCoordinates; }
static int rw(const RECT& r) { return r.right - r.left; }
static int rh(const RECT& r) { return r.bottom - r.top; }

// SDR content brightness slider value for a GDI display name, in nits (0 = unknown).
static float sdrWhiteNits(const wchar_t* gdiName) {
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return 0;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return 0;
    for (UINT32 i = 0; i < np; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof src;
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (wcscmp(src.viewGdiDeviceName, gdiName)) continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL w = {};
        w.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        w.header.size = sizeof w;
        w.header.adapterId = paths[i].targetInfo.adapterId;
        w.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&w.header) != ERROR_SUCCESS) return 0;
        return w.SDRWhiteLevel * 80.0f / 1000.0f;
    }
    return 0;
}

static bool rtxHdrLoaded() {
    return GetModuleHandleW(L"nvAIHDR.dll") || GetModuleHandleW(L"nvAIHDRx.dll");
}

// ---------------------------------------------------------------------------
// App state.

struct Options {
    int screen = -1;          // -1 = primary
    float sdrWhite = 0;       // 0 = read from Windows
    bool composition = true;  // DirectComposition swap chain (--hwnd: a plain HWND one)
    std::wstring followTitle;
    int selftestSeconds = 0;
    bool list = false;
    // Debug: --pattern draws a grey ramp (and stays visible to capture); --measure N
    // runs no overlay and logs how bright the screen comes out, so a second copy can
    // read what the first one's pattern looks like after the driver and DWM.
    bool pattern = false;
    bool noCalibration = false;  // --no-calibration: trust Windows' SDR white level instead of measuring
    int measureSeconds = 0;
};

struct App {
    Options opt;
    HINSTANCE inst;
    HWND ctrl = nullptr, overlay = nullptr;
    HWINEVENTHOOK pickHook = nullptr;
    NOTIFYICONDATAW tray = {};
    UINT taskbarCreated = 0;
    bool running = true, paused = false, picking = false;
    HWND settingsWnd = nullptr, sharpSlider = nullptr, sharpLabel = nullptr;
    int sharpness = 0;  // 0-100, 0 = off; saved in settings.ini
    HWND follow = nullptr;
    int screenIndex = 0;

    // Render device: the NVIDIA adapter (RTX HDR runs there).
    ComPtr<IDXGIAdapter1> renderAdapter;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;
    bool usingComposition = false;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> dtarget;
    ComPtr<IDCompositionVisual> dvisual;
    UINT swapW = 0, swapH = 0;

    // Duplication: on the adapter that drives the screen (may be another GPU).
    Screen screen;
    ComPtr<ID3D11Device> dupDev;
    ComPtr<ID3D11DeviceContext> dupCtx;
    ComPtr<IDXGIOutputDuplication> dupl;
    bool crossAdapter = false;
    ComPtr<ID3D11Texture2D> staging;   // on dupDev, cross-adapter only
    ComPtr<ID3D11Texture2D> frame;     // on dev
    ComPtr<ID3D11ShaderResourceView> frameSrv;
    DXGI_FORMAT frameFormat = DXGI_FORMAT_UNKNOWN;
    bool haveFrame = false;
    float scale = 1;
    bool calibrated = false, needCalibration = false;
    ComPtr<ID3D11Texture1D> invTex;
    ComPtr<ID3D11ShaderResourceView> invSrv;
    ComPtr<ID3D11SamplerState> linSampler;

    RECT view = {};       // desktop rect the overlay covers (always a whole monitor)
    RECT area = {};       // same as view; the shader's bands/pattern/marker are laid out in it
    bool visible = false;
    bool dirty = true;
    DWORD lastTopmost = 0;

    // Self-test.
    DWORD startTick = 0;
    UINT presented = 0;
    bool markerChecked = false, markerSeenInCapture = false;
} g;

// ---------------------------------------------------------------------------

static bool isShellOrOwn(HWND h) {
    if (!h) return true;
    h = GetAncestor(h, GA_ROOT);
    if (h == g.overlay || h == g.ctrl) return true;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    static const wchar_t* shell[] = {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
                                     L"NotifyIconOverflowWindow", L"Windows.UI.Core.CoreWindow"};
    for (auto s : shell)
        if (!wcscmp(cls, s)) return true;
    return false;
}

static void updateTrayTip();

static void setFollow(HWND h) {
    g.follow = h;
    g.dirty = true;
    if (h) {
        // Program name only: window titles can hold private things (page titles, file names).
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        wchar_t path[MAX_PATH] = L"?";
        DWORD len = MAX_PATH;
        if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            QueryFullProcessImageNameW(proc, 0, path, &len);
            CloseHandle(proc);
        }
        const wchar_t* exe = wcsrchr(path, L'\\');
        logf("HDR only while %s is in front", narrow(exe ? exe + 1 : path).c_str());
    } else {
        logf("whole screen mode");
    }
    updateTrayTip();
}

static void CALLBACK onForeground(HWINEVENTHOOK, DWORD, HWND h, LONG, LONG, DWORD, DWORD) {
    if (!g.picking || isShellOrOwn(h)) return;
    g.picking = false;
    setFollow(GetAncestor(h, GA_ROOT));
}

// ---------------------------------------------------------------------------
// Graphics setup.

static bool compileShaders() {
    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(kShader, sizeof kShader - 1, "rtx-hdr-desktop", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kShader, sizeof kShader - 1, "rtx-hdr-desktop", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err))) {
        logf("shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    g.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g.vs);
    g.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g.ps);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g.dev->CreateSamplerState(&sd, &g.linSampler);
    D3D11_BUFFER_DESC bd = {sizeof(Constants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    return SUCCEEDED(g.dev->CreateBuffer(&bd, nullptr, &g.cb));
}

static bool createDevice(IDXGIAdapter1* adapter, ComPtr<ID3D11Device>& dev, ComPtr<ID3D11DeviceContext>& ctx) {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                   D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) logf("D3D11CreateDevice failed 0x%08lx", hr);
    return SUCCEEDED(hr);
}

static bool initRenderDevice() {
    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        DXGI_ADAPTER_DESC1 ad;
        adapter->GetDesc1(&ad);
        if (ad.VendorId == 0x10DE) {
            g.renderAdapter = adapter;
            break;
        }
    }
    if (!g.renderAdapter) {
        logf("no NVIDIA GPU found - RTX HDR needs one; using the default adapter");
        factory->EnumAdapters1(0, &g.renderAdapter);
    }
    DXGI_ADAPTER_DESC1 ad;
    g.renderAdapter->GetDesc1(&ad);
    logf("render GPU: %s", narrow(ad.Description).c_str());
    return createDevice(g.renderAdapter.Get(), g.dev, g.ctx) && compileShaders();
}

static bool createSwapChain(UINT w, UINT h) {
    ComPtr<IDXGIDevice> dxgiDev;
    g.dev.As(&dxgiDev);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDev->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = w;
    sd.Height = h;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    HRESULT hr = E_FAIL;
    if (!g.opt.composition) {
        sd.Scaling = DXGI_SCALING_NONE;
        hr = factory->CreateSwapChainForHwnd(g.dev.Get(), g.overlay, &sd, nullptr, nullptr, &g.swap);
        if (SUCCEEDED(hr)) {
            g.usingComposition = false;
            factory->MakeWindowAssociation(g.overlay, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
            logf("swap chain: HWND flip model %ux%u", w, h);
            return true;
        }
        logf("HWND swap chain failed 0x%08lx - trying DirectComposition", hr);
    }
    sd.Scaling = DXGI_SCALING_STRETCH;
    hr = factory->CreateSwapChainForComposition(g.dev.Get(), &sd, nullptr, &g.swap);
    if (FAILED(hr)) {
        logf("composition swap chain failed 0x%08lx", hr);
        return false;
    }
    if (FAILED(hr = DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&g.dcomp))) ||
        FAILED(hr = g.dcomp->CreateTargetForHwnd(g.overlay, TRUE, &g.dtarget)) ||
        FAILED(hr = g.dcomp->CreateVisual(&g.dvisual)) || FAILED(hr = g.dvisual->SetContent(g.swap.Get())) ||
        FAILED(hr = g.dtarget->SetRoot(g.dvisual.Get())) || FAILED(hr = g.dcomp->Commit())) {
        logf("DirectComposition setup failed 0x%08lx", hr);
        return false;
    }
    g.usingComposition = true;
    logf("swap chain: DirectComposition flip model %ux%u", w, h);
    return true;
}

static bool ensureSwapChain(UINT w, UINT h) {
    if (!g.swap) {
        if (!createSwapChain(w, h)) return false;
    } else if (w != g.swapW || h != g.swapH) {
        g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        g.ctx->Flush();
        HRESULT hr = g.swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            logf("ResizeBuffers failed 0x%08lx", hr);
            return false;
        }
    }
    g.swapW = w;
    g.swapH = h;
    return true;
}

static void closeDuplication() {
    g.dupl.Reset();
    g.staging.Reset();
    g.frame.Reset();
    g.frameSrv.Reset();
    g.haveFrame = false;
}

static bool openDuplication() {
    closeDuplication();
    DXGI_ADAPTER_DESC1 rd;
    g.renderAdapter->GetDesc1(&rd);
    g.crossAdapter = memcmp(&rd.AdapterLuid, &g.screen.adapterDesc.AdapterLuid, sizeof(LUID)) != 0;
    if (g.crossAdapter) {
        if (!g.dupDev && !createDevice(g.screen.adapter.Get(), g.dupDev, g.dupCtx)) return false;
    } else {
        g.dupDev = g.dev;
        g.dupCtx = g.ctx;
    }
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
    HRESULT hr = g.screen.output->DuplicateOutput1(g.dupDev.Get(), 0, 2, formats, &g.dupl);
    if (FAILED(hr)) {
        logf("DuplicateOutput1 failed 0x%08lx", hr);
        return false;
    }
    DXGI_OUTDUPL_DESC dd;
    g.dupl->GetDesc(&dd);
    g.frameFormat = dd.ModeDesc.Format;
    bool linear = g.frameFormat == DXGI_FORMAT_R16G16B16A16_FLOAT;
    float white = g.opt.sdrWhite > 0 ? g.opt.sdrWhite : sdrWhiteNits(g.screen.desc.DeviceName);
    if (white <= 0) white = 80;
    g.scale = 80.0f / white;
    g.calibrated = false;
    g.needCalibration = linear && !g.opt.noCalibration;
    logf("capturing %s %ux%u %s%s, SDR white %.0f nits%s", narrow(g.screen.desc.DeviceName).c_str(), dd.ModeDesc.Width,
         dd.ModeDesc.Height, linear ? "FP16 scRGB (Windows HDR on)" : "8-bit (Windows HDR off - RTX HDR needs it on)",
         g.crossAdapter ? ", copied across GPUs" : "", white, dd.Rotation != DXGI_MODE_ROTATION_IDENTITY ? ", ROTATED (not handled)" : "");

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = dd.ModeDesc.Width;
    td.Height = dd.ModeDesc.Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = g.frameFormat;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &g.frame))) return false;
    g.dev->CreateShaderResourceView(g.frame.Get(), nullptr, &g.frameSrv);
    if (g.crossAdapter) {
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g.dupDev->CreateTexture2D(&td, nullptr, &g.staging))) return false;
    }
    g.dirty = true;
    return true;
}

static bool selectScreen(int index) {
    auto list = enumScreens();
    if (list.empty()) return false;
    if (index < 0 || index >= (int)list.size()) {
        index = 0;
        for (int i = 0; i < (int)list.size(); i++) {
            RECT r = screenRect(list[i]);
            if (r.left == 0 && r.top == 0) index = i;
        }
    }
    bool sameAdapter = g.screen.adapter &&
                       !memcmp(&g.screen.adapterDesc.AdapterLuid, &list[index].adapterDesc.AdapterLuid, sizeof(LUID));
    if (!sameAdapter && g.dupDev != g.dev) {
        g.dupDev.Reset();
        g.dupCtx.Reset();
    }
    g.screen = list[index];
    g.screenIndex = index;
    return openDuplication();
}

// ---------------------------------------------------------------------------
// The screen to show in HDR this frame. Returns false when nothing should show:
// in one-window mode, whenever that window isn't the one in front.

static bool wantedArea(RECT& out) {
    if (g.follow) {
        if (!IsWindow(g.follow)) {
            logf("followed window closed");
            setFollow(nullptr);
            return wantedArea(out);
        }
        HWND fg = GetForegroundWindow();
        if (IsIconic(g.follow) || (fg && GetAncestor(fg, GA_ROOT) != g.follow)) return false;
        HMONITOR hm = MonitorFromWindow(g.follow, MONITOR_DEFAULTTONEAREST);
        if (hm != g.screen.desc.Monitor) {
            auto list = enumScreens();
            for (int i = 0; i < (int)list.size(); i++)
                if (list[i].desc.Monitor == hm) {
                    selectScreen(i);
                    break;
                }
        }
    }
    out = screenRect(g.screen);
    return true;
}

static void hideOverlay() {
    if (g.visible) ShowWindow(g.overlay, SW_HIDE);
    g.visible = false;
}

static void placeOverlay(const RECT& area) {
    RECT r = screenRect(g.screen);
    DWORD now = GetTickCount();
    bool moved = !EqualRect(&r, &g.view);
    if (moved || !g.visible || now - g.lastTopmost > 1000) {
        SetWindowPos(g.overlay, HWND_TOPMOST, r.left, r.top, rw(r), rh(r), SWP_NOACTIVATE | SWP_SHOWWINDOW);
        g.lastTopmost = now;
    }
    if (moved || !EqualRect(&area, &g.area)) g.dirty = true;
    g.view = r;
    g.area = area;
    g.visible = true;
}

// ---------------------------------------------------------------------------
// Frame loop.

static void checkMarker() {
    // The overlay draws a magenta square at its top-left corner in self-test mode.
    // If WDA_EXCLUDEFROMCAPTURE works, that square is NOT in the captured frame.
    RECT mon = screenRect(g.screen);
    D3D11_TEXTURE2D_DESC td;
    g.frame->GetDesc(&td);
    td.Width = td.Height = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> px;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &px))) return;
    UINT x = g.area.left - mon.left + 3, y = g.area.top - mon.top + 3;
    D3D11_BOX box = {x, y, 0, x + 1, y + 1, 1};
    g.ctx->CopySubresourceRegion(px.Get(), 0, 0, 0, 0, g.frame.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g.ctx->Map(px.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    float r, gr, b;
    if (g.frameFormat == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        auto h = (const uint16_t*)m.pData;
        auto half = [](uint16_t v) {
            int e = (v >> 10) & 31, f = v & 1023;
            float x = e ? ldexpf((float)(f | 1024), e - 25) : ldexpf((float)f, -24);
            return (v & 0x8000) ? -x : x;
        };
        r = half(h[0]), gr = half(h[1]), b = half(h[2]);
    } else {
        auto p = (const uint8_t*)m.pData;
        b = p[0] / 255.f, gr = p[1] / 255.f, r = p[2] / 255.f;
    }
    g.ctx->Unmap(px.Get(), 0);
    g.markerChecked = true;
    g.markerSeenInCapture = r > 0.2f && b > 0.2f && gr < 0.05f * r;
    logf("self-test: captured pixel under the marker = %.3f %.3f %.3f -> overlay %s", r, gr, b,
         g.markerSeenInCapture ? "IS in the capture (exclusion failed)" : "is not in the capture (good)");
}

static void draw(bool bands = false) {
    if (!ensureSwapChain(rw(g.view), rh(g.view))) return;
    ComPtr<ID3D11Texture2D> back;
    g.swap->GetBuffer(0, IID_PPV_ARGS(&back));
    ComPtr<ID3D11RenderTargetView> rtv;
    g.dev->CreateRenderTargetView(back.Get(), nullptr, &rtv);

    RECT mon = screenRect(g.screen);
    Constants c = {};
    c.area = g.area;
    OffsetRect(&c.area, -mon.left, -mon.top);
    c.scale = g.scale;
    c.sharp = g.sharpness / 100.0f;
    c.mode = (g.frameFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1u : 0u) | (g.opt.selftestSeconds ? 2u : 0u) |
             (g.opt.pattern ? 4u : 0u) | (bands ? 8u : 0u) | (g.calibrated ? 16u : 0u);
    g.ctx->UpdateSubresource(g.cb.Get(), 0, nullptr, &c, 0, 0);

    D3D11_VIEWPORT vp = {0, 0, (float)g.swapW, (float)g.swapH, 0, 1};
    g.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    g.ctx->RSSetViewports(1, &vp);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(g.ps.Get(), nullptr, 0);
    g.ctx->PSSetConstantBuffers(0, 1, g.cb.GetAddressOf());
    ID3D11ShaderResourceView* srvs[2] = {g.frameSrv.Get(), g.invSrv.Get()};
    g.ctx->PSSetShaderResources(0, 2, srvs);
    g.ctx->PSSetSamplers(0, 1, g.linSampler.GetAddressOf());
    g.ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none[2] = {};
    g.ctx->PSSetShaderResources(0, 2, none);

    HRESULT hr = g.swap->Present(1, 0);
    if (FAILED(hr)) logf("Present failed 0x%08lx", hr);
    g.presented++;
    g.dirty = false;
}

// Measures what DWM makes of each SDR grey level on this screen (SDR white,
// MHC2 calibration curves such as SDR Boost's, ...) and uploads the inverse, so
// the overlay hands RTX HDR the same SDR picture the apps drew. The overlay is
// visible to capture for these few frames: a grey ramp flashes for ~0.1 s.
static float halfToFloat(uint16_t v);

static bool calibrate() {
    g.needCalibration = false;
    RECT mon = screenRect(g.screen);
    RECT a = g.area;
    OffsetRect(&a, -mon.left, -mon.top);
    SetWindowDisplayAffinity(g.overlay, WDA_NONE);

    D3D11_TEXTURE2D_DESC td;
    g.frame->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> cpu;
    g.dupDev->CreateTexture2D(&td, nullptr, &cpu);

    float f[CAL_STEPS] = {};
    bool got = false;
    DWORD start = GetTickCount();
    for (int presents = 0; cpu && GetTickCount() - start < 1500;) {
        if (presents < 60) draw(true), presents++;
        DXGI_OUTDUPL_FRAME_INFO fi;
        ComPtr<IDXGIResource> res;
        if (g.dupl->AcquireNextFrame(50, &fi, &res) != S_OK) continue;
        if (presents >= 4 && fi.LastPresentTime.QuadPart) {
            ComPtr<ID3D11Texture2D> tex;
            res.As(&tex);
            g.dupCtx->CopyResource(cpu.Get(), tex.Get());
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(g.dupCtx->Map(cpu.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                int y = (a.top + a.bottom) / 2;
                auto row = (const uint16_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch);
                for (int k = 0; k < CAL_STEPS; k++) {
                    int x = a.left + (int)((k + 0.5f) * rw(a) / CAL_STEPS);
                    const uint16_t* px = row + x * 4;
                    f[k] = (halfToFloat(px[0]) + halfToFloat(px[1]) + halfToFloat(px[2])) / 3;
                }
                g.dupCtx->Unmap(cpu.Get(), 0);
                // The bands must be on screen: rising, black at the left.
                got = f[CAL_STEPS - 1] > f[CAL_STEPS / 2] && f[CAL_STEPS / 2] > f[0] && f[0] < 0.05f;
            }
        }
        g.dupl->ReleaseFrame();
        if (got) break;
    }
    SetWindowDisplayAffinity(g.overlay, WDA_EXCLUDEFROMCAPTURE);
    g.dirty = true;
    if (!got) {
        logf("calibration failed (grey ramp not seen in the capture) - using Windows' SDR white level");
        return false;
    }

    for (int k = 1; k < CAL_STEPS; k++) f[k] = std::max(f[k], f[k - 1]);  // monotonic
    float vmax = f[CAL_STEPS - 1];
    std::vector<float> inv(INV_SIZE);
    int k = 0;
    for (int i = 0; i < INV_SIZE; i++) {
        float v = vmax * i / (INV_SIZE - 1);
        while (k < CAL_STEPS - 2 && f[k + 1] < v) k++;
        float span = f[k + 1] - f[k];
        float t = span > 0 ? std::min(std::max((v - f[k]) / span, 0.0f), 1.0f) : 0.0f;
        inv[i] = std::min(std::max((k + t) / (CAL_STEPS - 1), 0.0f), 1.0f);
    }
    D3D11_TEXTURE1D_DESC d1 = {INV_SIZE, 1, 1, DXGI_FORMAT_R32_FLOAT, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
    D3D11_SUBRESOURCE_DATA init = {inv.data()};
    g.invTex.Reset();
    g.invSrv.Reset();
    if (FAILED(g.dev->CreateTexture1D(&d1, &init, &g.invTex))) return false;
    g.dev->CreateShaderResourceView(g.invTex.Get(), nullptr, &g.invSrv);
    g.scale = 1 / vmax;
    g.calibrated = true;
    logf("calibrated: SDR grey 25%% / 50%% / 75%% / white = %.0f / %.0f / %.0f / %.0f nits on screen",
         f[16] * 80, f[32] * 80, f[48] * 80, vmax * 80);
    return true;
}

// NVIDIA's own overlay (Alt+Z) is a capture-excluded window: our mirror can't show
// it and our topmost window covers it. Step aside while it has the focus.
static bool nvidiaOverlayOpen() {
    HWND fg = GetForegroundWindow();
    wchar_t cls[64] = {};
    if (fg) GetClassNameW(fg, cls, 64);
    static std::wstring last;
    bool open = !wcscmp(cls, L"CEF-OSC-WIDGET");
    if (last != cls) {
        last = cls;
        logf("foreground window class: %s%s", narrow(cls).c_str(), open ? " (NVIDIA overlay - hiding)" : "");
    }
    return open;
}

static void tick() {
    if (nvidiaOverlayOpen()) {
        hideOverlay();
        Sleep(16);
        return;
    }
    if (!g.screen.output && !selectScreen(g.opt.screen)) {
        Sleep(250);
        return;
    }
    if (!g.dupl && !openDuplication()) {
        Sleep(250);
        return;
    }
    RECT r;
    if (!wantedArea(r)) {
        hideOverlay();
        Sleep(16);
        return;
    }
    if (!g.dupl) return;  // selectScreen inside wantedView may have failed

    DXGI_OUTDUPL_FRAME_INFO fi;
    ComPtr<IDXGIResource> res;
    HRESULT hr = g.dupl->AcquireNextFrame(16, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        if (g.haveFrame) {
            placeOverlay(r);
            if (g.dirty) draw();
        }
        return;
    }
    if (FAILED(hr)) {
        logf("AcquireNextFrame 0x%08lx - reopening", hr);  // mode change, secure desktop, ...
        closeDuplication();
        Sleep(100);
        return;
    }
    bool changed = fi.LastPresentTime.QuadPart && fi.TotalMetadataBufferSize;  // no dirty/move rects = nothing redrawn
    if (changed || !g.haveFrame) {
        ComPtr<ID3D11Texture2D> tex;
        res.As(&tex);
        if (!g.crossAdapter) {
            g.ctx->CopyResource(g.frame.Get(), tex.Get());
        } else {
            g.dupCtx->CopyResource(g.staging.Get(), tex.Get());
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(g.dupCtx->Map(g.staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                g.ctx->UpdateSubresource(g.frame.Get(), 0, nullptr, m.pData, m.RowPitch, 0);
                g.dupCtx->Unmap(g.staging.Get(), 0);
            }
        }
        g.haveFrame = true;
        g.dirty = true;
    }
    g.dupl->ReleaseFrame();
    placeOverlay(r);
    if (g.needCalibration && g.swap) {
        calibrate();
        return;
    }
    if (g.dirty) draw();
    if (g.opt.selftestSeconds && !g.markerChecked && g.presented > 30) checkMarker();
}

// ---------------------------------------------------------------------------
// Tray icon and menu.

static void updateTrayTip() {
    if (!g.tray.cbSize) return;
    swprintf_s(g.tray.szTip, L"%s - %s%s", APP_NAME, g.paused ? L"paused" : g.follow ? L"with one app in front" : L"whole screen",
               rtxHdrLoaded() ? L", RTX HDR loaded" : L", RTX HDR not loaded");
    g.tray.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g.tray);
}

static void addTray() {
    g.tray.cbSize = sizeof g.tray;
    g.tray.hWnd = g.ctrl;
    g.tray.uID = 1;
    g.tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g.tray.uCallbackMessage = WM_TRAY;
    g.tray.hIcon = LoadIconW(g.inst, MAKEINTRESOURCEW(1));
    if (!g.tray.hIcon) g.tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g.tray.szTip, APP_NAME);
    Shell_NotifyIconW(NIM_ADD, &g.tray);
    updateTrayTip();
}

static void setPaused(bool p) {
    g.paused = p;
    if (p) {
        hideOverlay();
        closeDuplication();
    }
    g.dirty = true;
    logf(p ? "paused" : "resumed");
    updateTrayTip();
}

static void showMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, CMD_STATUS,
                rtxHdrLoaded() ? L"RTX HDR: loaded by the driver" : L"RTX HDR: not loaded (check the driver profile)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    auto list = enumScreens();
    for (int i = 0; i < (int)list.size() && i < 16; i++) {
        wchar_t label[128];
        RECT r = screenRect(list[i]);
        swprintf_s(label, L"Whole screen %d (%dx%d, %s)", i + 1, rw(r), rh(r), list[i].adapterDesc.Description);
        AppendMenuW(m, MF_STRING | (!g.follow && i == g.screenIndex ? MF_CHECKED : 0), CMD_SCREEN0 + i, label);
    }
    AppendMenuW(m, MF_STRING | (g.follow || g.picking ? MF_CHECKED : 0), CMD_PICK,
                g.picking ? L"Only with one app in front: click its window now..."
                          : L"Only while one app is in front: pick it...   (Ctrl+Alt+F10 = the active one)");
    AppendMenuW(m, MF_STRING | (g.paused ? MF_CHECKED : 0), CMD_PAUSE, L"Pause\tCtrl+Alt+F11");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_SETTINGS, L"Settings (sharpness)...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_EXIT, L"Exit\tCtrl+Alt+F12");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g.ctrl);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.ctrl, nullptr);
    PostMessageW(g.ctrl, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void openSettings();

static void onCommand(UINT id) {
    if (id == CMD_SETTINGS) {
        openSettings();
    } else if (id == CMD_EXIT) {
        g.running = false;
    } else if (id == CMD_PAUSE) {
        setPaused(!g.paused);
    } else if (id == CMD_PICK) {
        if (g.follow) {
            setFollow(nullptr);
        } else {
            g.picking = true;
            logf("pick mode: waiting for a window to be activated");
        }
    } else if (id >= CMD_SCREEN0 && id < CMD_SCREEN0 + 16) {
        g.picking = false;
        setFollow(nullptr);
        selectScreen(id - CMD_SCREEN0);
    }
}

static LRESULT CALLBACK ctrlProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g.taskbarCreated && msg) {
        addTray();
        return 0;
    }
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) showMenu();
        return 0;
    case WM_COMMAND:
        onCommand(LOWORD(wp));
        return 0;
    case WM_HOTKEY:
        if (wp == HK_QUIT) g.running = false;
        else if (wp == HK_PAUSE) setPaused(!g.paused);
        else if (wp == HK_FOLLOW) {
            HWND fg = GetForegroundWindow();
            if (g.follow || isShellOrOwn(fg)) setFollow(nullptr);
            else setFollow(GetAncestor(fg, GA_ROOT));
        }
        return 0;
    case WM_CLOSE:
        g.running = false;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Settings: %APPDATA%\RTX HDR Desktop\settings.ini and a small window with sliders.

static std::wstring settingsPath() {
    wchar_t dir[MAX_PATH];
    if (!GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH)) return L"";
    std::wstring d = std::wstring(dir) + L"\\RTX HDR Desktop";
    CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\settings.ini";
}

static void loadSettings() {
    g.sharpness = std::min(std::max((int)GetPrivateProfileIntW(L"picture", L"sharpness", 0, settingsPath().c_str()), 0), 100);
}

static void saveSettings() {
    wchar_t v[16];
    swprintf_s(v, L"%d", g.sharpness);
    WritePrivateProfileStringW(L"picture", L"sharpness", v, settingsPath().c_str());
}

static void updateSharpLabel() {
    wchar_t t[64];
    if (g.sharpness) swprintf_s(t, L"Sharpness: %d", g.sharpness);
    else wcscpy_s(t, L"Sharpness: off");
    SetWindowTextW(g.sharpLabel, t);
}

static LRESULT CALLBACK settingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HSCROLL:
        if ((HWND)lp == g.sharpSlider) {
            g.sharpness = (int)SendMessageW(g.sharpSlider, TBM_GETPOS, 0, 0);
            updateSharpLabel();
            g.dirty = true;
            if (LOWORD(wp) == TB_ENDTRACK || LOWORD(wp) == TB_THUMBPOSITION) saveSettings();
        }
        return 0;
    case WM_CLOSE:
        saveSettings();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g.settingsWnd = g.sharpSlider = g.sharpLabel = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void openSettings() {
    if (g.settingsWnd) {
        SetForegroundWindow(g.settingsWnd);
        return;
    }
    float s = GetDpiForSystem() / 96.0f;
    auto px = [s](int v) { return (int)(v * s); };
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int w = px(360), hgt = px(150);
    g.settingsWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, SETTINGS_CLASS, APP_NAME L" settings",
                                    WS_POPUP | WS_CAPTION | WS_SYSMENU, wa.right - w - px(16), wa.bottom - hgt - px(16), w, hgt,
                                    nullptr, nullptr, g.inst, nullptr);
    NONCLIENTMETRICSW ncm = {sizeof ncm};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, GetDpiForSystem());
    static HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    g.sharpLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, px(16), px(14), px(320), px(20), g.settingsWnd,
                                   nullptr, g.inst, nullptr);
    g.sharpSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS, px(8), px(38),
                                    px(330), px(30), g.settingsWnd, nullptr, g.inst, nullptr);
    HWND hint = CreateWindowExW(0, L"STATIC", L"Sharpens the picture before RTX HDR. 0 = off.", WS_CHILD | WS_VISIBLE, px(16),
                                px(74), px(320), px(20), g.settingsWnd, nullptr, g.inst, nullptr);
    for (HWND c : {g.sharpLabel, g.sharpSlider, hint}) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(g.sharpSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(g.sharpSlider, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(g.sharpSlider, TBM_SETPOS, TRUE, g.sharpness);
    updateSharpLabel();
    ShowWindow(g.settingsWnd, SW_SHOW);
    SetForegroundWindow(g.settingsWnd);
}

static LRESULT CALLBACK overlayProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// --measure: brightest pixels of the area on screen, in nits (needs Windows HDR on).

static float halfToFloat(uint16_t v) {
    int e = (v >> 10) & 31, f = v & 1023;
    float x = e ? ldexpf((float)(f | 1024), e - 25) : ldexpf((float)f, -24);
    return (v & 0x8000) ? -x : x;
}

static int measure() {
    if (!selectScreen(g.opt.screen)) return 1;
    RECT area;
    if (!wantedArea(area)) return 1;
    RECT mon = screenRect(g.screen);
    OffsetRect(&area, -mon.left, -mon.top);
    if (g.frameFormat != DXGI_FORMAT_R16G16B16A16_FLOAT) {
        logf("measure: Windows HDR is off on this screen - nothing to measure");
        return 1;
    }
    D3D11_TEXTURE2D_DESC td;
    g.frame->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> cpu;
    if (FAILED(g.dupDev->CreateTexture2D(&td, nullptr, &cpu))) return 1;
    DWORD end = GetTickCount() + g.opt.measureSeconds * 1000;
    int frames = 0;
    while (GetTickCount() < end) {
        DXGI_OUTDUPL_FRAME_INFO fi;
        ComPtr<IDXGIResource> res;
        if (g.dupl->AcquireNextFrame(500, &fi, &res) != S_OK) continue;
        if (fi.LastPresentTime.QuadPart) {
            ComPtr<ID3D11Texture2D> tex;
            res.As(&tex);
            g.dupCtx->CopyResource(cpu.Get(), tex.Get());
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(g.dupCtx->Map(cpu.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                // Peak and the average of the right-most tenth (the ramp's white end).
                float peak = 0, whiteSum = 0;
                int whiteN = 0, x0 = area.right - rw(area) / 10;
                for (int y = area.top; y < area.bottom; y += 2) {
                    auto row = (const uint16_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch);
                    for (int x = area.left; x < area.right; x += 2) {
                        const uint16_t* px = row + x * 4;
                        float l = 0.2126f * halfToFloat(px[0]) + 0.7152f * halfToFloat(px[1]) + 0.0722f * halfToFloat(px[2]);
                        peak = std::max(peak, l);
                        if (x >= x0) whiteSum += l, whiteN++;
                    }
                }
                g.dupCtx->Unmap(cpu.Get(), 0);
                if (frames++ % 20 == 0)
                    logf("measure: peak %.0f nits, white end %.0f nits (SDR white %.0f)", peak * 80,
                         whiteN ? whiteSum / whiteN * 80 : 0, 80 / g.scale);
            }
        }
        g.dupl->ReleaseFrame();
    }
    return 0;
}

// ---------------------------------------------------------------------------

static bool parseArgs(Options& o) {
    int argc;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&]() -> const wchar_t* { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--list") o.list = true;
        else if (a == L"--screen") o.screen = _wtoi(next()) - 1;
        else if (a == L"--sdr-white") o.sdrWhite = (float)_wtof(next()), o.noCalibration = true;
        else if (a == L"--hwnd") o.composition = false;
        else if (a == L"--pattern") o.pattern = true;
        else if (a == L"--no-calibration") o.noCalibration = true;
        else if (a == L"--measure") o.measureSeconds = _wtoi(next());
        else if (a == L"--window") o.followTitle = next();
        else if (a == L"--selftest") o.selftestSeconds = _wtoi(next());
        else {
            logf("unknown option: %s", narrow(a.c_str()).c_str());
            logf("options: --list | --screen N | --window \"title text\" | --sdr-white NITS | --no-calibration | --hwnd | --selftest SECONDS");
            LocalFree(argv);
            return false;
        }
    }
    LocalFree(argv);
    return true;
}

static void listScreens() {
    auto list = enumScreens();
    for (int i = 0; i < (int)list.size(); i++) {
        RECT r = screenRect(list[i]);
        float white = sdrWhiteNits(list[i].desc.DeviceName);
        logf("screen %d: %s %dx%d at %d,%d on %s, %s, SDR white %.0f nits", i + 1, narrow(list[i].desc.DeviceName).c_str(), rw(r),
             rh(r), r.left, r.top, narrow(list[i].adapterDesc.Description).c_str(),
             list[i].desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? "HDR on" : "HDR off", white);
    }
}

struct FindTitle {
    const wchar_t* text;
    HWND found;
};

static BOOL CALLBACK findTitleProc(HWND h, LPARAM lp) {
    auto f = (FindTitle*)lp;
    wchar_t title[256] = {};
    GetWindowTextW(h, title, 256);
    if (IsWindowVisible(h) && wcsstr(title, f->text) && !isShellOrOwn(h)) {
        f->found = h;
        return FALSE;
    }
    return TRUE;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // DuplicateOutput1 needs it
    g.inst = inst;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        g_console = true;
    }
    if (!parseArgs(g.opt)) return 2;
    wchar_t logPath[MAX_PATH];
    GetTempPathW(MAX_PATH, logPath);
    wcscat_s(logPath, g.opt.measureSeconds ? L"rtx-hdr-desktop-measure.log" : L"rtx-hdr-desktop.log");
    g_log = _wfsopen(logPath, L"w", _SH_DENYWR);  // others may read it while we run
    logf("RTX HDR Desktop " APP_VERSION);
    if (g.opt.list) {
        listScreens();
        return 0;
    }
    if (g.opt.measureSeconds) {
        if (!initRenderDevice()) return 1;
        return measure();
    }

    // A second launch closes the running copy, so one shortcut toggles it.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\RtxHdrDesktop");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(CTRL_CLASS, nullptr);
        if (other) PostMessageW(other, WM_CLOSE, 0, 0);
        logf("already running - asked it to exit");
        return 0;
    }

    WNDCLASSEXW wc = {sizeof wc};
    wc.hInstance = inst;
    wc.lpfnWndProc = ctrlProc;
    wc.lpszClassName = CTRL_CLASS;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = overlayProc;
    wc.lpszClassName = OVERLAY_CLASS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    wc.lpfnWndProc = settingsProc;
    wc.lpszClassName = SETTINGS_CLASS;
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    loadSettings();

    g.ctrl = CreateWindowExW(WS_EX_TOOLWINDOW, CTRL_CLASS, APP_NAME, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED;
    if (g.opt.composition) ex |= WS_EX_NOREDIRECTIONBITMAP;
    g.overlay = CreateWindowExW(ex, OVERLAY_CLASS, APP_NAME L" overlay", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, inst, nullptr);
    SetLayeredWindowAttributes(g.overlay, 0, 255, LWA_ALPHA);
    if (!g.opt.pattern && !SetWindowDisplayAffinity(g.overlay, WDA_EXCLUDEFROMCAPTURE))
        logf("SetWindowDisplayAffinity failed (%lu) - needs Windows 10 2004 or newer", GetLastError());

    if (!initRenderDevice()) return 1;
    if (!selectScreen(g.opt.screen)) logf("could not start capturing yet - will retry");

    if (!g.opt.followTitle.empty()) {
        FindTitle f = {g.opt.followTitle.c_str(), nullptr};
        EnumWindows(findTitleProc, (LPARAM)&f);
        if (f.found) setFollow(f.found);
        else logf("no visible window title contains \"%s\" - using the whole screen", narrow(g.opt.followTitle.c_str()).c_str());
    }

    RegisterHotKey(g.ctrl, HK_QUIT, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F12);
    RegisterHotKey(g.ctrl, HK_PAUSE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F11);
    RegisterHotKey(g.ctrl, HK_FOLLOW, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F10);
    g.pickHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, onForeground, 0, 0, WINEVENT_OUTOFCONTEXT);
    g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    addTray();

    g.startTick = GetTickCount();
    bool reportedHdr = false;
    while (g.running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g.running) break;
        if (g.paused) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
            continue;
        }
        tick();
        if (!reportedHdr && rtxHdrLoaded()) {
            reportedHdr = true;
            logf("RTX HDR module loaded by the driver after %lu ms", GetTickCount() - g.startTick);
            updateTrayTip();
        }
        if (g.opt.selftestSeconds && GetTickCount() - g.startTick > (DWORD)g.opt.selftestSeconds * 1000) break;
    }

    int result = 0;
    if (g.opt.selftestSeconds) {
        logf("self-test: swap chain %s, %u presents, overlay %dx%d at %d,%d, HDR area %dx%d at %d,%d",
             g.usingComposition ? "DirectComposition" : "HWND", g.presented, rw(g.view), rh(g.view), g.view.left, g.view.top,
             rw(g.area), rh(g.area), g.area.left, g.area.top);
        logf("self-test: capture exclusion %s", !g.markerChecked ? "not checked" : g.markerSeenInCapture ? "FAILED" : "ok");
        logf("self-test: RTX HDR module %s", rtxHdrLoaded() ? "loaded" : "NOT loaded");
        HMODULE mods[512];
        DWORD need = 0;
        if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) {
            std::string nv;
            for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 512; i++) {
                wchar_t name[MAX_PATH];
                if (GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, MAX_PATH) && !_wcsnicmp(name, L"nv", 2))
                    nv += narrow(name) + " ";
            }
            logf("self-test: NVIDIA modules: %s", nv.c_str());
        }
        result = (g.presented && g.markerChecked && !g.markerSeenInCapture) ? 0 : 1;
    }

    if (g.pickHook) UnhookWinEvent(g.pickHook);
    Shell_NotifyIconW(NIM_DELETE, &g.tray);
    hideOverlay();
    closeDuplication();
    g.swap.Reset();
    DestroyWindow(g.overlay);
    DestroyWindow(g.ctrl);
    if (mutex) CloseHandle(mutex);
    logf("exit");
    return result;
}
