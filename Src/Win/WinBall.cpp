#include "pch.h"
#include <algorithm>
#include "WinBall.h"
#include "../Setting.h"
#include "../Lang.h"
#include "../App.h"
#include "../PinSource.h"
#include "WinCap.h"
#include "WinHistory.h"

using namespace Microsoft::WRL;

namespace {
	std::unique_ptr<WinBall> ballIns;
	// 接管窗口过程的那一跳。一个进程只有一枚球，所以这两个指针放文件级静态就够
	WNDPROC ballOrigProc{ nullptr };
	WinBall* ballPtr{ nullptr };
	// 直径（逻辑像素）
	constexpr float ballSize{ 46.f };

	enum MenuId : UINT { menuCap = 1, menuPinClip, menuPinFile, menuHistory, menuHide };

	LRESULT CALLBACK ballProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (msg == WM_DROPFILES && ballPtr) {
			ballPtr->onDropFiles(reinterpret_cast<HDROP>(wParam));
			return 0;
		}
		return CallWindowProc(ballOrigProc, hwnd, msg, wParam, lParam);
	}
}

WinBall::WinBall()
{
	auto setting = Setting::get();
	auto size = ballSize * dpi;
	w = size;
	h = size;
	auto savedX = setting->getToolNum(L"ball", L"x", -1.f);
	auto savedY = setting->getToolNum(L"ball", L"y", -1.f);
	if (savedX >= 0 && savedY >= 0) {
		x = static_cast<int>(savedX);
		y = static_cast<int>(savedY);
	}
	else {
		// 第一次：主显示器右边垂直居中。贴右边而不是往下压，免得压住任务栏的通知区
		MONITORINFO mi{ .cbSize = sizeof(MONITORINFO) };
		GetMonitorInfo(MonitorFromPoint({ 0,0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
		x = mi.rcWork.right - static_cast<int>(size);
		y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - static_cast<int>(size)) / 2;
	}
	onMouseDown.add([this](POINT pos, BOOL) { this->onDown(pos); });
	onMouseMove.add([this](POINT pos) { this->onMove(pos); });
	onMouseUp.add([this](POINT, BOOL) { this->onUp(); });
	// 同 WinPin：DPI 变了系统会先擅自缩放窗口，等它把建议矩形应用完（紧随而来的 WM_SIZE）再掰回来
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (!dpiChanged) return;
		dpiChanged = false;
		applySize();
		});
}

WinBall::~WinBall()
{
}

void WinBall::init()
{
	if (ballIns) return;
	if (!Setting::get()->getToolFlag(L"ball", L"show", false)) return;
	auto ptr = new WinBall();
	ballIns.reset(ptr);
	ballPtr = ptr;
	ptr->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
}

void WinBall::dispose()
{
	ballPtr = nullptr;
	ballIns.reset();
}

bool WinBall::hasBall()
{
	return ballIns != nullptr;
}

void WinBall::onCreated()
{
	canvas = body->makeChild<Ling::Canvas>();
	canvas->setSizePercent(100.f, 100.f);
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x1677ff, 0.94f), brushBg.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, 0.9f), brushBorder.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushIcon.GetAddressOf());
	// 悬浮球不能出现在自己的截图里。excludeFromCapture 是静态的 —— 本函数跑在 App 构造
	// 期间（App::init 里的 app.reset 还没执行），那时 App::get() 还是空的
	App::excludeFromCapture(hwnd);
	// 拖放用 WM_DROPFILES：不必为了 OLE 的 IDropTarget 去动 Ling，代价是只认文件、
	// 不认浏览器里拖出来的那一块裸位图。提权运行时，普通权限的资源管理器发来的这条消息
	// 会被 UIPI 拦掉，所以放行它（连同拖放实现里有时用到的 WM_COPYDATA）
	DragAcceptFiles(hwnd, TRUE);
	ChangeWindowMessageFilterEx(hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
	ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
	ballOrigProc = reinterpret_cast<WNDPROC>(GetWindowLongPtr(hwnd, GWLP_WNDPROC));
	SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ballProc));
	show();
}

