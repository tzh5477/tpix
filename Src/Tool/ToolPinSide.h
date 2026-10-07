#pragma once
#include <include/Ling.h>

class WinPin;
class Tip;
// 编辑界面（贴图窗口）右边缘外那条竖排工具条：长截图 / 录屏 / 文本识别 / 二维码识别。
//
// 这几个动作原来挂在截图选区界面（WinCap 的 ToolCapStage）上，只有"框完选区"那一小段
// 时间里点得到；而它们要的东西（一张已经拿到手的图）恰恰是编辑界面里才齐的。现在框完选区
// 直接进编辑界面，这条竖排就跟着搬到这边来，一直陪着这张图。
//
// 与选区下方那条横排（ToolMain / ToolSub）是两个独立窗口 —— "右侧"和"下方"是两个位置，
// 一个矩形窗口摆不下，只能拆开。宿主是 WinPin，摆放规则见 WinPin::layoutTools。
class ToolPinSide : public Ling::WinBase
{
public:
	ToolPinSide(WinPin* win);
	~ToolPinSide();
private:
	void onCreated() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	void onClick(Ling::Button* btn);
	// 竖排一条：宽一个格子，高按按钮数
	void refreshSize();
	Ling::Button* makeBtn(Ling::Node* parent, const std::wstring& id,
		const std::wstring& code, const std::wstring& tipKey);
private:
	struct BtnDef
	{
		std::wstring id;
		std::wstring code;   // 图标字体码位
		std::wstring tip;    // 提示 key，Lang::get 之后展示
	};
	WinPin* win;
	// 悬停提示。要 hwnd，所以在 onCreated 里才建得起来
	std::unique_ptr<Tip> tip;
	// onDpiChanged 与 onSizeChanged 之间的接力标记，与 ToolMain / ToolSub 同一个套路
	bool dpiChanged{ false };
	// 沿用 ToolMain 那一排的格子大小，两个方向看起来才是一套东西
	static constexpr float btnSize{ 32.f };
	// 码位与提示 key 从原来那份表里原样搬过来（见 ToolCap → ToolPinSide 的迁移），
	// 用户从截图那一刻起看到的图标就没变过
	static const std::vector<BtnDef> sideBtns;
};
