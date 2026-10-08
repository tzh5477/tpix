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
	// 「几何图形」那枚按钮上画哪个图标。矩形与圆形并成同一枚工具之后，主工具条上
	// 不再有第二枚按钮可看"当前是哪一类"，于是图标跟着类别走：现在是矩形就画方框、
	// 是圆形就画圆 —— 与子面板上那两枚类别小图标选中的那一档一致。
	// ToolSub 上换类别时调它（那里是类别的落盘方）
	void syncGeomIcon();
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
	// 描一支 I 形文本光标（「选文」那枚）。同样是归一化坐标 + 等比缩放，见实现
	void paintTextSelectIcon(ID2D1DeviceContext* ctx, float w, float h);
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
	// 直接点它、点中哪个元素就切到哪个元素的工具上（见 WinPin::onUp）。
	// geom 那一枚是「几何图形」：矩形与圆形并成同一个工具，画哪一类由子面板上那两枚
	// 类别小图标定（见 ToolSub::makeGeomKindBtns），主工具条上不再各占一格 ——
	// 两格并一格省出来的宽度正好够子面板多摆两枚类别图标。
	// 尾部三枚的顺序：复制（不关窗）→ 保存 → 剪切板（复制后关窗）→ 关闭收尾
	std::vector<std::wstring> btnIds = { L"selector",L"geom",L"arrow",L"number",L"line",L"text",L"mosaic", L"eraser",L"watermark",L"pin",L"|",L"undo",L"redo",L"|",L"pinHide",L"textSelect",L"copy",L"save",L"clipboard",L"close" };
	// 「选文」（textSelect）与「选择对象」一样是空串：那两枚都不写字，自己画。
	// textSelect 原来借的是「文字识别」那枚 \ue67b —— 编辑界面右边缘那条竖排
	//（ToolPinSide）上正好也有一枚 \ue67b，同一个屏里撞脸。图标字体只有 41 个码位
	// 且全部有主（见工作区笔记），没有现成的 I 形文本光标，于是照「选择对象」那套自绘：
	// 一支文本光标最能说清"在这儿选字"（另两个空闲的 E909 右箭头 / E97F 调色板都不合适）
	// 「复制」那枚 \ue90b 是照 Doc/tools/mkicons.py 补进字体的两张错位的页
	// geom 那枚的码位只是初值（矩形那一档）。真正显示什么由 syncGeomIcon 按当前类别定，
	// 它会在窗口建好与每次换类别时重设一次
	std::vector<std::wstring> btnCodes = { L"",L"\ue8e8",L"\ue603",L"\ue776",L"\ue601",L"\ue6ec",L"\ue82e",L"\ue6be",L"\ue607",L"\ue6a2",L"|",L"\ued85",L"\ued8a",L"|",L"\ue907",L"",L"\ue90b",L"\ue608",L"\ue6ad",L"\ue62d" };
	std::vector<Ling::Button*> btns;
	// 「几何图形」那一枚。换类别时要换它上面的图标，存下来省得每次去 btns 里按 id 找
	Ling::Button* geomBtn{ nullptr };
	// 「选择对象」那枚按钮里垫的自绘画布（Button 不能自绘，Canvas 收不到鼠标，叠起来才两样都有）
	Ling::Canvas* selectIcon{ nullptr };
	// 「选文」那枚里垫的自绘画布，同上（画的是一支 I 形文本光标）
	Ling::Canvas* textSelectIcon{ nullptr };
	// 指针的白底与深色描边。与 ToolSub 那几支一样绑在设备上，建一次够
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushCursor;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushCursorEdge;
	// 自绘图标（那支 I 形文本光标）的墨色。取值与 Button 默认字色同色，
	// 免得同一排里有一枚比别的深一档
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushIconInk;
	// 悬停提示。要 hwnd，所以在 onCreated 里才建得起来
	std::unique_ptr<Tip> tip;
};

