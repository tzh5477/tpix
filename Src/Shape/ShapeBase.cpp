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

D2D1_RECT_F ShapeBase::closeBtnRect() const
{
	D2D1_RECT_F b{};
	if (!getShapeBounds(b)) return D2D1::RectF(0.f, 0.f, 0.f, 0.f);
	auto rad{ draggerSize * 0.9f };
	// 默认摆在外接矩形右上角的"外面"一点：角上正压着那个控制点，叠在一起会互相打架
	auto gap{ draggerSize * 1.6f };
	auto cx{ b.right + gap };
	auto cy{ b.top - gap };
	// 顶到画布边上就翻到内侧 —— 否则按钮被画布裁掉，点都点不到
	auto img = win->getImgSize();
	if (img.width > 0 && cx + rad > (float)img.width) cx = b.right - gap;
	if (cy - rad < 0.f) cy = b.top + gap;
	return D2D1::RectF(cx - rad, cy - rad, cx + rad, cy + rad);
}

bool ShapeBase::hitCloseBtn(const float x, const float y)
{
	return isInRect(closeBtnRect(), x, y);
}

void ShapeBase::paintCloseBtn(ID2D1DeviceContext* ctx)
{
	auto box = closeBtnRect();
	if (box.right <= box.left) return;
	auto c = D2D1::Point2F((box.left + box.right) / 2.f, (box.top + box.bottom) / 2.f);
	auto rad{ (box.right - box.left) / 2.f };
	// 与序号那四个动作按钮同一套画法：先垫一层白圆再描边，压在底图上才看得清
	ctx->FillEllipse(D2D1::Ellipse(c, rad, rad), brushDraggerFill.Get());
	ctx->DrawEllipse(D2D1::Ellipse(c, rad, rad), brushDragger.Get(), win->getDpi());
	auto k{ rad * 0.42f };
	auto stroke{ draggerSize * 0.15f };
	ctx->DrawLine({ c.x - k, c.y - k }, { c.x + k, c.y + k }, brushDragger.Get(), stroke);
	ctx->DrawLine({ c.x - k, c.y + k }, { c.x + k, c.y - k }, brushDragger.Get(), stroke);
}
