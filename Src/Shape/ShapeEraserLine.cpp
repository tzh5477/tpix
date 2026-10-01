#include "pch.h"
#include "Canvas.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeEraserLine.h"

ShapeEraserLine::ShapeEraserLine(Canvas* win) :ShapeLineBase(win)
{
	auto toolSub = win->getToolSub();
	auto d2d = Ling::D2D::get();
	// 橡皮没有颜色可选，这个半透明品红只在拖拽过程中当占位提示
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0xF00FF0, 0.38f), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
}

ShapeEraserLine::~ShapeEraserLine()
{

}

// strokeWidth 直接就是涂抹擦除的笔宽，paint 里描边、盖底两处都用它
void ShapeEraserLine::applyStyle()
{
	strokeWidth = win->getToolSub()->getSliderVal();
}

void ShapeEraserLine::paint(ID2D1DeviceContext* ctx)
{
	// makePath 要等第一次 mouseDown 才建 path，这之前可能先来一次 paint
	if (!path) return;
	if (!isErasing) {
		ctx->DrawGeometry(path.Get(), brush.Get(), strokeWidth, roundStyle.Get());
		return;
	}
	// 底图画刷懒创建：只有真进入擦除态才需要它，构造时就建等于每个橡皮都白抱一张位图刷
	initBackgroundBrush();
	if (bgBrush) {
		ctx->DrawGeometry(path.Get(), bgBrush.Get(), strokeWidth, roundStyle.Get());
	}
}

void ShapeEraserLine::mouseUp(const float x, const float y)
{
	ShapeLineBase::mouseUp(x, y);
	// 退化的几何（只有一个点的笔画）盖回底图什么也看不出来，
	// 保持占位色让用户看到自己画了个无效的东西
	isErasing = path && linePoints.size() > 1;
}

void ShapeEraserLine::makePath()
{
	ShapeLineBase::makePath();
	resetEraser();
}

void ShapeEraserLine::resetEraser()
{
	isErasing = false;
	bgBrush.Reset();
}

// 底图画刷懒创建。底图与窗口同尺寸同坐标，所以不需要像 ShapeMosaic 那样平移画刷
void ShapeEraserLine::initBackgroundBrush()
{
	if (bgBrush || !win->screenImg) return;
	auto bitmapBrushProps = D2D1::BitmapBrushProperties(
		D2D1_EXTEND_MODE_CLAMP,
		D2D1_EXTEND_MODE_CLAMP,
		D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
	auto brushProps = D2D1::BrushProperties();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateBitmapBrush(win->screenImg.Get(), &bitmapBrushProps, &brushProps, bgBrush.GetAddressOf());
}
