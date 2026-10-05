#pragma once
#include <include/Ling.h>

class ToolSub;

// 水印的样式浮层：不透明度 / 大小 / 间距三项，竖排一行一项（对齐 pixpin）。
//
// 为什么从工具条搬到这里：水印那一行原本挤了文字框 + 字体 + 时间 + 位置 + 旋转 +
// 三个横滑块 + 色板，八九个控件抢一条 32 像素高的窄条，滑块被压得几乎拖不动。
// 竖排浮层里每一项都能给到正常宽度，滑块拖着也顺手。
//
// 它是独立顶层窗口（与 SelectPopup 同一套做法），按工具条的位置摆在下面。
// 鼠标移出就收起 —— 这与"点一下弹出来、点外面才收"的下拉列表不是一回事：
// 滑块要能一路拖到底，中途把面板收了等于没法调。
class WinWatermarkPanel
{
public:
	// sub 是水印工具条（它持有水印样式的真值与落盘那一半），anchor 是它里面的一个节点，
	// 浮层按 anchor 的下边缘摆。值的读写与重画都经由 sub 那三个 setWatermark*，
	// 浮层自己不碰 config.json
	static void show(ToolSub* sub, Ling::Node* anchor);
	// 收起。没开着时是空操作
	static void close();
	static bool isOpen();
};
