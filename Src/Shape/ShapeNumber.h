#pragma once
#include <include/Ling.h>
#include "ShapeBase.h"
class ShapeNumber : public ShapeBase
{
public:
	enum class NumStyle { Arabic = 0, AlphaLower, AlphaUpper, Roman, Chinese };
	// 外圈样式。Circle / Square 是 pixpin 那种纯粹的圈号（没有尾巴），是默认的两项；
	// 带尾巴的"指向某处"版本单独留成带 Arrow 的两个
	enum class RingStyle { Circle = 0, Square, CircleArrow, SquareArrow, None };
	ShapeNumber(Canvas* win);
	~ShapeNumber();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void mouseWheel(const float x, const float y, const short delta) override;
	void setCursor() override;
	// 选中态下的按键：+/- 改编号并级联，F2 编辑序号里的文字
	void onKey(UINT key) override;
	// 单击就是它的正常用法：落一个序号徽章，不需要拖动
	bool isValidWithoutDrag() override { return true; };
	// ToolSub 上的编号样式 / 外圈样式变了，重排自己的几何与文字
	void applyStyle() override;
	// 退出编辑并把 TextBox 里的文字收回来。除了本类内部，Canvas（导出图片前）
	// 与 History（删除 shape 前）也会调，同 ShapeText::finishEdit
	void finishEdit();
	// ShapeBase 上的统一收尾口子。窗口（关窗 / 缩放 / 导出）与 History（删 shape）都只认它，
	// 没有这个转发的话正在编辑的序号收不了尾：TextBox 不隐藏、编辑标记不清、订阅不摘
	void finishEditing() override { finishEdit(); }
	// val 序列化成序号字符串。ToolSub 的样式切换按钮要显示当前样式下的样子，所以是 public static
	static std::wstring serializeVal(const int val, const NumStyle style);
public:
	int val{ 1 };
	bool isEditing{ false };
private:
	// 把本序号调成 newVal，与它撞号的那一个顶到 newVal+1，再撞就继续往下顶（级联）
	void setValAndPush(const int newVal);
	D2D1_POINT_2F localPoint(const float degrees);
	D2D1_POINT_2F transformPoint(const D2D1_POINT_2F& point);
	void makePath();
	// 半径变化后要重排文字，两处调用点合到一起
	void makeTextLayout();
	// 外圈带不带指向尾巴。不带尾巴时那个"指向"控制点没有意义，不画也不响应
	bool hasTail() const;
	// 徽章左侧那两个 + / − 小按钮的位置（它们不跟尾巴转，恒在左边）
	void updateValueBtns();
	// 画其中一个按钮。box 是它的外接方框，plus 决定画 + 还是 −
	void paintValueBtn(ID2D1DeviceContext* ctx, const D2D1_RECT_F& box, bool plus);
	// 编号加减一：级联顶号、重排所有序号的文字、刷新
	void bumpVal(int delta);
	// 从 ToolSub 拉一份当前样式
	void setAttr();
	// 序号里写的文字。空串表示只显示编号本身
	std::wstring displayText();
	void startEdit();
private:
	std::vector<D2D1_RECT_F> draggers;
	// 编号的 + / − 两个小按钮。它们不是"图上某个位置"的控制点，所以不进 draggers ——
	// draggers 里的点都要跟着 angle 转，这两个恒在徽章左边
	D2D1_RECT_F valuePlus{}, valueMinus{};
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushText;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layoutText;
	// 文字编辑期间挂在 Canvas 那个共用 TextBox 上的两个订阅，退出编辑必须摘掉
	winrt::event_token textChangedTok{}, focusTok{};
	// cx/cy 在 mouseDown 里才落定，之间可能先 paint 一次，给初值免得读到垃圾值
	float pressX{ 0.f }, pressY{ 0.f }, cx{ 0.f }, cy{ 0.f }, r{ 0.f }, angle{ 270.f };
	D2D1_POINT_2F tip{ 0,0 }, mid{ 0,0 };
	bool isFill{ false }, isWheel{ false };
	std::wstring customText;
	// 当前颜色 RGBA 原值。Ling::Color 没有从 D2D1_COLOR_F 构造的口子，TextBox::setColor 得用它
	UINT32 colorValue{ 0xFF000000 };
	NumStyle numStyle{ NumStyle::Arabic };
	RingStyle ringStyle{ RingStyle::Circle };
};
