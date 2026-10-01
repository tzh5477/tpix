#pragma once
#include <include/Ling.h>

class WinCap;
class Tip;
// 选区右边缘外那条竖排的工具条：长截图 / 录屏 / 文字识别 / 二维码识别。
// 与选区下方横排的 ToolCap 是两个独立窗口 —— "右侧"和"下方"是两个位置，
// 一个矩形窗口摆不下，只能拆开。
class ToolCapStage : public Ling::WinBase
{
public:
	ToolCapStage(WinCap* win);
	~ToolCapStage();
private:
	void onCreated() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	void onClick(Ling::Button* btn);
	// 按当前 dpi 重算窗口尺寸（竖着一条：宽一格、高按按钮数）
	void refreshSize();
	Ling::Button* makeBtn(Ling::Node* parent, const std::wstring& id,
		const std::wstring& code, const std::wstring& tipKey);
private:
	struct BtnDef
	{
		std::wstring id;
		std::wstring code;   // 图标字体码位，系统字库画的文本也是它
		std::wstring tip;    // 提示 key，Lang::get 之后展示
	};
	WinCap* win;
	// onDpiChanged 与 onSizeChanged 之间的接力标记，与 ToolCap 同一个套路
	bool dpiChanged{ false };
	// 悬停提示。要 hwnd，所以得在 onCreated 里才建得起来
	std::unique_ptr<Tip> tip;
	// 沿用 ToolCap 的格子大小，两个窗口看起来才是一套东西
	static constexpr float btnSize{ 32.f };
	static const std::vector<BtnDef> stageBtns;
};
