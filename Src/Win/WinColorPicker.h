#pragma once
#include <include/Ling.h>
#include <functional>
#include <vector>

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
//
// 颜色的读与写由调用方以 Source 提供：面板自己不碰 config.json、也不知道颜色属于谁
// —— 工具条（ToolSub）与设置页主题色（WinSettingCommon）共用这一个面板
class WinColorPicker
{
public:
	// 两排栅格的数据与颜色的写回。颜色 = RRGGBBAA（与 Ling::Color 同一个排法）。
	// presets / customs 是 show 那一刻的快照，面板开着的时候外部变了不重铺
	struct Source
	{
		std::function<UINT32()> current{ nullptr };       // 面板初值 + 格子上对勾的判据
		std::function<void(UINT32)> onUse{ nullptr };     // 点任意色格：立即生效（面板随后关闭）
		std::function<void(UINT32)> onConfirm{ nullptr }; // 「确定」：SV 调出来的颜色（生效 + 存自定义）
		std::vector<UINT32> presets;                      // 「常用颜色」栅格
		std::vector<UINT32> customs;                      // 「自定义」栅格（可为空）
	};
	// anchor 是色板行上锚定的那块（面板按它定位，点到它不算"点到别处"）；
	// owner 是 anchor 所在的窗口 —— 它挪位置面板就收
	static void show(const Source& src, Ling::Node* anchor, Ling::WinBase* owner);
	static void close();
	static bool isOpen();
};
