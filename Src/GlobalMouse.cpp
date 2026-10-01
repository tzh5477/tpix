#include "pch.h"
#include <include/Ling.h>
#include <algorithm>
#include <format>
#include <thread>
#include "GlobalMouse.h"
#include "App.h"
#include "Ocr.h"
#include "Setting.h"
#include "ShotHistory.h"
#include "Util.h"
#include "Win/WinPin.h"
using namespace Microsoft::WRL;

namespace {
    HHOOK mouseHook{ nullptr };

    void pollDrag();
    class Marquee;
    // 声明在类定义之前：Marquee 自己的 onDestroy 里要按地址比对后把它放掉
    std::unique_ptr<Marquee> marquee;

    // 框选层：铺满虚拟桌面的透明窗口，只画一个框和它的尺寸。
    // 建一次一直留着，手势开始时 show、结束就 hide —— 它盖在所有窗口之上，
    // 用完不收必然碍事
    class Marquee : public Ling::WinBase
    {
    public:
        Marquee()
        {
            auto [sx, sy, sw, sh] = App::get()->getScreenArea();
            // 直接写物理像素：x/y/w/h 就是物理的，setter 收的是逻辑像素
            x = sx; y = sy; w = (float)sw; h = (float)sh;
            // 与 WinOverlay 同一套：加了 WS_EX_LAYERED 就得给一次 alpha，
            // 否则整扇窗可能压根不显示；穿透 + NOACTIVATE 是为了不打扰手势底下的窗口
            createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT
                | WS_EX_NOACTIVATE | WS_EX_LAYERED, WS_POPUP);
            onDestroy.add([this]() {
                // 不能在销毁回调里同步 reset 自己
                Ling::App::get()->dq.TryEnqueue([this]() {
                    if (marquee.get() == this) marquee.reset();
                });
            });
        }
        // 屏幕坐标（物理像素）
        void setRect(const RECT& r) { rect = r; refresh(); }
    private:
        void onCreated() override
        {
            auto dc = Ling::D2D::get()->deviceContext.Get();
            dc->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.3f), brushDim.GetAddressOf());
            dc->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.72f), brushBg.GetAddressOf());
            dc->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.5f, 1.f), brushLine.GetAddressOf());
            dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushText.GetAddressOf());
            canvas = body->makeChild<Ling::Canvas>();
            canvas->enableSwapChain();
            canvas->setSizePercent(100.f, 100.f);
            // 抓屏走的是 GDI，挡不住，但能挡住桌面复制那一路（录屏、远程协助）
            App::excludeFromCapture(hwnd);
            SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
            // 兜底：系统可能按 LowLevelHooksTimeout 把钩子悄悄摘掉，那时松手的消息
            // 再也收不到，框选层就会一直挂在屏幕上。这里只盯"手势是不是该结束了"
            setTimer(100, timerId);
            onTimer.add([](UINT id) { if (id == timerId) pollDrag(); });
        }
        void layout() override
        {
            Ling::WinBase::layout();
            if (!canvas) return;
            auto ctx = canvas->startPaint();
            if (!ctx) return;
            ctx->Clear(0);   // 透明：没画到的地方照常看见底下的桌面
            // 换算到客户区：窗口原点在虚拟桌面的 (x, y)
            const auto l = (float)rect.left - x;
            const auto t = (float)rect.top - y;
            const auto r = (float)rect.right - x;
            const auto b = (float)rect.bottom - y;
            ctx->FillRectangle(D2D1::RectF(0.f, 0.f, w, t), brushDim.Get());
            ctx->FillRectangle(D2D1::RectF(0.f, t, l, b), brushDim.Get());
            ctx->FillRectangle(D2D1::RectF(r, t, w, b), brushDim.Get());
            ctx->FillRectangle(D2D1::RectF(0.f, b, w, h), brushDim.Get());
            ctx->DrawRectangle(D2D1::RectF(l, t, r, b), brushLine.Get(), 1.5f * dpi);
            auto label = std::format(L"{} × {}", rect.right - rect.left, rect.bottom - rect.top);
            auto textLayout = Ling::D2D::get()->makeTextLayout(label, 12.f * dpi);
            if (textLayout) {
                DWRITE_TEXT_METRICS tm{};
                textLayout->GetMetrics(&tm);
                auto pad{ 3.f * dpi };
                // 标签摆在框的上沿外面；框贴着屏幕顶边时翻到框内下沿，免得画到屏幕外
                auto ly = t - tm.height - pad * 2 - 2.f * dpi;
                if (ly < 0.f) ly = b + 2.f * dpi;
                ctx->FillRectangle(D2D1::RectF(l, ly, l + tm.width + pad * 2, ly + tm.height + pad * 2),
                    brushBg.Get());
                ctx->DrawTextLayout(D2D1::Point2F(l + pad, ly + pad), textLayout.Get(),
                    brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
            canvas->finishPaint();
        }
        static constexpr UINT timerId{ 110 };
        RECT rect{};
        Ling::Canvas* canvas{ nullptr };
        ComPtr<ID2D1SolidColorBrush> brushDim, brushBg, brushLine, brushText;
    };

    bool dragging{ false };
    POINT from{ 0, 0 }, to{ 0, 0 };
    // 这一次手势按的是哪个键：0 左 / 1 中 / 2 右，决定松手后干什么
    int gestureBtn{ 0 };

    bool isWinDown()
    {
        return (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
    }
    bool isBtnDown(const int btn)
    {
        constexpr DWORD vks[]{ VK_LBUTTON, VK_MBUTTON, VK_RBUTTON };
        return (GetAsyncKeyState(vks[btn]) & 0x8000) != 0;
    }

    RECT normalized()
    {
        return { (std::min)(from.x, to.x), (std::min)(from.y, to.y),
            (std::max)(from.x, to.x), (std::max)(from.y, to.y) };
    }

    void cancel()
    {
        dragging = false;
        if (marquee) marquee->hide();
    }

    void finish()
    {
        if (!dragging) return;
        dragging = false;
        // 抓屏走 GDI，铺在最上头的框选层会被一起拍进去，所以先收再抓
        if (marquee) marquee->hide();
        auto rc = normalized();
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        // 没真拖动（Win+单击）不算一次手势：不做任何事，也别留下一个空贴图
        if (w < 4 || h < 4) return;
        auto pixels = Util::captureScreen(rc.left, rc.top, w, h);
        if (pixels.empty()) return;
        if (gestureBtn == 0) {
            WinPin::initFromData(rc.left, rc.top, w, h, pixels);
            return;
        }
        if (gestureBtn == 1) {
            Util::saveToClipboard(w, h, pixels.data());
            // 与 WinCap::copyToClipboard 同一套：自己刚写进去的那次变更不该再被记一遍
            ShotHistory::get()->skipNextClipboard();
            ShotHistory::get()->addImage(ShotHistory::Source::Shot, w, h, pixels.data());
            return;
        }
        // 一个识别语言包都没装时硬走内置 OCR 只会拿到空串，退回外部插件让用户自己解决
        if (!Ocr::isAvailable()) {
            Util::openWithImageReader(w, h, pixels.data());
            return;
        }
        // 识别要几百毫秒到几秒，不能压在 UI 线程上；结果回 UI 线程再写剪贴板
        auto lang = Setting::get()->getToolStr(L"ocr", L"lang", L"");
        std::thread([data = std::move(pixels), w, h, lang = std::move(lang)]() mutable {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            auto text = Ocr::recognize(w, h, data.data(), lang);
            Ling::App::get()->dq.TryEnqueue([text = std::move(text)]() {
                if (!text.empty()) Ling::Util::setTextToClipboard(text);
            });
        }).detach();
    }

    void pollDrag()
    {
        // 鼠标消息被吞掉了，但物理按键状态不问自明：松手了却没收到 up，就是钩子没了
        if (!dragging || isBtnDown(gestureBtn)) return;
        finish();
    }

    void start(const POINT pt, const int btn)
    {
        dragging = true;
        gestureBtn = btn;
        from = pt;
        to = pt;
        if (!marquee) marquee = std::make_unique<Marquee>();
        marquee->setRect(normalized());
        marquee->show();
    }

    LRESULT CALLBACK mouseProc(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HC_ACTION) {
            auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            switch (wParam) {
            case WM_LBUTTONDOWN:
                if (!dragging && isWinDown()) { start(ms->pt, 0); return 1; }
                break;
            case WM_MBUTTONDOWN:
                if (!dragging && isWinDown()) { start(ms->pt, 1); return 1; }
                break;
            case WM_RBUTTONDOWN:
                if (!dragging && isWinDown()) { start(ms->pt, 2); return 1; }
                break;
            case WM_MOUSEMOVE:
                if (dragging) {
                    // Esc 取消。框选层是穿透的、拿不到焦点，WM_KEYDOWN 到不了这里，只能问
                    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { cancel(); return 1; }
                    to = ms->pt;
                    if (marquee) marquee->setRect(normalized());
                    return 1;
                }
                break;
            case WM_LBUTTONUP: case WM_MBUTTONUP: case WM_RBUTTONUP:
                if (dragging) { finish(); return 1; }
                break;
            default:
                break;
            }
        }
        return CallNextHookEx(mouseHook, code, wParam, lParam);
    }
}

void GlobalMouse::setEnabled(bool on)
{
    if (on) {
        if (mouseHook) return;
        // WH_MOUSE_LL 不需要 DLL，回调回到装它的那条线程（UI 线程，有消息循环）。
        // 代价是这条线程一旦卡住不能及时处理消息，系统会按 LowLevelHooksTimeout
        // 悄悄把钩子摘掉 —— 那时手势失灵，别的都不受影响
        mouseHook = SetWindowsHookEx(WH_MOUSE_LL, mouseProc, nullptr, 0);
        return;
    }
    if (!mouseHook) return;
    UnhookWindowsHookEx(mouseHook);
    mouseHook = nullptr;
    cancel();
}

void GlobalMouse::dispose()
{
    setEnabled(false);
    // 只能走 close()：WinBase 的析构不销毁 hwnd，直接 reset 会留下一个还在收消息的野窗口
    if (marquee) marquee->close();
}
