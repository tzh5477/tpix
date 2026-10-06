#include "pch.h"
#include "Canvas.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeRect.h"

ShapeRect::ShapeRect(Canvas* win) :ShapeRectBase(win)
{
	kind = Kind::Rect;
	// 允许"矩形↔圆"互转（马赛克、擦除也在这个基类下，它们没有这回事）
	allowShapeToggle = true;
	auto toolSub = win->getToolSub();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	isFill = toolSub->isRectFill;
}

ShapeRect::~ShapeRect()
{
}

std::unique_ptr<ShapeBase> ShapeRect::clone(const float dx, const float dy) const
{
	return cloneSelf(*this, dx, dy);
}
