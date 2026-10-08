#include "pch.h"
#include "Canvas.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeEllipse.h"

ShapeEllipse::ShapeEllipse(Canvas* win) :ShapeRectBase(win)
{
	kind = Kind::Ellipse;
	// 同上：照工具条那套来画，也跟着工具条换类别翻成矩形
	useToolStyle = true;
	auto toolSub = win->getToolSub();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	isFill = toolSub->isGeomFill;
}

ShapeEllipse::~ShapeEllipse()
{
}

std::unique_ptr<ShapeBase> ShapeEllipse::clone(const float dx, const float dy, Canvas* target) const
{
	return cloneSelf(*this, dx, dy, target);
}
