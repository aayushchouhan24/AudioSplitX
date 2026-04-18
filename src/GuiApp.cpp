#include "AudioEngine.h"
#include "Common.h"
#include "DeviceEnumerator.h"
#include "Resource.h"

#include <Windows.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

using namespace Gdiplus;

namespace {

#ifndef DWMWA_BORDER_COLOR
constexpr DWORD DWMWA_BORDER_COLOR_FALLBACK = 34;
#else
constexpr DWORD DWMWA_BORDER_COLOR_FALLBACK = DWMWA_BORDER_COLOR;
#endif

#ifndef DWMWA_CAPTION_COLOR
constexpr DWORD DWMWA_CAPTION_COLOR_FALLBACK = 35;
#else
constexpr DWORD DWMWA_CAPTION_COLOR_FALLBACK = DWMWA_CAPTION_COLOR;
#endif

enum class HitAction {
    Close,
    Minimize,
    Maximize,
    Refresh,
    StartStop,
    SourceSelect,
    OutputToggle,
    DelayDec,
    DelayInc,
    DelayValue,
};

struct HitZone {
    RectF rect;
    HitAction action;
    size_t index = 0;
};

struct UiColors {
    Color bg { 255, 7, 8, 10 };
    Color title { 255, 12, 14, 18 };
    Color panel { 255, 16, 18, 24 };
    Color row { 255, 22, 25, 33 };
    Color rowHover { 255, 28, 32, 42 };
    Color stroke { 255, 42, 49, 60 };
    Color text { 255, 238, 243, 246 };
    Color muted { 255, 145, 154, 167 };
    Color dim { 255, 82, 91, 106 };
    Color accent { 255, 255, 106, 32 };
    Color green { 255, 92, 236, 166 };
    Color red { 255, 255, 76, 76 };
};

std::wstring compact(std::wstring text, size_t maxChars)
{
    if (text.size() <= maxChars) {
        return text;
    }
    if (maxChars <= 3) {
        return text.substr(0, maxChars);
    }
    text.resize(maxChars - 3);
    text += L"...";
    return text;
}

class GuiApp {
public:
    int run(HINSTANCE instance, int show);

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    bool create(HINSTANCE instance, int show);
    void refreshEndpoints();
    void startStop();
    void startEngine();
    void stopEngine();
    void paint();
    void draw(Graphics& g, int width, int height);
    void drawTitleBar(Graphics& g, const RectF& area);
    void drawStatusStrip(Graphics& g, const RectF& area);
    void drawSourcePanel(Graphics& g, const RectF& area);
    void drawOutputPanel(Graphics& g, const RectF& area);
    void drawEndpointRow(Graphics& g, const RectF& row, const asx::AudioEndpoint& endpoint, bool active, bool disabled,
        const std::wstring& subtext, HitAction action, size_t index);
    void drawButton(Graphics& g, const RectF& r, const wchar_t* label, const Color& fill, const Color& border, const Color& text,
        bool hovered = false);
    void drawRound(Graphics& g, const RectF& r, float radius, const Color& fill, const Color& border);
    void drawText(Graphics& g, const std::wstring& text, const RectF& r, float size, const Color& color,
        int style = FontStyleRegular, StringAlignment align = StringAlignmentNear, StringAlignment lineAlign = StringAlignmentNear);
    void addZone(const RectF& rect, HitAction action, size_t index = 0);
    bool isHovered(HitAction action, size_t index = 0) const;
    void setHoveredZone(const HitZone* zone);
    void adjustDelayMs(size_t index, double deltaMs);
    void beginDelayEdit(size_t index);
    void commitDelayEdit();
    void cancelDelayEdit();
    bool hitTest(float x, float y, HitZone& zone) const;
    LRESULT hitTestChrome(int x, int y) const;

