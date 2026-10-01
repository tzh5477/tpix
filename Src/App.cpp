#include "pch.h"
#include <atomic>
#include <chrono>
#include <thread>
#include "App.h"
#include "Setting.h"
#include "Tray.h"
#include "Lang.h"
#include "ShotHistory.h"
#include "Update.h"
#include "Util.h"
#include "./Win/WinCap.h"
#include "./Win/WinDelay.h"
#include "./Win/WinPin.h"
#include "./Win/WinHistory.h"
#include "./Win/WinOcr.h"
#include "./Win/WinBall.h"
#include "./Win/WinSetting.h"

std::unique_ptr<App> app;

namespace {
    // 定时自动截图在跑的标记。退出时置 false，那条线程最多再睡一秒就自己退了
    std::atomic<bool> autoShotOn{ false };
    // 线程确实退出来了才会置回 false。光有 autoShotOn 不够：线程可能正读到 Setting 的
    // 指针、那头已经开始拆对象了，所以退出流程在这里等一下
    std::atomic<bool> autoShotAlive{ false };

    // 每秒醒一次看配置，而不是按间隔睡死：用户在设置里改开关或间隔要立刻生效，
    // 睡在旧间隔里就得等满一轮才认新值
    void startAutoShot()
    {
        autoShotOn = true;
        std::thread([]() {
            autoShotAlive = true;
            auto nextShot = std::chrono::steady_clock::now();
            while (autoShotOn.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                auto setting = Setting::get();
                if (!setting || !setting->getAutoShot()) {
                    // 关着的时候把下一次的时间一直往后推，打开的那一刻才开始真正计时
                    nextShot = std::chrono::steady_clock::now()
                        + std::chrono::minutes(setting ? setting->getAutoShotMin() : 5);
                    continue;
                }
                auto now = std::chrono::steady_clock::now();
                if (now < nextShot) continue;
                nextShot = now + std::chrono::minutes(setting->getAutoShotMin());
                // 抓屏本身哪个线程都行，但存盘要读 Setting、写历史，统一回 UI 线程做
                Ling::App::get()->dq.TryEnqueue([]() {
                    // 排队到自己之前可能已经进退出流程了，那时 App / Setting / ShotHistory 都在拆
                    if (!autoShotOn.load()) return;
                    auto [x, y, w, h] = App::get()->getScreenArea();
                    // 此刻屏幕上没有 tpix 的窗口，重新取一次快照：上次手动截图留下的那个形状
                    // 和位置都是旧的，画上去就是一个幽灵指针
                    Util::snapshotCursor();
                    auto data = Util::captureScreen(x, y, w, h, true);
                    if (data.empty()) return;
                    // 没人盯着的时候弹另存为对话框没人点，所以固定走自动保存那套目录 + 模板
                    auto path = Util::autoSavePath();
                    if (path.empty()) return;
                    if (!Util::saveToFile(path, w, h, data.data(), (Util::ImgFormat)Util::getSaveFormat())) return;
                    ShotHistory::get()->addImage(ShotHistory::Source::Shot, w, h, data.data());
                });
            }
            autoShotAlive = false;
        }).detach();
    }
}

App::~App()
{
}

void App::init()
{
    auto ptr = new App();
    app.reset(ptr);
}

