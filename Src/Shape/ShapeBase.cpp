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
	// 迷你条的三个配角：淡灰边、极淡的投影、删除那格的红。边框取色与圆角半径
	// 跟取色面板（WinColorPicker）那一套对齐 —— 白底浮层的既有约定，别再发明第二种
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0xDCDFE4), brushBarBorder.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.08f), brushBarShadow.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0xE24B4A), brushDelete.GetAddressOf());
}

ShapeBase::~ShapeBase()
{}

bool ShapeBase::isInRect(const D2D1_RECT_F rect, const float x, const float y) const
{
	return (x > rect.left && x<rect.right && y>rect.top && y < rect.bottom);
}

// 迷你条的两套间距。rad 是每格图标的半径、pad 是条两端的内边距，
// cellGap 是**格与格之间**的净空 —— 它刻意比 pad 大：两枚图标本身各有半宽，
// 原来两格中心只隔 (rad*2 + pad)，看着就是"挤在一起两个方块"（作者：太挤了，拉开一点）
static float barRad(const float draggerSize) { return draggerSize * 0.9f; }
static float barPad(const float draggerSize) { return draggerSize * 0.42f; }
static float barCellGap(const float draggerSize) { return draggerSize * 1.05f; }

// 复制与 × 并成的一条迷你条：挂在选中框下方居中（放不下就翻到框顶上），横向夹在图内。
// 原来这两枚分居左上 / 右上两个角，与八向手柄挤在同一圈窄带里，重叠时点错一枚就是误删
//（见 WinPin::canHitActionBtn）。摆到下方居中之后离四个角的手柄都远，而且整条是一块
// 连续的热区 —— 比两个散在角上的点好认，也好躲开
//
// 几何抽成静态的 barRectFor / barCellRect：多选那一批的批量条（WinPin::batchBarRect）
// 走的是同一份 —— 两处的格宽、间距、内边距必须一致，否则"单选一条、多选一条"看着像两家人
D2D1_RECT_F ShapeBase::barRectFor(const D2D1_RECT_F& b, const D2D1_SIZE_U& img,
	const float draggerSize, const int cells)
{
	if (cells <= 0) return D2D1::RectF(0.f, 0.f, 0.f, 0.f);
	auto rad{ barRad(draggerSize) };
	auto pad{ barPad(draggerSize) };
	auto gap{ barCellGap(draggerSize) };
	auto barW{ cells * rad * 2.f + pad * 2.f + (cells - 1) * gap };
	auto barH{ rad * 2.f + pad };
	auto cx{ (b.left + b.right) / 2.f };
	if (img.width > 0) {
		// 夹进图里：元素贴着图边时整条都得看得见、点得到。
		// 图比条还窄时 clamp 的上下界会反过来，std::clamp 在这种情况下是未定义行为，先判一下
		auto lo{ barW / 2.f }, hi{ (float)img.width - barW / 2.f };
		cx = lo < hi ? std::clamp(cx, lo, hi) : (float)img.width / 2.f;
	}
	auto cy{ b.bottom + draggerSize * 2.2f + barH / 2.f };
	if (img.height > 0 && cy + barH / 2.f > (float)img.height) {
		// 翻到框顶上；顶上也放不下（图很矮）就贴着图内夹住，宁可压在元素上也不能被裁掉
		cy = b.top - draggerSize * 2.2f - barH / 2.f;
		auto lo{ barH / 2.f }, hi{ (float)img.height - barH / 2.f };
		cy = lo < hi ? std::clamp(cy, lo, hi) : (float)img.height / 2.f;
	}
	return D2D1::RectF(cx - barW / 2.f, cy - barH / 2.f, cx + barW / 2.f, cy + barH / 2.f);
}

