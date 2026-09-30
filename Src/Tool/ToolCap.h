#pragma once
#include <include/Ling.h>

class WinCap;
class Tip;
// 框选完成后出现在选区右下方的工具条。分两行：
// 主行是长截图 / 录屏 / OCR / 二维码 + 启用的标注工具 + 更多；
// 更多的那一行（.expand）里放关闭 / 保存 / 剪切板 / 图像标记，以及被配置关掉的标注工具。
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
	// 按当前 dpi 把窗口尺寸算出来并应用。构造时算一次，DPI 变了或展开/收起时再算
	void refreshSize();
	// 建一个工具按钮。除了"更多"用系统字体画字符（见 .cpp），其余一律走图标字体
	Ling::Button* makeBtn(Ling::Node* parent, const std::wstring& id,
		const std::wstring& code, const std::wstring& tipKey);
	// 展开 / 收起第二行。展开会让窗口长高，所以要让宿主重排位置
	void setExpanded(const bool expanded);
	// 按配置算出主行 / 更多行各摆几个按钮。尺寸（构造时就要）与实际摆放（onCreated）
	// 都从这里取，免得两处规则各写一份
	void computeRowCounts(int& mainCount, int& extraCount);
private:
	struct BtnDef
	{
		std::wstring id;
		std::wstring code;   // 图标字体的码位；用系统字体画的短文本也放这儿
		std::wstring tip;    // 语言 key，空串表示不挂提示
	};
	WinCap* win;
	// onDpiChanged 与 onSizeChanged 之间的接力标记，见构造函数里的注释
	bool dpiChanged{ false };

	// 悬停提示。要 hwnd，所以在 onCreated 里才建得起来
	std::unique_ptr<Tip> tip;
	Ling::Node* rowMain{ nullptr };
	Ling::Node* rowExtra{ nullptr };
	Ling::Button* btnMore{ nullptr };
	bool expanded{ false };
	// 以下都是逻辑像素，交给 Ling 的 setter 时由其内部乘 dpi
	static constexpr float btnSize{ 32.f };
	static constexpr float spliterW{ 1.f };
	// 截图之后的那几条路。不含 mark —— 每个标注工具都能直接带着工具进贴图窗口，
	// mark 退到"更多"里当"只贴图标不预选工具"的兜底
	static const std::vector<BtnDef> stageBtns;
	// 标注工具。是否出现在主行取决于配置 toolCap.<id>
	static const std::vector<BtnDef> shapeBtns;
	// 默认进主行的标注工具，其余默认收在"更多"里
	static const std::vector<std::wstring> defaultShapeIds;
	static const std::vector<BtnDef> actionBtns;
};
