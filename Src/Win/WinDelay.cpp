#include "pch.h"
#include "../Lang.h"
#include "../App.h"
#include "WinCap.h"
#include "WinDelay.h"

namespace {
	std::unique_ptr<WinDelay> winDelay;
	// 定时器 id 用 100：本窗口只有这一个定时器，id 随手给即可
	constexpr UINT tickId{ 100 };
}

WinDelay::WinDelay(int seconds)
	: Ling::WinBase(), left(seconds)
{
	// 关窗按钮的点击栈上不能同步 reset（use-after-free），推迟到下一轮消息循环
	onDestroy.add([]() {
		Ling::App::get()->dq.TryEnqueue([]() { winDelay.reset(); });
	});
	setTitle(Lang::get(L"delay.title"));
	setSize(200.f, 140.f);
	setCenter();
	createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
}

WinDelay::~WinDelay()
{
}

void WinDelay::start(int seconds)
{
	if (winDelay) return;
	winDelay.reset(new WinDelay(seconds));
}

void WinDelay::dispose()
{
	winDelay.reset();
}

void WinDelay::onCreated()
{
	enableShadow();
	// 倒数完紧接着就抓屏，那时 hide()+close() 还没被合成器消化掉，窗口会被自己截进去。
	// 用抓屏摘除标记，比赌时序稳（与 ToolVideo / WinBall 同一套做法）
	App::excludeFromCapture(hwnd);
	body->setBg(0xFFFFFFFF);
	body->setFlexDirection(Ling::FlexDirection::Column);
	body->setAlignItems(Ling::Align::Center);
	body->setJustifyContent(Ling::Justify::Center);

	label = body->makeChild<Ling::Label>();
	label->setFontSize(52.f);
	label->setText(std::to_wstring(left));

	auto tip = body->makeChild<Ling::Label>();
	tip->setFontSize(12.f);
	tip->setText(Lang::get(L"delay.cancel"));

	onKeyDown.add([this](UINT key) {
		if (key == VK_ESCAPE) finish(false);
	});
	// 点一下也算取消：热键触发时前台还在别的进程，键盘未必落得到这个窗口上
	onMouseDown.add([this](POINT, bool) { finish(false); });
	onTimer.add([this](UINT id) {
		if (id == tickId) tick();
	});
	setTimer(1000, tickId);
	show();
	// 上面 show 只做了 ShowWindow，不抢焦点的话 Esc 会被别的窗口吃掉
	SetFocus(hwnd);
}

void WinDelay::tick()
{
	--left;
	if (left <= 0) {
		finish(true);
		return;
	}
	label->setText(std::to_wstring(left));
}

void WinDelay::finish(bool startCap)
{
	killTimer(tickId);
	// 先藏后关：截图紧跟着就取屏，DestroyWindow 是同步的，但合成器那一帧可能还在屏上
	hide();
	close();
	if (startCap) WinCap::initNow();
}
