#pragma once
#include <include/Ling.h>
#include <functional>
#include <string>
#include <vector>

// 下拉选择框：点一下弹出行列，一次点中想要的那一档，不用在几档之间来回转。
//
// 弹的是独立顶层窗口，不是挂在宿主窗口里的一层：标注工具条只有一行高，
// 设置页靠底部的那些行列表也会伸到窗口外面，宿主窗口装不下它。
//
// 同一时刻只可能有一个下拉开着（点开另一个要先收起前一个），所以做成静态的。
class SelectPopup
{
public:
	// 列表项上要画的小图。传了它就不再显示 items[i] 的文字，改由它在自己那一格里画出来
	// （线条的那三个下拉用它把十档端点 / 六档线型 / 五档箭头画成真实的线条，而不是
	// 一个一个符号）。rect 是那一格的范围（物理像素，原点 = 格子左上角），index 是第几档。
	// SelectPopup 本身不认这些形状 —— 画什么完全由调用方给
	using SamplePainter = std::function<void(ID2D1DeviceContext*, const D2D1_RECT_F&, int)>;
	// anchor 是被点的那个按钮，拿它的位置和宽度定弹出点。
	// items 是全部选项；cur 是当前选中项下标（-1 表示都不选）；
	// onPick 收用户选中的下标，由调用方自己去落盘与刷新。
	// 同一个按钮再点一次就是收起，调用方不用自己记开关状态。
	// fontFamily 给装了图标字体的那些按钮用（马赛克模式、开 / 关两项），不传就是普通字体。
	// minW 是列表的最小宽度（逻辑像素），给内容比按钮宽的那种列表用 —— 字体名能长到十几个字符。
	// paintSample 传了就是"自绘项"（上面的 SamplePainter）。items 仍要照传：
	// 档位数按它的长度算，宿主也还拿它当按钮上的兜底文字
	static void show(Ling::WinBase* owner, Ling::Node* anchor,
		const std::vector<std::wstring>& items, int cur,
		std::function<void(int)> onPick, const std::wstring& fontFamily = {},
		float minW = 0.f, SamplePainter paintSample = {});
	// 收起。点到列表外、宿主窗口被移动或销毁都会自动走到这里，一般不用外部调
	static void close();
	static bool isOpen();
};
