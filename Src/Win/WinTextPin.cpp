#include "pch.h"
#include "WinTextPin.h"
#include "WinPin.h"
#include <windows.h>

namespace {
	// 默认窗口尺寸（逻辑像素）。文本钉窗比贴图小一圈，居中摆放就好找
	constexpr int kDefW{ 380 }, kDefH{ 260 };
	// 缩放时的最小尺寸（逻辑像素）。别缩成一条看不清内容
	constexpr int kMinW{ 160 }, kMinH{ 100 };
	// 关闭按钮宽度（逻辑像素），与 onCreated 里 setWidth 的那个值保持一致 ——
	// onHitTest 要靠它把那枚按钮从"标题栏拖拽区"里挖出来，否则点关闭会被当成拖窗
	constexpr float kCloseW{ 30.f };
}

std::vector<std::unique_ptr<WinTextPin>> WinTextPin::winTextPins;

WinTextPin::WinTextPin(const std::wstring& text)
	: Ling::WinBase(), content(text)
{
	// 拖窗 / 缩放交给系统（见 onHitTest），窗口自己不再订阅鼠标事件 ——
	// 文本选择那些交互由 Ling::TextBox 自己挂在窗口事件上，不需这里转发。
	onDestroy.add([this]() { this->onClosed(); });
}

void WinTextPin::init(const std::wstring& text)
{
	if (text.empty()) return;
	auto ptr = new WinTextPin(text);
	std::unique_ptr<WinTextPin> win{ ptr };
	// 默认尺寸与位置：屏幕工作区正中。dpi 在 WinBase 构造期已就绪，x/y/w/h 都是物理像素
	auto monitor = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO mi{ sizeof(MONITORINFO) };
	GetMonitorInfo(monitor, &mi);
	int physW = (int)(kDefW * ptr->dpi);
	int physH = (int)(kDefH * ptr->dpi);
	ptr->w = (float)physW;
	ptr->h = (float)physH;
	ptr->x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - physW) / 2;
	ptr->y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - physH) / 2;
	ptr->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
	winTextPins.push_back(std::move(win));
}

void WinTextPin::dispose()
{
	winTextPins.clear();
}

bool WinTextPin::hasWindow()
{
	return !winTextPins.empty();
}

void WinTextPin::onMinMaxInfo(MINMAXINFO* mmi)
{
	// 允许拖边框缩放，但不缩到看不清内容。默认那条把最小跟踪尺寸放到 800×600，
	// 那样小窗口根本缩不成，这里改小
	mmi->ptMinTrackSize.x = (LONG)(kMinW * dpi);
	mmi->ptMinTrackSize.y = (LONG)(kMinH * dpi);
}

LRESULT WinTextPin::onHitTest(const POINT pos)
{
	// onHitTest 收到的是屏幕坐标，先换成客户区坐标再判边界 / 标题栏
	POINT pt = pos;
	ScreenToClient(hwnd, &pt);
	// 四边 / 四角：交给系统做窗口缩放（拖边框改大小）
	auto result = borderHitTest(pt);
	if (result != HTCLIENT) return result;
	// 标题栏当拖拽带。挖掉右边那枚关闭按钮 —— 它要收自己的点击，不能当标题栏吞掉。
	// 返回 HTCAPTION 后由系统接管移动，平滑、不会再抖（见头文件 onHitTest 的说明）
	if (pt.y >= 0 && pt.y < kTitleH * dpi && pt.x < w - kCloseW * dpi) return HTCAPTION;
	return HTCLIENT;
}

void WinTextPin::onCreated()
{
	disableBorderRadius();
	enableShadow();
	body->setFlexDirection(Ling::FlexDirection::Column);

	// 标题栏：深色一条，左边标题、右边关闭按钮。点住它拖动整扇窗
	titleBar = body->makeChild<Ling::Node>();
	titleBar->setHeight(kTitleH);
	titleBar->setBg(Ling::Color(0x2B2B2BFF));
	titleBar->setFlexDirection(Ling::FlexDirection::Row);
	titleBar->setAlignItems(Ling::Align::Stretch);

	titleLabel = titleBar->makeChild<Ling::Label>();
	titleLabel->setText(L"文本");
	titleLabel->setColor(Ling::Color(0xFFFFFFFF));
	titleLabel->setPaddingLeft(10.f);
	titleLabel->setFlexGrow(1.f);

	closeBtn = titleBar->makeChild<Ling::Button>();
	closeBtn->setText(L"×");
	closeBtn->setWidth(kCloseW);
	closeBtn->setHeight(kTitleH);
	closeBtn->setBg(Ling::Color(0));
	closeBtn->setHoverBg(Ling::Color(0xE81123FF));
	closeBtn->setColor(Ling::Color(0xFFFFFFFF));
	closeBtn->setHoverColor(Ling::Color(0xFFFFFFFF));
	closeBtn->onClick.add([this](Ling::Button*) { close(); });

	// 文本区：真控件，双击选词 / 三击选段 / Ctrl+A C X V 全自带。
	// 宽高不在这里定死，交给 layout() —— Ling::TextBox 构造函数里写死了 setWidth(240)，
	// 而 flexGrow 只管主轴（纵向），交叉轴（横向）压不过那个确定宽度，不显式撑满的话
	// 控件只有 240 宽，滚动条会落在窗口中间而不是右缘
	textBox = body->makeChild<Ling::TextBox>();
	textBox->setBg(Ling::Color(0xFFFFFFFF));
	textBox->setPadding(8.f);
	textBox->setFontSize(14.f);
	textBox->setText(content);
	textBox->focus();
	show();
}

void WinTextPin::layout()
{
	// 文本区尺寸跟着窗口走：宽度撑满，高度 = 窗口高 - 标题栏高（w/h 是物理像素，除以 dpi
	// 换成 Node 要的逻辑像素）。建窗与每次拖边框缩放都会走到这里
	if (textBox) {
		float tw = w / dpi;
		float th = h / dpi - kTitleH;
		textBox->setWidth(tw > 1.f ? tw : 1.f);
		textBox->setHeight(th > 1.f ? th : 1.f);
	}
	Ling::WinBase::layout();
}

void WinTextPin::onClosed()
{
	if (isClosed) return;
	isClosed = true;
	Ling::App::get()->dq.TryEnqueue([this]() {
		std::erase_if(winTextPins, [this](const std::unique_ptr<WinTextPin>& p) { return p.get() == this; });
		// 用完即走模式：文本钉窗和图片贴图都没了才退出进程
		if (winTextPins.empty() && !WinPin::hasWindow()) {
			if (Ling::App::get()->args[L"--auto-quit"] == L"true") {
				Ling::App::get()->quit(0);
			}
		}
	});
}