    HWND hwnd_ = nullptr;
    ULONG_PTR gdiplusToken_ = 0;
    UiColors c_;
    asx::DeviceEnumerator enumerator_;
    std::vector<asx::AudioEndpoint> endpoints_;
    std::vector<bool> outputSelected_;
    std::vector<double> outputDelayMs_;
    size_t sourceIndex_ = 0;
    std::vector<HitZone> zones_;
    std::unique_ptr<asx::AudioEngine> engine_;
    bool routing_ = false;
    std::wstring status_ = L"Choose source and outputs.";
    HICON appIconLarge_ = nullptr;
    HICON appIconSmall_ = nullptr;
    bool hoverValid_ = false;
    HitAction hoverAction_ = HitAction::Refresh;
    size_t hoverIndex_ = 0;
    bool mouseTracking_ = false;
    size_t editingDelayIndex_ = SIZE_MAX;
    std::wstring delayEditText_;
};

int GuiApp::run(HINSTANCE instance, int show)
{
    GdiplusStartupInput gdiplusInput;
    if (GdiplusStartup(&gdiplusToken_, &gdiplusInput, nullptr) != Ok) {
        MessageBoxW(nullptr, L"Failed to initialize GDI+.", L"AudioSplitX", MB_ICONERROR);
        return 1;
    }

    const int result = create(instance, show) ? [] {
        MSG msg {};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }() : 1;

    if (gdiplusToken_) {
        GdiplusShutdown(gdiplusToken_);
    }

    if (appIconLarge_) {
        DestroyIcon(appIconLarge_);
        appIconLarge_ = nullptr;
    }
    if (appIconSmall_) {
        DestroyIcon(appIconSmall_);
        appIconSmall_ = nullptr;
    }

    return result;
}

bool GuiApp::create(HINSTANCE instance, int show)
{
    appIconLarge_ = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(IDI_APP_ICON),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXICON),
        GetSystemMetrics(SM_CYICON),
        LR_DEFAULTCOLOR));

    appIconSmall_ = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(IDI_APP_ICON),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));

    WNDCLASSEXW wc {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = GuiApp::windowProc;
    wc.lpszClassName = L"AudioSplitX.Gui";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIcon = appIconLarge_ ? appIconLarge_ : LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIconSm = appIconSmall_ ? appIconSmall_ : wc.hIcon;
    wc.hbrBackground = nullptr;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Could not register the AudioSplitX window class.", L"AudioSplitX", MB_ICONERROR);
        return false;
    }

    const DWORD style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"AudioSplitX", style,
        CW_USEDEFAULT, CW_USEDEFAULT, 1040, 680, nullptr, nullptr, instance, this);
    if (!hwnd_) {
        MessageBoxW(nullptr, L"Could not create the AudioSplitX main window.", L"AudioSplitX", MB_ICONERROR);
        return false;
    }

    if (appIconLarge_) {
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIconLarge_));
    }
    if (appIconSmall_) {
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIconSmall_));
    }

    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
    const COLORREF darkFrame = RGB(7, 8, 10);
    DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR_FALLBACK, &darkFrame, sizeof(darkFrame));
    DwmSetWindowAttribute(hwnd_, DWMWA_CAPTION_COLOR_FALLBACK, &darkFrame, sizeof(darkFrame));
    const DWORD cornerPreference = 2;
    DwmSetWindowAttribute(hwnd_, 33, &cornerPreference, sizeof(cornerPreference));

    refreshEndpoints();
    SetTimer(hwnd_, 1, 120, nullptr);
    ShowWindow(hwnd_, show);
    UpdateWindow(hwnd_);
    return true;
}

