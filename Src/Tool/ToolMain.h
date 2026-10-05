#pragma once
#include <include/Ling.h>
class WinPin;
class Tip;
class ToolMain : public Ling::WinBase
{
public:
	ToolMain(WinPin* win);
	~ToolMain();
	static void init();
	float getBtnCenterX();
	// 选中 id 对应的工具（等同于用户点了那个按钮，但不带"再点一次取消选中"的开关语义）。
	// 从 ToolCap 上一个标注工具直接进贴图窗口时用它预选，省掉第二步点击
	void selectTool(const std::wstring& id);
	// 取消当前选中：清空 curId、把所有按钮恢复常态配色，并重排工具组（curId 空了 ToolSub 会隐藏）。
	void cancelSelect();
	// 重新显示工具条时用。ToolSub 的 hideTools 只是把窗口藏起来并置掉 hasTools 标志，
	// 之后它就再也不会自己出来（updatePosition 见 hasTools 为假直接返回），
	// 必须按当前 curId 重跑一遍 show*Tools 才重建得出来。
	// 从缩略图 / 贴边细条还原时正是这种"curId 还在、子工具条却没了"的情况
	void refreshToolSub();
	// 两态开关按钮的选中底色（现在只有「选文」用）。开关状态本身住在 WinPin 上 ——
	// ESC、拿起别的标注工具、关窗都要把它复位，按钮自己记不住，所以由那边回报过来
	void setToggle(const std::wstring& id, bool on);
public:
	std::wstring curId;
private:
	void onCreated() override;
	void onClick(Ling::Button* btn);
	void onMinMaxInfo(MINMAXINFO* mmi);
	// 未选中态配色，选中态在 onClick 里就地设置
	void applyNormalStyle(Ling::Button* btn);
	// 按当前 dpi 把窗口尺寸算出来并应用。构造时算一次，DPI 变了再算一次
	void refreshSize();
private:
	WinPin* win;
	// onDpiChanged 与 onSizeChanged 之间的接力标记，见构造函数里的注释
	bool dpiChanged{ false };
	// 逻辑像素，交给 Ling 的 setter 时由其内部乘 dpi
	static constexpr float btnSize{ 32.f };
	static constexpr float spliterW{ 1.f };
	std::vector<std::wstring> btnIds = { L"rect",L"ellipse",L"arrow",L"number",L"line",L"text",L"mosaic", L"eraser",L"watermark",L"pin",L"|",L"undo",L"redo",L"|",L"pinCrop",L"pinHide",L"textSelect",L"close",L"save",L"clipboard" };
	// textSelect 借的是「文字识别」那枚 \ue67b。图标字体只有 41 个码位，其余全都有主
	//（见工作区笔记），好在两者语义就是一件事 —— 选的就是识别出来的那些字，
	// 而且本工具条上没有第二个用它的按钮，不会在同一屏里撞脸
	std::vector<std::wstring> btnCodes = { L"\ue8e8",L"\ue6bc",L"\ue603",L"\ue776",L"\ue601",L"\ue6ec",L"\ue82e",L"\ue6be",L"\ue607",L"\ue6a2",L"|",L"\ued85",L"\ued8a",L"|",L"\ue905",L"\ue907",L"\ue67b",L"\ue62d",L"\ue608",L"\ue6ad" };
	std::vector<Ling::Button*> btns;
	// 悬停提示。要 hwnd，所以在 onCreated 里才建得起来
	std::unique_ptr<Tip> tip;
};

