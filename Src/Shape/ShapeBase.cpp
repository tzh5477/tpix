#include "pch.h"
#include "App.h"
#include "Canvas.h"
#include "History.h"
#include "ShapeBase.h"

using Microsoft::WRL::ComPtr;

ShapeBase::ShapeBase(Canvas* win):win{win}, draggerSize{6*win->getDpi()}
{
	auto d2d = Ling::D2D::get();
	// 控制点：浅蓝描边（原来的黑色压在标注上很扎眼），选中时填白把下层线挡住
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x4A9EFF), brushDragger.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), brushDraggerFill.GetAddressOf());
}

ShapeBase::~ShapeBase()
{}

bool ShapeBase::isInRect(const D2D1_RECT_F rect, const float x, const float y) const
{
	return (x > rect.left && x<rect.right && y>rect.top && y < rect.bottom);
}

D2D1_RECT_F ShapeBase::actionBtnRect(const int i) const
{
	D2D1_RECT_F b{};
	if (i < 0 || i >= actionBtnTotal()) return D2D1::RectF(0.f, 0.f, 0.f, 0.f);
	if (!getShapeBounds(b)) return D2D1::RectF(0.f, 0.f, 0.f, 0.f);
	auto rad{ draggerSize * 0.9f };
	// 默认摆在外接矩形右上角的"外面"一点：角上正压着那个控制点，叠在一起会互相打架
	auto gap{ draggerSize * 1.6f };
	// 图标之间留一点缝。整排往右上角外面排：末尾那枚（×）紧贴右上角、位置恒为
	// b.right + gap，派生类多挂的图标顺着往右长 —— 这样 × 永远不挪位，用户不用重新找它
	auto step{ rad * 2.f + draggerSize * 0.5f };
	auto last{ actionBtnTotal() - 1 };
	auto cx{ b.right + gap + (last - i) * step };
	auto cy{ b.top - gap };
	// 顶到画布边上就整排翻到内侧 —— 否则按钮被画布裁掉，点都点不到
	auto img = win->getImgSize();
	if (img.width > 0 && cx + rad > (float)img.width) cx = b.right - gap - (last - i) * step;
	if (cy - rad < 0.f) cy = b.top + gap;
	return D2D1::RectF(cx - rad, cy - rad, cx + rad, cy + rad);
}

int ShapeBase::hitActionBtn(const float x, const float y) const
{
	for (int i = 0; i < actionBtnTotal(); i++) {
		if (isInRect(actionBtnRect(i), x, y)) return i;
	}
	return -1;
}

void ShapeBase::paintActionBtns(ID2D1DeviceContext* ctx)
{
	for (int i = 0; i < actionBtnTotal(); i++) {
		auto box = actionBtnRect(i);
		if (box.right <= box.left) continue;
		auto c = D2D1::Point2F((box.left + box.right) / 2.f, (box.top + box.bottom) / 2.f);
		auto rad{ (box.right - box.left) / 2.f };
		// 与序号那四个动作按钮同一套画法：先垫一层白圆再描边，压在底图上才看得清
		ctx->FillEllipse(D2D1::Ellipse(c, rad, rad), brushDraggerFill.Get());
		ctx->DrawEllipse(D2D1::Ellipse(c, rad, rad), brushDragger.Get(), win->getDpi());
		if (i == actionBtnTotal() - 1) {
			auto k{ rad * 0.42f };
			auto stroke{ draggerSize * 0.15f };
			ctx->DrawLine({ c.x - k, c.y - k }, { c.x + k, c.y + k }, brushDragger.Get(), stroke);
			ctx->DrawLine({ c.x - k, c.y + k }, { c.x + k, c.y - k }, brushDragger.Get(), stroke);
		}
		else {
			paintActionIcon(ctx, i, c, rad);
		}
	}
}

