#include "pch.h"
#include "Canvas.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeRect.h"

ShapeRect::ShapeRect(Canvas* win) :ShapeRectBase(win)
{
	kind = Kind::Rect;
	// 照工具条上"当前那一套"来画，也跟着工具条换类别翻成圆
	//（马赛克、擦除也在这个基类下，它们没有这回事）
	useToolStyle = true;
	auto toolSub = win->getToolSub();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	isFill = toolSub->isGeomFill;
}

ShapeRect::~ShapeRect()
{
}

std::unique_ptr<ShapeBase> ShapeRect::clone(const float dx, const float dy, Canvas* target) const
{
	return cloneSelf(*this, dx, dy, target);
}

std::unique_ptr<ShapeBase> ShapeRect::snapshot() const
{
	return snapshotSelf(*this);
}
