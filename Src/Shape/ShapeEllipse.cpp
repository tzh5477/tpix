#include "pch.h"
#include "Canvas.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeEllipse.h"

ShapeEllipse::ShapeEllipse(Canvas* win) :ShapeRectBase(win)
{
	auto toolSub = win->getToolSub();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	isFill = toolSub->isEllipseFill;
}

ShapeEllipse::~ShapeEllipse()
{

}

void ShapeEllipse::paint(ID2D1DeviceContext* ctx)
{
	D2D1_ELLIPSE ellipse = D2D1::Ellipse({ cx,cy }, rx, ry);
	if (isFill) {
		ctx->FillEllipse(ellipse, brush.Get());
	}
	else {
		ctx->DrawEllipse(ellipse, brush.Get(), strokeWidth);
	}
}

void ShapeEllipse::hitBody(const float x, const float y)
{
	// 填充椭圆没有可见边框，边缘命中区用固定宽度（2 逻辑像素）即可，
	// 别跟滑块线宽走 —— 填充时滑块那套值跟这条边没有任何关系
	auto half{ isFill ? 2.f * win->getDpi() : strokeWidth / 2.f + win->getDpi() };
	auto dx = x - cx;
	auto dy = y - cy;
	auto outerRx = rx + half, outerRy = ry + half;
	auto innerRx = rx - half > 0 ? rx - half : 0.f;
	auto innerRy = ry - half > 0 ? ry - half : 0.f;
	bool insideOuter = (dx / outerRx) * (dx / outerRx) + (dy / outerRy) * (dy / outerRy) <= 1.f;
	bool insideInner = innerRx > 0 && innerRy > 0 && (dx / innerRx) * (dx / innerRx) + (dy / innerRy) * (dy / innerRy) <= 1.f;
	if (insideOuter && !insideInner) {
		hoverDraggerIndex = 8;
	}
}

void ShapeEllipse::syncFromRect()
{
	cx = (rect.left + rect.right) / 2.f;
	cy = (rect.top + rect.bottom) / 2.f;
	rx = (rect.right - rect.left) / 2.f;
	ry = (rect.bottom - rect.top) / 2.f;
}

int ShapeEllipse::firstDraggerIndex() const
{
	return 0;
}

// 同 ShapeRect::mouseWheel：光标停在边框或八个 dragger 上时滚滚轮 = 调线宽
void ShapeEllipse::mouseWheel(const float x, const float y, const short delta)
{
	if (isFill) return;
	auto next = strokeWidth + (delta < 0 ? -win->getDpi() : win->getDpi());
	auto applied = win->getToolSub()->setShapeSliderVal(L"ellipse", next);
	if (applied == strokeWidth) return;
	strokeWidth = applied;
	win->refresh();
}
