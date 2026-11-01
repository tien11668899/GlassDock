// start-glass.dll: a XAML Diagnostics TAP that start-glass.exe loads into StartMenuExperienceHost.
// It watches the Start menu's visual tree and
// - makes its acrylic background see-through (frosted glass) by lowering the tint of the
//   AcrylicBrush behind the menu;
// - rounds the menu's corners with Apple-style continuous curvature: the container of that
//   background gets a composition clip shaped like Figma's smoothed corners (60 % is the iOS
//   preset), kept in step with the menu's size;
// - gives everything inside one smaller radius with the same curvature (concentric: outer 16
//   less the 8 DIP inset of the navigation buttons) and one spacing rhythm: navigation
//   buttons, list highlights, tiles, the tile group title and the menus (context, power);
// - slides the menu out from under the dock when it opens (as wide as the dock at first, growing
//   to its own size, fading in, on S1mp1e's spring), and then grows its tiles in one after
//   another, the way S1mp1e's recipe book slides out and brings in its recipes.
//
// Settings come from start-glass.ini next to the DLL (one "key=value" per line):
//   tint=30          TintOpacity in percent (Windows uses about 90 for the dark Start menu)
//   luminosity=55    TintLuminosityOpacity in percent
//   radius=16        corner radius in DIPs (0: square corners)
//   smoothing=60     corner smoothing in percent (0: circular arcs, 60: iOS)
//   inner_radius=8   corner radius of everything inside: buttons, list rows, tiles (0: off)
//   nav_inset=8      navigation buttons are drawn this far inside their 48 DIP cells
//   row_gap=4        gap between the highlights of list rows
//   tile_gap=8       gap between tiles (Windows lays them out 4 apart)
//   row_trim=2       list highlights end this much short of their rows on the right
//   tile_shift=2     tiles move this much left (rounded up to whole pixels); with row_trim the
//                    navigation, list and tile columns and the right edge get one gap (20 DIPs)
//   targets=...      element names to change, comma separated (empty: every acrylic background)
//   open=1           the menu slides out from under the dock (0: Windows' own opening)
//   cascade=1        and then its tiles grow in one after another (0: they come with it)
//   log=1            append what was found to start-glass.log in the Start host's temp folder
//   dump=1           with log=1, also write a layout map of the menu and of each menu opened

#define NOMINMAX // keep std::min / std::max usable
#include <windows.h>
#include <unknwn.h>
#include <ocidl.h>
#include <xamlOM.h>
#include <corewindow.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <windows.graphics.interop.h>
#undef GetCurrentTime // a WinBase macro that breaks the C++/WinRT XAML headers
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>
#include <fstream>
#include <sstream>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include "clsid.h"

using namespace winrt;
using namespace winrt::Windows::UI::Composition;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Hosting;
using namespace winrt::Windows::UI::Xaml::Media;
using WinRTObject = winrt::Windows::Foundation::IInspectable; // not the ABI ::IInspectable

namespace {

struct Settings {
    double tint = 0.30;
    double luminosity = 0.55;
    float radius = 16;
    float smoothing = 0.6f;
    float innerRadius = 8;
    float navInset = 8;
    float rowGap = 4;
    float tileGap = 8;
    float rowTrim = 2;
    float tileShift = 2;
    std::vector<std::wstring> targets;
    bool open = true;
    bool cascade = true;
    bool log = false;
    bool dump = false;
};

Settings g_settings;

std::wstring ModuleDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring dir = path;
    return dir.substr(0, dir.find_last_of(L'\\'));
}

std::wstring Trim(std::wstring s) {
    const auto first = s.find_first_not_of(L" \t\r\n");
    const auto last = s.find_last_not_of(L" \t\r\n");
    return first == std::wstring::npos ? L"" : s.substr(first, last - first + 1);
}

void LoadSettings() {
    std::wifstream file(ModuleDir() + L"\\start-glass.ini");
    std::wstring line;
    while (std::getline(file, line)) {
        const auto eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        const std::wstring key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
        if (key == L"tint") g_settings.tint = _wtoi(value.c_str()) / 100.0;
        else if (key == L"luminosity") g_settings.luminosity = _wtoi(value.c_str()) / 100.0;
        else if (key == L"radius") g_settings.radius = static_cast<float>(_wtoi(value.c_str()));
        else if (key == L"smoothing") g_settings.smoothing = std::clamp(_wtoi(value.c_str()), 0, 100) / 100.0f;
        else if (key == L"inner_radius") g_settings.innerRadius = static_cast<float>(_wtoi(value.c_str()));
        else if (key == L"nav_inset") g_settings.navInset = static_cast<float>(_wtoi(value.c_str()));
        else if (key == L"row_gap") g_settings.rowGap = static_cast<float>(_wtoi(value.c_str()));
        else if (key == L"tile_gap") g_settings.tileGap = static_cast<float>(_wtoi(value.c_str()));
        else if (key == L"row_trim") g_settings.rowTrim = static_cast<float>(_wtoi(value.c_str()));
        else if (key == L"tile_shift") g_settings.tileShift = static_cast<float>(_wtoi(value.c_str()));
        else if (key == L"open") g_settings.open = value != L"0";
        else if (key == L"cascade") g_settings.cascade = value != L"0";
        else if (key == L"log") g_settings.log = value == L"1";
        else if (key == L"dump") g_settings.dump = value == L"1";
        else if (key == L"targets") {
            std::wstringstream list(value);
            std::wstring name;
            while (std::getline(list, name, L',')) {
                if (!Trim(name).empty()) g_settings.targets.push_back(Trim(name));
            }
        }
    }
}

// The Start host is an AppContainer: its own temp folder is writable.
void Log(std::wstring const& line) {
    if (!g_settings.log) return;
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    std::wofstream file(std::wstring(temp) + L"start-glass.log", std::ios::app);
    file << line << L"\n";
}

std::wstring Describe(WinRTObject const& object) {
    return object ? std::wstring(get_class_name(object)) : L"(null)";
}

bool IsTarget(std::wstring_view name) {
    if (g_settings.targets.empty()) return true;
    for (auto const& target : g_settings.targets) {
        if (name == target) return true;
    }
    return false;
}

// A rectangle with continuous-curvature corners, the way Figma draws "corner smoothing":
// each corner is a circular arc that shrinks as smoothing grows, eased into the straight
// edges by two cubic Béziers that start (1 + smoothing) x radius away from the corner.
// Composition asks for it as a Direct2D geometry through IGeometrySource2DInterop.
struct SmoothRoundedRect : implements<SmoothRoundedRect, winrt::Windows::Graphics::IGeometrySource2D, ABI::Windows::Graphics::IGeometrySource2DInterop> {
    SmoothRoundedRect(D2D1_RECT_F rect, float radius, float smoothing) : m_rect(rect), m_radius(radius), m_smoothing(smoothing) {}

    HRESULT STDMETHODCALLTYPE GetGeometry(ID2D1Geometry** value) noexcept override {
        com_ptr<ID2D1Factory> factory;
        const HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, factory.put());
        return SUCCEEDED(hr) ? Build(factory.get(), value) : hr;
    }

    HRESULT STDMETHODCALLTYPE TryGetGeometryUsingFactory(ID2D1Factory* factory, ID2D1Geometry** value) noexcept override {
        return Build(factory, value);
    }

