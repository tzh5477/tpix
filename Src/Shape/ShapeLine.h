#pragma once
#include <include/Ling.h>
#include "ShapeLineBase.h"
// 直线：折线族里"取工具条当前颜色描一条线"的那个。
// 拖首端、拖末端、整体平移、线段命中判定都在 ShapeLineBase，这里只剩画什么
class ShapeLine : public ShapeLineBase
{
public:
	ShapeLine(Canvas* win);
	~ShapeLine();
	void paint(ID2D1DeviceContext* ctx) override;
};