void App::dispose()
{
    // 定时自动截图那条线程：先让它别再丢任务，再等它确实退出来 ——
    // 它手上可能正拿着 Setting / ShotHistory 的指针，下面的 dispose 会把对象拆掉
    autoShotOn = false;
    for (int i = 0; i < 40 && autoShotAlive.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // 退出前把还开着的贴图存一份，下次启动 restoreAll 摆回原位。
    // 必须在 WinPin::dispose 之前：窗口一放掉，底图位图就跟着没了
    WinPin::saveAll();
    // 窗口对象是文件级静态变量，交给静态析构就晚了（那时 CoUninitialize 已经跑完），
    // 所以趁这里把还开着的窗口先放掉
    WinPin::dispose();
    WinCap::dispose();
    WinDelay::dispose();    // 倒计时窗口：退出时可能正倒数到一半
    WinSetting::dispose();
    WinOcr::dispose();
    WinBall::dispose();
    WinHistory::dispose();
    ShotHistory::dispose();   // 必须在 Setting 之前：析构里要写索引文件
    Lang::dispose();
    Setting::dispose();
    app.reset();
}

App* App::get()
{
    return app.get();
}

void App::takeScreenShot(int x, int y, int w, int h, ID2D1Bitmap1** img)
{
    // 与滚动截图同一套抓屏代码：BGRA 布局一致，鼠标指针也是在同一个地方画上去的
    auto data = Util::captureScreen(x, y, w, h, Setting::get()->getIncludeCursor());
    if (data.empty()) return;
    D2D1_BITMAP_PROPERTIES1 props = {
       .pixelFormat{D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)},
       .dpiX{96.0f}, .dpiY{96.0f}, .bitmapOptions{D2D1_BITMAP_OPTIONS_NONE}
    };
    auto d2d = Ling::D2D::get();
    auto hr = d2d->deviceContext->CreateBitmap(D2D1::SizeU(w, h), data.data(), w * 4, props, img);
}

std::tuple<int, int, int, int> App::getScreenArea()
{
	return std::make_tuple(GetSystemMetrics(SM_XVIRTUALSCREEN), 
        GetSystemMetrics(SM_YVIRTUALSCREEN), 
        GetSystemMetrics(SM_CXVIRTUALSCREEN), 
        GetSystemMetrics(SM_CYVIRTUALSCREEN));
}

void App::excludeFromCapture(HWND hwnd)
{
    if (!hwnd) return;
    // 老系统上这个调用不但不失败，还会把窗口变成捕获画面里的一整块黑（实测 build 18363：
    // 返回 TRUE，读回来的 affinity 就是 0x11 —— 内核照存，可那会儿的 DWM 只认"非零即
    // 受保护内容"，一律涂黑）。所以必须自己拦住，让老系统退回"照旧被录进去"。
    // GetVersionEx 会被兼容性清单骗，只有 RtlGetVersion 给的是真版本号
    static const bool supported = []() {
        OSVERSIONINFOW vi{ sizeof(vi) };
        auto rtlGetVersion = (LONG(WINAPI*)(OSVERSIONINFOW*))GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        return rtlGetVersion && rtlGetVersion(&vi) == 0 && vi.dwBuildNumber >= 19041;
    }();
    if (!supported) return;
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
}

App::App()
{
    Ling::init(L"ScreenCapture");
    auto app = Ling::App::get();
    app->initArgs();
    Ling::D2D::addFonts({ L"icon.ttf" });
    // 录制中直接退出会让编码线程和 D3D 设备一起卡住，退出前先把录制停掉
    app->onBeforeQuit.add([]() { WinCap::stopIfRecording(); });
    Setting::init();
    Lang::init();
    // 建在 Setting 之后：历史目录从数据目录来。剪贴板监听也在这里挂上，
    // 用完即走（--auto-quit）那条路同样要记历史，所以不跟着托盘走
    ShotHistory::init();
    if (app->args[L"--auto-quit"] == L"true") {
        WinCap::init();
    }
    else {
        bool flag = app->refuseSecondInstance();
        if (flag) return;
        Tray::init();
        startAutoShot();
        // 开机自启不启动截图；--enter=tray 也一样，升级完重启新版本走的就是它 ——
        // 都是"只挂个托盘图标待命"，这条路上一个窗口都不建，图形设备也就根本不会创建。
        // 恢复贴图也要建窗口，所以放在这个判断之后
        if (app->args[L"--auto-start"] == L"true" || app->args[L"--enter"] == L"tray") {
            Update::checkLater();
            return;
        }
        WinPin::restoreAll();   // 上次退出前贴着的图，回到原来的位置
		WinBall::init();        // 悬浮球：配置里开过就一直挂着，位置是上次拖到的地方
		WinCap::init();//默认情况下，应用启动随即进入截图模式
    }
}
