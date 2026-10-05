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
	// 离框多远。角上正压着八向手柄，而 WinPin::onDown 里 hitActionBtn 排在 shape 派发之前：
	// 两者贴太近时，瞄着角手柄去 resize 就会先被按钮截住（点 × 直接把元素删了）。
	// 3.2 个手柄宽 = 按钮内边缘离角手柄外边缘还有约 2 个手柄宽，鼠标走过去不会中途改判
	auto gap{ draggerSize * 3.2f };
	// 图标之间留一点缝
	auto step{ rad * 2.f + draggerSize * 0.5f };
	auto last{ actionBtnTotal() - 1 };
	// 末尾那枚（×）恒定在右上角；派生类自己的动作图标在左上角，从角上往外排。
	// 分居两个角：挤在同一条边上时相邻两枚只隔一个手柄宽，鼠标移过去极易点错 ——
	// 点错 × 就是把刚画的东西删了，点错互转就是形状忽然变了
	float cx{}, cy{ b.top - gap };
	if (i == last) {
		cx = b.right + gap;
	}
	else {
		cx = b.left - gap - (last - 1 - i) * step;
	}
	// 顶到画布边上就翻到内侧 —— 否则按钮被画布裁掉，点都点不到
	auto img = win->getImgSize();
	if (img.width > 0) {
		if (cx + rad > (float)img.width) cx = b.right - gap;
		if (cx - rad < 0.f) cx = b.left + gap;
	}
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

// 手柄挂在哪：外接框右下角的外侧，与右上角那枚 × 同一段距离 —— 两个角对称，
// 看上去就是"外侧三个角各一枚按钮"（左上动作图标 / 右上 × / 右下旋转）。
// 关键是它属于"外接框"而不属于图形本身：bounds 传进来的已经含旋转（见 getShapeBounds），
// 这里不再跟着图形转，否则手柄会跑到斜边上去、而不是待在框的右下角
void ShapeBase::updateRotateDragger()
{
	D2D1_RECT_F b{};
	if (!getShapeBounds(b)) {
		rotateDragger = D2D1::RectF(0.f, 0.f, 0.f, 0.f);
		return;
	}
	auto rad{ draggerSize * 0.9f };
	// 与 actionBtnRect 用同一个 gap：三个角离框的距离一致，摆在一起才像一排
	auto gap{ draggerSize * 3.2f };
	auto cx{ b.right + gap }, cy{ b.bottom + gap };
	// 贴到画布下边缘 / 右边缘就翻到内侧 —— 否则手柄被裁掉，鼠标够不着也就没法转
	auto img = win->getImgSize();
	if (img.width > 0 && cx + rad > (float)img.width) cx = b.right - gap;
	if (img.height > 0 && cy + rad > (float)img.height) cy = b.bottom - gap;
	auto c = D2D1::Point2F((b.left + b.right) / 2.f, (b.top + b.bottom) / 2.f);
	// 静止方向 = 从中心指向手柄。rotateAngleAt 拿鼠标方向减掉它才是转过的角度，
	// 所以手柄翻到内侧时这里也要跟着翻（否则第一下就跳一大截角度）
	rotateRestAngle = atan2f(cx - c.x, -(cy - c.y)) * 180.f / 3.14159265358979323846f;
	rotateDragger = D2D1::RectF(cx - rad, cy - rad, cx + rad, cy + rad);
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
		(rotateDragger.bottom + rotateDragger.top) / 2.f);
	// 圆底与另外两枚角标同大：三个角看上去是同一套按钮，而不是"手柄 + 图标"两样东西
	auto r{ draggerSize * 0.9f };
	ctx->FillEllipse(D2D1::Ellipse(c, r, r), brushDraggerFill.Get());
	ctx->DrawEllipse(D2D1::Ellipse(c, r, r), brushDragger.Get(), dpi);
	// 图标：半弧 + 一支箭头（转圈的意思），与另外两个角标的画法一样用浅蓝。
	// 弧从右下角起、越过顶部、停在左侧（正对屏幕左边），末端一支箭头顺着走向往下 ——
	// 一眼就是"转"，而不是原来那种两头箭头的整圆
	auto arcR{ draggerSize * 0.42f };
	auto arrowSize{ draggerSize * 0.34f };
	const float start = -25.f, sweep = 205.f;
	const int steps = 28;
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
	// 末端那一支箭头：沿圆弧该点的切向指出去，两腰落在切向的法向上
	d2d->d2dFactory->CreatePathGeometry(rotateArrows.ReleaseAndGetAddressOf());
	ComPtr<ID2D1GeometrySink> headSink;
	rotateArrows->Open(headSink.GetAddressOf());
	{
		auto deg{ start + sweep };
		auto rad = deg * 3.14159265358979323846f / 180.f;
		// 切向（对 deg 求导，屏幕 y 取负所以纵坐标也反号）
		auto tx{ -sinf(rad) }, ty{ -cosf(rad) };
		// 法向：切向转 90 度
		auto nx{ -ty }, ny{ tx };
		auto bx = c.x + arcR * cosf(rad), by = c.y - arcR * sinf(rad);
		auto tip = D2D1::Point2F(bx + tx * arrowSize, by + ty * arrowSize);
		auto p1 = D2D1::Point2F(bx + nx * arrowSize * 0.55f, by + ny * arrowSize * 0.55f);
		auto p2 = D2D1::Point2F(bx - nx * arrowSize * 0.55f, by - ny * arrowSize * 0.55f);
		headSink->BeginFigure(p1, D2D1_FIGURE_BEGIN_FILLED);
		headSink->AddLine(tip);
		headSink->AddLine(p2);
		headSink->EndFigure(D2D1_FIGURE_END_CLOSED);
	}
	headSink->Close();
	ctx->FillGeometry(rotateArrows.Get(), brushDragger.Get());
}
