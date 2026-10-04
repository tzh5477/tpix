#pragma once
#include <include/Ling.h>
#include "ShapeRectBase.h"
// 椭圆 / 圆。命中判定是椭圆环带而不是一圈直边 —— 这一条在 ShapeRectBase 里，
// 与矩形共用同一套 rect + 旋转角。这里只设 kind 与自己的画刷
class ShapeEllipse : public ShapeRectBase
{
public:
	ShapeEllipse(Canvas* win);
	~ShapeEllipse();
};
