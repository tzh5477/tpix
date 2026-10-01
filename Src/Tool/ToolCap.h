#pragma once
#include <include/Ling.h>

class WinCap;
class Tip;
// 框选完成后出现在选区下方的工具条，横排一行：
// 标注工具 9 个 + 分隔线 + 图像标记 / 保存 / 剪切板 / 关闭。
// 长截图 / 录屏 / OCR / 二维码这四条挪到了选区右边缘外的 ToolCapStage 上，
// 原先"更多"那行折叠也一起去掉了，按钮全部平铺。
// 标注工具点到就直接进贴图窗口并预选该工具，省掉原先"先点图像标记，再点工具"的那一步。
class ToolCap : public Ling::WinBase
{
public:
	ToolCap(WinCap* win);
	~ToolCap();
private:
	void onCreated() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	void onClick(Ling::Button* btn);
	// 按当前 dpi 把窗口尺寸算出来并应用。构造时算一次，DPI 变了再算
	void refreshSize();
	// 建一个工具按钮，一律走图标字体
	Ling::Button* makeBtn(Ling::Node* parent, const std::wstring& id,
		const std::wstring& code, const std::wstring& tipKey);
private:
	struct BtnDef
	{
		std::wstring id;
		std::wstring code;   // 图标字体的码位
		std::wstring tip;    // 语言 key，空串表示不挂提示
	};
	WinCap* win;
	// onDpiChanged 与 onSizeChanged 之间的接力标记，见构造函数里的注释
	bool dpiChanged{ false };

	// 悬停提示。要 hwnd，所以在 onCreated 里才建得起来
	std::unique_ptr<Tip> tip;
	// 以下都是逻辑像素，交给 Ling 的 setter 时由其内部乘 dpi
	static constexpr float btnSize{ 32.f };
	static constexpr float spliterW{ 1.f };
	// 标注工具。不再按配置分主次 —— 折叠去掉之后全部平铺在这一行
	static const std::vector<BtnDef> shapeBtns;
	static const std::vector<BtnDef> actionBtns;
};
