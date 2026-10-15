#include "pch.h"
#include <algorithm>
#include "Canvas.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "History.h"
#include "ShapeRect.h"
#include "ShapeText.h"

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

void ShapeRect::paintActionIcon(ID2D1DeviceContext* ctx, const int i, const D2D1_POINT_2F& c,
	const float rad, ID2D1Brush* brush, const float strokeW)
{
	if (i != 0 || !ctx || !brush) return;
	auto k = rad * 0.55f;
	ctx->DrawLine({ c.x - k, c.y - k }, { c.x + k, c.y - k }, brush, strokeW);
	ctx->DrawLine({ c.x, c.y - k }, { c.x, c.y + k }, brush, strokeW);
}

void ShapeRect::onAction(const int i)
{
	if (i != 0) return;
	D2D1_RECT_F b{};
	if (!getShapeBounds(b)) return;
	auto x = b.right;
	auto y = b.top;
	auto img = win->getImgSize();
	x = std::clamp(x, 0.f, (float)img.width);
	y = std::clamp(y, 0.f, (float)img.height);
	auto t = std::make_unique<ShapeText>(win);
	t->toolId = L"text";
	t->setTextAt(L"", x, y);
	if (auto* added = win->history->addShape(std::move(t))) {
		static_cast<ShapeText*>(added)->startEdit();
	}
}

std::unique_ptr<ShapeBase> ShapeRect::clone(const float dx, const float dy, Canvas* target) const
{
	return cloneSelf(*this, dx, dy, target);
}

std::unique_ptr<ShapeBase> ShapeRect::snapshot() const
{
	return snapshotSelf(*this);
}
