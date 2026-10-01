#pragma once
#include <include/Ling.h>
#include "ShapeBase.h"

// 矩形族标注的中间基类。
//
// 矩形几何那一整套交互（八向手柄、整体拖动、Shift 约束、手柄重算）在这里只写一遍，
// ShapeRect / ShapeEllipse / ShapeMosaicRect / ShapeEraserRect 都从它派生，
// 各自只管 paint 用什么画刷、滚轮改谁的线宽。
//
// 画刷一律留给派生类建：矩形族里不都是"取工具条当前颜色"—— 马赛克那几个
// 拖拽过程中用的是半透明品红占位色，等 mouseUp 才换成真正的马赛克/擦除画刷。
class ShapeRectBase : public ShapeBase
{
public:
	ShapeRectBase(Canvas* win);
	~ShapeRectBase();
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void setCursor() override;
protected:
	// 命中八向手柄。命中就定下索引直接返回
	void hitDraggers(const float x, const float y);
	// 命中"整体拖动"（索引 8）。默认判一圈边框，椭圆族覆写成椭圆环带
	virtual void hitBody(const float x, const float y);
	// 按当前 rect 重算八个手柄的位置，抬手时与整体移动后都要走一遍
	void makeDraggers();
	// rect 被 mouseDrag 改过之后回调，派生类把自己的衍生几何重算一遍。
	// 基类留空：矩形本身没有 rect 之外的几何，椭圆族要跟着重算 cx/cy/rx/ry
	virtual void syncFromRect();
	// 首次落笔（shape 还没成形）用哪个手柄当锚点。矩形、椭圆都一样只是四角的角手柄，
	// 拆成虚函数是为了派生类一行就够，不用把整套 mouseDown 抄下来改个常量
	virtual int firstDraggerIndex() const;
protected:
	// 底图坐标系里的轴对齐矩形。left/right/top/bottom 由 mouseDrag 按按下点算出
	D2D1_RECT_F rect{ 0,0,0,0 };
	std::vector<D2D1_RECT_F> draggers;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	float strokeWidth{ 1.f }, pressX{ 0.f }, pressY{ 0.f };
	bool isFill{ false };
};