D2D1_RECT_F ShapeBase::barCellRect(const D2D1_RECT_F& bar, const int i, const float draggerSize)
{
	auto rad{ barRad(draggerSize) };
	auto pad{ barPad(draggerSize) };
	auto bx{ bar.left + pad + rad + i * (rad * 2.f + barCellGap(draggerSize)) };
	auto cy{ (bar.top + bar.bottom) / 2.f };
	return D2D1::RectF(bx - rad, cy - rad, bx + rad, cy + rad);
}

void ShapeBase::paintBarFrame(ID2D1DeviceContext* ctx, const D2D1_RECT_F& bar, const int cells,
	const float draggerSize, const float dpi,
	ID2D1Brush* fill, ID2D1Brush* border, ID2D1Brush* shadow)
{
	if (!ctx || !fill || !border) return;
	if (bar.right <= bar.left) return;
	// 白底 + 淡灰边 + 一层往下偏 1px 的极淡投影。之前只描了一圈浅蓝边，
	// 压在浅色画面上边界糊成一团，看着就像两个图标飘在白块上 —— 加投影之后
	// 整条才"浮"得起来，圆角取固定 6 逻辑像素（与取色面板一致），不随条高缩放
	auto r{ 6.f * dpi };
	if (shadow) {
		auto sh = D2D1::RectF(bar.left, bar.top + dpi, bar.right, bar.bottom + dpi);
		ctx->FillRoundedRectangle(D2D1::RoundedRect(sh, r, r), shadow);
	}
	ctx->FillRoundedRectangle(D2D1::RoundedRect(bar, r, r), fill);
	ctx->DrawRoundedRectangle(D2D1::RoundedRect(bar, r, r), border, dpi);
	// 格与格之间一条竖分隔线。位置取相邻两格图标框之间的正中间 ——
	// 别拿 bar 的中点算：两格的净空与两端的内边距并不相等，拿中点那根线就偏出去了
	for (int i = 1; i < cells; i++) {
		auto a = barCellRect(bar, i - 1, draggerSize);
		auto b = barCellRect(bar, i, draggerSize);
		auto mx{ (a.right + b.left) / 2.f };
		ctx->DrawLine(D2D1::Point2F(mx, bar.top + dpi * 3.f),
			D2D1::Point2F(mx, bar.bottom - dpi * 3.f), border, dpi);
	}
}

D2D1_RECT_F ShapeBase::actionBarRect() const
{
	D2D1_RECT_F b{};
	if (!getShapeBounds(b)) return D2D1::RectF(0.f, 0.f, 0.f, 0.f);
	return barRectFor(b, win->getImgSize(), draggerSize, actionBtnTotal());
}

