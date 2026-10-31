// taskbar-dock.dll: the liquid-glass base ("dock") behind the Start button and the centred taskbar
// icons, like the Dock on a Mac. GlassDock loads it and calls dock_place() whenever the icon
// row moves. The dock is a window of its own right below the taskbar in z-order (which stays
// fully transparent); it reaches SHADOW_PAD px above the taskbar for the glass's shadow, and
// clicks go through it.
//
// The glass is S1mp1e's liquid glass (glass.hlsl: S1mp1e's glass.fsh, which follows reglass and
// iyinchao's liquid-glass-studio) with the settings of S1mp1e's hotbar: clear glass that shows
// what is behind it, bent inward by Snell's law (IOR 1.4) along a 13 px band at the edge with the
// colours split a little there, a 2 px frost, a white Fresnel rim and a soft drop shadow, and no
// tint at all.
//
// Behind the icons it draws what S1mp1e's hotbar and inventory do, handed over from Explorer by
// taskbar_hooks.dll instead of painted there:
// - the hover highlight (WM_HOVER_GLASS), Explorer's white rounded square, which moves from icon
//   to icon on S1mp1e's pair of springs (a quick leading one and a slower trailing one, so it
//   stretches while it travels), fading in where the pointer comes onto the icons and out where
//   it leaves them;
// - the active app (WM_SELECT_GLASS) as the glass of the hotbar's selection, on the same springs,
//   on top of the hover highlight; it stays while the user switches apps (sliding from the old
//   one to the new one) and fades out only when no app becomes active (the desktop).
//
// start-glass grows the Start menu out of the dock (reading the dock's place from its
// TaskbarDockShape property); when the shell hides the menu, the dock shrinks a picture of it
// back in (see "the Start menu going back into the dock").
//
// What is behind the dock comes from the Magnification API, which can capture the screen without
// given windows: here the dock and the windows in front of it (the taskbar with its icons,
// whatever pops up over them). A thread of its own captures that (up to `fps` times a second
// while it changes, four times a second once it stays the same, never while the dock is hidden),
// draws with Direct3D 11 into a composition swap chain, and runs the motion. When the dock
// changes (a button came or went), both of its edges glide to the new place together, and the
// Start button and the task list, which GlassDock centres, ride along with its left edge.
//
// The dock shows only while the taskbar itself is on top (Explorer drops the taskbar behind a
// full-screen app, such as a game) and no full-screen window is in front, and tells
// GlassDock's thread (WM_APP + 0x10) when windows come and go, so the icon row is measured
// again at once.
//
// Settings come from taskbar-dock.ini next to the DLL (one "key=value" per line):
//   frost=2          frost in px (glass.fsh's (1 - alpha) x 4: S1mp1e's hotbar 2, its panels 4)
//   shadow=100       the drop shadow's strength in percent of glass.fsh's
//   smoothing=60     corner smoothing in percent (0: glass.fsh's circular arcs, 60: iOS)
//   period=380       how long the dock takes to glide to a new place, in ms
//   fps=30           captures per second of the desktop behind it while that changes
//   log=1            append what happens to %TEMP%\taskbar-dock.log

#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <dispatcherqueue.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_3.h>
#include <magnification.h>
#include <windows.ui.composition.interop.h>
#undef GetCurrentTime // a WinBase macro that breaks the C++/WinRT headers
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

#include "out/glass_vs.h" // glass.hlsl, compiled by build.cmd
#include "out/glass_ps.h"
#include "out/ghost_ps.h"

using namespace winrt;
using namespace winrt::Windows::UI::Composition;
using namespace winrt::Windows::UI::Composition::Desktop;

namespace {

constexpr UINT WM_TASKS_CHANGED = WM_APP + 0x10; // posted to GlassDock's thread
constexpr UINT WM_HOVER_GLASS = WM_APP + 0x11;   // from taskbar_hooks.dll: a hover highlight
constexpr UINT WM_SELECT_GLASS = WM_APP + 0x12;  // from taskbar_hooks.dll: the active app's plate
constexpr UINT WM_GPU_LOST = WM_APP + 1;         // to the dock window: draw on a new device

// The window reaches this far above the taskbar: there glass.fsh's shadow has faded out.
constexpr int SHADOW_PAD = 48;
// glass.fsh's refraction: right at the edge the offset is OFFSET_SCALE x the screen height x an
// edge factor of -tan(asin(1 / 1.4) - pi / 2), and red goes 1 - (0.985 - 1) x 7 times as far.
constexpr float OFFSET_SCALE = 0.08f;
constexpr float EDGE_FACTOR = 0.9798f;
constexpr float RED_SPREAD = 1.105f;
constexpr int SLACK = 64;         // px captured beyond that on both sides, so a slide needs no new place
// Between captures while only what is around the dock changes (a video above the taskbar, say):
// only the bottom edge's 13 px band shows that, squeezed, so four times a second is enough.
constexpr int AROUND_MS = 250;
constexpr int IDLE_MS = 250;      // between captures once nothing changes

// S1mp1e's hotbar selection (glass.fsh's vertex colour 0xFFFFE0FF): sharp (no frost), lifted by
// 1 - 0xE0 / 255, corners half its half size (corner knob 1).
constexpr float SELECTION_LIFT = 1.0f - 0xE0 / 255.0f;
// Explorer's hover plate: white at 0x19 / 255.
constexpr float HOVER_WHITE = 0x19 / 255.0f;
// S1mp1e's fades (ms): the hover highlight in and out, the hotbar / tab selection both ways.
constexpr float HOVER_IN_MS = 100, HOVER_OUT_MS = 150, SELECT_FADE_MS = 100;
// How long the selection waits, when no app is active, for one to become active before it fades.
constexpr float SELECT_GRACE_MS = 300;

struct Settings {
    float frost = 2.0f;     // px
    float shadow = 1.0f;
    float smoothing = 0.6f;
    float period = 0.38f;   // seconds
    int fps = 30;
    bool log = false;
};

Settings g_settings;
SRWLOCK g_logLock = SRWLOCK_INIT;

void Log(std::wstring const& line) {
    if (!g_settings.log) return;
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    AcquireSRWLockExclusive(&g_logLock); // both threads write
    {
        std::wofstream file(std::wstring(temp) + L"taskbar-dock.log", std::ios::app);
        file << GetTickCount() << L" " << line << L"\n";
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

void LoadSettings(HMODULE self) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring ini = path;
    ini = ini.substr(0, ini.find_last_of(L'\\')) + L"\\taskbar-dock.ini";
    std::wifstream file(ini);
    std::wstring line;
    while (std::getline(file, line)) {
        const auto eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        const std::wstring key = line.substr(0, eq);
        const wchar_t* text = line.c_str() + eq + 1;
        const int value = _wtoi(text);
        if (key == L"frost") g_settings.frost = std::clamp(static_cast<float>(_wtof(text)), 0.0f, 8.0f);
        else if (key == L"shadow") g_settings.shadow = std::clamp(value, 0, 200) / 100.0f;
        else if (key == L"smoothing") g_settings.smoothing = std::clamp(value, 0, 100) / 100.0f;
        else if (key == L"period") g_settings.period = std::clamp(value, 50, 2000) / 1000.0f;
        else if (key == L"fps") g_settings.fps = std::clamp(value, 1, 60);
        else if (key == L"log") g_settings.log = value == 1;
    }
}

LONGLONG Now() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

double Seconds(LONGLONG ticks) {
    static const double frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    return ticks / frequency;
}

// The glide's curve: Apple's ease-out for things settling into place (a quick start that glides
// to rest), as a cubic Bézier from (0, 0) to (1, 1) through these two control points.
constexpr float EASE_X1 = 0.2f, EASE_Y1 = 0.8f, EASE_X2 = 0.2f, EASE_Y2 = 1.0f;

// Progress (0..1) of that curve at time fraction `t`.
float Ease(float t) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    auto bezier = [](float a, float b, float s) { return 3 * a * s * (1 - s) * (1 - s) + 3 * b * s * s * (1 - s) + s * s * s; };
    float lo = 0, hi = 1, s = t;
    for (int i = 0; i < 24; ++i) { // solve x(s) = t
        s = (lo + hi) / 2;
        (bezier(EASE_X1, EASE_X2, s) < t ? lo : hi) = s;
    }
    return bezier(EASE_Y1, EASE_Y2, s);
}

// Where the dock is and what moves with it, in screen px.
struct Placement {
    HWND taskbar = nullptr;
    RECT window{};                // the dock window
    RECT monitor{};               // its monitor
    float left = 0, right = 0;    // the dock's edges once settled
    float top = 0, height = 0;
    float radius = 0;
    int icon = 0;                 // the taskbar icons' size
    float fromLeft = 0, fromRight = 0; // where its edges were when the glide began
    LONGLONG glideStart = 0;      // when that was (QueryPerformanceCounter); 0: none
    HWND list = nullptr;          // the task list and the Start button, which ride along with the
    HWND start = nullptr;         // dock's left edge, this far from it
    float listOffset = 0, startOffset = 0;
};

// The dock's edges as drawn at `now`; true while they glide.
bool Edges(Placement const& p, LONGLONG now, float& left, float& right) {
    const float t = p.glideStart ? static_cast<float>(Seconds(now - p.glideStart)) / g_settings.period : 1;
    if (t >= 1) {
        left = p.left;
        right = p.right;
        return false;
    }
    const float e = Ease(t);
    left = p.fromLeft + (p.left - p.fromLeft) * e;
    right = p.fromRight + (p.right - p.fromRight) * e;
    return true;
}

// A highlight behind a taskbar button, as taskbar_hooks.dll tells about it.
struct Highlight {
    uint32_t id = 0;            // which button
    RECT rect{};                // the button, screen px
    float fade = 0;             // how far Explorer has faded its plate in, 0..1
    ULONGLONG updated = 0;      // GetTickCount64 of the last word about it
};
constexpr int MAX_HIGHLIGHTS = 8; // one fading in while others fade out

// The highlights of one kind (hover / active).
struct Highlights {
    Highlight items[MAX_HIGHLIGHTS];
    int count = 0;

