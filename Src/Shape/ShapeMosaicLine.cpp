#include "pch.h"
#include "Canvas.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeMosaicLine.h"

ShapeMosaicLine::ShapeMosaicLine(Canvas* win) :ShapeLineBase(win), mosaicPaint{ win, this }
{
	auto toolSub = win->getToolSub();
	auto d2d = Ling::D2D::get();
	// 涂抹马赛克没有颜色可选，这个半透明品红只在拖拽过程中当占位提示
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0xF00FF0, 0.38f), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
}

ShapeMosaicLine::~ShapeMosaicLine()
{

}

// strokeWidth 既是描边宽度也是马赛克块大小的来源，改了要把马赛克重新糊一遍
void ShapeMosaicLine::applyStyle()
{
	strokeWidth = win->getToolSub()->getSliderVal();
	if (mosaicBrush) buildMosaic();
}

void ShapeMosaicLine::paint(ID2D1DeviceContext* ctx)
{
	// makePath 要等第一次 mouseDown 才建 path，这之前可能先来一次 paint
	if (!path) return;
	if (mosaicBrush) {
		ctx->DrawGeometry(path.Get(), mosaicBrush.Get(), strokeWidth, roundStyle.Get());
		return;
	}
	ctx->DrawGeometry(path.Get(), brush.Get(), strokeWidth, roundStyle.Get());
}

void ShapeMosaicLine::mouseUp(const float x, const float y)
{
	ShapeLineBase::mouseUp(x, y);
	buildMosaic();
}

void ShapeMosaicLine::makePath()
{
	ShapeLineBase::makePath();
	resetMosaic();
}

void ShapeMosaicLine::resetMosaic()
{
	mosaicBrush.Reset();
}

void ShapeMosaicLine::buildMosaic()
{
	if (!path) return;
	// 描边后的实际覆盖范围，比 path 本身宽 strokeWidth
	D2D1_RECT_F bounds{};
	if (FAILED(path->GetWidenedBounds(strokeWidth, roundStyle.Get(), nullptr, &bounds))) return;
	// 块越大越糊。滑块调的是笔画粗细，顺带让粗笔画对应大色块，下限 6px 保证肉眼可见
	int blockSize = std::max(6, (int)std::round(strokeWidth / 3.0f));
	mosaicBrush = mosaicPaint.makeMosaicBrush(bounds, blockSize);
}
