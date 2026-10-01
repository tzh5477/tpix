#include "pch.h"
#include "App.h"
#include "Canvas.h"
#include "ShapeBase.h"

ShapeBase::ShapeBase(Canvas* win):win{win}, draggerSize{6*win->getDpi()}
{
	auto d2d = Ling::D2D::get();
	// 控制点：浅蓝描边（原来的黑色压在标注上很扎眼），选中时填白把下层线挡住
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x4A9EFF), brushDragger.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), brushDraggerFill.GetAddressOf());
}

ShapeBase::~ShapeBase()
{}

bool ShapeBase::isInRect(const D2D1_RECT_F rect, const float x, const float y)
{
	return (x > rect.left && x<rect.right && y>rect.top && y < rect.bottom);
}
