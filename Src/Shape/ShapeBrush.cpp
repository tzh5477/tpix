#include "pch.h"
#include "Canvas.h"
#include "Tool/ToolSub.h"
#include "ShapeBrush.h"

ShapeBrush::ShapeBrush(Canvas* win) :ShapeLineBase(win)
{
	auto toolSub = win->getToolSub();
	highlighter = (toolSub->brushKind == 1);
	auto color = toolSub->getSelectedColor();
	Ling::D2D::get()->deviceContext->CreateSolidColorBrush(color, brush.GetAddressOf());
	// 外观（颜色 / 线宽 / 荧光笔那层半透明）全走 applyStyle —— 与 ShapeLine 同一条路，
	// 滚轮调粗细也回这里，两处各写一遍迟早对不上
	applyStyle();
}

// 外观：颜色、线宽，以及荧光笔的半透明。滚轮调粗细走的也是这里，所以它不能顺手换画法
//（画法在 applyToolStyle 里，理由同 ShapeLine::applyStyle）
void ShapeBrush::applyStyle()
{
	auto toolSub = win->getToolSub();
	auto color = toolSub->getSelectedColor();
	// 荧光笔是"在图上覆一层半透明的颜色"：alpha 现乘，不写死进颜色里 ——
	// 用户可以挑一个本来就带透明的颜色，那样荧光笔画出来只会更淡，而不是变成一个不透明的块
	if (highlighter) color.a *= 0.4f;
	brush->SetColor(color);
	strokeWidth = toolSub->getSliderVal();
	makePath();
}

// 画法：笔画（画笔 / 荧光笔）。工具条上「类型」那一档只有用户真去动它时才套过来
void ShapeBrush::applyToolStyle()
{
	highlighter = (win->getToolSub()->brushKind == 1);
	// 颜色与线宽一并重刷：荧光笔那层 alpha 是烘在画刷颜色里的，换了画法不重刷颜色就不生效
	applyStyle();
}

void ShapeBrush::paint(ID2D1DeviceContext* ctx)
{
	// path 要等第一次鼠标落点才建得出来，之前可能先来一次 paint
	if (!path) return;
	// 画笔走 ShapeLineBase 建好的圆头圆角描边；荧光笔传 nullptr = D2D 的默认描边样式
	//（平口端点 + 尖角拐角），划过去像一支马克笔
	ctx->DrawGeometry(path.Get(), brush.Get(), strokeWidth, highlighter ? nullptr : roundStyle.Get());
}

bool ShapeBrush::mouseWheel(const float x, const float y, const short delta)
{
	auto next = strokeWidth + (delta < 0 ? -win->getDpi() : win->getDpi());
	auto applied = win->getToolSub()->setShapeSliderVal(L"brush", next);
	if (applied == strokeWidth) return false;   // 已经顶到值域的头了，不用重画
	strokeWidth = applied;
	makePath();
	win->refresh();
	return true;
}

std::unique_ptr<ShapeBase> ShapeBrush::clone(const float dx, const float dy, Canvas* target) const
{
	return cloneSelf(*this, dx, dy, target);
}

std::unique_ptr<ShapeBase> ShapeBrush::snapshot() const
{
	return snapshotSelf(*this);
}
