#pragma once
#include <include/Ling.h>

class WinCap;
class Tip;
class ToolLong : public Ling::WinBase
{
public:
	ToolLong(WinCap* win);
	~ToolLong();
	// 手动 / 自动切过之后把模式按钮的图标换成对应的那个
	void refreshMode();
private:
	void onCreated() override;
	void onClick(Ling::Button* btn);
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	// 按当前 dpi 把窗口尺寸算出来并应用。构造时算一次，DPI 变了再算一次
	void refreshSize();
private:
	WinCap* win;
	// onDpiChanged 与 onSizeChanged 之间的接力标记，见构造函数里的注释
	bool dpiChanged{ false };
	// 逻辑像素，交给 Ling 的 setter 时由其内部乘 dpi
	static constexpr float btnSize{ 32.f };
	// mode 是"自动 / 手动"开关，图标跟着状态在“开 / 关”两个码位之间换
	// 只有这两枚。原先中间还有裁剪长图 / 贴图 / 保存 / 剪贴板四枚 —— 它们都是"绕过编辑界面"
	// 的出口（直接裁、直接钉、直接存、直接进剪切板），而滚到底现在会自动把这轮成图开进
	// 编辑界面（cc59899），那四枚要的东西编辑界面里全都有，留在滚动过程中只会让人分心
	std::vector<std::wstring> btnIds = { L"mode",L"close" };
	std::vector<std::wstring> btnCodes = { L"\ue688",L"\ue62d" };
	// 模式按钮的两个图标：开关"开"（自动）与"关"（手动）
	static constexpr const wchar_t* modeIconOn{ L"\ue688" };
	static constexpr const wchar_t* modeIconOff{ L"\ue687" };
	Ling::Button* btnMode{ nullptr };
	// 悬停提示。要 hwnd，所以在 onCreated 里才建得起来
	std::unique_ptr<Tip> tip;
};
