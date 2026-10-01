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
	// WS_EX_NOACTIVATE：这个窗口不能抢焦点。延时截图要的就是"挡它几秒钟让我去摆画面" ——
	// 这期间用户多半正摊着一个右键菜单或下拉菜单，一旦激活过来，焦点转移的那一刻菜单就收起了，
	// 摆好的画面跟着没了，延时也就白等了。代价是本窗口收不到键盘（见 onCreated 里的 Esc 兜底）
	createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP);
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

	// 拿了 NOACTIVATE 就等于永远没有焦点，按键事件收不到，Esc 只能靠 tick() 定时去问。
	// 顺带的好消息也正在这里：焦点不转移，用户摊开的菜单才不会被引走。
	// 另外点一下也算取消 —— 热键触发时前台还在别的进程，键盘未必落得到这个窗口上
	onMouseDown.add([this](POINT, bool) { finish(false); });
	onTimer.add([this](UINT id) {
		if (id == tickId) tick();
	});
	// "上次查询之后按过"这个位是按线程记的：按热键之前用户可能刚用 Esc 关掉一层菜单，
	// 不清掉的话第一次 tick 就会把它误判成取消。这里先空读一次
	GetAsyncKeyState(VK_ESCAPE);
	setTimer(1000, tickId);
	show();
}

void WinDelay::tick()
{
	// 取 & 1（上次查询之后按过）而不是 & 0x8000（此刻按住）：倒计时一秒才走一次，
	// 只看按住的话，一次寻常的短按几乎必然落在两次 tick 之间，取消键就形同虚设了
	if (GetAsyncKeyState(VK_ESCAPE) & 1) {
		finish(false);
		return;
	}
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