private:
    HRESULT Build(ID2D1Factory* factory, ID2D1Geometry** value) const noexcept {
        *value = nullptr;
        com_ptr<ID2D1PathGeometry> path;
        com_ptr<ID2D1GeometrySink> sink;
        HRESULT hr = factory->CreatePathGeometry(path.put());
        if (SUCCEEDED(hr)) hr = path->Open(sink.put());
        if (FAILED(hr)) return hr;

        const float x0 = m_rect.left, y0 = m_rect.top;
        const float w = m_rect.right - m_rect.left, h = m_rect.bottom - m_rect.top;
        // Corner budget: half the shorter side. Too little room: give up smoothing first.
        const float budget = std::min(w, h) / 2;
        const float r = std::min(m_radius, budget);
        float s = m_smoothing, p = (1 + s) * r;
        if (p > budget) {
            s = std::max(0.0f, budget / r - 1);
            p = budget;
        }
        constexpr float degree = 3.14159265f / 180;
        const float arcMeasure = 90 * (1 - s);
        const float arc = std::sin(arcMeasure / 2 * degree) * r * std::sqrt(2.0f);
        const float alpha = (90 - arcMeasure) / 2;
        const float beta = 45 * s;
        const float c = r * std::tan(alpha / 2 * degree) * std::cos(beta * degree);
        const float d = c * std::tan(beta * degree);
        const float b = (p - arc - c - d) / 3;
        const float a = 2 * b;

        auto at = [&](float x, float y) { return D2D1::Point2F(x0 + x, y0 + y); };
        auto curve = [&](float x1, float y1, float x2, float y2, float x3, float y3) {
            sink->AddBezier(D2D1::BezierSegment(at(x1, y1), at(x2, y2), at(x3, y3)));
        };
        auto arcTo = [&](float x, float y) {
            sink->AddArc(D2D1::ArcSegment(at(x, y), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        };

        sink->BeginFigure(at(p, 0), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(at(w - p, 0)); // top edge, then the top-right corner
        curve(w - p + a, 0, w - p + a + b, 0, w - p + a + b + c, d);
        arcTo(w - d, p - a - b - c);
        curve(w, p - a - b, w, p - a, w, p);
        sink->AddLine(at(w, h - p)); // right edge, bottom-right corner
        curve(w, h - p + a, w, h - p + a + b, w - d, h - p + a + b + c);
        arcTo(w - p + a + b + c, h - d);
        curve(w - p + a + b, h, w - p + a, h, w - p, h);
        sink->AddLine(at(p, h)); // bottom edge, bottom-left corner
        curve(p - a, h, p - a - b, h, p - a - b - c, h - d);
        arcTo(d, h - p + a + b + c);
        curve(0, h - p + a + b, 0, h - p + a, 0, h - p);
        sink->AddLine(at(0, p)); // left edge, top-left corner
        curve(0, p - a, 0, p - a - b, d, p - a - b - c);
        arcTo(p - a - b - c, d);
        curve(p - a - b, 0, p - a, 0, p, 0);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        hr = sink->Close();
        if (SUCCEEDED(hr)) *value = path.detach();
        return hr;
    }

    D2D1_RECT_F m_rect;
    float m_radius;
    float m_smoothing;
};

std::wstring Thickness4(Thickness const& t) {
    std::wstringstream s;
    s << t.Left << L"," << t.Top << L"," << t.Right << L"," << t.Bottom;
    return s.str();
}

std::wstring Radius4(CornerRadius const& r) {
    std::wstringstream s;
    s << r.TopLeft << L"," << r.TopRight << L"," << r.BottomRight << L"," << r.BottomLeft;
    return s.str();
}

// Layout map for designing the inner rounding and spacing: every visible element under
// `node` with its rect relative to `origin`, margin, padding, corner radius and background.
// Names are developer x:Names; no text content is written.
void Dump(DependencyObject const& node, UIElement const& origin, int depth, int& budget) {
    if (budget <= 0 || depth > 40) return;
    auto element = node.try_as<FrameworkElement>();
    if (element && element.Visibility() == Visibility::Visible && element.ActualWidth() > 0 && element.ActualHeight() > 0) {
        --budget;
        const auto r = element.TransformToVisual(origin).TransformBounds(
            { 0, 0, static_cast<float>(element.ActualWidth()), static_cast<float>(element.ActualHeight()) });
        std::wstringstream line;
        line << L"D" << depth << L" " << std::wstring(depth, L' ') << get_class_name(element).c_str() << L" '" << element.Name().c_str() << L"' "
             << r.X << L"," << r.Y << L" " << r.Width << L"x" << r.Height;
        const auto margin = element.Margin();
        if (margin.Left || margin.Top || margin.Right || margin.Bottom) line << L" m=" << Thickness4(margin);
        Brush background{ nullptr };
        if (auto border = element.try_as<Border>()) {
            background = border.Background();
            if (border.Padding() != Thickness{}) line << L" p=" << Thickness4(border.Padding());
            if (border.CornerRadius() != CornerRadius{}) line << L" r=" << Radius4(border.CornerRadius());
        } else if (auto control = element.try_as<Control>()) {
            background = control.Background();
            if (control.Padding() != Thickness{}) line << L" p=" << Thickness4(control.Padding());
            if (control.CornerRadius() != CornerRadius{}) line << L" r=" << Radius4(control.CornerRadius());
        } else if (auto grid = element.try_as<Grid>()) {
            background = grid.Background();
            if (grid.Padding() != Thickness{}) line << L" p=" << Thickness4(grid.Padding());
            if (grid.CornerRadius() != CornerRadius{}) line << L" r=" << Radius4(grid.CornerRadius());
        } else if (auto presenter = element.try_as<ContentPresenter>()) {
            background = presenter.Background();
            if (presenter.Padding() != Thickness{}) line << L" p=" << Thickness4(presenter.Padding());
            if (presenter.CornerRadius() != CornerRadius{}) line << L" r=" << Radius4(presenter.CornerRadius());
        } else if (auto panel = element.try_as<Panel>()) {
            background = panel.Background();
        }
        if (background) line << L" bg=" << get_class_name(background).c_str();
        Log(line.str());
    }
    const int count = VisualTreeHelper::GetChildrenCount(node);
    for (int i = 0; i < count && budget > 0; ++i) {
        Dump(VisualTreeHelper::GetChild(node, i), origin, depth + 1, budget);
    }
}

// What composition draws for `element` besides its XAML content: a child visual (with a drop
// shadow, if it has one) and a ThemeShadow.
std::wstring Extras(UIElement const& element) {
    std::wstringstream line;
    try {
        if (auto child = ElementCompositionPreview::GetElementChildVisual(element)) {
            line << L" child=" << get_class_name(child).c_str() << L" " << child.Size().x << L"x" << child.Size().y << L" at "
                 << child.Offset().x << L"," << child.Offset().y;
            if (auto sprite = child.try_as<SpriteVisual>()) {
                if (auto shadow = sprite.Shadow().try_as<DropShadow>()) {
                    line << L" DROPSHADOW blur " << shadow.BlurRadius() << L" offset " << shadow.Offset().y << L" opacity " << shadow.Opacity();
                }
            }
            if (auto container = child.try_as<ContainerVisual>()) line << L" children " << container.Children().Count();
        }
        if (element.Shadow()) line << L" THEMESHADOW";
        if (element.Translation().z != 0) line << L" z=" << element.Translation().z;
        if (element.Opacity() != 1) line << L" opacity=" << element.Opacity();
    } catch (...) {
        line << L" (extras failed)";
    }
    return line.str();
}

// Everything around the menu: each ancestor of `container` up to the root, with all of its
// children (the one on the way marked with >), and what composition draws for each besides the
// XAML. The menu's shadow is drawn by one of these, not inside the menu.
void DumpAround(UIElement const& container) {
    DependencyObject node = container;
    for (int level = 1; level < 40; ++level) {
        auto parent = VisualTreeHelper::GetParent(node);
        if (!parent) break;
        auto element = parent.try_as<FrameworkElement>();
        std::wstringstream line;
        line << L"UP" << level << L" " << get_class_name(parent).c_str() << L" '" << (element ? element.Name().c_str() : L"") << L"'";
        if (element) {
            const auto r = element.TransformToVisual(container).TransformBounds(
                { 0, 0, static_cast<float>(element.ActualWidth()), static_cast<float>(element.ActualHeight()) });
            line << L" " << r.X << L"," << r.Y << L" " << r.Width << L"x" << r.Height << Extras(element);
        }
        Log(line.str());
        const int count = VisualTreeHelper::GetChildrenCount(parent);
        for (int i = 0; i < count; ++i) {
            auto child = VisualTreeHelper::GetChild(parent, i);
            auto sibling = child.try_as<FrameworkElement>();
            std::wstringstream entry;
            entry << L"   " << (child == node ? L">" : L" ") << L" " << get_class_name(child).c_str() << L" '"
                  << (sibling ? sibling.Name().c_str() : L"") << L"'";
            if (sibling) {
                const auto r = sibling.TransformToVisual(container).TransformBounds(
                    { 0, 0, static_cast<float>(sibling.ActualWidth()), static_cast<float>(sibling.ActualHeight()) });
                entry << L" " << r.X << L"," << r.Y << L" " << r.Width << L"x" << r.Height
                      << (sibling.Visibility() == Visibility::Visible ? L"" : L" collapsed") << Extras(sibling);
                const auto m = sibling.Margin();
                if (m.Left || m.Top || m.Right || m.Bottom) entry << L" m=" << Thickness4(m);
            }
            Log(entry.str());
        }
        node = parent;
    }
}

// Writes the layout map once, a few seconds after the menu first appeared (its lists and
// tiles are realised by then).
void ScheduleDump(UIElement const& container) {
    if (!g_settings.dump) return;
    DispatcherTimer timer;
    timer.Interval(std::chrono::seconds(4));
    timer.Tick([weakContainer = make_weak(container)](WinRTObject const& sender, WinRTObject const&) {
        sender.as<DispatcherTimer>().Stop();
        try {
            if (auto container = weakContainer.get()) {
                Log(L"=== around the menu");
                DumpAround(container);
                Log(L"=== layout dump");
                int budget = 2500;
                Dump(container, container, 0, budget);
                Log(L"=== end of dump");
            }
        } catch (...) {
            Log(L"dump failed");
        }
    });
    timer.Start();
}

struct Inset {
    float left, top, right, bottom;
};

// Elements already given a clip or a shift; the visual tree reports recycled list items again.
std::unordered_set<void*> g_clipped;
std::unordered_set<void*> g_shifted;

// Device pixels per DIP (1.25 at 125 %).
float Scale(UIElement const& element) {
    try {
        if (auto root = element.XamlRoot()) return static_cast<float>(root.RasterizationScale());
    } catch (...) {
    }
    return 1;
}

CompositionPathGeometry SmoothGeometry(Compositor const& compositor, D2D1_RECT_F const& rect, float radius) {
    auto shape = make<SmoothRoundedRect>(rect, radius, g_settings.smoothing);
    return compositor.CreatePathGeometry(CompositionPath(shape.as<winrt::Windows::Graphics::IGeometrySource2D>()));
}

// Draws `element` (and its content) only inside its own bounds less `inset`, and `trimRight`
// more on the right, with smoothed corners of `radius`, now and whenever it changes size.
// Purely visual: layout and input stay. The clip's edges land on whole device pixels so they
// stay crisp (a 2 DIP inset is 2.5 px at 125 %). Elements are drawn at whole pixels even where
// layout reports half ones (the app list), so this works in the element's own pixel grid:
// edges round half up, which keeps equal items equal in size and gap; the trim rounds up.
void ClipSmooth(FrameworkElement const& element, Inset inset, float radius, float trimRight = 0) {
    if (radius <= 0 || !g_clipped.insert(get_abi(element)).second) return;
    auto apply = [weak = make_weak(element), inset, radius, trimRight](auto&&...) {
        try {
            auto element = weak.get();
            if (!element) return;
            const float w = static_cast<float>(element.ActualWidth());
            const float h = static_cast<float>(element.ActualHeight());
            const float scale = Scale(element);
            auto snap = [scale](float v) { return std::floor(v * scale + 0.51f) / scale; };
            const float trim = std::ceil(trimRight * scale - 0.01f) / scale;
            const auto rect = D2D1::RectF(snap(inset.left), snap(inset.top), snap(w - inset.right) - trim, snap(h - inset.bottom));
            if (rect.right <= rect.left || rect.bottom <= rect.top) return;
            auto visual = ElementCompositionPreview::GetElementVisual(element);
            auto compositor = visual.Compositor();
            visual.Clip(compositor.CreateGeometricClip(SmoothGeometry(compositor, rect, radius)));
            if (g_settings.log) {
                std::wstringstream line;
                line << L"CLIP " << get_class_name(element).c_str() << L" '" << element.Name().c_str() << L"' " << w << L"x" << h
                     << L" clip px " << rect.left * scale << L"," << rect.top * scale << L".." << rect.right * scale << L"," << rect.bottom * scale;
                Log(line.str());
            }
        } catch (...) {
            Log(L"clip failed");
        }
    };
    element.SizeChanged(apply);
    apply();
}

// A menu (a tile's context menu, the power and account menus) gets the inner radius with
// smoothed corners. Its square 1 DIP border would be cut off at the corners, so it becomes a
// 1 px line along the same curve, in the border's colour.
void RoundMenu(Control const& presenter) {
    const float r = g_settings.innerRadius;
    if (r <= 0 || !g_clipped.insert(get_abi(presenter)).second) return;
    struct Outline {
        ShapeVisual visual{ nullptr };
        CompositionSpriteShape line{ nullptr };
    };
    auto outline = std::make_shared<Outline>();
    auto apply = [weak = make_weak(presenter), outline, r](auto&&...) {
        try {
            auto presenter = weak.get();
            if (!presenter) return;
            const float w = static_cast<float>(presenter.ActualWidth());
            const float h = static_cast<float>(presenter.ActualHeight());
            if (w <= 0 || h <= 0) return;
            auto visual = ElementCompositionPreview::GetElementVisual(presenter);
            auto compositor = visual.Compositor();
            if (!outline->visual) { // first layout: the style's border is in place
                winrt::Windows::UI::Color color{ 0x66, 0x75, 0x75, 0x75 };
                if (auto brush = presenter.BorderBrush().try_as<SolidColorBrush>()) {
                    color = brush.Color();
                    color.A = static_cast<uint8_t>(color.A * brush.Opacity());
                }
                presenter.BorderThickness({});
                outline->visual = compositor.CreateShapeVisual();
                outline->line = compositor.CreateSpriteShape();
                outline->line.StrokeBrush(compositor.CreateColorBrush(color));
                outline->visual.Shapes().Append(outline->line);
                ElementCompositionPreview::SetElementChildVisual(presenter, outline->visual);
                if (g_settings.dump) {
                    int budget = 120;
                    Log(L"=== menu dump");
                    Dump(presenter, presenter, 0, budget);
                }
            }
            const float px = 1 / Scale(presenter);
            visual.Clip(compositor.CreateGeometricClip(SmoothGeometry(compositor, D2D1::RectF(0, 0, w, h), r)));
            outline->visual.Size({ w, h });
            outline->line.Geometry(SmoothGeometry(compositor, D2D1::RectF(px / 2, px / 2, w - px / 2, h - px / 2), r - px / 2));
            outline->line.StrokeThickness(px);
        } catch (...) {
            Log(L"menu rounding failed");
        }
    };
    presenter.SizeChanged(apply);
    apply();
}

// Moves `element` left by `dips`, rounded up to whole device pixels, keeping its size: once,
// after its first layout, when its template's margin is in place.
void ShiftLeft(FrameworkElement const& element, float dips) {
    if (dips <= 0 || !g_shifted.insert(get_abi(element)).second) return;
    auto done = std::make_shared<bool>(false);
    element.SizeChanged([done, dips](WinRTObject const& sender, SizeChangedEventArgs const&) {
        if (*done) return;
        *done = true;
        auto element = sender.as<FrameworkElement>();
        const float scale = Scale(element);
        const double shift = std::ceil(dips * scale - 0.01f) / scale;
        auto m = element.Margin();
        element.Margin({ m.Left - shift, m.Top, m.Right + shift, m.Bottom });
        std::wstringstream line;
        line << L"SHIFT " << element.Name().c_str() << L" by " << shift << L" DIP";
        Log(line.str());
    });
}

// ---- Opening out of the dock ----
//
// The menu slides out from under the dock (taskbar-dock.dll's glass bar), the way S1mp1e's recipe
// book slides out from behind the inventory: it starts as wide as the dock, hidden under it, and
// rises out of it growing to its own size, fading in, on S1mp1e's critically damped spring (the
// recipe book's: 1 - (1 + 14 t) e^(-14 t)); then, the way S1mp1e's recipes come in, the tiles
// grow from their centres one after another. The menu's window ends at the top of the taskbar,
// so whatever of the menu is still below that edge is hidden under the dock. The menu is scaled
// evenly about its bottom middle; all of it follows one progress value on the compositor, so it
// runs at the screen's rate. The shell cloaks the window while the menu is closed: the menu is
// made unseen then, so the first frame after the uncloak starts the slide. Closing cannot be
// animated here (the shell cloaks the window at once): taskbar-dock.dll slides a picture of the
// menu back under the dock, finding the menu by the StartGlassMenu property.
//
// Opening and going back are one motion on one spring, and either can cut the other short: the
// menu's window is marked with where the opening set off from (StartGlassOpenAt/
// StartGlassOpenFrom, a Motion), the dock marks its own with where the going back did
// (TaskbarDockCloseAt/TaskbarDockCloseFrom), and whichever takes over goes on from where the other
// had got to, at its speed: opened while its picture goes back, the menu grows on from there.

constexpr float OPEN_W = 14, OPEN_S = 0.6f;       // RecipeBookSlide's spring, and until it has settled
constexpr float GONE = 0.002f;                    // under the dock from this much progress down
constexpr float FADE_IN = 0.4f;                   // fully faded in at this much progress
constexpr float GROW_W = 17, GROW_S = 0.5f;       // RecipeCascade's spring
constexpr float GROW_AFTER_S = 0.15f;             // tiles start growing once the menu mostly has
constexpr float GROW_STAGGER_S = 0.018f;          // RecipeCascade.STAGGER_NS
constexpr float GROW_SPAN_S = 0.36f;              // a recipe page's 20 items: more tiles go faster

weak_ref<Border> g_background{ nullptr };         // the menu's acrylic background
weak_ref<UIElement> g_menu{ nullptr };            // its container: what grows
weak_ref<UIElement> g_shadow{ nullptr };          // StartUI's shadow image around it, which grows along
CompositionClip g_menuClip{ nullptr };            // RoundCorners' smoothed clip, given back once grown
HWND g_startWindow = nullptr;                     // the menu's CoreWindow
std::vector<weak_ref<FrameworkElement>> g_growers; // the tiles
std::unordered_set<void*> g_growerSet;

struct Morph {
    CompositionPropertySet values{ nullptr };     // P (0: under the dock, 1: the menu) and the shapes
    bool bound = false;                           // the menu follows P (until it is in place)
    unsigned turn = 0;                            // bumped with every opening and closing
    LONGLONG closedAt = 0;                        // when the shell hid the menu last (Now)
};
Morph g_morph;

// Milliseconds on a steady clock, for the log.
long long Ms() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

// Progress (0..1) of S1mp1e's critically damped spring `w` at `t` seconds.
float SpringProgress(float w, float t) {
    return 1 - (1 + w * t) * std::exp(-w * t);
}

// QueryPerformanceCounter, which every process shares, and seconds of it.
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

// A spring's motion as it set off: when (Now), from what progress, how fast (progress per second).
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
// floats from the low end). `at` is written last and read on both sides of `from`.
// taskbar-dock.dll has the same two.
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

// `motion` on to `to` along spring `w`, as key frames a 120th of a second apart, `seconds` long.
ScalarKeyFrameAnimation SpringScalar(Compositor const& compositor, float w, float seconds, Motion const& motion, float to) {
    auto animation = compositor.CreateScalarKeyFrameAnimation();
    auto linear = compositor.CreateLinearEasingFunction();
    const int keys = std::max(2, static_cast<int>(seconds * 120));
    animation.InsertKeyFrame(0, motion.from);
    for (int i = 1; i <= keys; ++i) {
        float x = to, v = 0;
        if (i < keys) SpringAt(w, motion, to, seconds * i / keys, x, v);
        animation.InsertKeyFrame(static_cast<float>(i) / keys, x, linear);
    }
    animation.Duration(std::chrono::microseconds(static_cast<int64_t>(seconds * 1e6f)));
    return animation;
}

Vector3KeyFrameAnimation SpringGrow(Compositor const& compositor, float w, float seconds) {
    auto animation = compositor.CreateVector3KeyFrameAnimation();
    auto linear = compositor.CreateLinearEasingFunction();
    const int keys = std::max(2, static_cast<int>(seconds * 120));
    animation.InsertKeyFrame(0, { 0, 0, 1 });
    for (int i = 1; i <= keys; ++i) {
        const float p = i == keys ? 1 : SpringProgress(w, seconds * i / keys);
        animation.InsertKeyFrame(static_cast<float>(i) / keys, { p, p, 1 }, linear);
    }
    animation.Duration(std::chrono::microseconds(static_cast<int64_t>(seconds * 1e6f)));
    return animation;
}

// A tile, to grow in when the menu opens.
void AddGrower(FrameworkElement const& element) {
    if (!g_settings.open || !g_settings.cascade || !g_growerSet.insert(get_abi(element)).second) return;
    g_growers.push_back(make_weak(element));
}

// The dock's rectangle (screen px) and corner radius, as taskbar-dock.dll publishes them.
bool DockShape(RECT& shape, int& radius) {
    HWND dock = FindWindowW(L"TaskbarDock", nullptr);
    const auto packed = dock ? reinterpret_cast<uintptr_t>(GetPropW(dock, L"TaskbarDockShape")) : 0;
    if (!packed) return false;
    auto part = [packed](int i) { return static_cast<LONG>(static_cast<int16_t>(static_cast<uint16_t>(packed >> (16 * i)))); };
    shape = { part(0), part(1), part(2), part(3) };
    radius = static_cast<int>(reinterpret_cast<intptr_t>(GetPropW(dock, L"TaskbarDockRadius")));
    return shape.right > shape.left && shape.bottom > shape.top;
}

// Where the menu's background settles across its window (DIPs), measured while it is laid out:
// when the shell shows the window, it is not laid out yet for a moment and reports nonsense.
float g_menuX = 0;

// The morph's values, made once.
void EnsureMorph(Compositor const& compositor) {
    if (g_morph.values) return;
    g_morph.values = compositor.CreatePropertySet();
    g_morph.values.InsertScalar(L"P", 1);
    // Every value the expressions read must be there before they start (MeasureMorph sets them).
    for (auto name : { L"S0", L"BarH", L"MenuW", L"MenuH", L"Cx", L"Cy", L"Tx0", L"Ty0", L"RBar", L"RMenu", L"Fade" }) {
        g_morph.values.InsertScalar(name, 1);
    }
    g_morph.values.InsertScalar(L"ShadowX", 0);
    g_morph.values.InsertScalar(L"ShadowY", 0);
}

// StartUI draws the menu's shadow with an Image named DropShadow next to the menu (a few levels
// up, 13 DIPs larger all round), so it does not grow with the menu by itself.
UIElement FindShadow(UIElement const& menu) {
    DependencyObject node = menu;
    for (int level = 0; level < 8; ++level) {
        auto parent = VisualTreeHelper::GetParent(node);
        if (!parent) break;
        const int count = VisualTreeHelper::GetChildrenCount(parent);
        for (int i = 0; i < count; ++i) {
            auto element = VisualTreeHelper::GetChild(parent, i).try_as<FrameworkElement>();
            if (element && element.Name() == L"DropShadow") return element;
        }
        node = parent;
    }
    return nullptr;
}

// Where the menu's origin is in the shadow's coordinates (13 DIPs in, at whole pixels), for
// growing the shadow about the menu's bottom middle. Measured while the menu is not morphed.
void MeasureShadow(UIElement const& menu) {
    auto shadow = g_shadow.get();
    if (!shadow || g_morph.bound || !g_morph.values) return;
    const auto at = shadow.TransformToVisual(menu).TransformPoint({ 0, 0 });
    g_morph.values.InsertScalar(L"ShadowX", -at.X);
    g_morph.values.InsertScalar(L"ShadowY", -at.Y);
    if (g_settings.log) Log(L"SHADOW at " + std::to_wstring(at.X) + L"," + std::to_wstring(at.Y) + L" from the menu");
}

// Puts the menu's and the dock's shapes (in the window's DIPs) into the morph's values, and the
// menu's place on the screen into the window's StartGlassMenu property (four i16 from the low
// end: left, top, right, bottom; StartGlassRadius: its corner radius in px) for
// taskbar-dock.dll. `settled`: the menu is laid out where it stays (closing, or grown), so its
// place across is measured; else the last one measured is used. The menu always sits at the
// bottom of its window. False when the menu is not laid out.
bool MeasureMorph(UIElement const& menu, Border const& background, bool settled) {
    const float w = static_cast<float>(background.ActualWidth()), h = static_cast<float>(background.ActualHeight());
    auto root = menu.XamlRoot();
    if (w <= 0 || h <= 0 || !root || !g_startWindow) return false;
    EnsureMorph(ElementCompositionPreview::GetElementVisual(menu).Compositor());
    const float scale = static_cast<float>(root.RasterizationScale());
    const float windowHeight = static_cast<float>(root.Size().Height);
    const auto inMenu = background.TransformToVisual(menu).TransformPoint({ 0, 0 });
    auto inWindow = background.TransformToVisual(nullptr).TransformPoint({ 0, 0 });
    if (settled && std::fabs(inWindow.Y + h - windowHeight) < 2) g_menuX = inWindow.X;
    inWindow.X = g_menuX;
    inWindow.Y = windowHeight - h;
    RECT window{};
    GetWindowRect(g_startWindow, &window);
    auto part = [](LONG v) { return static_cast<uintptr_t>(static_cast<uint16_t>(static_cast<int16_t>(v))); };
    const LONG left = window.left + std::lround(inWindow.X * scale), top = window.top + std::lround(inWindow.Y * scale);
    const uintptr_t packed = part(left) | part(top) << 16 | part(left + std::lround(w * scale)) << 32 | part(top + std::lround(h * scale)) << 48;
    SetPropW(g_startWindow, L"StartGlassMenu", reinterpret_cast<HANDLE>(packed));
    SetPropW(g_startWindow, L"StartGlassRadius", reinterpret_cast<HANDLE>(static_cast<intptr_t>(std::lround(g_settings.radius * scale))));

    // The dock in the window's DIPs (below its bottom edge); without one, a dock-sized bar
    // under the menu's middle.
    RECT dock{};
    int dockRadius = 0;
    float barLeft = 0, barWidth = std::min(w, 240.0f), barHeight = 40, barRadius = 8;
    if (DockShape(dock, dockRadius)) {
        barLeft = (dock.left - window.left) / scale;
        barWidth = (dock.right - dock.left) / scale;
        barHeight = (dock.bottom - dock.top) / scale;
        barRadius = dockRadius / scale;
    } else {
        barLeft = inWindow.X + (w - barWidth) / 2;
    }
    barWidth = std::clamp(barWidth, 8.0f, w);
    barHeight = std::clamp(barHeight, 8.0f, h);
    auto& v = g_morph.values;
    v.InsertScalar(L"S0", barWidth / w);
    v.InsertScalar(L"BarH", barHeight);
    v.InsertScalar(L"MenuW", w);
    v.InsertScalar(L"MenuH", h);
    v.InsertScalar(L"Cx", inMenu.X + w / 2); // the menu's bottom middle, in its container
    v.InsertScalar(L"Cy", inMenu.Y + h);
    // From there to where the growth starts: the dock's middle on the window's bottom edge,
    // which is the top of the taskbar.
    v.InsertScalar(L"Tx0", barLeft + barWidth / 2 - (inWindow.X + w / 2));
    v.InsertScalar(L"Ty0", windowHeight - (inWindow.Y + h));
    v.InsertScalar(L"RBar", barRadius);
    v.InsertScalar(L"RMenu", g_settings.radius);
    v.InsertScalar(L"Fade", FADE_IN);
    if (settled) MeasureShadow(menu);
    if (g_settings.log) {
        const auto menuAt = menu.TransformToVisual(nullptr).TransformPoint({ 0, 0 });
        std::wstringstream line;
        line << Ms() << L" MEASURE window " << window.left << L"," << window.top << L".." << window.right << L"," << window.bottom
             << L" root " << root.Size().Width << L"x" << windowHeight << L" scale " << scale << L" background at " << inWindow.X << L","
             << inWindow.Y << L" " << w << L"x" << h << L" (in menu " << inMenu.X << L"," << inMenu.Y << L"; menu at " << menuAt.X << L","
             << menuAt.Y << L") bar " << barLeft << L" " << barWidth << L"x" << barHeight << L" Tx0 " << barLeft + barWidth / 2 - (inWindow.X + w / 2)
             << L" Ty0 " << windowHeight - (inWindow.Y + h);
        Log(line.str());
    }
    return true;
}

// The menu follows the morph's progress P: its scale, its place, its opacity and a rounded clip.
// Its shadow does the same about the same point, so that it stays around the menu as it grows.
void BindMorph(UIElement const& menu) {
    auto visual = ElementCompositionPreview::GetElementVisual(menu);
    auto compositor = visual.Compositor();
    EnsureMorph(compositor);
    if (g_morph.bound) return;
    auto expression = [&](std::wstring const& text) {
        auto animation = compositor.CreateExpressionAnimation(text);
        animation.SetReferenceParameter(L"m", g_morph.values);
        return animation;
    };
    // s: the menu's scale, from the dock's width up, about its bottom middle; that point starts
    // as far below the window's bottom edge (the top of the taskbar) as the scaled menu is tall,
    // so the menu starts hidden under the dock and slides up out of it.
    const std::wstring s = L"(m.S0 + (1 - m.S0) * m.P)";
    auto follow = [&](UIElement const& element, std::wstring const& centre) {
        auto target = ElementCompositionPreview::GetElementVisual(element);
        ElementCompositionPreview::SetIsTranslationEnabled(element, true);
        target.StartAnimation(L"CenterPoint", expression(centre));
        target.StartAnimation(L"Scale", expression(L"Vector3(" + s + L", " + s + L", 1)"));
        target.StartAnimation(L"Translation", expression(L"Vector3(m.Tx0 * (1 - m.P), (m.Ty0 + m.S0 * m.MenuH) * (1 - m.P), 0)"));
        target.StartAnimation(L"Opacity", expression(L"Clamp(m.P / m.Fade, 0, 1)"));
    };
    follow(menu, L"Vector3(m.Cx, m.Cy, 0)");
    if (auto shadow = g_shadow.get()) follow(shadow, L"Vector3(m.Cx + m.ShadowX, m.Cy + m.ShadowY, 0)");
    g_morph.bound = true;
}

// The menu and its shadow stop following P: as laid out.
void UnbindMorph(UIElement const& menu) {
    for (auto element : { menu, g_shadow.get() }) {
        if (!element) continue;
        auto visual = ElementCompositionPreview::GetElementVisual(element);
        for (auto name : { L"CenterPoint", L"Scale", L"Translation", L"Opacity" }) visual.StopAnimation(name);
        visual.Scale({ 1, 1, 1 });
        visual.Properties().InsertVector3(L"Translation", { 0, 0, 0 });
        visual.Opacity(1);
    }
    g_morph.bound = false;
}

// The menu closed: where it settles is measured, and it stays unseen until it opens again.
// Nothing else changes while it is closed: GlassDock measures the menu's width through UI
// Automation then, which sees composition transforms.
void PrepareOpen() {
    auto menu = g_menu.get();
    auto background = g_background.get();
    if (!menu || !background) return;
    g_morph.closedAt = Now();
    ++g_morph.turn;
    if (g_morph.bound) UnbindMorph(menu);
    MeasureMorph(menu, background, true);
    ElementCompositionPreview::GetElementVisual(menu).Opacity(0);
    if (auto shadow = g_shadow.get()) ElementCompositionPreview::GetElementVisual(shadow).Opacity(0);
}

// The tiles grow from their centres one after another, in reading order.
void GrowTiles(UIElement const& menu, Compositor const& compositor) {
    struct Item {
        FrameworkElement element;
        float top, left, width, height;
    };
    std::vector<Item> items;
    const auto size = menu.ActualSize();
    for (auto it = g_growers.begin(); it != g_growers.end();) {
        auto element = it->get();
        if (!element) {
            it = g_growers.erase(it);
            continue;
        }
        ++it;
        const float w = static_cast<float>(element.ActualWidth()), h = static_cast<float>(element.ActualHeight());
        if (w <= 0 || h <= 0 || element.Visibility() != Visibility::Visible) continue;
        const auto at = element.TransformToVisual(menu).TransformPoint({ 0, 0 });
        if (at.X + w <= 0 || at.Y + h <= 0 || at.X >= size.x || at.Y >= size.y) continue; // scrolled out of the menu
        items.push_back({ element, at.Y, at.X, w, h });
    }
    std::sort(items.begin(), items.end(), [](Item const& a, Item const& b) {
        return std::fabs(a.top - b.top) > 1 ? a.top < b.top : a.left < b.left;
    });
    const float stagger = items.empty() ? 0 : std::min(GROW_STAGGER_S, GROW_SPAN_S / items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        auto grown = ElementCompositionPreview::GetElementVisual(items[i].element);
        grown.CenterPoint({ items[i].width / 2, items[i].height / 2, 0 });
        auto grow = SpringGrow(compositor, GROW_W, GROW_S);
        grow.DelayTime(std::chrono::microseconds(static_cast<int64_t>((GROW_AFTER_S + i * stagger) * 1e6f)));
        grow.DelayBehavior(AnimationDelayBehavior::SetInitialValueBeforeDelay);
        grown.StartAnimation(L"Scale", grow);
    }
}

// The tiles where they belong at once, done growing in if they still were.
void TilesIn() {
    for (auto const& weak : g_growers) {
        if (auto element = weak.get()) {
            auto grown = ElementCompositionPreview::GetElementVisual(element);
            grown.StopAnimation(L"Scale");
            grown.Scale({ 1, 1, 1 });
        }
    }
}

// The menu opened: it grows out of the dock, then its tiles grow in. When the dock's picture of
// it is still going back, the menu grows on from where the picture is, at its speed, tiles and all.
void OpenMenu() {
    auto menu = g_menu.get();
    auto background = g_background.get();
    if (!menu || !background) return;
    if (!MeasureMorph(menu, background, false)) {
        PublishMotion(g_startWindow, L"StartGlassOpenAt", L"StartGlassOpenFrom", { Now(), 1, 0 }); // there at once
        UnbindMorph(menu);
        return;
    }
    Motion open{ Now(), 0, 0 };
    Motion close;
    HWND dock = FindWindowW(L"TaskbarDock", nullptr);
    // The dock's going back must be of this closing (it marks it as it starts): else it showed none.
    if (dock && ReadMotion(dock, L"TaskbarDockCloseAt", L"TaskbarDockCloseFrom", close) && Seconds(g_morph.closedAt - close.at) < 0.1) {
        SpringAt(OPEN_W, close, 0, static_cast<float>(Seconds(open.at - close.at)), open.from, open.speed);
        if (open.from <= GONE) open = { open.at, 0, 0 };
        open.from = std::min(open.from, 1.0f);
    }
    PublishMotion(g_startWindow, L"StartGlassOpenAt", L"StartGlassOpenFrom", open);
    g_morph.values.StopAnimation(L"P");
    g_morph.values.InsertScalar(L"P", open.from);
    BindMorph(menu);
    auto compositor = ElementCompositionPreview::GetElementVisual(menu).Compositor();
    const unsigned turn = ++g_morph.turn;
    auto batch = compositor.CreateScopedBatch(CompositionBatchTypes::Animation);
    g_morph.values.StartAnimation(L"P", SpringScalar(compositor, OPEN_W, OPEN_S, open, 1));
    batch.End();
    batch.Completed([turn](auto&&, auto&&) {
        if (turn != g_morph.turn) return; // closed (or opened again) meanwhile
        try {
            auto menu = g_menu.get();
            auto background = g_background.get();
            if (menu) UnbindMorph(menu);
            if (menu && background) MeasureMorph(menu, background, true); // laid out by now: where taskbar-dock.dll takes its picture
        } catch (...) {
        }
    });
    if (open.from == 0) GrowTiles(menu, compositor);
    else TilesIn(); // as the picture going back has them
    Log(std::to_wstring(Ms()) + L" OPEN from " + std::to_wstring(open.from) + L" moving " + std::to_wstring(open.speed));
}

void CALLBACK OnStartCloak(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND) {
        DWORD process = 0;
        GetWindowThreadProcessId(hwnd, &process);
        wchar_t name[64] = {};
        GetClassNameW(hwnd, name, 64);
        Log(std::to_wstring(Ms()) + L" FOREGROUND " + name + L" pid " + std::to_wstring(process));
        return;
    }
    if (hwnd != g_startWindow || object != OBJID_WINDOW) return;
    Log(std::to_wstring(Ms()) + (event == EVENT_OBJECT_UNCLOAKED ? L" UNCLOAKED" : L" CLOAKED"));
    try {
        if (event == EVENT_OBJECT_UNCLOAKED) OpenMenu();
        else PrepareOpen();
    } catch (...) {
        Log(L"open animation failed");
        try {
            if (auto menu = g_menu.get()) UnbindMorph(menu);
        } catch (...) {
        }
    }
}

// Watches the shell show and hide the menu's window. On the menu's (this) thread: the events
// come through its message loop.
void EnsureWatching() {
    if (g_startWindow || !g_settings.open) return;
    auto core = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread();
    if (!core) return;
    HWND window = nullptr;
    if (FAILED(core.as<ICoreWindowInterop>()->get_WindowHandle(&window)) || !window) return;
    g_startWindow = window;
    SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, OnStartCloak, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (g_settings.log) {
        SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnStartCloak, 0, 0, WINEVENT_OUTOFCONTEXT);
        core.Activated([](auto&&, winrt::Windows::UI::Core::WindowActivatedEventArgs const& args) {
            Log(std::to_wstring(Ms()) + L" ACTIVATED state " + std::to_wstring(static_cast<int>(args.WindowActivationState())));
        });
        core.VisibilityChanged([](auto&&, winrt::Windows::UI::Core::VisibilityChangedEventArgs const& args) {
            Log(std::to_wstring(Ms()) + L" VISIBLE " + std::to_wstring(args.Visible()));
        });
        core.KeyDown([](auto&&, winrt::Windows::UI::Core::KeyEventArgs const& args) {
            Log(std::to_wstring(Ms()) + L" KEY " + std::to_wstring(static_cast<int>(args.VirtualKey())));
        });
    }
    Log(L"watching the Start window");
}

