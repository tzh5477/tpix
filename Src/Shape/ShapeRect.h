#pragma once
#include <include/Ling.h>
#include "ShapeRectBase.h"
// 矩形族里"取工具条当前颜色画一个矩形"的那一个。
// 八向手柄、整体拖动、旋转、Shift 约束都在 ShapeRectBase，
// 这里只剩"建自己的画刷"。
//
// 与 ShapeEllipse 的差别只有 kind 一个值 —— "矩形↔圆"互转因此就是翻它，
// 不用把对象换掉（换了会连带 history / selected 一起动）
class ShapeRect : public ShapeRectBase
{
public:
	ShapeRect(Canvas* win);
	~ShapeRect();
};