D2D1_RECT_F ShapeBase::actionBtnRect(const int i) const
{
	if (i < 0 || i >= actionBtnTotal()) return D2D1::RectF(0.f, 0.f, 0.f, 0.f);
	auto bar = actionBarRect();
	if (bar.right <= bar.left) return D2D1::RectF(0.f, 0.f, 0.f, 0.f);
	return barCellRect(bar, i, draggerSize);
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
	const bool hasDel{ barHasDelete() };
	const bool hasCopy{ copyable() };
	const int last{ actionBtnTotal() - 1 };
	// 条身交给共用的那一份（多选那一批的批量条走的是同一个函数，观感才对得齐）
	paintBarFrame(ctx, actionBarRect(), actionBtnTotal(), draggerSize, win->getDpi(),
		brushDraggerFill.Get(), brushBarBorder.Get(), brushBarShadow.Get());
	for (int i = 0; i < actionBtnTotal(); i++) {
		auto box = actionBtnRect(i);
		if (box.right <= box.left) continue;
		auto c = D2D1::Point2F((box.left + box.right) / 2.f, (box.top + box.bottom) / 2.f);
		auto rad{ (box.right - box.left) / 2.f };
		// 不再垫白色圆底、也不再描那个圆框（作者：只保留圆圈内部的小图标）。
		// 图标统一走"白描边 + 原色"两遍（paintIconHaloed），压在任意底图上都读得出来
		if (hasDel && i == last) {
			// × 是删除，红色 —— 危险操作得在颜色上就跟复制那枚分开
			auto k{ rad * 0.42f };
			auto stroke{ draggerSize * 0.18f };
			ID2D1Brush* del{ brushDelete.Get() };
			ID2D1Brush* halo{ brushDraggerFill.Get() };
			paintIconHaloed(ctx, stroke, [&](ID2D1Brush* b, float w) {
				// 第一遍是白描边衬底（在白底条上不可见，无妨），第二遍才是主体
				ID2D1Brush* bb{ b == halo ? halo : del };
				ctx->DrawLine({ c.x - k, c.y - k }, { c.x + k, c.y + k }, bb, w);
				ctx->DrawLine({ c.x - k, c.y + k }, { c.x + k, c.y - k }, bb, w);
			});
		}
		else if (hasCopy && i == actionCount()) {
			// 复制：两枚叠着的方框。没有圆底之后两枚都只描边 —— 再把前面那枚填白的话，
			// 后一枚压在下半截的边会被整块盖掉，反倒看不出是"两张纸叠着"
			auto k{ rad * 0.46f }, off{ rad * 0.32f };
			paintIconHaloed(ctx, win->getDpi(), [&](ID2D1Brush* b, float w) {
				ctx->DrawRectangle(D2D1::RectF(c.x - k - off, c.y - k - off, c.x + k - off, c.y + k - off), b, w);
				ctx->DrawRectangle(D2D1::RectF(c.x - k + off, c.y - k + off, c.x + k + off, c.y + k + off), b, w);
			});
		}
		else {
			paintIconHaloed(ctx, win->getDpi(), [&](ID2D1Brush* b, float w) {
				paintActionIcon(ctx, i, c, rad, b, w);
			});
		}
	}
}

void ShapeBase::onActionBtn(const int i)
{
	if (i < 0 || i >= actionBtnTotal()) return;
	if (barHasDelete() && i == actionBtnTotal() - 1) {
		// 走 History 的统一删除口子：它会先把可能开着的编辑器收尾，再删、再刷新
		win->history->removeActiveShape();
		return;
	}
	if (copyable() && i == actionCount()) {
		// 复制：照原样再画一份，往右下挪开一点摆在原件的旁边。收下之后它就是选中态，
		// 接着能直接拖到想要的位置 / 改样式 —— 与刚画完的那一笔同一套
		auto off{ draggerSize * 3.f };
		if (auto copy = clone(off, off)) {
			win->history->addShape(std::move(copy));
		}
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
	// 与 actionBtnRect 用同一个 gap（3.0f，见那里的说明）：三个角离框的距离一致，摆在一起才像一排
	auto gap{ draggerSize * 3.0f };
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
	auto c = D2D1::Point2F((rotateDragger.left + rotateDragger.right) / 2.f,
		(rotateDragger.bottom + rotateDragger.top) / 2.f);
	paintRotateHandleAt(ctx, c);
}

void ShapeBase::paintRotateHandleAt(ID2D1DeviceContext* ctx, const D2D1_POINT_2F& c)
{
	auto d2d = Ling::D2D::get();
	auto dpi = win->getDpi();
	// 不再垫白圆底（作者：只保留圆圈内部的小图标）。
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
	// 两段几何先一起白描边打底，再上原色：弧描边、箭头填充（描边那遍把箭头的轮廓也描上，
	// 填充之后外圈仍留一道白边）
	paintIconHaloed(ctx, dpi, [&](ID2D1Brush* b, float w) {
		ctx->DrawGeometry(rotateArc.Get(), b, w);
		ctx->DrawGeometry(rotateArrows.Get(), b, w);
	});
	ctx->DrawGeometry(rotateArc.Get(), brushDragger.Get(), dpi);
	ctx->FillGeometry(rotateArrows.Get(), brushDragger.Get());
}