// The menu (its acrylic background and the container that grows) is known; if it is closed
// now, it gets ready for its first opening.
void WatchMenu(Border const& background, UIElement const& container) {
    if (!g_settings.open) return;
    g_background = make_weak(background);
    g_menu = make_weak(container);
    if (auto shadow = FindShadow(container)) {
        g_shadow = make_weak(shadow);
        EnsureMorph(ElementCompositionPreview::GetElementVisual(container).Compositor());
        MeasureShadow(container);
        Log(L"shadow found");
    }
    EnsureWatching();
    BOOL cloaked = FALSE;
    if (g_startWindow && SUCCEEDED(DwmGetWindowAttribute(g_startWindow, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        PrepareOpen();
    }
    Log(L"menu found");
}

// One corner radius and one spacing rhythm for everything inside the menu (see the settings
// at the top): navigation buttons become inset rounded squares, list row highlights get gaps,
// tiles get wider gaps, all with the inner radius and the same corner smoothing.
void StyleElement(std::wstring_view type, std::wstring_view name, WinRTObject const& object) {
    const float r = g_settings.innerRadius;
    auto element = object.try_as<FrameworkElement>();
    if (!element || r <= 0) return;
    // The diagnostics report some StartUI classes by their XAML base type (the navigation
    // pane's places arrive as ListViewItem), so those are matched on the runtime class.
    const std::wstring cls{ get_class_name(element) };
    if (cls == L"StartUI.NavigationPaneButton" || cls == L"StartUI.ExpandCollapseButton" || cls == L"StartUI.AppListViewItem") {
        const float n = g_settings.navInset;
        ClipSmooth(element, { n, n, n, n }, r);
    } else if ((type == L"Windows.UI.Xaml.Controls.Primitives.ListViewItemPresenter" && name == L"Root") ||
               (type == L"Windows.UI.Xaml.Controls.Button" && name == L"Header")) {
        const float v = g_settings.rowGap / 2; // app list rows and their letter headers
        ClipSmooth(element, { 0, v, 0, v }, r, g_settings.rowTrim);
        // Reveal draws the hover fill 1 DIP inside a border light; the clip hides that border
        // everywhere but on the left, where it would leave a soft edge. No border, full fill.
        if (auto presenter = element.try_as<Primitives::ListViewItemPresenter>()) presenter.RevealBorderThickness({});
        else if (auto button = element.try_as<Control>()) button.BorderThickness({});
    } else if (type == L"StartUI.TileViewControl") {
        // The tile's presenter also draws its hover and press frame: clip that one.
        const float t = std::max(0.0f, (g_settings.tileGap - 4) / 2);
        if (auto presenter = VisualTreeHelper::GetParent(element).try_as<FrameworkElement>()) {
            ClipSmooth(presenter, { t, t, t, t }, r);
            AddGrower(presenter);
        }
    } else if (cls == L"Windows.UI.Xaml.Controls.TileGridNestedPanel" && name == L"NestedPanel") {
        ShiftLeft(element, g_settings.tileShift); // a group's tiles
    } else if (cls == L"StartUI.GroupHeaderControl") {
        // A group's title row (highlighted on hover): as wide as its tiles. The title text
        // already starts at the tiles' left edge once they have moved.
        ClipSmooth(element, { 0, 0, std::max(0.0f, (g_settings.tileGap - 4) / 2), 0 }, r, g_settings.tileShift);
    } else if (cls == L"Windows.UI.Xaml.Controls.MenuFlyoutPresenter") {
        RoundMenu(element.as<Control>());
    } else if (cls == L"Windows.UI.Xaml.Controls.MenuFlyoutItem" || cls == L"Windows.UI.Xaml.Controls.MenuFlyoutSubItem" ||
               cls == L"Windows.UI.Xaml.Controls.ToggleMenuFlyoutItem") {
        // A menu item's highlight sits 4 DIPs in from the menu's sides, as far as the first and
        // last items already sit from its top and bottom, its radius concentric with the menu's
        // (inner = outer - gap).
        ClipSmooth(element, { 4, 0, 4, 0 }, r - 4);
        element.as<Control>().BorderThickness({}); // no Reveal border band (see the list rows)
    }
}

// Clips the background's container (the menu with its content) to the background's
// rectangle with smoothed corners, now and whenever the menu changes size.
void RoundCorners(Border const& background) {
    if (g_settings.radius <= 0) return;
    auto container = VisualTreeHelper::GetParent(background).try_as<UIElement>();
    if (!container) return;
    auto update = [weakBackground = make_weak(background), weakContainer = make_weak(container)](auto&&...) {
        try {
            auto background = weakBackground.get();
            auto container = weakContainer.get();
            if (!background || !container) return;
            const float w = static_cast<float>(background.ActualWidth());
            const float h = static_cast<float>(background.ActualHeight());
            if (w <= 0 || h <= 0) return;
            const auto origin = background.TransformToVisual(container).TransformPoint({ 0, 0 });
            const auto rect = D2D1::RectF(origin.X, origin.Y, origin.X + w, origin.Y + h);
            auto visual = ElementCompositionPreview::GetElementVisual(container);
            auto compositor = visual.Compositor();
            g_menuClip = compositor.CreateGeometricClip(SmoothGeometry(compositor, rect, g_settings.radius));
            visual.Clip(g_menuClip);
            std::wstringstream line;
            line << L"ROUND " << Describe(container) << L" clip " << origin.X << L"," << origin.Y << L" " << w << L"x" << h
                 << L" radius=" << g_settings.radius << L" smoothing=" << g_settings.smoothing;
            Log(line.str());
        } catch (...) {
            Log(L"rounding failed");
        }
    };
    background.SizeChanged(update);
    update();
    ScheduleDump(container);
    try {
        WatchMenu(background, container);
    } catch (hresult_error const& e) {
        Log(L"watching the menu failed: " + std::wstring(e.message()));
    } catch (...) {
        Log(L"watching the menu failed");
    }
}

// The menu's background (and every other acrylic one, see `targets`): see-through, and the
// menu's corners rounded. Once per background.
void StyleAcrylic(WinRTObject const& object, std::wstring_view type, std::wstring_view name) {
    Brush background{ nullptr };
    auto border = object.try_as<Border>();
    if (border) background = border.Background();
    else if (auto panel = object.try_as<Panel>()) background = panel.Background();
    else if (auto control = object.try_as<Control>()) background = control.Background();
    if (!background) return;

    auto acrylic = background.try_as<AcrylicBrush>();
    if (!acrylic) {
        Log(L"  " + std::wstring(type) + L" '" + std::wstring(name) + L"' background " + Describe(background));
        return;
    }
    static std::unordered_set<void*> styled;
    if (!styled.insert(get_abi(object)).second) return;
    std::wstringstream line;
    line << L"ACRYLIC " << type << L" '" << name << L"' tint=" << acrylic.TintOpacity()
         << L" source=" << static_cast<int>(acrylic.BackgroundSource());
    if (IsTarget(name)) {
        acrylic.TintOpacity(g_settings.tint);
        acrylic.TintLuminosityOpacity(Windows::Foundation::IReference<double>(g_settings.luminosity));
        line << L" -> tint=" << g_settings.tint << L" luminosity=" << g_settings.luminosity;
        if (border) RoundCorners(border);
    }
    Log(line.str());
}

struct VisualTreeWatcher : implements<VisualTreeWatcher, IVisualTreeServiceCallback2> {
    explicit VisualTreeWatcher(com_ptr<IXamlDiagnostics> diagnostics) : m_diagnostics(std::move(diagnostics)) {}

    void Advise() {
        check_hresult(m_diagnostics.as<IVisualTreeService3>()->AdviseVisualTreeChange(this));
    }

    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation, VisualElement element, VisualMutationType mutation) noexcept override try {
        if (mutation != Add) return S_OK;
        const std::wstring_view type{ element.Type, SysStringLen(element.Type) };
        const std::wstring_view name{ element.Name, SysStringLen(element.Name) };

        WinRTObject object;
        check_hresult(m_diagnostics->GetIInspectableFromHandle(element.Handle, reinterpret_cast<::IInspectable**>(put_abi(object))));
        EnsureWatching(); // on the menu's thread
        StyleElement(type, name, object);
        StyleAcrylic(object, type, name);
        // A background the menu gets only later (it may be made before its brush is set): when it comes.
        if (auto border = object.try_as<Border>(); border && !g_settings.targets.empty() && IsTarget(name)) {
            border.RegisterPropertyChangedCallback(Border::BackgroundProperty(),
                [type = std::wstring(type), name = std::wstring(name)](DependencyObject const& sender, DependencyProperty const&) {
                    try {
                        StyleAcrylic(sender, type, name);
                    } catch (...) {
                    }
                });
        }
        return S_OK;
    } catch (...) {
        return S_OK; // never let a failure reach the Start menu
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle, VisualElementState, LPCWSTR) noexcept override {
        return S_OK;
    }

private:
    com_ptr<IXamlDiagnostics> m_diagnostics;
};

com_ptr<VisualTreeWatcher> g_watcher;

struct StartGlassTap : implements<StartGlassTap, IObjectWithSite> {
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) noexcept override try {
        m_site.copy_from(site);
        if (!site || g_watcher) return S_OK; // one watcher per Start host
        LoadSettings();
        Log(L"start-glass loaded");
        g_watcher = make_self<VisualTreeWatcher>(m_site.as<IXamlDiagnostics>());
        // AdviseVisualTreeChange cannot be called from inside SetSite: give it its own thread.
        HANDLE thread = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            try { g_watcher->Advise(); } catch (...) { Log(L"AdviseVisualTreeChange failed"); }
            return 0;
        }, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
        return S_OK;
    } catch (...) {
        return to_hresult();
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** site) noexcept override {
        return m_site ? m_site.as(riid, site) : E_FAIL;
    }

private:
    com_ptr<IUnknown> m_site;
};

struct TapFactory : implements<TapFactory, IClassFactory> {
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** object) noexcept override try {
        *object = nullptr;
        return outer ? CLASS_E_NOAGGREGATION : make<StartGlassTap>().as(riid, object);
    } catch (...) {
        return to_hresult();
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override {
        return S_OK;
    }
};

} // namespace

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* object) try {
    *object = nullptr;
    return clsid == CLSID_StartGlassTap ? make<TapFactory>().as(riid, object) : CLASS_E_CLASSNOTAVAILABLE;
} catch (...) {
    return to_hresult();
}

STDAPI DllCanUnloadNow() {
    return S_FALSE; // the visual tree service keeps calling into this DLL
}
