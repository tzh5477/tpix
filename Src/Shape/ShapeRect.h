#pragma once
#include <include/Ling.h>
#include "ShapeRectBase.h"
// 矩形族里"取工具条当前颜色画一个几何图形"的那一个。
// 八向手柄、整体拖动、旋转、圆角、Shift 约束都在 ShapeRectBase，
// 这里只剩"画什么"和"建自己的画刷"。
//
// 与 ShapeEllipse 的差别只有 kind 一个值 —— "换类别"因此就是翻它，
// 不用把对象换掉（换了会连带 history / selected 一起动）。矩形 / 三角形 / 菱形都走这一个类，
// 由构造参数定下新画的是哪一类（圆心那套扇区手柄只有 ShapeEllipse 建）
class ShapeRect : public ShapeRectBase
{
public:
	// k 是"新建的这一笔画成什么"（矩形 / 三角形 / 菱形，圆走 ShapeEllipse）。
	// 建成之后照样能被工具条换类别翻成任意一类
	ShapeRect(Canvas* win, Kind k = Kind::Rect);
	~ShapeRect();
	int actionCount() const override { return 1; }
	void paintActionIcon(ID2D1DeviceContext* ctx, const int i, const D2D1_POINT_2F& c,
		const float rad, ID2D1Brush* brush, const float strokeW) override;
	void onAction(const int i) override;
	// 复制（见 ShapeBase::clone）：矩形与圆共用同一套几何，各写一行 cloneSelf
	// 定下"复制出来还是我自己这一类"
	bool copyable() const override { return true; }
	std::unique_ptr<ShapeBase> clone(const float dx, const float dy, Canvas* target = nullptr) const override;
	std::unique_ptr<ShapeBase> snapshot() const override;
	// 多选那圈提示框线要离外接框再让多远（见 ShapeBase::selectionGap）。
	// 矩形与椭圆共用这一族，但只有**轮廓贴着外接框**的形状需要让（空心的矩形 / 三角形）：
	// 那几笔画的线就落在框上、描边以它为中线，框线压上去就把红描边混成紫的；
	// 圆 / 菱形只在外接框的个别点上碰到，填满的又没有描边，都让 0
	float selectionGap() const override { return (isFill || !outlineOnBBox()) ? 0.f : strokeWidth * 0.5f; }
};
