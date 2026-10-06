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
	// 重画「选择对象」那枚按钮里的鼠标指针。画在内嵌的那张画布上，与 ToolSub 的样例同一套做法
	void layout() override;
	void onClick(Ling::Button* btn);
	void onMinMaxInfo(MINMAXINFO* mmi);
	// 描一个鼠标指针的轮廓（各点坐标见实现）。w / h 是画布尺寸，物理像素
	void paintSelectIcon(ID2D1DeviceContext* ctx, float w, float h);
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
	// 没有"图片剪裁"这个按钮：剪裁的 8 个采样点常驻在贴图窗口的边框上，随时可拖
	//（见 WinPin::paintCropHandles），不必先按一个按钮进"剪裁态"。
	// 排在最前的 select 是「选择对象」：不用先判断"这是哪个组件"再回去切工具，
	// 直接点它、点中哪个元素就切到哪个元素的工具上（见 WinPin::onUp）
	std::vector<std::wstring> btnIds = { L"selector",L"rect",L"ellipse",L"arrow",L"number",L"line",L"text",L"mosaic", L"eraser",L"watermark",L"pin",L"|",L"undo",L"redo",L"|",L"pinHide",L"textSelect",L"close",L"save",L"clipboard" };
	// textSelect 借的是「文字识别」那枚 \ue67b。图标字体只有 41 个码位，其余全都有主
	//（见工作区笔记），好在两者语义就是一件事 —— 选的就是识别出来的那些字，
	// 而且本工具条上没有第二个用它的按钮，不会在同一屏里撞脸。
	// 第一项（「选择对象」）是空串：那枚按钮不写字，自己画一个鼠标指针上去
	//（icon 字体里没有指针形状，41 个码位全部有主；见 layout / paintSelectIcon）
	std::vector<std::wstring> btnCodes = { L"",L"\ue8e8",L"\ue6bc",L"\ue603",L"\ue776",L"\ue601",L"\ue6ec",L"\ue82e",L"\ue6be",L"\ue607",L"\ue6a2",L"|",L"\ued85",L"\ued8a",L"|",L"\ue907",L"\ue67b",L"\ue62d",L"\ue608",L"\ue6ad" };
	std::vector<Ling::Button*> btns;
	// 「选择对象」那枚按钮里垫的自绘画布（Button 不能自绘，Canvas 收不到鼠标，叠起来才两样都有）
	Ling::Canvas* selectIcon{ nullptr };
	// 指针的白底与深色描边。与 ToolSub 那几支一样绑在设备上，建一次够
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushCursor;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushCursorEdge;
	// 悬停提示。要 hwnd，所以在 onCreated 里才建得起来
	std::unique_ptr<Tip> tip;
};

