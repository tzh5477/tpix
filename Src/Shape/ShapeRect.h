#pragma once
#include <include/Ling.h>
#include "ShapeRectBase.h"
// 矩形族里"取工具条当前颜色画一个矩形"的那一个。
// 八向手柄、整体拖动、Shift 约束都在 ShapeRectBase，这里只剩画什么和谁改线宽
class ShapeRect : public ShapeRectBase
{
public:
	ShapeRect(Canvas* win);
	~ShapeRect();
	void paint(ID2D1DeviceContext* ctx) override;
	void mouseWheel(const float x, const float y, const short delta) override;
	void applyStyle() override;
};
