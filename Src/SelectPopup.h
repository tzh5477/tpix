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
	// anchor 是被点的那个按钮，拿它的位置和宽度定弹出点。
	// items 是全部选项；cur 是当前选中项下标（-1 表示都不选）；
	// onPick 收用户选中的下标，由调用方自己去落盘与刷新。
	// 同一个按钮再点一次就是收起，调用方不用自己记开关状态。
	// fontFamily 给装了图标字体的那些按钮用（马赛克模式、开 / 关两项），不传就是普通字体
	static void show(Ling::WinBase* owner, Ling::Node* anchor,
		const std::vector<std::wstring>& items, int cur,
		std::function<void(int)> onPick, const std::wstring& fontFamily = {});
	// 收起。点到列表外、宿主窗口被移动或销毁都会自动走到这里，一般不用外部调
	static void close();
	static bool isOpen();
};