LRESULT CALLBACK GuiApp::windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    GuiApp* app = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<GuiApp*>(cs->lpCreateParams);
        app->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<GuiApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    return app ? app->handleMessage(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT GuiApp::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wParam) {
            return 0;
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);

    case WM_NCHITTEST:
        return hitTestChrome(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 760;
        info->ptMinTrackSize.y = 560;
        return 0;
    }

    case WM_TIMER:
        if (routing_ && engine_ && !engine_->running()) {
            stopEngine();
            status_ = L"Routing stopped by audio engine.";
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        HitZone hit;
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        const bool hasHit = hitTest(x, y, hit);

        if (editingDelayIndex_ != SIZE_MAX) {
            if (!hasHit || hit.action != HitAction::DelayValue || hit.index != editingDelayIndex_) {
                commitDelayEdit();
            }
        }

        if (!hasHit) {
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        switch (hit.action) {
        case HitAction::Close:
            SendMessageW(hwnd_, WM_CLOSE, 0, 0);
            break;
        case HitAction::Minimize:
            ShowWindow(hwnd_, SW_MINIMIZE);
            break;
        case HitAction::Maximize:
            ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
            break;
        case HitAction::Refresh:
            if (!routing_) {
                refreshEndpoints();
            }
            break;
        case HitAction::StartStop:
            startStop();
            break;
        case HitAction::SourceSelect:
            if (!routing_ && hit.index < endpoints_.size()) {
                sourceIndex_ = hit.index;
                if (sourceIndex_ < outputSelected_.size()) {
                    outputSelected_[sourceIndex_] = false;
                }
                status_ = L"Source selected. Choose outputs.";
            }
            break;
        case HitAction::OutputToggle:
            if (!routing_ && hit.index < outputSelected_.size() && hit.index != sourceIndex_) {
                outputSelected_[hit.index] = !outputSelected_[hit.index];
                status_ = outputSelected_[hit.index] ? L"Output enabled." : L"Output disabled.";
            }
            break;
        case HitAction::DelayDec:
            adjustDelayMs(hit.index, -5.0);
            break;
        case HitAction::DelayInc:
            adjustDelayMs(hit.index, 5.0);
            break;
        case HitAction::DelayValue:
            if (!routing_) {
                beginDelayEdit(hit.index);
            }
            break;
        }

        SetFocus(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!mouseTracking_) {
            TRACKMOUSEEVENT tme {};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd_;
            TrackMouseEvent(&tme);
            mouseTracking_ = true;
        }

        HitZone hit;
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        if (hitTest(x, y, hit)) {
            setHoveredZone(&hit);
        } else {
            setHoveredZone(nullptr);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        mouseTracking_ = false;
        setHoveredZone(nullptr);
        return 0;

    case WM_CHAR:
        if (editingDelayIndex_ != SIZE_MAX && !routing_) {
            if (wParam >= L'0' && wParam <= L'9') {
                if (delayEditText_.size() < 4) {
                    if (delayEditText_ == L"0") {
                        delayEditText_.clear();
                    }
                    delayEditText_.push_back(static_cast<wchar_t>(wParam));
                }
                status_ = L"Editing output delay (ms).";
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            if (wParam == VK_BACK) {
                if (!delayEditText_.empty()) {
                    delayEditText_.pop_back();
                }
                if (delayEditText_.empty()) {
                    delayEditText_ = L"0";
                }
                status_ = L"Editing output delay (ms).";
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            if (wParam == VK_RETURN) {
                commitDelayEdit();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            if (wParam == 27) {
                cancelDelayEdit();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);

    case WM_KEYDOWN:
        if (editingDelayIndex_ != SIZE_MAX && !routing_) {
            if (wParam == VK_UP) {
                adjustDelayMs(editingDelayIndex_, 1.0);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_DOWN) {
                adjustDelayMs(editingDelayIndex_, -1.0);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                cancelDelayEdit();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }
        return DefWindowProcW(hwnd_, msg, wParam, lParam);

    case WM_PAINT:
        paint();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_CLOSE:
        stopEngine();
        DestroyWindow(hwnd_);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd_, 1);
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }
}

void GuiApp::refreshEndpoints()
{
    try {
        endpoints_ = enumerator_.listRenderEndpoints();
        outputSelected_.assign(endpoints_.size(), false);
        outputDelayMs_.assign(endpoints_.size(), 0.0);
        sourceIndex_ = 0;

        for (size_t i = 0; i < endpoints_.size(); ++i) {
            const std::wstring lower = asx::toLower(endpoints_[i].name);
            if (lower.find(L"cable") != std::wstring::npos || lower.find(L"virtual") != std::wstring::npos) {
                sourceIndex_ = i;
                break;
            }
            if (endpoints_[i].isDefault) {
                sourceIndex_ = i;
            }
        }

        status_ = L"Ready. Select source and outputs.";
    } catch (const std::exception& ex) {
        endpoints_.clear();
        outputSelected_.clear();
        status_ = L"Refresh failed: " + asx::widen(ex.what());
    }
}

void GuiApp::startStop()
{
    if (routing_) {
        stopEngine();
        status_ = L"Routing stopped.";
    } else {
        startEngine();
    }
}

void GuiApp::startEngine()
{
    if (endpoints_.empty() || sourceIndex_ >= endpoints_.size()) {
        status_ = L"No source endpoint selected.";
        return;
    }

    std::vector<asx::AudioEndpoint> outputs;
    std::vector<double> outputManualDelayMs;
    for (size_t i = 0; i < endpoints_.size(); ++i) {
        if (i != sourceIndex_ && i < outputSelected_.size() && outputSelected_[i]) {
            outputs.push_back(endpoints_[i]);
            outputManualDelayMs.push_back(i < outputDelayMs_.size() ? outputDelayMs_[i] : 0.0);
        }
    }

    if (outputs.empty()) {
        status_ = L"Select at least one output.";
        return;
    }

    try {
        asx::AudioEngineConfig config;
        config.source = endpoints_[sourceIndex_];
        config.outputs = std::move(outputs);
        config.outputManualDelayMs = std::move(outputManualDelayMs);
        config.preferExclusive = true;
        config.consoleMeter = false;
        config.endpointBufferMs = 10.0;
        config.captureRingMs = 750.0;
        config.outputRingMs = 750.0;

        engine_ = std::make_unique<asx::AudioEngine>(std::move(config));
        engine_->start();
        routing_ = true;
        status_ = L"Routing active.";
    } catch (const std::exception& ex) {
        engine_.reset();
        routing_ = false;
        status_ = L"Start failed: " + asx::widen(ex.what());
    }
}

void GuiApp::stopEngine()
{
    if (engine_) {
        engine_->stop();
        engine_.reset();
    }
    routing_ = false;
}

void GuiApp::paint()
{
    PAINTSTRUCT ps {};
    HDC hdc = BeginPaint(hwnd_, &ps);

    RECT client {};
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    HDC memDc = CreateCompatibleDC(hdc);
    HBITMAP bitmap = CreateCompatibleBitmap(hdc, width, height);
    HGDIOBJ old = SelectObject(memDc, bitmap);

    Graphics g(memDc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    draw(g, width, height);

    BitBlt(hdc, 0, 0, width, height, memDc, 0, 0, SRCCOPY);
    SelectObject(memDc, old);
    DeleteObject(bitmap);
    DeleteDC(memDc);
    EndPaint(hwnd_, &ps);
}

void GuiApp::draw(Graphics& g, int width, int height)
{
    zones_.clear();
    SolidBrush bg(c_.bg);
    g.FillRectangle(&bg, 0, 0, width, height);

    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    const float pad = std::clamp(w * 0.024f, 18.0f, 28.0f);
    const RectF title(0, 0, w, 56.0f);
    drawTitleBar(g, title);

    const RectF status(pad, h - 58.0f, w - pad * 2.0f, 38.0f);

    const float contentTop = 78.0f;
    const float contentBottom = status.Y - 18.0f;
    const float contentH = std::max(260.0f, contentBottom - contentTop);
    const bool singleColumn = w < 930.0f;

    if (singleColumn) {
        const float half = (contentH - 16.0f) * 0.48f;
        drawSourcePanel(g, RectF(pad, contentTop, w - pad * 2.0f, half));
        drawOutputPanel(g, RectF(pad, contentTop + half + 16.0f, w - pad * 2.0f, contentH - half - 16.0f));
    } else {
        const float leftW = std::clamp(w * 0.35f, 320.0f, 430.0f);
        drawSourcePanel(g, RectF(pad, contentTop, leftW, contentH));
        drawOutputPanel(g, RectF(pad + leftW + 18.0f, contentTop, w - pad * 2.0f - leftW - 18.0f, contentH));
    }

    drawStatusStrip(g, status);
}

void GuiApp::drawTitleBar(Graphics& g, const RectF& area)
{
    SolidBrush brush(c_.title);
    g.FillRectangle(&brush, area);

    drawText(g, L"AudioSplitX", RectF(22.0f, 15.0f, 180.0f, 24.0f), 15.0f, c_.text, FontStyleBold);
    drawText(g, routing_ ? L"ROUTING" : L"STOPPED", RectF(150.0f, 18.0f, 110.0f, 20.0f), 10.0f, routing_ ? c_.green : c_.muted, FontStyleBold);

    const RectF refresh(area.GetRight() - 264.0f, 12.0f, 92.0f, 32.0f);
    const RectF start(area.GetRight() - 162.0f, 12.0f, 92.0f, 32.0f);
    drawButton(g, refresh, L"Refresh", c_.row, c_.stroke, c_.text, isHovered(HitAction::Refresh));
    drawButton(g, start, routing_ ? L"Stop" : L"Start", routing_ ? Color(255, 58, 24, 28) : Color(255, 18, 70, 50),
        routing_ ? c_.red : c_.green, c_.text, isHovered(HitAction::StartStop));
    addZone(refresh, HitAction::Refresh);
    addZone(start, HitAction::StartStop);

    const RectF min(area.GetRight() - 64.0f, 0.0f, 32.0f, 32.0f);
    const RectF close(area.GetRight() - 32.0f, 0.0f, 32.0f, 32.0f);
    if (isHovered(HitAction::Minimize)) {
        drawRound(g, min, 6.0f, c_.rowHover, c_.stroke);
    }
    if (isHovered(HitAction::Close)) {
        drawRound(g, close, 6.0f, Color(255, 70, 28, 32), c_.red);
    }
    drawText(g, L"_", min, 14.0f, c_.muted, FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
    drawText(g, L"x", close, 13.0f, c_.muted, FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
    addZone(min, HitAction::Minimize);
    addZone(close, HitAction::Close);
}

void GuiApp::drawStatusStrip(Graphics& g, const RectF& area)
{
    drawRound(g, area, 7.0f, c_.panel, c_.stroke);
    const size_t selected = std::count(outputSelected_.begin(), outputSelected_.end(), true);
    std::wstring summary = routing_ ? L"Active" : L"Idle";
    summary += L" | outputs: " + std::to_wstring(selected);
    drawText(g, summary, RectF(area.X + 14.0f, area.Y + 10.0f, 190.0f, 18.0f), 10.0f, routing_ ? c_.green : c_.muted, FontStyleBold);
    drawText(g, compact(status_, 120), RectF(area.X + 210.0f, area.Y + 10.0f, area.Width - 224.0f, 18.0f), 10.0f, c_.text);
}

void GuiApp::drawSourcePanel(Graphics& g, const RectF& area)
{
    drawRound(g, area, 8.0f, c_.panel, c_.stroke);
    drawText(g, L"Source", RectF(area.X + 18.0f, area.Y + 16.0f, 120.0f, 24.0f), 14.0f, c_.text, FontStyleBold);
    drawText(g, L"Where Spotify/app audio enters", RectF(area.X + 18.0f, area.Y + 42.0f, area.Width - 36.0f, 18.0f), 9.5f, c_.muted);

    float y = area.Y + 74.0f;
    const float rowH = 52.0f;
    const float bottom = area.GetBottom() - 14.0f;
    for (size_t i = 0; i < endpoints_.size() && y + rowH <= bottom; ++i) {
        const bool active = i == sourceIndex_;
        std::wstring sub = endpoints_[i].isDefault ? L"default device" : L"available endpoint";
        drawEndpointRow(g, RectF(area.X + 14.0f, y, area.Width - 28.0f, rowH - 8.0f), endpoints_[i], active, routing_, sub,
            HitAction::SourceSelect, i);
        y += rowH;
    }
}

void GuiApp::drawOutputPanel(Graphics& g, const RectF& area)
{
    drawRound(g, area, 8.0f, c_.panel, c_.stroke);
    drawText(g, L"Outputs", RectF(area.X + 18.0f, area.Y + 16.0f, 120.0f, 24.0f), 14.0f, c_.text, FontStyleBold);
    drawText(g, L"Click devices to include in synchronized playback", RectF(area.X + 18.0f, area.Y + 42.0f, area.Width - 36.0f, 18.0f), 9.5f, c_.muted);

    float y = area.Y + 74.0f;
    const float rowH = 52.0f;
    const float bottom = area.GetBottom() - 14.0f;
    for (size_t i = 0; i < endpoints_.size() && y + rowH <= bottom; ++i) {
        const bool isSource = i == sourceIndex_;
        const bool selected = i < outputSelected_.size() && outputSelected_[i];
        std::wstring sub = isSource ? L"source - cannot output here" : (selected ? L"enabled output" : L"disabled");
        if (!isSource && i < outputDelayMs_.size()) {
            sub += L" | delay " + std::to_wstring(static_cast<int>(std::lround(outputDelayMs_[i]))) + L" ms";
        }
        drawEndpointRow(g, RectF(area.X + 14.0f, y, area.Width - 28.0f, rowH - 8.0f), endpoints_[i], selected, routing_ || isSource, sub,
            HitAction::OutputToggle, i);
        y += rowH;
    }
}

void GuiApp::drawEndpointRow(Graphics& g, const RectF& row, const asx::AudioEndpoint& endpoint, bool active, bool disabled,
    const std::wstring& subtext, HitAction action, size_t index)
{
    const Color border = active ? (action == HitAction::SourceSelect ? c_.accent : c_.green) : c_.stroke;
    const Color fill = active ? Color(255, 25, 30, 32) : c_.row;
    drawRound(g, row, 7.0f, fill, border);

    const RectF dot(row.X + 14.0f, row.Y + 14.0f, 14.0f, 14.0f);
    SolidBrush dotBrush(active ? (action == HitAction::SourceSelect ? c_.accent : c_.green) : Color(255, 54, 62, 74));
    g.FillEllipse(&dotBrush, dot);

    const size_t titleChars = static_cast<size_t>(std::max(20.0f, row.Width / 10.0f));
    drawText(g, compact(endpoint.name, titleChars), RectF(row.X + 42.0f, row.Y + 8.0f, row.Width - 210.0f, 18.0f), 11.5f,
        disabled && !active ? c_.muted : c_.text, FontStyleBold);
    drawText(g, subtext, RectF(row.X + 42.0f, row.Y + 27.0f, row.Width - 210.0f, 14.0f), 8.5f, c_.muted);

    if (!disabled || action == HitAction::SourceSelect) {
        addZone(row, action, index);
    }

    if (action == HitAction::OutputToggle && index != sourceIndex_) {
        const int delayMs = (index < outputDelayMs_.size()) ? static_cast<int>(std::lround(outputDelayMs_[index])) : 0;
        const RectF toggle(row.GetRight() - 48.0f, row.Y + 10.0f, 34.0f, 24.0f);
        const RectF incBtn(toggle.X - 30.0f, row.Y + 10.0f, 24.0f, 24.0f);
        const RectF delayValue(incBtn.X - 90.0f, row.Y + 10.0f, 84.0f, 24.0f);
        const RectF decBtn(delayValue.X - 30.0f, row.Y + 10.0f, 24.0f, 24.0f);

        const bool decHover = isHovered(HitAction::DelayDec, index);
        const bool incHover = isHovered(HitAction::DelayInc, index);
        const bool valueHover = isHovered(HitAction::DelayValue, index);
        const bool valueActive = editingDelayIndex_ == index;

        std::wstring delayLabel = std::to_wstring(delayMs) + L" ms";
        if (valueActive) {
            delayLabel = (delayEditText_.empty() ? L"0" : delayEditText_) + L" ms";
        }

        drawButton(g, decBtn, L"-", c_.row, c_.stroke, disabled ? c_.dim : c_.text, decHover);
        drawRound(g, delayValue, 6.0f, valueActive ? Color(255, 22, 28, 35) : Color(255, 13, 16, 20),
            valueActive ? c_.accent : (valueHover ? c_.muted : c_.stroke));
        drawText(g, delayLabel, delayValue, 9.0f, disabled ? c_.dim : c_.text,
            FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
        drawButton(g, incBtn, L"+", c_.row, c_.stroke, disabled ? c_.dim : c_.text, incHover);

        const bool toggleHover = isHovered(HitAction::OutputToggle, index);
        drawRound(g, toggle, 12.0f,
            active ? (toggleHover ? Color(255, 34, 95, 68) : Color(255, 24, 78, 56))
                   : (toggleHover ? Color(255, 40, 45, 56) : Color(255, 32, 37, 46)),
            active ? c_.green : c_.stroke);
        SolidBrush knob(active ? c_.green : c_.dim);
        g.FillEllipse(&knob, active ? RectF(toggle.GetRight() - 20.0f, toggle.Y + 5.0f, 14.0f, 14.0f)
                                    : RectF(toggle.X + 6.0f, toggle.Y + 5.0f, 14.0f, 14.0f));

        if (!disabled) {
            addZone(decBtn, HitAction::DelayDec, index);
            addZone(delayValue, HitAction::DelayValue, index);
            addZone(incBtn, HitAction::DelayInc, index);
        }
    }
}

void GuiApp::drawButton(Graphics& g, const RectF& r, const wchar_t* label, const Color& fill, const Color& border, const Color& text,
    bool hovered)
{
    const Color hoverFill = Color(fill.GetA(),
        static_cast<BYTE>(std::min(255, fill.GetR() + 10)),
        static_cast<BYTE>(std::min(255, fill.GetG() + 10)),
        static_cast<BYTE>(std::min(255, fill.GetB() + 10)));
    const Color hoverBorder = Color(border.GetA(),
        static_cast<BYTE>(std::min(255, border.GetR() + 16)),
        static_cast<BYTE>(std::min(255, border.GetG() + 16)),
        static_cast<BYTE>(std::min(255, border.GetB() + 16)));
    drawRound(g, r, 7.0f, hovered ? hoverFill : fill, hovered ? hoverBorder : border);
    drawText(g, label, r, 9.5f, text, FontStyleBold, StringAlignmentCenter, StringAlignmentCenter);
}

void GuiApp::drawRound(Graphics& g, const RectF& r, float radius, const Color& fill, const Color& border)
{
    GraphicsPath path;
    const float d = radius * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();

    SolidBrush brush(fill);
    Pen pen(border, 1.0f);
    g.FillPath(&brush, &path);
    g.DrawPath(&pen, &path);
}

void GuiApp::drawText(Graphics& g, const std::wstring& text, const RectF& r, float size, const Color& color, int style,
    StringAlignment align, StringAlignment lineAlign)
{
    FontFamily family(L"Segoe UI");
    Font font(&family, size, style, UnitPoint);
    SolidBrush brush(color);
    StringFormat format;
    format.SetAlignment(align);
    format.SetLineAlignment(lineAlign);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(text.c_str(), static_cast<INT>(text.size()), &font, r, &format, &brush);
}

void GuiApp::addZone(const RectF& rect, HitAction action, size_t index)
{
    zones_.push_back(HitZone { rect, action, index });
}

bool GuiApp::isHovered(HitAction action, size_t index) const
{
    return hoverValid_ && hoverAction_ == action && hoverIndex_ == index;
}

void GuiApp::setHoveredZone(const HitZone* zone)
{
    const bool newValid = zone != nullptr;
    const HitAction newAction = newValid ? zone->action : HitAction::Refresh;
    const size_t newIndex = newValid ? zone->index : 0;

    if (hoverValid_ != newValid || hoverAction_ != newAction || hoverIndex_ != newIndex) {
        hoverValid_ = newValid;
        hoverAction_ = newAction;
        hoverIndex_ = newIndex;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void GuiApp::adjustDelayMs(size_t index, double deltaMs)
{
    if (routing_ || index >= outputDelayMs_.size() || index == sourceIndex_) {
        return;
    }

    outputDelayMs_[index] = std::clamp(outputDelayMs_[index] + deltaMs, 0.0, 5000.0);
    if (editingDelayIndex_ == index) {
        delayEditText_ = std::to_wstring(static_cast<int>(std::lround(outputDelayMs_[index])));
    }
    status_ = L"Output delay updated.";
}

void GuiApp::beginDelayEdit(size_t index)
{
    if (routing_ || index >= outputDelayMs_.size() || index == sourceIndex_) {
        return;
    }

    editingDelayIndex_ = index;
    delayEditText_ = std::to_wstring(static_cast<int>(std::lround(outputDelayMs_[index])));
    status_ = L"Editing output delay (ms). Type number, Enter to apply, Esc to cancel.";
}

void GuiApp::commitDelayEdit()
{
    if (editingDelayIndex_ == SIZE_MAX || editingDelayIndex_ >= outputDelayMs_.size()) {
        editingDelayIndex_ = SIZE_MAX;
        delayEditText_.clear();
        return;
    }

    int value = 0;
    try {
        value = std::stoi(delayEditText_.empty() ? std::wstring(L"0") : delayEditText_);
    } catch (...) {
        value = static_cast<int>(std::lround(outputDelayMs_[editingDelayIndex_]));
    }

    outputDelayMs_[editingDelayIndex_] = std::clamp(static_cast<double>(value), 0.0, 5000.0);
    delayEditText_ = std::to_wstring(static_cast<int>(std::lround(outputDelayMs_[editingDelayIndex_])));
    status_ = L"Output delay updated.";
    editingDelayIndex_ = SIZE_MAX;
    delayEditText_.clear();
}

void GuiApp::cancelDelayEdit()
{
    editingDelayIndex_ = SIZE_MAX;
    delayEditText_.clear();
    status_ = L"Delay edit canceled.";
}

bool GuiApp::hitTest(float x, float y, HitZone& zone) const
{
    for (auto it = zones_.rbegin(); it != zones_.rend(); ++it) {
        if (x >= it->rect.X && x <= it->rect.GetRight() && y >= it->rect.Y && y <= it->rect.GetBottom()) {
            zone = *it;
            return true;
        }
    }
    return false;
}

LRESULT GuiApp::hitTestChrome(int x, int y) const
{
    RECT rc {};
    GetWindowRect(hwnd_, &rc);
    const int border = 8;
    const bool left = x >= rc.left && x < rc.left + border;
    const bool right = x <= rc.right && x > rc.right - border;
    const bool top = y >= rc.top && y < rc.top + border;
    const bool bottom = y <= rc.bottom && y > rc.bottom - border;

    if (top && left) {
        return HTTOPLEFT;
    }
    if (top && right) {
        return HTTOPRIGHT;
    }
    if (bottom && left) {
        return HTBOTTOMLEFT;
    }
    if (bottom && right) {
        return HTBOTTOMRIGHT;
    }
    if (left) {
        return HTLEFT;
    }
    if (right) {
        return HTRIGHT;
    }
    if (top) {
        return HTTOP;
    }
    if (bottom) {
        return HTBOTTOM;
    }

    POINT pt { x, y };
    ScreenToClient(hwnd_, &pt);
    if (pt.y < 56 && pt.x < (rc.right - rc.left) - 288) {
        return HTCAPTION;
    }
    return HTCLIENT;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    try {
        asx::ComApartment com;
        GuiApp app;
        return app.run(instance, show);
    } catch (const std::exception& ex) {
        MessageBoxW(nullptr, asx::widen(ex.what()).c_str(), L"AudioSplitX", MB_ICONERROR);
        return 1;
    }
}
