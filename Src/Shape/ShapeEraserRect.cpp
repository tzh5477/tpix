#include "pch.h"
#include "Canvas.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeEraserRect.h"

ShapeEraserRect::ShapeEraserRect(Canvas* win) :ShapeRectBase(win)
{
	auto toolSub = win->getToolSub();
	auto d2d = Ling::D2D::get();
	// 橡皮没有颜色可选，这个半透明品红只在拖拽过程中当占位提示 ——
	// 松开鼠标后 isErasing 置位，改用底图画刷把这块盖回原样
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0xF00FF0, 0.38f), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	// 整块填充、没有可摸的边框，命中带按固定 2*dpi 宽走而不是按线宽 —— 同 ShapeMosaicRect
	isFill = true;
}

ShapeEraserRect::~ShapeEraserRect()
{

}

void ShapeEraserRect::paint(ID2D1DeviceContext* ctx)
{
	if (!isErasing) {
		ctx->FillRectangle(rect, brush.Get());
		return;
	}
	// 底图画刷懒创建：只有真进入擦除态才需要它，构造时就建等于每个橡皮都白抱一张位图刷
	initBackgroundBrush();
	if (bgBrush) {
		ctx->FillRectangle(rect, bgBrush.Get());
	}
}

// 几何一变就退回占位色，等 mouseUp 再重新进入擦除态
void ShapeEraserRect::mouseDrag(const float x, const float y)
{
	resetEraser();
	ShapeRectBase::mouseDrag(x, y);
}

void ShapeEraserRect::mouseUp(const float x, const float y)
{
	ShapeRectBase::mouseUp(x, y);
	// 退化的几何（零面积矩形）盖回底图什么也看不出来，
	// 保持占位色让用户看到自己画了个无效的东西
	isErasing = rect.right > rect.left && rect.bottom > rect.top;
}

void ShapeEraserRect::resetEraser()
{
	isErasing = false;
	bgBrush.Reset();
}

// 底图画刷懒创建。底图与窗口同尺寸同坐标，所以不需要像 ShapeMosaic 那样平移画刷
void ShapeEraserRect::initBackgroundBrush()
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
