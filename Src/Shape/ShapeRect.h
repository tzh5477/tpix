#pragma once
#include <include/Ling.h>
#include "ShapeRectBase.h"
// 矩形族里"取工具条当前颜色画一个矩形"的那一个。
// 八向手柄、整体拖动、旋转、圆角、Shift 约束都在 ShapeRectBase，
// 这里只剩"画什么"和"建自己的画刷"。
//
// 与 ShapeEllipse 的差别只有 kind 一个值 —— "矩形↔圆"互转因此就是翻它，
// 不用把对象换掉（换了会连带 history / selected 一起动）
class ShapeRect : public ShapeRectBase
{
public:
	ShapeRect(Canvas* win);
	~ShapeRect();
	// 复制（见 ShapeBase::clone）：矩形与圆共用同一套几何，各写一行 cloneSelf
	// 定下"复制出来还是我自己这一类"
	bool copyable() const override { return true; }
	std::unique_ptr<ShapeBase> clone(const float dx, const float dy, Canvas* target = nullptr) const override;
};