void ShapeBase::onActionBtn(const int i)
{
	if (i < 0 || i >= actionBtnTotal()) return;
	if (i == actionBtnTotal() - 1) {
		// 走 History 的统一删除口子：它会先把可能开着的编辑器收尾，再删、再刷新
		win->history->removeActiveShape();
		return;
	}
	onAction(i);
}

D2D1_POINT_2F ShapeBase::rotatePoint(const D2D1_POINT_2F& p, const D2D1_POINT_2F& c, const float deg)
{
	float radians = deg * 3.14159265358979323846f / 180.f;
	float cosValue = cosf(radians), sinValue = sinf(radians);
	float dx = p.x - c.x, dy = p.y - c.y;
	return { c.x + dx * cosValue - dy * sinValue, c.y + dx * sinValue + dy * cosValue };
}

D2D1_POINT_2F ShapeBase::unrotatePoint(const D2D1_POINT_2F& p, const D2D1_POINT_2F& c, const float deg)
{
	if (deg == 0.f) return p;
	return rotatePoint(p, c, -deg);
}

D2D1_POINT_2F ShapeBase::transformPoint(ID2D1DeviceContext* ctx, const D2D1_POINT_2F& p)
{
	// ID2D1RenderTarget::GetTransform 返回 void、走出参，没有按值返回的重载
	D2D1_MATRIX_3X2_F m{};
	ctx->GetTransform(&m);
	return { p.x * m._11 + p.y * m._21 + m._31, p.x * m._12 + p.y * m._22 + m._32 };
}

D2D1_RECT_F ShapeBase::rotatedBounds(const D2D1_RECT_F& r, const float deg)
{
	if (deg == 0.f) return r;
	auto c = D2D1::Point2F((r.left + r.right) / 2.f, (r.top + r.bottom) / 2.f);
	auto p0 = rotatePoint(D2D1::Point2F(r.left, r.top), c, deg);
	auto p1 = rotatePoint(D2D1::Point2F(r.right, r.top), c, deg);
	auto p2 = rotatePoint(D2D1::Point2F(r.right, r.bottom), c, deg);
	auto p3 = rotatePoint(D2D1::Point2F(r.left, r.bottom), c, deg);
	return D2D1::RectF(std::min({ p0.x, p1.x, p2.x, p3.x }), std::min({ p0.y, p1.y, p2.y, p3.y }),
		std::max({ p0.x, p1.x, p2.x, p3.x }), std::max({ p0.y, p1.y, p2.y, p3.y }));
}

void ShapeBase::updateRotateDragger(const D2D1_RECT_F& bounds)
{
	auto half{ draggerSize / 2 };
	auto dx{ (bounds.right - bounds.left) / 2.f };
	auto dy{ (bounds.bottom - bounds.top) / 2.f };
	auto len = sqrtf(dx * dx + dy * dy);
	// 零尺寸的框（空文本）下 div 会算出 NaN，退回"右下方向"
	auto ux{ len > 0.f ? dx / len : 0.7071f };
	auto uy{ len > 0.f ? dy / len : 0.7071f };
	// 静止方向：由中心指向右下角。顺时针为正、0 度朝上（与 rotateAngleAt 同一套）
	rotateRestAngle = atan2f(dx, -dy) * 180.f / 3.14159265358979323846f;
	auto offset = draggerSize * 0.8f;
	auto c = D2D1::Point2F((bounds.left + bounds.right) / 2.f, (bounds.top + bounds.bottom) / 2.f);
	auto p = D2D1::Point2F(bounds.right + ux * offset, bounds.bottom + uy * offset);
	rotateDragger = D2D1::RectF(p.x - half, p.y - half, p.x + half, p.y + half);
	// 手柄要不要跟着元素一起转由调用方决定：文本那边手柄坐标存的就是转好之后的，
	// 这里只按轴对齐的外接框算，转不转在外面套
}

float ShapeBase::rotateAngleAt(const D2D1_POINT_2F& center, const float x, const float y) const
{
	// 手柄静止时挂在右下角（方向见 updateRotateDragger），鼠标方向减掉静止方向才是这次转过的角度
	return atan2f(x - center.x, -(y - center.y)) * 180.f / 3.14159265358979323846f - rotateRestAngle;
}

