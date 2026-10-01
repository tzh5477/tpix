#pragma once
#include <include/Ling.h>
#include "ShapeBase.h"
// 折线族标注的中间基类：直线、涂抹马赛克、涂抹橡皮擦共用它。
//
// 这一族的共同点是"几何是一串点 + 一条 path"，交互只有三档：拖首端、拖末端
// （Shift 按下时是拖端点而不补点，效果是一条直线）、整体平移；再加上"点在
// 线段的哪一圈上算命中"。这四件事在下面只写一遍，将来加画笔、荧光笔、折线
// 继承它再覆写 paint 就行。
//
// 用什么画刷由派生类自己管：马赛克那两个每改一次几何，之前算好的马赛克就作废了，
// 于是把"重建 path"开成虚函数，让它们在调完基类之后顺手把各自的缓存丢掉
class ShapeLineBase : public ShapeBase
{
public:
	ShapeLineBase(Canvas* win);
	~ShapeLineBase();
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void setCursor() override;
protected:
	virtual void makePath();
	// 命中首尾两个夹点
	void hitDraggers(const float x, const float y);
	void makeDraggers();
	float pointToSegmentDistance(const D2D1_POINT_2F& p, const D2D1_POINT_2F& a, const D2D1_POINT_2F& b);
	void hitTest(const D2D1_POINT_2F& mousePos);
protected:
	// 折线族只有首尾两个夹点，不像矩形族是八个
	std::vector<D2D1_RECT_F> draggers;
	std::vector<D2D1_POINT_2F> linePoints;
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
	// 圆头圆角描边：自由画出来的折线拐点是尖角、端点是方头，只有这一族用得上，
	// 所以不进 ShapeRectBase
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> roundStyle;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	float strokeWidth{ 1.f }, pressX{ 0.f }, pressY{ 0.f };
};