    // The strongest of those at least `fade` strong (the one told about last among equals).
    Highlight const* Strongest(float fade) const {
        Highlight const* best = nullptr;
        for (int i = 0; i < count; ++i) {
            Highlight const& h = items[i];
            if (h.fade >= fade && (!best || h.fade > best->fade || (h.fade == best->fade && h.updated >= best->updated))) best = &h;
        }
        return best;
    }

    // The one for button `id`, if any.
    Highlight const* Find(uint32_t id) const {
        for (int i = 0; i < count; ++i) {
            if (items[i].id == id) return &items[i];
        }
        return nullptr;
    }

    // The pointer's button, with the pointer at `point` (within `top`..`bottom` down): the one
    // whose middle is nearest across, if the pointer is on it. Neighbouring buttons overlap (by
    // the room between their icons), and Explorer gives the pointer to the one whose icon is nearest.
    Highlight const* Under(POINT point, float top, float bottom) const {
        if (point.y < top || point.y >= bottom) return nullptr;
        Highlight const* under = nullptr;
        float nearest = 0;
        for (int i = 0; i < count; ++i) {
            Highlight const& h = items[i];
            const float distance = std::fabs(point.x + 0.5f - (h.rect.left + h.rect.right) / 2.0f);
            if (distance > (h.rect.right - h.rect.left) / 2.0f + 1) continue; // a pixel more: the gap between two buttons
            if (!under || distance < nearest || (distance == nearest && h.updated >= under->updated)) {
                under = &h;
                nearest = distance;
            }
        }
        return under;
    }
};

// What GlassDock's thread hands the render thread, under `lock`.
struct Shared {
    SRWLOCK lock = SRWLOCK_INIT;
    HANDLE wake = nullptr;              // set when any of this changes, or the dock shows
    com_ptr<ID3D11Device> device;       // made on GlassDock's thread, where composition wants
    com_ptr<IDXGISwapChain1> swapChain; // the swap chain; drawn into by the render thread
    com_ptr<IDXGISwapChain1> ghostChain; // the closing Start menu's picture's
    unsigned gpu = 0;                   // bumped with every new device
    Placement placement;
    unsigned placed = 0;                // bumped with every change of placement
    Highlights hovers, actives;
    unsigned lit = 0;                   // bumped with every change of those
};

Shared s;

struct Dock {
    HWND hwnd = nullptr;
    HWND below = nullptr;       // the taskbar: the dock sits right under it
    UINT shellHook = 0;         // RegisterWindowMessage("SHELLHOOK")
    DWORD notifyThread = 0;     // GlassDock's thread
    bool wanted = false;        // GlassDock wants it shown
    RECT bounds{};              // the taskbar's rectangle; the window reaches SHADOW_PAD above it
    HWND list = nullptr;        // the task list and Start, repainted when the dock shows (Explorer
    HWND start = nullptr;       // then tells it about the highlights it has up)
    HANDLE renderer = nullptr;  // the render thread
    winrt::Windows::System::DispatcherQueueController queue{ nullptr };
    Compositor compositor{ nullptr };
    DesktopWindowTarget target{ nullptr };
    SpriteVisual glass{ nullptr }; // shows the swap chain
    // A window of its own, over the Start menu's place, for the menu's picture shrinking back
    // into the dock when the shell hides the menu.
    HWND ghost = nullptr;
    DesktopWindowTarget ghostTarget{ nullptr };
    SpriteVisual ghostPicture{ nullptr };
};

Dock g;
HMODULE g_module = nullptr;

// Whether the window in front covers its whole monitor (a game, a full-screen video).
bool FullScreenInFront() {
    HWND front = GetForegroundWindow();
    if (!front || front == g.hwnd) return false;
    wchar_t name[64] = {};
    GetClassNameW(front, name, 64);
    if (!wcscmp(name, L"Progman") || !wcscmp(name, L"WorkerW") || !wcscmp(name, L"Shell_TrayWnd")) return false; // the desktop
    RECT window{};
    MONITORINFO monitor{ sizeof(monitor) };
    if (!GetWindowRect(front, &window) || !GetMonitorInfoW(MonitorFromWindow(front, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
    return window.left <= monitor.rcMonitor.left && window.top <= monitor.rcMonitor.top &&
           window.right >= monitor.rcMonitor.right && window.bottom >= monitor.rcMonitor.bottom;
}

// Shown while GlassDock wants it, the taskbar is on top (Explorer drops it behind a
// full-screen app) and no full-screen window is in front; always directly below the taskbar.
void Apply() {
    if (!g.hwnd) return;
    const bool taskbarOnTop = g.below && (GetWindowLongW(g.below, GWL_EXSTYLE) & WS_EX_TOPMOST);
    const bool fullScreen = FullScreenInFront();
    Log(L"apply wanted=" + std::to_wstring(g.wanted) + L" taskbar-on-top=" + std::to_wstring(taskbarOnTop) +
        L" fullscreen=" + std::to_wstring(fullScreen));
    if (g.wanted && taskbarOnTop && !fullScreen) {
        // Its own IME window sits between: compare with the first visible window below the taskbar.
        HWND next = GetWindow(g.below, GW_HWNDNEXT);
        while (next && next != g.hwnd && !IsWindowVisible(next)) next = GetWindow(next, GW_HWNDNEXT);
        const bool hidden = !IsWindowVisible(g.hwnd);
        if (hidden || next != g.hwnd) {
            SetWindowPos(g.hwnd, g.below, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        if (hidden) { // Explorer painted its highlights itself meanwhile: paint again, to hand them over
            if (g.list) InvalidateRect(g.list, nullptr, FALSE);
            if (g.start) InvalidateRect(g.start, nullptr, FALSE);
        }
    } else if (IsWindowVisible(g.hwnd)) {
        ShowWindow(g.hwnd, SW_HIDE);
    }
    SetEvent(s.wake); // the render thread captures again when the dock shows, and rests while it is hidden
}

// Moves `window` (a child window, in another process) so that its left edge is at `x` on the
// screen; nothing when it is there already. Asynchronous: Explorer moves it on its own thread.
void MoveLeftTo(HWND window, int x) {
    RECT r{};
    if (!window || !GetWindowRect(window, &r) || r.left == x) return;
    POINT at{ x, r.top };
    ScreenToClient(GetParent(window), &at);
    SetWindowPos(window, nullptr, at.x, at.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

// A Direct3D device and the swap chain the render thread draws the glass into, which the glass
// visual shows pixel for pixel. Made again when the device is lost (a driver update, say).
bool MakeGpu(int width, int height) noexcept try {
    com_ptr<ID3D11Device> device;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                    ARRAYSIZE(levels), D3D11_SDK_VERSION, device.put(), nullptr, nullptr));
    com_ptr<IDXGIAdapter> adapter;
    check_hresult(device.as<IDXGIDevice>()->GetAdapter(adapter.put()));
    com_ptr<IDXGIFactory2> factory;
    check_hresult(adapter->GetParent(IID_PPV_ARGS(factory.put())));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    // A swap chain shown pixel for pixel, from the top-left, by `visual`.
    auto chainFor = [&](SpriteVisual const& visual, DXGI_SWAP_CHAIN_DESC1 const& chain) {
        com_ptr<IDXGISwapChain1> swapChain;
        check_hresult(factory->CreateSwapChainForComposition(device.get(), &chain, nullptr, swapChain.put()));
        ICompositionSurface surface{ nullptr };
        check_hresult(g.compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>()->CreateCompositionSurfaceForSwapChain(
            swapChain.get(), reinterpret_cast<ABI::Windows::UI::Composition::ICompositionSurface**>(put_abi(surface))));
        auto brush = g.compositor.CreateSurfaceBrush(surface);
        brush.Stretch(CompositionStretch::None);
        brush.HorizontalAlignmentRatio(0);
        brush.VerticalAlignmentRatio(0);
        visual.Brush(brush);
        return swapChain;
    };
    auto swapChain = chainFor(g.glass, desc);
    DXGI_SWAP_CHAIN_DESC1 ghost = desc; // sized to the menu by the render thread
    ghost.Width = ghost.Height = 64;
    ghost.Flags = 0;
    auto ghostChain = chainFor(g.ghostPicture, ghost);
    AcquireSRWLockExclusive(&s.lock);
    s.device = device;
    s.swapChain = swapChain;
    s.ghostChain = ghostChain;
    ++s.gpu;
    ReleaseSRWLockExclusive(&s.lock);
    SetEvent(s.wake);
    return true;
} catch (hresult_error const& e) {
    Log(L"no Direct3D: " + std::wstring(e.message()));
    return false;
}

// ---- the render thread ----

// The Magnification API's control and what it captured.
struct Lens {
    HWND host = nullptr;            // an invisible window holding the magnifier control
    HWND control = nullptr;
    int width = 0, height = 0;      // their size: the capture's
    std::vector<HWND> skipped;      // the windows the capture leaves out
    std::vector<BYTE> incoming;     // the capture just taken: BGRA rows, top-down, unpadded
    int incomingWidth = 0, incomingHeight = 0;
    std::vector<BYTE> pixels;       // the capture the glass is drawn from,
    RECT rect{};                    // taken here (screen px)
};

Lens lens;
bool g_kick = false;      // something happened on the desktop: capture again soon
bool g_dragging = false;  // a window is being moved or sized: keep capturing at full rate

// Called by MagSetWindowSource with the capture, on the render thread.
BOOL CALLBACK OnCaptured(HWND, void* source, MAGIMAGEHEADER header, void*, MAGIMAGEHEADER, RECT, RECT, HRGN) {
    const size_t row = static_cast<size_t>(header.width) * 4;
    lens.incoming.resize(row * header.height);
    for (UINT y = 0; y < header.height; ++y) {
        memcpy(lens.incoming.data() + y * row, static_cast<BYTE const*>(source) + static_cast<size_t>(y) * header.stride, row);
    }
    lens.incomingWidth = header.width;
    lens.incomingHeight = header.height;
    return TRUE;
}

void CALLBACK OnSystemEvent(HWINEVENTHOOK, DWORD event, HWND, LONG, LONG, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_MOVESIZESTART) g_dragging = true;
    if (event == EVENT_SYSTEM_MOVESIZEEND) g_dragging = false;
    g_kick = true;
}

// Captures `rect` (screen px) as it looks without the windows `skip` (into lens.incoming).
bool CaptureWithout(RECT const& rect, std::vector<HWND> const& skip) {
    const int w = rect.right - rect.left, h = rect.bottom - rect.top;
    if (w <= 0 || h <= 0) return false;
    if (w != lens.width || h != lens.height) {
        SetWindowPos(lens.host, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(lens.control, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        lens.width = w;
        lens.height = h;
    }
    if (skip != lens.skipped) {
        MagSetWindowFilterList(lens.control, MW_FILTERMODE_EXCLUDE, static_cast<int>(skip.size()), const_cast<HWND*>(skip.data()));
        lens.skipped = skip;
    }
    lens.incomingWidth = lens.incomingHeight = 0;
    MagSetWindowSource(lens.control, rect);
    return lens.incomingWidth == w && lens.incomingHeight == h;
}

// Captures `rect` (screen px) as it looks without the dock and the windows in front of it.
bool Capture(RECT const& rect, HWND taskbar) {
    std::vector<HWND> skip{ lens.host, g.hwnd };
    if (taskbar) skip.push_back(taskbar);
    for (HWND above = GetWindow(g.hwnd, GW_HWNDPREV); above && skip.size() < 24; above = GetWindow(above, GW_HWNDPREV)) {
        RECT r{};
        if (above != taskbar && IsWindowVisible(above) && GetWindowRect(above, &r) && IntersectRect(&r, &r, &rect)) skip.push_back(above);
    }
    return CaptureWithout(rect, skip);
}

enum class Change { None, Around, Behind };

// Where the last change was (screen px), for the log.
RECT g_changed{};

// What differs between the capture just taken and the last one, taken at the same place: the
// desktop right behind the dock (`dock`, screen px), only what is around it, or nothing.
Change Compare(RECT const& dock) {
    if (lens.incoming.size() != lens.pixels.size()) return Change::Behind;
    if (!memcmp(lens.incoming.data(), lens.pixels.data(), lens.pixels.size())) return Change::None;
    const LONG width = lens.rect.right - lens.rect.left, height = lens.rect.bottom - lens.rect.top;
    const size_t row = static_cast<size_t>(width) * 4;
    if (g_settings.log) {
        g_changed = { LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN };
        auto pixel = [&](std::vector<BYTE> const& v, LONG x, LONG y) { return *reinterpret_cast<uint32_t const*>(v.data() + y * row + x * 4); };
        for (LONG y = 0; y < height; ++y) {
            for (LONG x = 0; x < width; ++x) {
                if (pixel(lens.incoming, x, y) == pixel(lens.pixels, x, y)) continue;
                g_changed = { std::min(g_changed.left, x), std::min(g_changed.top, y), std::max(g_changed.right, x + 1), std::max(g_changed.bottom, y + 1) };
            }
        }
        OffsetRect(&g_changed, lens.rect.left, lens.rect.top);
    }
    RECT behind{};
    if (!IntersectRect(&behind, &dock, &lens.rect)) return Change::Around;
    const size_t from = static_cast<size_t>(behind.left - lens.rect.left) * 4, bytes = static_cast<size_t>(behind.right - behind.left) * 4;
    for (LONG y = behind.top; y < behind.bottom; ++y) {
        const size_t at = (y - lens.rect.top) * row + from;
        if (memcmp(lens.incoming.data() + at, lens.pixels.data() + at, bytes)) return Change::Behind;
    }
    return Change::Around;
}

// Laid out as glass.hlsl's constant buffer.
struct Constants {
    float centre[2];
    float halfSize[2];
    float captureOrigin[2];
    float captureScale[2];
    float corner;
    float exponent;
    float reach;
    float frost;
    float shadowScale;
    float lift;
    float alpha;
    float unused;
    float fill[4];
};
static_assert(sizeof(Constants) == 80, "glass.hlsl's cbuffer Glass");

// The render thread's Direct3D objects, made again for every new device.
struct Gpu {
    unsigned id = 0;
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    com_ptr<IDXGISwapChain2> swapChain;
    HANDLE frameReady = nullptr;    // signalled when the swap chain takes another frame
    com_ptr<ID3D11RenderTargetView> target;
    int targetWidth = 0, targetHeight = 0;
    com_ptr<ID3D11VertexShader> vertexShader;
    com_ptr<ID3D11PixelShader> pixelShader;
    com_ptr<ID3D11Buffer> constants;
    com_ptr<ID3D11SamplerState> sampler;
    com_ptr<ID3D11RasterizerState> rasterizer;
    com_ptr<ID3D11BlendState> over;  // premultiplied colour over what is drawn already
    com_ptr<ID3D11Texture2D> backdrop;
    com_ptr<ID3D11ShaderResourceView> backdropView;
    int backdropWidth = 0, backdropHeight = 0;
    // The closing Start menu's picture and the window it is drawn in.
    com_ptr<IDXGISwapChain1> ghostChain;
    com_ptr<ID3D11RenderTargetView> ghostTarget;
    int ghostWidth = 0, ghostHeight = 0;
    com_ptr<ID3D11PixelShader> ghostShader;
    com_ptr<ID3D11Buffer> ghostConstants;
    com_ptr<ID3D11Texture2D> picture;
    com_ptr<ID3D11ShaderResourceView> pictureView;
    int pictureWidth = 0, pictureHeight = 0;
};

// Laid out as glass.hlsl's constant buffer Ghost.
struct GhostConstants {
    float menuRect[4];
    float barRect[4];
    float progress;
    float menuRadius;
    float barRadius;
    float exponent;
    float fadeIn;
    float cornerScale;
    float unused[2];
};
static_assert(sizeof(GhostConstants) == 64, "glass.hlsl's cbuffer Ghost");

// Takes the device and swap chains GlassDock's thread made; false when they do not work.
bool Setup(Gpu& gpu, unsigned id, com_ptr<ID3D11Device> const& device, com_ptr<IDXGISwapChain1> const& swapChain,
           com_ptr<IDXGISwapChain1> const& ghostChain) {
    if (gpu.frameReady) CloseHandle(gpu.frameReady);
    gpu = Gpu{};
    gpu.id = id;
    if (!device || !swapChain) return false;
    gpu.device = device;
    device->GetImmediateContext(gpu.context.put());
    gpu.swapChain = swapChain.try_as<IDXGISwapChain2>();
    if (!gpu.swapChain) return false;
    gpu.swapChain->SetMaximumFrameLatency(1);
    gpu.frameReady = gpu.swapChain->GetFrameLatencyWaitableObject();
    DXGI_SWAP_CHAIN_DESC1 desc{};
    gpu.swapChain->GetDesc1(&desc);
    gpu.targetWidth = desc.Width;
    gpu.targetHeight = desc.Height;
    HRESULT hr = device->CreateVertexShader(g_fullScreen, sizeof(g_fullScreen), nullptr, gpu.vertexShader.put());
    if (SUCCEEDED(hr)) hr = device->CreatePixelShader(g_glass, sizeof(g_glass), nullptr, gpu.pixelShader.put());
    D3D11_BUFFER_DESC buffer{ sizeof(Constants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    if (SUCCEEDED(hr)) hr = device->CreateBuffer(&buffer, nullptr, gpu.constants.put());
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(hr)) hr = device->CreateSamplerState(&sampler, gpu.sampler.put());
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    rasterizer.ScissorEnable = TRUE;
    if (SUCCEEDED(hr)) hr = device->CreateRasterizerState(&rasterizer, gpu.rasterizer.put());
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) hr = device->CreateBlendState(&blend, gpu.over.put());
    gpu.ghostChain = ghostChain;
    if (SUCCEEDED(hr)) hr = device->CreatePixelShader(g_ghost, sizeof(g_ghost), nullptr, gpu.ghostShader.put());
    D3D11_BUFFER_DESC ghostBuffer{ sizeof(GhostConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    if (SUCCEEDED(hr)) hr = device->CreateBuffer(&ghostBuffer, nullptr, gpu.ghostConstants.put());
    if (FAILED(hr)) Log(L"glass setup failed: " + std::to_wstring(hr));
    return SUCCEEDED(hr);
}

// Puts the capture the glass is drawn from on the GPU.
bool Upload(Gpu& gpu) {
    const int w = lens.rect.right - lens.rect.left, h = lens.rect.bottom - lens.rect.top;
    if (w != gpu.backdropWidth || h != gpu.backdropHeight) {
        gpu.backdropView = nullptr;
        gpu.backdrop = nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(gpu.device->CreateTexture2D(&desc, nullptr, gpu.backdrop.put())) ||
            FAILED(gpu.device->CreateShaderResourceView(gpu.backdrop.get(), nullptr, gpu.backdropView.put()))) {
            gpu.backdropWidth = gpu.backdropHeight = 0;
            return false;
        }
        gpu.backdropWidth = w;
        gpu.backdropHeight = h;
    }
    gpu.context->UpdateSubresource(gpu.backdrop.get(), 0, nullptr, lens.pixels.data(), w * 4, 0);
    return true;
}

// The superellipse exponent whose corner runs through the middle of the smoothed corner's arc,
// (1 - 1/sqrt 2) x radius in from both edges, while starting (1 + smoothing) x radius from the
// corner like it: 2 (a circular arc) without smoothing, about 3.4 with iOS's 60 %.
float Exponent(float smoothing) {
    return -1.0f / std::log2(1.0f - (1.0f - 0.70710678f) / (1.0f + smoothing));
}

// One thing to draw, in screen px: glass, or a flat plate when `fill` has an alpha.
struct Element {
    float left, top, right, bottom;
    float radius;
    float smoothing;
    float frost = 0, lift = 0, alpha = 1;
    float fill[4] = {};
};

// Draws `elements` back to front and presents them. False when the device is gone.
bool Draw(Gpu& gpu, Placement const& p, Element const* elements, int count) {
    const int windowWidth = p.window.right - p.window.left, windowHeight = p.window.bottom - p.window.top;
    if (windowWidth != gpu.targetWidth || windowHeight != gpu.targetHeight) {
        gpu.target = nullptr;
        gpu.context->OMSetRenderTargets(0, nullptr, nullptr);
        gpu.context->Flush();
        if (FAILED(gpu.swapChain->ResizeBuffers(0, windowWidth, windowHeight, DXGI_FORMAT_UNKNOWN,
                                                DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))) {
            return false;
        }
        gpu.targetWidth = windowWidth;
        gpu.targetHeight = windowHeight;
    }
    if (!gpu.target) {
        com_ptr<ID3D11Texture2D> buffer;
        if (FAILED(gpu.swapChain->GetBuffer(0, IID_PPV_ARGS(buffer.put()))) ||
            FAILED(gpu.device->CreateRenderTargetView(buffer.get(), nullptr, gpu.target.put()))) {
            return false;
        }
    }
    WaitForSingleObjectEx(gpu.frameReady, 100, FALSE);

    const float clear[4] = {};
    gpu.context->ClearRenderTargetView(gpu.target.get(), clear);
    D3D11_VIEWPORT viewport{ 0, 0, static_cast<float>(gpu.targetWidth), static_cast<float>(gpu.targetHeight), 0, 1 };
    ID3D11RenderTargetView* targets[] = { gpu.target.get() };
    gpu.context->OMSetRenderTargets(1, targets, nullptr);
    const float factor[4] = {};
    gpu.context->OMSetBlendState(gpu.over.get(), factor, 0xFFFFFFFF);
    gpu.context->RSSetViewports(1, &viewport);
    gpu.context->RSSetState(gpu.rasterizer.get());
    gpu.context->IASetInputLayout(nullptr);
    gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gpu.context->VSSetShader(gpu.vertexShader.get(), nullptr, 0);
    gpu.context->PSSetShader(gpu.pixelShader.get(), nullptr, 0);
    ID3D11Buffer* buffers[] = { gpu.constants.get() };
    gpu.context->PSSetConstantBuffers(0, 1, buffers);
    ID3D11ShaderResourceView* views[] = { gpu.backdropView.get() };
    gpu.context->PSSetShaderResources(0, 1, views);
    ID3D11SamplerState* samplers[] = { gpu.sampler.get() };
    gpu.context->PSSetSamplers(0, 1, samplers);

    for (int i = 0; i < count; ++i) {
        Element const& e = elements[i];
        const float x = e.left - p.window.left, y = e.top - p.window.top;
        const float w = e.right - e.left, h = e.bottom - e.top;
        if (w <= 0 || h <= 0 || (e.alpha <= 0 && e.fill[3] <= 0)) continue;
        // The corner: (1 + smoothing) x radius long, less smoothing first where there is no room.
        const float budget = std::min(w, h) / 2;
        const float radius = std::min(e.radius, budget);
        float smoothing = e.smoothing, corner = (1 + smoothing) * radius;
        if (corner > budget) {
            smoothing = radius > 0 ? std::max(0.0f, budget / radius - 1) : 0;
            corner = budget;
        }
        Constants c{};
        c.centre[0] = x + w / 2;
        c.centre[1] = y + h / 2;
        c.halfSize[0] = w / 2;
        c.halfSize[1] = h / 2;
        c.captureOrigin[0] = static_cast<float>(lens.rect.left - p.window.left);
        c.captureOrigin[1] = static_cast<float>(lens.rect.top - p.window.top);
        c.captureScale[0] = 1.0f / gpu.backdropWidth;
        c.captureScale[1] = 1.0f / gpu.backdropHeight;
        c.corner = corner;
        c.exponent = Exponent(smoothing);
        c.reach = OFFSET_SCALE * (p.monitor.bottom - p.monitor.top);
        c.frost = e.frost;
        c.shadowScale = g_settings.shadow;
        c.lift = e.lift;
        c.alpha = e.alpha;
        std::copy(e.fill, e.fill + 4, c.fill);
        constexpr float shadowReach = SHADOW_PAD + 8; // where the shadow has faded out
        D3D11_RECT scissor{ static_cast<LONG>(std::floor(x - shadowReach)), static_cast<LONG>(std::floor(y - shadowReach)),
                            static_cast<LONG>(std::ceil(x + w + shadowReach)), static_cast<LONG>(std::ceil(y + h + shadowReach)) };
        scissor.left = std::max(scissor.left, 0L);
        scissor.top = std::max(scissor.top, 0L);
        scissor.right = std::min(scissor.right, static_cast<LONG>(gpu.targetWidth));
        scissor.bottom = std::min(scissor.bottom, static_cast<LONG>(gpu.targetHeight));
        if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) continue;
        gpu.context->UpdateSubresource(gpu.constants.get(), 0, nullptr, &c, 0, 0);
        gpu.context->RSSetScissorRects(1, &scissor);
        gpu.context->Draw(3, 0);
    }
    const HRESULT hr = gpu.swapChain->Present(1, 0);
    return hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET;
}

bool Contains(RECT const& outer, RECT const& inner) {
    return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right && inner.bottom <= outer.bottom;
}

// ---- motion, as S1mp1e moves its highlights ----

// S1mp1e's Spring (liquidglass/client/animation/Spring.java): semi-implicit Euler with angular
// frequency `omega` (1/s) and damping ratio `zeta`, in steps of at most 1/120 s as the hotbar takes them.
struct Spring {
    float omega, zeta;
    float value = 0, velocity = 0, target = 0;

    void Snap(float v) {
        value = target = v;
        velocity = 0;
    }

    void Update(float dt) {
        while (dt > 0) {
            const float h = std::min(dt, 1.0f / 120);
            velocity += (-omega * omega * (value - target) - 2 * zeta * omega * velocity) * h;
            value += velocity * h;
            dt -= h;
        }
        if (!std::isfinite(value) || !std::isfinite(velocity)) Snap(target);
        if (std::fabs(value - target) < 0.01f && std::fabs(velocity) < 0.5f) Snap(target); // at rest: stop drawing
    }

    bool Moving() const { return value != target || velocity != 0; }
};

// S1mp1e's Fade: linear over its own time; here from wherever it is when it turns round.
struct Fade {
    float from = 0, to = 0, ms = 1;
    LONGLONG start = 0;

    float At(LONGLONG now) const {
        if (!start) return to;
        const float t = static_cast<float>(Seconds(now - start) * 1000) / ms;
        return t >= 1 ? to : from + (to - from) * t;
    }

    void Go(float target, float durationMs, LONGLONG now) {
        if (target == to) return;
        from = At(now);
        to = target;
        ms = durationMs;
        start = now;
    }

    bool Moving(LONGLONG now) const { return start && Seconds(now - start) * 1000 < ms; }
};

// A highlight that goes from button to button like S1mp1e's hotbar selection and inventory
// hover: two springs chase the new place, the leading one quick (omega 55) and the trailing one
// slower (30), both critically damped, so it stretches while it travels and gathers again where
// it arrives. It appears in place (fading in) when it had faded out, and fades out where it is left.
struct Glider {
    Spring lead{ 55, 1 }, trail{ 30, 1 };
    Fade fade;
    bool on = false;
    uint32_t button = 0;  // the one it is on

    void To(uint32_t to, float x, float fadeMs, LONGLONG now) {
        if (on && to == button) { // the same button moved (the row glides): move along with it
            const float shift = x - lead.target;
            lead.value += shift;
            trail.value += shift;
            lead.target = trail.target = x;
        } else if (on || fade.At(now) > 0.05f) {
            lead.target = trail.target = x;
        } else {
            lead.Snap(x);
            trail.Snap(x);
        }
        button = to;
        on = true;
        fade.Go(1, fadeMs, now);
    }

    void Off(float fadeMs, LONGLONG now) {
        on = false;
        fade.Go(0, fadeMs, now);
    }

    void Update(float dt) {
        lead.Update(dt);
        trail.Update(dt);
    }

    bool Moving(LONGLONG now) const { return fade.Moving(now) || lead.Moving() || trail.Moving(); }
    float Left() const { return std::min(lead.value, trail.value); }
    float Right() const { return std::max(lead.value, trail.value); }
};

// Where the icon of a button Explorer told about is centred across (screen px): Explorer centres
// it in the button (Start's logo in its window), rounding down.
float IconCentre(RECT const& button, int icon) {
    return button.left + (button.right - button.left - icon) / 2 + icon / 2.0f;
}

// ---- the Start menu going back into the dock ----
//
// start-glass grows the Start menu out of the dock when it opens. When it closes, the shell hides
// its window at once, so the dock shows it going back: while the menu is open, a picture of it is
// taken now and then (with the menu, without the windows of ours and the taskbar), and when the
// shell hides it, that picture shrinks back into the dock the way the menu grew out of it, in a
// window of its own over the menu's place. start-glass marks the menu's window with where the
// menu is (StartGlassMenu: four i16 from the low end, screen px) and its corner radius
// (StartGlassRadius, px).
//
// Opening and going back are one motion on one spring, and either can cut the other short: each
// side marks where its part set off from (start-glass the menu's window with StartGlassOpenAt/
// StartGlassOpenFrom, the dock its own with TaskbarDockCloseAt/TaskbarDockCloseFrom, a Motion
// each), and whichever takes over goes on from where the other had got to, at its speed. Closed
// while it still grows, the menu's picture goes back from there (the last picture taken, when
// there is none of this opening yet); opened while the picture goes back, the menu grows from it.

constexpr float CLOSE_W = 14;                     // the opening's spring (S1mp1e's recipe book's)
constexpr float GONE = 0.002f;                    // the picture is under the dock from this progress down
constexpr float PICTURE_AFTER_S = 0.7f;           // the first picture once the menu has grown,
constexpr float PICTURE_EVERY_S = 0.3f;           // and again this often while it is open

// A spring's motion as it set off: when (QueryPerformanceCounter, the same in every process),
// from what progress, how fast (progress per second).
struct Motion {
    LONGLONG at = 0;
    float from = 0, speed = 0;
};

// Where S1mp1e's critically damped spring `w`, set off as `motion` towards `target`, is `t`
// seconds later, and how fast it moves then.
void SpringAt(float w, Motion const& motion, float target, float t, float& x, float& v) {
    const float a = motion.from - target, b = motion.speed + w * a, e = std::exp(-w * t);
    x = target + (a + b * t) * e;
    v = (motion.speed - w * b * t) * e;
}

// A motion in two properties of `window`: `at`, and `from` with where from and how fast (two
// floats from the low end). `at` is written last and read on both sides of `from`. start-glass
// has the same two.
void PublishMotion(HWND window, wchar_t const* at, wchar_t const* from, Motion const& motion) {
    uint32_t x = 0, v = 0;
    std::memcpy(&x, &motion.from, sizeof(x));
    std::memcpy(&v, &motion.speed, sizeof(v));
    SetPropW(window, from, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(x) | static_cast<uintptr_t>(v) << 32));
    SetPropW(window, at, reinterpret_cast<HANDLE>(static_cast<intptr_t>(motion.at)));
}

bool ReadMotion(HWND window, wchar_t const* at, wchar_t const* from, Motion& motion) {
    for (int tries = 0; tries < 3; ++tries) {
        const auto when = reinterpret_cast<intptr_t>(GetPropW(window, at));
        const auto packed = reinterpret_cast<uintptr_t>(GetPropW(window, from));
        if (reinterpret_cast<intptr_t>(GetPropW(window, at)) != when) continue; // written meanwhile
        if (!when) return false;
        const auto x = static_cast<uint32_t>(packed), v = static_cast<uint32_t>(packed >> 32);
        motion.at = when;
        std::memcpy(&motion.from, &x, sizeof(x));
        std::memcpy(&motion.speed, &v, sizeof(v));
        return true;
    }
    return false;
}

struct StartMenu {
    HWND window = nullptr;          // the Start menu's window while it is open
    HWND hidden = nullptr;          // the one the shell hid last
    LONGLONG openedAt = 0, closedAt = 0;
    bool closed = false;            // the shell just hid it
    LONGLONG pictureAt = 0;         // when its picture was last taken (or tried; 0: lost with the device)
    RECT rect{};                    // where the picture goes back from (screen px)
    float radius = 0;               // its corner radius (px)
    Motion close;                   // the picture going back (at 0: not)
};

StartMenu g_start;

// Whether `hwnd` is the Start menu's window: start-glass marks it, but only once it has measured
// the menu, which may be in the first opening itself (the Start host just started); until then
// it is the core window of the Start host.
bool IsStartWindow(HWND hwnd) {
    static HWND known = nullptr;
    if (hwnd == known || GetPropW(hwnd, L"StartGlassMenu")) {
        known = hwnd;
        return true;
    }
    wchar_t name[64] = {};
    if (!GetClassNameW(hwnd, name, 64) || std::wcscmp(name, L"Windows.UI.Core.CoreWindow") != 0) return false;
    DWORD id = 0;
    GetWindowThreadProcessId(hwnd, &id);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
    if (!process) return false;
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    const bool start = QueryFullProcessImageNameW(process, 0, path, &size) && std::wcsstr(path, L"\\StartMenuExperienceHost.exe");
    CloseHandle(process);
    if (start) known = hwnd;
    return start;
}

void CALLBACK OnCloakEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG, DWORD, DWORD) {
    if (object != OBJID_WINDOW || !IsStartWindow(hwnd)) return;
    if (event == EVENT_OBJECT_UNCLOAKED) {
        g_start.window = hwnd;
        g_start.openedAt = Now();
        g_start.closed = false;
        if (g_start.close.at) { // opened again while going back: start-glass grows it on from the picture
            g_start.close.at = 0;
            ShowWindowAsync(g.ghost, SW_HIDE);
        }
    } else if (hwnd == g_start.window) {
        g_start.window = nullptr;
        g_start.hidden = hwnd;
        g_start.closedAt = Now();
        g_start.closed = true;
    }
}

// Where start-glass has marked the menu in `window` (screen px).
RECT MenuRect(HWND window) {
    const auto packed = reinterpret_cast<uintptr_t>(GetPropW(window, L"StartGlassMenu"));
    auto part = [packed](int i) { return static_cast<LONG>(static_cast<int16_t>(static_cast<uint16_t>(packed >> (16 * i)))); };
    return { part(0), part(1), part(2), part(3) };
}

// The shell hid the Start menu: its picture goes back into the dock from where start-glass had got
// the menu to, when there is a picture of it (of its size) and the dock is there; else it is just
// gone. Either way the dock marks where the going back set off from, for the next opening.
void StartClosing(Gpu const& gpu, bool shown) {
    Motion close{ g_start.closedAt, 0, 0 };
    const RECT rect = MenuRect(g_start.hidden);
    const int w = rect.right - rect.left, h = rect.bottom - rect.top;
    Motion open;
    const bool marked = ReadMotion(g_start.hidden, L"StartGlassOpenAt", L"StartGlassOpenFrom", open);
    if (g_settings.log) {
        Log(L"Start closed: shown " + std::to_wstring(shown) + L", picture " + std::to_wstring(gpu.pictureWidth) + L"x" +
            std::to_wstring(gpu.pictureHeight) + L" (menu " + std::to_wstring(w) + L"x" + std::to_wstring(h) + L"), opening " +
            (marked ? std::to_wstring(Seconds(g_start.closedAt - open.at)) + L" s ago from " + std::to_wstring(open.from) +
                          L", " + std::to_wstring(Seconds(open.at - g_start.openedAt)) + L" s after the uncloak"
                    : std::wstring(L"unmarked")));
    }
    // start-glass's opening must be this one (it marks it as it starts): else the menu was not seen yet.
    if (shown && gpu.pictureView && w == gpu.pictureWidth && h == gpu.pictureHeight && marked && Seconds(g_start.openedAt - open.at) < 0.1) {
        float x = 0, v = 0;
        SpringAt(CLOSE_W, open, 1, static_cast<float>(Seconds(g_start.closedAt - open.at)), x, v);
        if (x > GONE) {
            close.from = std::min(x, 1.0f);
            close.speed = v;
            g_start.close = close;
            g_start.rect = rect;
            g_start.radius = static_cast<float>(reinterpret_cast<intptr_t>(GetPropW(g_start.hidden, L"StartGlassRadius")));
            SetWindowPos(g.ghost, HWND_TOPMOST, rect.left, rect.top, w, h, SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        }
    }
    PublishMotion(g.hwnd, L"TaskbarDockCloseAt", L"TaskbarDockCloseFrom", close);
    if (g_settings.log) {
        Log(L"Start closed at " + std::to_wstring(close.from) + L" moving " + std::to_wstring(close.speed) +
            (g_start.close.at == close.at && close.from > 0 ? L": going back" : L": gone"));
    }
}

// Takes the open Start menu's picture onto the GPU.
void TakePicture(Gpu& gpu, HWND taskbar) {
    const RECT rect = MenuRect(g_start.window);
    std::vector<HWND> skip{ lens.host, g.hwnd, g.ghost };
    if (taskbar) skip.push_back(taskbar);
    g_start.pictureAt = Now(); // tried; again in a while if it failed
    if (!CaptureWithout(rect, skip)) {
        Log(L"Start picture failed at " + std::to_wstring(rect.left) + L"," + std::to_wstring(rect.top) + L".." +
            std::to_wstring(rect.right) + L"," + std::to_wstring(rect.bottom));
        return;
    }
    Log(L"Start picture " + std::to_wstring(rect.right - rect.left) + L"x" + std::to_wstring(rect.bottom - rect.top));
    const int w = rect.right - rect.left, h = rect.bottom - rect.top;
    if (w != gpu.pictureWidth || h != gpu.pictureHeight) {
        gpu.pictureView = nullptr;
        gpu.picture = nullptr;
        gpu.pictureWidth = gpu.pictureHeight = 0;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(gpu.device->CreateTexture2D(&desc, nullptr, gpu.picture.put())) ||
            FAILED(gpu.device->CreateShaderResourceView(gpu.picture.get(), nullptr, gpu.pictureView.put()))) {
            return;
        }
        gpu.pictureWidth = w;
        gpu.pictureHeight = h;
    }
    gpu.context->UpdateSubresource(gpu.picture.get(), 0, nullptr, lens.incoming.data(), w * 4, 0);
}

// Draws the menu's picture at `progress` (1: the menu, 0: the dock-sized shape on top of the
// dock, where start-glass starts the menu from) and presents it. False when the device is gone.
bool DrawGhost(Gpu& gpu, Placement const& p, float progress) {
    const int w = gpu.pictureWidth, h = gpu.pictureHeight;
    if (!gpu.ghostChain || !gpu.pictureView || w <= 0 || h <= 0) return true;
    if (w != gpu.ghostWidth || h != gpu.ghostHeight) {
        gpu.ghostTarget = nullptr;
        gpu.context->OMSetRenderTargets(0, nullptr, nullptr);
        gpu.context->Flush();
        if (FAILED(gpu.ghostChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) return false;
        gpu.ghostWidth = w;
        gpu.ghostHeight = h;
    }
    if (!gpu.ghostTarget) {
        com_ptr<ID3D11Texture2D> buffer;
        if (FAILED(gpu.ghostChain->GetBuffer(0, IID_PPV_ARGS(buffer.put()))) ||
            FAILED(gpu.device->CreateRenderTargetView(buffer.get(), nullptr, gpu.ghostTarget.put()))) {
            return false;
        }
    }
    GhostConstants c{};
    c.menuRect[2] = static_cast<float>(w);
    c.menuRect[3] = static_cast<float>(h);
    c.barRect[0] = p.left - g_start.rect.left;
    c.barRect[1] = h - p.height;
    c.barRect[2] = p.right - g_start.rect.left;
    c.barRect[3] = static_cast<float>(h);
    c.progress = progress;
    c.menuRadius = g_start.radius;
    c.barRadius = p.radius;
    c.exponent = Exponent(g_settings.smoothing);
    c.fadeIn = 0.4f;
    c.cornerScale = 1 + g_settings.smoothing;
    const float clear[4] = {};
    gpu.context->ClearRenderTargetView(gpu.ghostTarget.get(), clear);
    D3D11_VIEWPORT viewport{ 0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1 };
    D3D11_RECT all{ 0, 0, w, h };
    ID3D11RenderTargetView* targets[] = { gpu.ghostTarget.get() };
    gpu.context->OMSetRenderTargets(1, targets, nullptr);
    gpu.context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    gpu.context->RSSetViewports(1, &viewport);
    gpu.context->RSSetScissorRects(1, &all);
    gpu.context->RSSetState(gpu.rasterizer.get());
    gpu.context->IASetInputLayout(nullptr);
    gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gpu.context->VSSetShader(gpu.vertexShader.get(), nullptr, 0);
    gpu.context->PSSetShader(gpu.ghostShader.get(), nullptr, 0);
    gpu.context->UpdateSubresource(gpu.ghostConstants.get(), 0, nullptr, &c, 0, 0);
    ID3D11Buffer* buffers[] = { gpu.ghostConstants.get() };
    gpu.context->PSSetConstantBuffers(1, 1, buffers);
    ID3D11ShaderResourceView* views[] = { gpu.pictureView.get() };
    gpu.context->PSSetShaderResources(1, 1, views);
    ID3D11SamplerState* samplers[] = { gpu.sampler.get() };
    gpu.context->PSSetSamplers(0, 1, samplers);
    gpu.context->Draw(3, 0);
    const HRESULT hr = gpu.ghostChain->Present(1, 0);
    return hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET;
}

DWORD WINAPI RenderLoop(void*) {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // captures in screen px
    if (!MagInitialize()) {
        Log(L"MagInitialize failed");
        return 0;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = g_module;
    wc.lpszClassName = L"TaskbarDockLens";
    RegisterClassW(&wc);
    lens.host = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"",
                                WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_module, nullptr);
    SetLayeredWindowAttributes(lens.host, 0, 0, LWA_ALPHA); // never seen
    lens.control = CreateWindowExW(0, WC_MAGNIFIER, L"", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, lens.host, nullptr, g_module, nullptr);
    if (!lens.control || !MagSetImageScalingCallback(lens.control, OnCaptured)) {
        Log(L"no magnifier: " + std::to_wstring(GetLastError()));
        return 0;
    }
    ShowWindow(lens.host, SW_SHOWNOACTIVATE);
    // Windows that come to the front, open, move or shrink may change what is behind the dock.
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND, nullptr, OnSystemEvent, 0, 0,
                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // The shell showing and hiding the Start menu.
    SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, OnCloakEvent, 0, 0,
                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    Gpu gpu;
    bool haveBackdrop = false, dirty = false, shownBefore = false, busyBefore = false;
    LONGLONG lastCapture = 0, lastFrame = 0;
    LONGLONG selectionLost = 0; // since when no app has been active (0: one is)
    int interval = 0;         // ms between captures
    int unchanged = 0;        // captures in a row that found nothing new
    unsigned drawn = 0, drawnLit = 0; // the placement and highlights last drawn
    int captures = 0, changes = 0, frames = 0;
    LONGLONG statsSince = Now();

    Glider hover, active;

    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);

        Placement p;
        Highlights hovers, actives;
        unsigned placed = 0, gpuId = 0, lit = 0;
        com_ptr<ID3D11Device> device;
        com_ptr<IDXGISwapChain1> swapChain, ghostChain;
        AcquireSRWLockShared(&s.lock);
        p = s.placement;
        placed = s.placed;
        gpuId = s.gpu;
        hovers = s.hovers;
        actives = s.actives;
        lit = s.lit;
        if (gpuId != gpu.id) {
            device = s.device;
            swapChain = s.swapChain;
            ghostChain = s.ghostChain;
        }
        ReleaseSRWLockShared(&s.lock);
        if (gpuId != gpu.id) {
            Setup(gpu, gpuId, device, swapChain, ghostChain);
            haveBackdrop = false;
            dirty = true;
            g_start.pictureAt = 0; // the picture was on the old device
        }

        const bool shown = gpu.pixelShader && IsWindowVisible(g.hwnd) && p.right > p.left && p.window.right > p.window.left;
        if (g_start.closed) {
            g_start.closed = false;
            StartClosing(gpu, shown);
        }
        if (!shown) { // nothing to do until GlassDock's thread says otherwise
            // The highlights keep where they are: shown again, they carry on from there.
            shownBefore = false;
            if (g_start.close.at) {
                g_start.close.at = 0;
                ShowWindowAsync(g.ghost, SW_HIDE);
            }
            MsgWaitForMultipleObjects(1, &s.wake, FALSE, INFINITE, QS_ALLINPUT);
            continue;
        }
        if (!shownBefore) g_kick = true; // the desktop may have changed while it was hidden
        shownBefore = true;

        const LONGLONG now = Now();
        // Springs step by the real time between frames; the first frame of a motion by one refresh.
        const float dt = busyBefore ? std::min(static_cast<float>(Seconds(now - lastFrame)), 0.1f) : 1.0f / 240;
        lastFrame = now;

        // The dock's edges, and the windows that ride along with its left edge.
        float left = 0, right = 0;
        const bool gliding = Edges(p, now, left, right);
        if (gliding) {
            MoveLeftTo(p.list, static_cast<int>(std::lround(left + p.listOffset)));
            MoveLeftTo(p.start, static_cast<int>(std::lround(left + p.startOffset)));
        }

        // The highlights: where Explorer has its hover and active plates now.
        const float centreY = p.top + p.height / 2;
        const float half = p.icon / 2.0f + std::round(p.icon * 4 / 30.0f); // 38 px squares around 30 px icons
        // Hovered: the button under the pointer among those Explorer has a hover plate on (the
        // active app has one too, so the plate alone does not tell). Selected: the strongest
        // active plate (the old app's fades out as the new one's grows in; a pressed button's is weaker).
        POINT cursor{};
        GetCursorPos(&cursor);
        Highlight const* hot = hovers.Under(cursor, p.top, p.top + p.height);
        if (hot) hover.To(hot->id, IconCentre(hot->rect, p.icon), HOVER_IN_MS, now);
        else if (hover.on) hover.Off(HOVER_OUT_MS, now);
        // Selected: the strongest active plate. Explorer animates them as the user switches apps:
        // the new app's grows up from the bottom while the old one's shrinks and fades away, so
        // the selection stays with the one it is on until another is clearly stronger. Switching
        // by clicking also leaves none active for a moment (the taskbar takes the click first):
        // the selection waits there for the new one and slides over; only when none comes (the
        // desktop) does it fade out.
        Highlight const* selected = actives.Strongest(0.5f);
        if (selected && active.on && selected->id != active.button) {
            Highlight const* current = actives.Find(active.button);
            if (current && current->fade >= 0.5f && selected->fade < current->fade + 0.1f) selected = current;
        }
        if (selected) {
            active.To(selected->id, IconCentre(selected->rect, p.icon), SELECT_FADE_MS, now);
            selectionLost = 0;
        } else if (active.on) {
            if (!selectionLost) selectionLost = now;
            if (Seconds(now - selectionLost) * 1000 >= SELECT_GRACE_MS) active.Off(SELECT_FADE_MS, now);
        }
        if (g_settings.log && lit != drawnLit) {
            Log(L"targets: hover " + (hot ? std::to_wstring(hot->id) + L" x " + std::to_wstring(IconCentre(hot->rect, p.icon)) : L"-") +
                L", active " + (selected ? std::to_wstring(selected->id) : L"-") + L" (hovers " + std::to_wstring(hovers.count) + L")");
        }

        hover.Update(dt);
        active.Update(dt);

        // The Start menu: its picture now and then while it is open; when the shell hides it,
        // the picture going back into the dock.
        if (g_start.window && Seconds(now - g_start.openedAt) >= PICTURE_AFTER_S &&
            (g_start.pictureAt < g_start.openedAt || Seconds(now - g_start.pictureAt) >= PICTURE_EVERY_S)) {
            TakePicture(gpu, p.taskbar);
        }
        float shrink = -1; // the picture's progress this frame (-1: none)
        if (g_start.close.at) {
            const float t = static_cast<float>(Seconds(now - g_start.close.at));
            float v = 0;
            SpringAt(CLOSE_W, g_start.close, 0, t, shrink, v);
            if (shrink <= GONE || t > 2) { // under the dock
                g_start.close.at = 0;
                shrink = -1;
                ShowWindowAsync(g.ghost, SW_HIDE);
            }
        }
        if (shrink >= 0) {
            if (!DrawGhost(gpu, p, std::min(shrink, 1.0f))) {
                Log(L"device lost");
                g_start.close.at = 0;
                ShowWindowAsync(g.ghost, SW_HIDE);
                Setup(gpu, gpu.id, nullptr, nullptr, nullptr);
                haveBackdrop = false;
                PostMessageW(g.hwnd, WM_GPU_LOST, 0, 0);
                continue;
            }
            if (!IsWindowVisible(g.ghost)) ShowWindowAsync(g.ghost, SW_SHOWNOACTIVATE); // its first frame is in
        }
        const bool busy = gliding || hover.Moving(now) || active.Moving(now) || shrink >= 0;

        // What the glass needs: the desktop behind the dock at both ends of its glide, and as far
        // around it as the refraction reaches.
        const int reach = static_cast<int>(std::ceil(OFFSET_SCALE * (p.monitor.bottom - p.monitor.top) * EDGE_FACTOR * RED_SPREAD +
                                                     g_settings.frost)) + 2;
        const float spanLeft = gliding ? std::min(p.left, p.fromLeft) : p.left;
        const float spanRight = gliding ? std::max(p.right, p.fromRight) : p.right;
        const RECT dock{ static_cast<LONG>(std::floor(spanLeft)), static_cast<LONG>(p.top), static_cast<LONG>(std::ceil(spanRight)),
                         static_cast<LONG>(p.top + p.height) };
        RECT need{ dock.left - reach, dock.top - reach, dock.right + reach, dock.bottom + reach };
        IntersectRect(&need, &need, &p.monitor);
        const bool inPlace = haveBackdrop && Contains(lens.rect, need);
        const int active_ms = 1000 / g_settings.fps;
        const int sinceCapture = static_cast<int>(Seconds(now - lastCapture) * 1000);
        if (!inPlace || (!busy && sinceCapture >= (g_kick || g_dragging ? active_ms : interval))) {
            RECT rect = lens.rect;
            if (!inPlace) { // a new place, with room to slide
                rect = { need.left - SLACK, need.top, need.right + SLACK, need.bottom };
                IntersectRect(&rect, &rect, &p.monitor);
            }
            g_kick = false;
            lastCapture = now;
            ++captures;
            if (Capture(rect, p.taskbar)) {
                const Change change = !inPlace || !EqualRect(&rect, &lens.rect) ? Change::Behind : Compare(dock);
                if (change != Change::None) {
                    lens.pixels.swap(lens.incoming);
                    lens.rect = rect;
                    haveBackdrop = Upload(gpu);
                    dirty = true;
                    ++changes;
                    unchanged = 0;
                    interval = change == Change::Behind ? active_ms : std::max(active_ms, AROUND_MS);
                } else if (++unchanged >= 8) {
                    interval = IDLE_MS;
                }
            } else {
                Log(L"capture failed at " + std::to_wstring(rect.left) + L"," + std::to_wstring(rect.top));
                interval = IDLE_MS;
            }
        }

        if (placed != drawn || lit != drawnLit || busy || busyBefore) dirty = true; // and once more where a motion ends
        busyBefore = busy;
        if (!gliding) { // settled (GlassDock's thread places them when nothing glides)
            MoveLeftTo(p.list, static_cast<int>(std::lround(p.left + p.listOffset)));
            MoveLeftTo(p.start, static_cast<int>(std::lround(p.left + p.startOffset)));
        }
        if (dirty && haveBackdrop) {
            Element elements[4];
            int count = 0;
            // The dock: S1mp1e's hotbar strip (frosted, no lift).
            elements[count++] = { left, p.top, right, p.top + p.height, p.radius, g_settings.smoothing, g_settings.frost, 0, 1 };
            // The hover highlight: Explorer's white square with its 6 px corners (circular).
            const float white = HOVER_WHITE * hover.fade.At(now);
            if (white > 0) {
                Element plate{ hover.Left() - half, centreY - half, hover.Right() + half, centreY + half, std::round(p.icon / 5.0f), 0 };
                std::fill(plate.fill, plate.fill + 4, white);
                elements[count++] = plate;
            }
            // The active app, on top: the hotbar's selection, sharp and lifted, corners half its half size.
            const float activeFade = active.fade.At(now);
            if (activeFade > 0) {
                elements[count++] = { active.Left() - half, centreY - half, active.Right() + half, centreY + half, half / 2,
                                      g_settings.smoothing, 0, SELECTION_LIFT, activeFade };
            }
            if (!Draw(gpu, p, elements, count)) {
                Log(L"device lost");
                Setup(gpu, gpu.id, nullptr, nullptr, nullptr); // until GlassDock's thread makes a new one
                haveBackdrop = false;
                PostMessageW(g.hwnd, WM_GPU_LOST, 0, 0);
                continue;
            }
            drawn = placed;
            drawnLit = lit;
            dirty = false;
            ++frames;
            // Moving: a frame per screen refresh; wait until the desktop has shown this one (the
            // swap chain's waitable does not pace a composition swap chain).
            if (busy) DwmFlush();
        }

        if (g_settings.log && Seconds(Now() - statsSince) >= 10) {
            Log(L"10 s: " + std::to_wstring(captures) + L" captures, " + std::to_wstring(changes) + L" changed, " +
                std::to_wstring(frames) + L" frames; capture " + std::to_wstring(lens.rect.left) + L"," + std::to_wstring(lens.rect.top) +
                L" " + std::to_wstring(lens.rect.right - lens.rect.left) + L"x" + std::to_wstring(lens.rect.bottom - lens.rect.top) +
                L"; last change " + std::to_wstring(g_changed.left) + L"," + std::to_wstring(g_changed.top) + L".." +
                std::to_wstring(g_changed.right) + L"," + std::to_wstring(g_changed.bottom));
            captures = changes = frames = 0;
            statsSince = Now();
        }

        // Until the next frame while something moves (Draw waits for it), every few ms while the
        // pointer is on the icons (to see it leave them), else until the next capture.
        DWORD wait = 0;
        if (!busy) {
            const int due = (g_kick || g_dragging ? active_ms : interval) - static_cast<int>(Seconds(Now() - lastCapture) * 1000);
            int until = hover.on ? std::min(due, 8) : due;
            if (active.on && selectionLost) { // wake up when the selection's wait is over
                until = std::min(until, static_cast<int>(SELECT_GRACE_MS - Seconds(Now() - selectionLost) * 1000) + 1);
            }
            if (g_start.window) { // and for the open Start menu's next picture
                const float next = g_start.pictureAt >= g_start.openedAt
                                       ? static_cast<float>(Seconds(g_start.pictureAt - g_start.openedAt)) + PICTURE_EVERY_S
                                       : PICTURE_AFTER_S;
                until = std::min(until, static_cast<int>((next - Seconds(Now() - g_start.openedAt)) * 1000) + 1);
            }
            wait = static_cast<DWORD>(std::max(until, 1));
        }
        MsgWaitForMultipleObjects(1, &s.wake, FALSE, wait, QS_ALLINPUT);
    }
}

// ---- GlassDock's thread ----

constexpr UINT_PTR RECHECK_TIMER = 1;
constexpr UINT_PTR STALE_TIMER = 2;

// A highlight from Explorer (`list`: hovers or actives): wParam the button's rectangle on the
// screen (four i16 from the low end: left, top, right, bottom), lParam which button (low 32
// bits) and how far its plate has faded in (0..255 in bits 32..40; 0: gone).
void OnHighlight(Highlights& list, WPARAM wparam, LPARAM lparam) {
    auto part = [&](int i) { return static_cast<LONG>(static_cast<int16_t>(static_cast<uint16_t>(wparam >> (16 * i)))); };
    const uint32_t id = static_cast<uint32_t>(lparam);
    const float fade = static_cast<float>((static_cast<uint64_t>(lparam) >> 32) & 0xFF) / 255.0f;
    AcquireSRWLockExclusive(&s.lock);
    int i = 0;
    while (i < list.count && list.items[i].id != id) ++i;
    if (fade <= 0) {
        if (i < list.count) {
            std::copy(list.items + i + 1, list.items + list.count, list.items + i);
            --list.count;
        }
    } else {
        if (i == list.count) {
            if (list.count < MAX_HIGHLIGHTS) {
                ++list.count;
            } else { // the one heard of longest ago makes room
                i = static_cast<int>(std::min_element(list.items, list.items + MAX_HIGHLIGHTS,
                    [](Highlight const& a, Highlight const& b) { return a.updated < b.updated; }) - list.items);
            }
        }
        list.items[i] = { id, RECT{ part(0), part(1), part(2), part(3) }, fade, GetTickCount64() };
    }
    ++s.lit;
    const bool any = s.hovers.count + s.actives.count > 0;
    ReleaseSRWLockExclusive(&s.lock);
    if (g_settings.log) {
        Log(std::wstring(&list == &s.hovers ? L"hover " : L"active ") + std::to_wstring(id) + L" fade " +
            std::to_wstring(static_cast<int>(fade * 255)) + L" at " + std::to_wstring(part(0)) + L".." + std::to_wstring(part(2)) +
            ((static_cast<uint64_t>(lparam) >> 40) & 1 ? L" start" : L""));
    }
    SetEvent(s.wake);
    if (any) SetTimer(g.hwnd, STALE_TIMER, 1000, nullptr);
}

// Hover highlights Explorer stopped telling about (it restarted, say) go once the pointer is
// elsewhere; active plates that went unmentioned for a minute go too.
void DropStale() {
    POINT cursor{};
    GetCursorPos(&cursor);
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&s.lock);
    bool dropped = false;
    auto keep = [&](Highlights& list, auto&& alive) {
        int kept = 0;
        for (int i = 0; i < list.count; ++i) {
            if (alive(list.items[i])) list.items[kept++] = list.items[i];
        }
        dropped |= kept != list.count;
        list.count = kept;
    };
    keep(s.hovers, [&](Highlight const& h) {
        RECT around = h.rect;
        InflateRect(&around, 8, 8);
        return now - h.updated < 2000 || PtInRect(&around, cursor);
    });
    keep(s.actives, [&](Highlight const& h) { return now - h.updated < 60000 || IsWindow(GetForegroundWindow()); });
    if (dropped) ++s.lit;
    const bool any = s.hovers.count + s.actives.count > 0;
    ReleaseSRWLockExclusive(&s.lock);
    if (dropped) SetEvent(s.wake);
    if (!any) KillTimer(g.hwnd, STALE_TIMER);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_HOVER_GLASS || message == WM_SELECT_GLASS) {
        OnHighlight(message == WM_HOVER_GLASS ? s.hovers : s.actives, wparam, lparam);
        return 0;
    }
    if (message == WM_TIMER && wparam == STALE_TIMER) {
        DropStale();
        return 0;
    }
    if (message == WM_TIMER && wparam == RECHECK_TIMER) {
        KillTimer(hwnd, RECHECK_TIMER);
        Apply();
        return 0;
    }
    if (message == WM_GPU_LOST) {
        MakeGpu(g.bounds.right - g.bounds.left, g.bounds.bottom - g.bounds.top + SHADOW_PAD);
        return 0;
    }
    if (g.shellHook && message == g.shellHook) {
        switch (wparam) {
        case HSHELL_RUDEAPPACTIVATED: // Windows 10 sends this for most activations, full screen or not
        case HSHELL_WINDOWACTIVATED:
            Apply(); // out of the way at once if the window in front is full screen
            SetTimer(hwnd, RECHECK_TIMER, 300, nullptr); // again once Explorer has moved the taskbar
            break;
        case HSHELL_WINDOWCREATED: // a task button may come or go
        case HSHELL_WINDOWDESTROYED:
        case HSHELL_WINDOWREPLACED:
            if (g.notifyThread) PostThreadMessageW(g.notifyThread, WM_TASKS_CHANGED, 0, 0);
            break;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool Create() {
    if (g.hwnd) return true;
    LoadSettings(g_module);
    s.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = g_module;
    wc.lpszClassName = L"TaskbarDock";
    RegisterClassW(&wc);
    // Layered and transparent: clicks go through it, also where it reaches above the taskbar.
    g.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED |
                             WS_EX_TRANSPARENT, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_module, nullptr);
    if (!g.hwnd) return false;
    SetLayeredWindowAttributes(g.hwnd, 0, 255, LWA_ALPHA);

    // Composition on this (GlassDock's) thread needs a dispatcher queue, which its message
    // loop runs.
    if (!winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) {
        DispatcherQueueOptions options{ sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE };
        check_hresult(CreateDispatcherQueueController(options,
            reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(put_abi(g.queue))));
    }
    g.compositor = Compositor();
    check_hresult(g.compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>()->CreateDesktopWindowTarget(
        g.hwnd, FALSE, reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(put_abi(g.target))));
    g.glass = g.compositor.CreateSpriteVisual();
    g.target.Root(g.glass);

    // The window for the closing Start menu's picture: like the dock's, hidden until then.
    WNDCLASSW ghostClass{};
    ghostClass.lpfnWndProc = DefWindowProcW;
    ghostClass.hInstance = g_module;
    ghostClass.lpszClassName = L"TaskbarDockGhost";
    RegisterClassW(&ghostClass);
    g.ghost = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED |
                              WS_EX_TRANSPARENT, ghostClass.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_module, nullptr);
    if (g.ghost) {
        SetLayeredWindowAttributes(g.ghost, 0, 255, LWA_ALPHA);
        check_hresult(g.compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>()->CreateDesktopWindowTarget(
            g.ghost, FALSE, reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(put_abi(g.ghostTarget))));
    }
    g.ghostPicture = g.compositor.CreateSpriteVisual();
    g.ghostPicture.Size({ 4096, 4096 }); // the swap chain shows at its own size, from the top-left
    if (g.ghostTarget) g.ghostTarget.Root(g.ghostPicture);

    g.shellHook = RegisterWindowMessageW(L"SHELLHOOK");
    RegisterShellHookWindow(g.hwnd);
    return true;
}

} // namespace

// Places the dock: `shape` (screen pixels, corner radius `radius`) inside `bounds` (the taskbar's
// rectangle), right below the window `below`; hidden when `visible` is 0. `notifyThread` hears
// when windows come and go. `list` and `start` (either may be null), the task list and the Start
// button, are to have their left edges at `listX` and `startX` on the screen: when the dock
// glides to its new place, they ride along with its left edge. Returns 0 when the dock cannot be
// drawn.
extern "C" int __stdcall dock_place(HWND below, const RECT* bounds, const RECT* shape, int radius, int visible, DWORD notifyThread,
                                    HWND list, int listX, HWND start, int startX) noexcept try {
    if (!Create()) return 0;
    g.below = below;
    g.notifyThread = notifyThread;
    g.list = list;
    g.start = start;
    g.wanted = visible != 0 && below && IsWindow(below);
    if (g.wanted) {
        if (!EqualRect(&g.bounds, bounds)) {
            g.bounds = *bounds;
            const RECT window{ bounds->left, bounds->top - SHADOW_PAD, bounds->right, bounds->bottom };
            const int w = window.right - window.left, h = window.bottom - window.top;
            SetWindowPos(g.hwnd, nullptr, window.left, window.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
            g.glass.Size({ static_cast<float>(w), static_cast<float>(h) });
            MONITORINFO monitor{ sizeof(monitor) };
            GetMonitorInfoW(MonitorFromRect(bounds, MONITOR_DEFAULTTONEAREST), &monitor);
            AcquireSRWLockExclusive(&s.lock);
            s.placement.window = window;
            s.placement.monitor = monitor.rcMonitor;
            s.placement.icon = (24 * static_cast<int>(GetDpiForWindow(below)) + 48) / 96; // 24 px icons at 96 dpi
            ++s.placed;
            ReleaseSRWLockExclusive(&s.lock);
            if (!s.swapChain && MakeGpu(w, h) && !g.renderer) g.renderer = CreateThread(nullptr, 0, RenderLoop, nullptr, 0, nullptr);
        }
        if (shape->right > shape->left && shape->bottom > shape->top) {
            std::wstring glide;
            AcquireSRWLockExclusive(&s.lock);
            Placement& p = s.placement;
            const Placement before = p;
            const LONGLONG now = Now();
            float drawnLeft = 0, drawnRight = 0;
            Edges(p, now, drawnLeft, drawnRight);
            const float left = static_cast<float>(shape->left), right = static_cast<float>(shape->right);
            if (p.right <= p.left || !IsWindowVisible(g.hwnd)) {
                p.glideStart = 0; // first placement, or not on screen: no motion to show
            } else if (left != p.left || right != p.right) {
                // Buttons came or went (or Start and the icons moved over): both edges glide to
                // the new place together, from where they are drawn now.
                p.fromLeft = drawnLeft;
                p.fromRight = drawnRight;
                p.glideStart = now;
                glide = L"glide " + std::to_wstring(drawnLeft) + L".." + std::to_wstring(drawnRight) + L" -> " +
                        std::to_wstring(left) + L".." + std::to_wstring(right);
            }
            p.taskbar = below;
            p.left = left;
            p.right = right;
            p.top = static_cast<float>(shape->top);
            p.height = static_cast<float>(shape->bottom - shape->top);
            p.radius = static_cast<float>(radius);
            p.list = list;
            p.start = start;
            p.listOffset = list ? listX - left : 0;
            p.startOffset = start ? startX - left : 0;
            float l = 0, r = 0;
            const bool gliding = Edges(p, now, l, r);
            if (memcmp(&before, &p, sizeof(p))) ++s.placed;
            ReleaseSRWLockExclusive(&s.lock);
            if (!glide.empty()) Log(glide);
            if (!gliding) { // nothing to ride along with: there at once
                MoveLeftTo(list, listX);
                MoveLeftTo(start, startX);
            }
            // Where the dock settles (screen px, four i16 from the low end) and its corner radius,
            // for start-glass, which grows the Start menu out of it.
            auto part = [](LONG v) { return static_cast<uintptr_t>(static_cast<uint16_t>(static_cast<int16_t>(v))); };
            const uintptr_t packed = part(shape->left) | part(shape->top) << 16 | part(shape->right) << 32 | part(shape->bottom) << 48;
            SetPropW(g.hwnd, L"TaskbarDockShape", reinterpret_cast<HANDLE>(packed));
            SetPropW(g.hwnd, L"TaskbarDockRadius", reinterpret_cast<HANDLE>(static_cast<intptr_t>(radius)));
            SetEvent(s.wake);
        }
    }
    Apply();
    return 1;
} catch (hresult_error const& e) {
    Log(L"dock_place failed: " + std::wstring(e.message()));
    return 0;
} catch (...) {
    Log(L"dock_place failed");
    return 0;
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
