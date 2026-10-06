#pragma once
#include <include/Ling.h>
#include "ShapeBase.h"
class ShapeArrow : public ShapeBase
{
public:
	ShapeArrow(Canvas* win);
	~ShapeArrow();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void mouseWheel(const float x, const float y, const short delta) override;
	void setCursor() override;
	void applyStyle() override;
	void applyToolStyle() override;
	// 「选择对象」框选要用：整支箭（杆 + 头）的外接框。path 就是 buildOutline 建的轮廓，
	// 四档都建 —— 细箭头那一档画的时候走描边（paintLineStyle），轮廓几何照样在，
	// 它本来就是那一档的命中几何
	bool getShapeBounds(D2D1_RECT_F& out) const override;
	// ---- 「箭头样式」下拉里的一格 ----
	// 4 档的形状全部出自 buildOutline / strokeHead 那两份几何（见 .cpp），
	// 与真正画在图上的是同一套顶点与比例 —— 预览要是自己另画一套，
	// 哪天再调一次箭头形状，下拉里看到的就是另一个东西了。
	// rect 是这一格的范围（物理像素），strokeW 是样例箭杆的粗细（物理像素）；
	// 每个枚举值一样长（都铺满整格），不会有"普通箭头很长、细箭头很短"这种参差
	static void paintSample(ID2D1DeviceContext* ctx, const D2D1_RECT_F& rect, int styleIndex,
		float strokeW, ID2D1Brush* brush);
	// 复制（见 ShapeBase::clone）
	bool copyable() const override { return true; }
	std::unique_ptr<ShapeBase> clone(const float dx, const float dy, Canvas* target = nullptr) const override;
protected:
	void fixupCopy() override;
	void translate(const float dx, const float dy) override;
private:
	void makeArrow();
	// 细箭头（arrowStyle 2）的画面：箭杆一条线 + 开口 V 头。
	// 返回 false 表示这一笔还没拖开，没有东西可画
	bool paintLineStyle(ID2D1DeviceContext* ctx);
	void constrainToEightDirections(const float anchorX, const float anchorY, const float mouseX, const float mouseY, float& targetX, float& targetY);
private:
	std::vector<D2D1_RECT_F> draggers; 
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	float arrowSize{ 0 }, pressX{ 0 }, pressY{ 0 }, startX{ 0 }, startY{ 0 }, endX{ 0 }, endY{ 0 };
	// 箭头样式。与 ToolSub::arrowStyle 同一套值，顺序即落盘值：
	//   0 普通（平口尾、箭杆等粗、平底实心头）  1 尖尾（尾部收成一个点、箭杆楔形）
	//   2 细箭头（一条线 + 开口 V 头，杆厚 = 滑块值，头长 = 9.7×杆厚 ——
	//     与线条端点那档"细箭头"同一支箭）      3 凹口实心（两翼后掠、后缘向内凹）
	// 四档的顶点都是尖的（作者要的"尖尖头"）—— 原先还有第 5 档"圆点"（箭杆收成一条线 +
	// 末端一个实心圆点），那个的顶点就是个圆头，已按作者要求整档去掉
	int arrowStyle{ 0 };
	bool isFill{ false };
};

