#include "pch.h"
#include "WinTextPin.h"
#include "WinPin.h"
#include <windows.h>

namespace {
	// 默认窗口尺寸（逻辑像素）。文本钉窗比贴图小一圈，居中摆放就好找
	constexpr int kDefW{ 380 }, kDefH{ 260 };
}

std::vector<std::unique_ptr<WinTextPin>> WinTextPin::winTextPins;

WinTextPin::WinTextPin(const std::wstring& text)
	: Ling::WinBase(), content(text)
{
	onMouseDown.add([this](POINT pos, bool isRight) { this->onDown(pos, isRight); });
	onMouseMove.add([this](POINT pos) { this->onMove(pos); });
	onMouseUp.add([this](POINT, bool) { this->onUp(); });
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
	// 同 WinBall / WinPin：小窗口得把最小跟踪尺寸放到 1，否则系统按回 800×600
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
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
	closeBtn->setWidth(30.f);
	closeBtn->setHeight(kTitleH);
	closeBtn->setBg(Ling::Color(0));
	closeBtn->setHoverBg(Ling::Color(0xE81123FF));
	closeBtn->setColor(Ling::Color(0xFFFFFFFF));
	closeBtn->setHoverColor(Ling::Color(0xFFFFFFFF));
	closeBtn->onClick.add([this](Ling::Button*) { close(); });

	// 文本区：真控件，双击选词 / 三击选段 / Ctrl+A C X V 全自带
	textBox = body->makeChild<Ling::TextBox>();
	textBox->setFlexGrow(1.f);
	textBox->setBg(Ling::Color(0xFFFFFFFF));
	textBox->setPadding(8.f);
	textBox->setFontSize(14.f);
	textBox->setText(content);
	textBox->focus();
	show();
}

void WinTextPin::onDown(POINT pos, bool isRight)
{
	// 只有点在标题栏（且不是关闭按钮）上才拖动；点文本区交给 TextBox 自己处理
	if (isRight) return;
	if (closeBtn && closeBtn->isPosIn(pos)) return;
	if (pos.y > kTitleH * dpi) return;
	dragging = true;
	dragStartMouse = pos;
	dragStartX = x;
	dragStartY = y;
	SetCapture(hwnd);
}

void WinTextPin::onMove(POINT pos)
{
	if (!dragging) return;
	setPosition(dragStartX + (pos.x - dragStartMouse.x), dragStartY + (pos.y - dragStartMouse.y));
}

void WinTextPin::onUp()
{
	if (!dragging) return;
	dragging = false;
	ReleaseCapture();
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