void ShapeBase::paintRotateHandle(ID2D1DeviceContext* ctx)
{
	if (rotateDragger.right <= rotateDragger.left) return;
	auto d2d = Ling::D2D::get();
	auto dpi = win->getDpi();
	auto c = D2D1::Point2F((rotateDragger.left + rotateDragger.right) / 2.f,
		(rotateDragger.top + rotateDragger.bottom) / 2.f);
	auto r{ draggerSize * 0.5f };
	// 底下一个白圆：手柄要压在图上，不垫一层会和底图糊在一起
	ctx->FillEllipse(D2D1::Ellipse(c, r, r), brushDraggerFill.Get());
	ctx->DrawEllipse(D2D1::Ellipse(c, r, r), brushDragger.Get(), dpi);
	// 圆弧：留一段缺口对着框（右下方向），看着就是个"转"的符号
	auto arcR{ draggerSize * 0.3f };
	auto arrowSize{ draggerSize * 0.26f };
	const float start = 20.f, sweep = 280.f;
	const int steps = 24;
	d2d->d2dFactory->CreatePathGeometry(rotateArc.ReleaseAndGetAddressOf());
	ComPtr<ID2D1GeometrySink> arcSink;
	rotateArc->Open(arcSink.GetAddressOf());
	// 屏幕角度 -> 点：0 度朝右、逆时针为正（屏幕 y 向下，所以纵坐标取负）
	auto pointAt = [&](float deg) {
		auto rad = deg * 3.14159265358979323846f / 180.f;
		return D2D1::Point2F(c.x + arcR * cosf(rad), c.y - arcR * sinf(rad));
	};
	arcSink->BeginFigure(pointAt(start), D2D1_FIGURE_BEGIN_HOLLOW);
	for (int i = 1; i <= steps; i++) {
		arcSink->AddLine(pointAt(start + sweep * i / steps));
	}
	arcSink->EndFigure(D2D1_FIGURE_END_OPEN);
	arcSink->Close();
	ctx->DrawGeometry(rotateArc.Get(), brushDragger.Get(), dpi);
	// 两端的箭头：指向圆弧的走向（起点朝回、终点朝前），拼成一个几何体一次填掉
	d2d->d2dFactory->CreatePathGeometry(rotateArrows.ReleaseAndGetAddressOf());
	ComPtr<ID2D1GeometrySink> headSink;
	rotateArrows->Open(headSink.GetAddressOf());
	auto addHead = [&](float deg, bool forward) {
		auto rad = deg * 3.14159265358979323846f / 180.f;
		// 圆弧在该点的切向（对 deg 求导），forward=false 时取反向
		auto sign = forward ? 1.f : -1.f;
		auto tx{ -sinf(rad) * sign }, ty{ -cosf(rad) * sign };
		// 法向：切向转 90 度
		auto nx{ -ty }, ny{ tx };
		auto tip = D2D1::Point2F(c.x + arcR * cosf(rad) + tx * arrowSize, c.y - arcR * sinf(rad) + ty * arrowSize);
		auto p1 = D2D1::Point2F(c.x + arcR * cosf(rad) + nx * arrowSize * 0.6f, c.y - arcR * sinf(rad) + ny * arrowSize * 0.6f);
		auto p2 = D2D1::Point2F(c.x + arcR * cosf(rad) - nx * arrowSize * 0.6f, c.y - arcR * sinf(rad) - ny * arrowSize * 0.6f);
		headSink->BeginFigure(p1, D2D1_FIGURE_BEGIN_FILLED);
		headSink->AddLine(tip);
		headSink->AddLine(p2);
		headSink->EndFigure(D2D1_FIGURE_END_CLOSED);
	};
	addHead(start, false);
	addHead(start + sweep, true);
	headSink->Close();
	ctx->FillGeometry(rotateArrows.Get(), brushDragger.Get());
}
