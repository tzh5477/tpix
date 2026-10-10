#pragma once
#include <include/Ling.h>

// 录制当中改「时长上限」的小窗。点录屏工具条上那一格计时弹出来。
//
// 为什么单独开一扇窗，而不是就地画在工具条上：工具条只有 32 逻辑像素高，塞不下输入框；
// 而 SelectPopup / Tip 那一类浮层都是 WS_EX_NOACTIVATE（收不了键盘），也承载不了 TextBox。
// 所以照 WinTextPin 那套来：自己的顶层小窗 + 自己 show() + 一个 TextBox。
//
// 它**只记着工具条的 HWND**（一个值，不是指针）：上限改完要让工具条立刻重画倒计时，
// 而工具条随时可能被销毁（典型是到点自动停止，那正是这个小窗最可能开着的时刻），
// 存 ToolVideo* 就是一颗定时炸弹。需要刷工具条时现问 ToolVideo::living()。
class WinRecordLimit : public Ling::WinBase
{
public:
	// 弹窗。已经开着就先收掉重开（等于把焦点抢回来），不会堆出第二扇。
	// toolHwnd 只用来定位；dpi 跟着工具条那块屏走
	static void show(HWND toolHwnd, float dpi);
	// 录屏工具条要销毁了：这个小窗跟着收掉，别让它孤儿一样留在桌面上 ——
	// 那时它已经改不了任何东西（工具条没了，倒计时也没了）
	static void closeIfOpen();
	// 退出流程里调：窗口对象是文件级静态变量，交给静态析构就在 CoUninitialize 之后了
	static void dispose();
private:
	explicit WinRecordLimit(HWND toolHwnd, float dpi);
	void onCreated() override;
	// ok 为真才落盘；为假是取消，一个字都不改。两者都关窗
	void apply(bool ok);
	// 落盘 + 让工具条立刻重画倒计时 + 关窗。「不限」按钮与「确定」解析出的数字都走这里
	void applyValue(int minutes);
	// 落在工具条上方（上面放不下就翻到下方），并夹进所在显示器的工作区
	void placeAboveTool();
	void closeWin();
private:
	HWND toolHwnd{ nullptr };
	Ling::TextBox* edit{ nullptr };
	// 关窗只能关一次：ESC 会同时走到 TextBox 的按键回调与窗口级按键事件上
	bool closed{ false };
};
