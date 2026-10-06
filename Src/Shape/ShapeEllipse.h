#pragma once
#include <include/Ling.h>
#include "ShapeRectBase.h"
// 椭圆 / 圆。命中判定是椭圆环带而不是一圈直边，还能切缺角（饼图）与内径（环形图）——
// 这三样都在 ShapeRectBase 里，与矩形共用同一套 rect + 旋转角。
// 这里只设 kind 与自己的画刷
class ShapeEllipse : public ShapeRectBase
{
public:
	ShapeEllipse(Canvas* win);
	~ShapeEllipse();
	// 复制（见 ShapeBase::clone）：与矩形同一套几何，只是 kind 不同
	bool copyable() const override { return true; }
	std::unique_ptr<ShapeBase> clone(const float dx, const float dy) const override;
};
