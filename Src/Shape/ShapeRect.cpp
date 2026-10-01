#include "pch.h"
#include "Canvas.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeRect.h"

ShapeRect::ShapeRect(Canvas* win) :ShapeRectBase(win)
{
	auto toolSub = win->getToolSub();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	isFill = toolSub->isRectFill;
}

ShapeRect::~ShapeRect()
{

}

void ShapeRect::paint(ID2D1DeviceContext* ctx)
{
	if (isFill) {
		ctx->FillRectangle(rect, brush.Get());
	}
	else {
		ctx->DrawRectangle(rect, brush.Get(), strokeWidth);
	}
}

// 光标停在边框或八个 dragger 上（此时 Canvas 才把滚轮事件转过来）滚滚轮 = 调线宽，
// 与序号那边用滚轮调大小是一回事。填充矩形没有边框可调，直接不管。
void ShapeRect::mouseWheel(const float x, const float y, const short delta)
{
	if (isFill) return;
	// 一格一个逻辑像素。上下限交给 ToolSub 那张滑块值域表夹，用它夹完的返回值 ——
	// 线宽与工具栏滑块因此永远是同一个数，也滚不出滑块能表达的范围
	auto next = strokeWidth + (delta < 0 ? -win->getDpi() : win->getDpi());
	auto applied = win->getToolSub()->setShapeSliderVal(L"rect", next);
	if (applied == strokeWidth) return;   //已经顶到值域的头了，不用重画
	strokeWidth = applied;
	win->refresh();
}
