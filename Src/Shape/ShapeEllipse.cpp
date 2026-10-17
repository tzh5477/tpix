#include "pch.h"
#include <algorithm>
#include "Canvas.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "History.h"
#include "ShapeEllipse.h"
#include "ShapeText.h"

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

void ShapeEllipse::paintActionIcon(ID2D1DeviceContext* ctx, const int i, const D2D1_POINT_2F& c,
	const float rad, ID2D1Brush* brush, const float strokeW)
{
	if (i != 0 || !ctx || !brush) return;
	auto k = rad * 0.55f;
	ctx->DrawLine({ c.x - k, c.y - k }, { c.x + k, c.y - k }, brush, strokeW);
	ctx->DrawLine({ c.x, c.y - k }, { c.x, c.y + k }, brush, strokeW);
}

void ShapeEllipse::onAction(const int i)
{
	if (i != 0) return;
	D2D1_RECT_F b{};
	if (!getShapeBounds(b)) return;
	// 与矩形同一条：新文本落在右上角，建文本 / 还选中 / 排编辑器见 ShapeBase::addTextAnnotation
	addTextAnnotation(b.right, b.top);
}

std::unique_ptr<ShapeBase> ShapeEllipse::clone(const float dx, const float dy, Canvas* target) const
{
	return cloneSelf(*this, dx, dy, target);
}

std::unique_ptr<ShapeBase> ShapeEllipse::snapshot() const
{
	return snapshotSelf(*this);
}
