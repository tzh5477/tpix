#pragma once
#include <include/Ling.h>
#include <functional>
#include <string>

// 水印的「内容编辑」弹窗（对齐 pixpin 的「输入水印」）。
//
// 为什么把文字、字体、时间三项收进一个弹窗，而不是都摆在工具条上：
// 工具条只有一行高，水印一项就占了文字框 + 字体 + 时间 + 位置 + 旋转 + 三个滑块，
// 挤得连色板都要换行。收进弹窗之后工具条只剩"位置 / 旋转 / 内容"三件事，
// 而文字、字体、时间本来就是一次写好、很少中途改的，弹窗里正好从容摆开。
//
// 弹的是独立顶层窗口（与 SelectPopup 同一套做法）：它比工具条高得多，
// 装不进工具条那一亩三分地。
class WinWatermarkText
{
public:
	// anchor 是被点的那个按钮（工具条上的「内容…」），弹窗按它的位置摆。
	// curText / curFamily 是当前值（弹窗打开时先填进去）。
	// onApply 只在用户点「应用」或按 Enter 时回调一次，参数就是要落盘的那份；
	// 取消 / Esc / × 都不回调 —— 试用不留在配置里
	static void show(Ling::WinBase* owner, Ling::Node* anchor,
		const std::wstring& curText, const std::wstring& curFamily,
		std::function<void(const std::wstring&, const std::wstring&)> onApply);
	// 收起。没开着时是空操作，调用方不用先判
	static void close();
};
