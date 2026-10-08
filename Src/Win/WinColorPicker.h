#pragma once
#include <include/Ling.h>

class ToolSub;

// 取色器：色板行末尾那块「当前色」点开的就是它。
// 对齐 pixpin —— 预设色摆在工具条上供快选，任意颜色点最大的那块进这里调。
//
// 为什么是 WS_EX_NOACTIVATE 的独立顶层窗口（与 SelectPopup / WinWatermarkPanel 同一套）：
// 工具条本身就不参与激活（见 ToolSub 构造里的注释），编辑文字时点颜色不该把键盘焦点抢走
// —— 丢焦点 = TextBox 失焦 = 正在输入的那行字被打断。这里更进一步：**弹窗里一个能输入的
// 控件都没有**，调色全靠鼠标拖（SV 方块、色相条、两排格子）。想键入色号就得让弹窗能拿到
// 焦点，代价是打断文字编辑 —— 不值。色号在弹窗里只做只读显示。
//
// 它是独立顶层窗口而不是挂在工具条里的一层：工具条只有一行高，装不下这个近乎方形的面板。
class WinColorPicker
{
public:
	// sub 是工具条 —— 颜色的真值与落盘都在它那儿（setColorIndex / applyColor），
	// 取色器自己不碰 config.json。anchor 是色板行末尾那块，弹窗按它定位。
	// 已经开着时再调一次是收起
	static void show(ToolSub* sub, Ling::Node* anchor);
	static void close();
	static bool isOpen();
};
