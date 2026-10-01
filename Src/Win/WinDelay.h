#pragma once
#include <include/Ling.h>

// 延时截图的倒计时。单独一个窗口，而不是画在 WinCap 上：
// WinCap 一建起来屏幕就被那张静态底图盖住了，在它上面倒数等于让人对着一张死图等，
// 而且底图是建窗那一刻取的，数完了还得重取一遍。所以先由这个小窗顶着，数完再让位。
class WinDelay:public Ling::WinBase
{
public:
	~WinDelay();
	// 调用方先判过延时大于 0 才进来。
	// enter 原样交给数完那一枪的 WinCap::initNow —— 延时只推迟"什么时候抓屏"，
	// 不改变"抓完走哪条路"
	static void start(int seconds, const std::wstring& enter = L"");
	static void dispose();
private:
	WinDelay(int seconds, const std::wstring& enter);
	void onCreated() override;
	void tick();
	// startCap 为真表示数完了，接着进截图；为假是用户按 Esc 取消了
	void finish(bool startCap);
private:
	int left{ 0 };
	std::wstring enter;
	Ling::Label* label{ nullptr };
};