void WinBall::applySize()
{
	auto size = ballSize * dpi;
	w = size;
	h = size;
	SetWindowPos(hwnd, nullptr, 0, 0, static_cast<int>(size), static_cast<int>(size),
		SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	refresh();
}

void WinBall::layout()
{
	Ling::WinBase::layout();
	if (!canvas) return;
	auto ctx = canvas->startPaint();
	if (!ctx) return;
	ctx->Clear(0);
	auto r = std::min(w, h) / 2.f - 1.f * dpi;
	auto c = D2D1::Point2F(w / 2.f, h / 2.f);
	ctx->FillEllipse(D2D1::Ellipse(c, r, r), brushBg.Get());
	ctx->DrawEllipse(D2D1::Ellipse(c, r, r), brushBorder.Get(), 1.5f * dpi);
	// 中间画一个取景框：截图的意思，纯几何，不依赖字体里有没有某个码位
	auto box = r * 0.4f;
	ctx->DrawRectangle(D2D1::RectF(c.x - box, c.y - box, c.x + box, c.y + box),
		brushIcon.Get(), 1.6f * dpi);
	canvas->finishPaint();
}

void WinBall::onDown(POINT pos)
{
	pressPos = pos;
	isMouseDown = true;
	hasDragged = false;
	SetCapture(hwnd);
}

void WinBall::onMove(POINT pos)
{
	if (!isMouseDown) return;
	// 光标一步没挪也会来 WM_MOUSEMOVE，所以跟按下点比一下再算拖动
	if (pos.x != pressPos.x || pos.y != pressPos.y) hasDragged = true;
	setPosition(x + pos.x - pressPos.x, y + pos.y - pressPos.y);
}

void WinBall::onUp()
{
	isMouseDown = false;
	ReleaseCapture();
	if (!hasDragged) {
		showMenu();
		return;
	}
	snapAndSave();
}

void WinBall::snapAndSave()
{
	MONITORINFO mi{ .cbSize = sizeof(MONITORINFO) };
	if (!GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
	auto& wa = mi.rcWork;
	// 往离得近的那条竖边靠。球是常驻的，停在半路会一直压着底下的内容
	auto toLeft = (x + static_cast<int>(w) / 2) < (wa.left + wa.right) / 2;
	auto newX = toLeft ? wa.left : wa.right - static_cast<int>(w);
	auto newY = std::clamp(y, static_cast<int>(wa.top),
		static_cast<int>(std::max(wa.top, wa.bottom - static_cast<int>(h))));
	setPosition(newX, newY);
	Setting::get()->setToolNum(L"ball", L"x", static_cast<float>(newX));
	Setting::get()->setToolNum(L"ball", L"y", static_cast<float>(newY));
}

void WinBall::showMenu()
{
	auto menu = CreatePopupMenu();
	AppendMenu(menu, MF_STRING, menuCap, Lang::get(L"ball.cap").data());
	AppendMenu(menu, MF_STRING, menuPinClip, Lang::get(L"ball.pinClip").data());
	AppendMenu(menu, MF_STRING, menuPinFile, Lang::get(L"ball.pinFile").data());
	AppendMenu(menu, MF_STRING, menuHistory, Lang::get(L"ball.history").data());
	AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenu(menu, MF_STRING, menuHide, Lang::get(L"ball.hide").data());
	auto id = Ling::App::get()->popupMenu(menu);
	if (id == menuCap) {
		WinCap::init();
	}
	else if (id == menuPinClip) {
		PinSource::fromClipboard();
	}
	else if (id == menuPinFile) {
		PinSource::fromFile(hwnd);
	}
	else if (id == menuHistory) {
		WinHistory::init();
	}
	else if (id == menuHide) {
		Setting::get()->setToolFlag(L"ball", L"show", false);
		// 本函数正跑在鼠标事件的回调里，直接 dispose 就是把自己从栈底下抽掉。
		// 同 WinPin::onClosed：推迟到下一轮消息循环再放
		Ling::App::get()->dq.TryEnqueue([]() { WinBall::dispose(); });
	}
}

void WinBall::onDropFiles(HDROP drop)
{
	auto count = DragQueryFile(drop, 0xFFFFFFFF, nullptr, 0);
	for (UINT i = 0; i < count; ++i)
	{
		wchar_t buf[MAX_PATH]{};
		if (DragQueryFile(drop, i, buf, MAX_PATH) == 0) continue;
		// 非图片（目录、txt…）由 PinSource 自己认出来什么都不做
		PinSource::fromPath(std::wstring{ buf });
	}
	DragFinish(drop);
}
