#include "pch.h"
#include "Canvas.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeEllipse.h"

ShapeEllipse::ShapeEllipse(Canvas* win) :ShapeRectBase(win)
{
	kind = Kind::Ellipse;
	// 允许"矩形↔圆"互转
	allowShapeToggle = true;
	auto toolSub = win->getToolSub();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	isFill = toolSub->isEllipseFill;
}

ShapeEllipse::~ShapeEllipse()
{
}

std::unique_ptr<ShapeBase> ShapeEllipse::clone(const float dx, const float dy, Canvas* target) const
{
	return cloneSelf(*this, dx, dy, target);
}
