#include "pch.h"
#include "Canvas.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeLine.h"

ShapeLine::ShapeLine(Canvas* win) :ShapeLineBase(win)
{
	auto toolSub = win->getToolSub();
	auto color = toolSub->getSelectedColor();
	if (toolSub->isLineTransparent) {
		color.a = 0.5f;
	}
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(color, brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
}

ShapeLine::~ShapeLine()
{

}

// 半透明开关也在 ToolSub 上，颜色带 alpha 时要连它一起重算
void ShapeLine::applyStyle()
{
	auto toolSub = win->getToolSub();
	auto color = toolSub->getSelectedColor();
	if (toolSub->isLineTransparent) {
		color.a = 0.5f;
	}
	brush->SetColor(color);
	strokeWidth = toolSub->getSliderVal();
}

void ShapeLine::paint(ID2D1DeviceContext* ctx)
{
	// makePath 要等第一次 mouseDown 才建 path，这之前可能先来一次 paint
	if (!path) return;
	ctx->DrawGeometry(path.Get(), brush.Get(), strokeWidth, roundStyle.Get());
}
