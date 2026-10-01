#include "pch.h"
#include <dwmapi.h>
#include <include/Ling.h>
#include "CutMask.h"
#include "../Util.h"
#include "../Setting.h"
using namespace Microsoft::WRL;

CutMask::CutMask(Ling::WinBase* win) :win{ win }
{
	strokeWidth = 2 * win->dpi;
	paddingTop *= win->dpi;
	paddingMargin *= win->dpi;
	polyStep *= win->dpi;
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushText.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.46f), brushBg.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x1677ff), brushBorder.GetAddressOf());
	initWinRect();
}

bool CutMask::highlight(POINT pos)
{
	// 手绘选区框出来之后就不再跟着窗口跑了：那会让刚画好的形状被整个拉歪。
	// 固定区域同理 —— 尺寸是钉死的，吸附只会把它撵成别的大小
	if (isPoly() || fixedW > 0.f) return false;
	for (auto& rect : winRect)
	{
		if (pos.x > rect.left && pos.y > rect.top && pos.x < rect.right && pos.y < rect.bottom) {
			if (maskRect.left != rect.left || maskRect.top != rect.top ||
				maskRect.right != rect.right || maskRect.bottom != rect.bottom) {
				maskRect = rect;
				makeLayout();
				win->refresh();
				return true;
			}
			break;
		}
	}
	return false;
}

void CutMask::initWinRect()
{
	winRect.clear();
	EnumWindows([](HWND hwnd, LPARAM lparam)
		{
			if (!hwnd) return TRUE;
			if (!IsWindowVisible(hwnd)) return TRUE;
			BOOL cloaked = FALSE;
			DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
			if (cloaked) return TRUE;
			RECT rect;
			DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(RECT));
			if (rect.right - rect.left <= 6 || rect.bottom - rect.top <= 6) return TRUE;
			auto self = (CutMask*)lparam;
			auto win = self->win;
			if (rect.left < win->x) rect.left = win->x;
			if (rect.top < win->y) rect.top = win->y;
			if (rect.right > win->x + win->w) rect.right = (LONG)(win->x + win->w);
			if (rect.bottom > win->y + win->h) rect.bottom = (LONG)(win->y + win->h);
			auto x = (float)(rect.left - win->x);
			auto y = (float)(rect.top - win->y);
			auto r = (float)(rect.right - win->x);
			auto b = (float)(rect.bottom - win->y);
			self->winRect.push_back(D2D1::RectF(x, y, r, b));
			return TRUE;
		}, (LPARAM)this);
}

void CutMask::makeLayout()
{
	layout.Reset();
	auto layoutStr = std::format(L"X:{} Y:{} R:{} B:{} W:{} H:{}",
		maskRect.left, maskRect.top, maskRect.right, maskRect.bottom,
		maskRect.right - maskRect.left, maskRect.bottom - maskRect.top);
	layout = Ling::D2D::get()->makeTextLayout(layoutStr, 10 * win->dpi);
	if (!layout) return;
	DWRITE_TEXT_METRICS tm = {};
	layout->GetMetrics(&tm);
	layoutRect = D2D1::RectF(maskRect.left, maskRect.top - paddingMargin - tm.height - paddingMargin * 2, maskRect.left + tm.width + paddingMargin * 2, maskRect.top - paddingMargin);
	// 标签被顶出窗口上边时折回 maskRect 内部
	if (layoutRect.top < 0) {
		auto h = layoutRect.bottom - layoutRect.top;
		auto w = layoutRect.right - layoutRect.left;
		layoutRect.top = maskRect.top + paddingMargin / 2;
		layoutRect.bottom = layoutRect.top + h;
		layoutRect.left = maskRect.left + paddingMargin;
		layoutRect.right = layoutRect.left + w;
	}
	layout->SetMaxWidth(layoutRect.right - layoutRect.left);
	layout->SetMaxHeight(layoutRect.bottom - layoutRect.top);
}

void CutMask::beginFixedSize()
{
	int w{ 0 }, h{ 0 };
	if (Setting::fixedSize(Setting::get()->getCapFixedIdx(), w, h)) {
		fixedW = (float)w;
		fixedH = (float)h;
		return;
	}
	fixedW = 0.f;
	fixedH = 0.f;
}

void CutMask::startMakeRect(POINT pos)
{
	// 只在上一次是手绘时才清理：无脑清会把悬停吸附出来的整窗矩形一起抹掉，
	// "点一下就截当前窗口"这条路就断了
	if (isPoly()) clearPoly();
	beginFixedSize();
	// 固定区域：按下那一刻框就已经成形了，拖到哪算哪。这样"点一下出一张固定尺寸图"
	// 也走得通 —— 否则不动鼠标根本不会有矩形
	if (fixedW > 0.f && fixedH > 0.f) {
		makeRect(pos);
		return;
	}
	pressPos = pos;
}

void CutMask::makeRect(POINT pos)
{
	if (fixedW > 0.f && fixedH > 0.f) {
		// 尺寸钉死，能动的只有左上角。往右 / 下越界就把左上角推回来，
		// 而不是把框缩小 —— 缩了就不是"固定尺寸"了
		const float left = std::clamp((float)pos.x, 0.f, std::max(0.f, win->w - fixedW));
		const float top = std::clamp((float)pos.y, 0.f, std::max(0.f, win->h - fixedH));
		maskRect = D2D1::RectF(left, top, left + fixedW, top + fixedH);
		makeLayout();
		win->refresh();
		return;
	}
	auto [left, right] = std::minmax(pressPos.x, pos.x);
	auto [top, bottom] = std::minmax(pressPos.y, pos.y);
	maskRect.left = (float)left;
	maskRect.right = (float)right;
	maskRect.top = (float)top;
	maskRect.bottom = (float)bottom;
	makeLayout();
	win->refresh();
}

bool CutMask::hasRect() const
{
	// 手绘时 maskRect 就是那条路径的外接矩形（syncPoly 保证同步），所以一律只看矩形自身。
	// 顺带把"拖成一条直线"（宽或高为 0）挡在门外 —— 那种选区导出出来是空图
	return maskRect.right > maskRect.left && maskRect.bottom > maskRect.top;
}

void CutMask::startPoly(POINT pos)
{
	poly.clear();
	// 起手就把上一次的框收掉：前两个点还没够 isPoly() 的门槛，syncPoly 不会重算 maskRect，
	// 留着会在这几毫秒里被当成"当前选区"画出来（多半是刚悬停吸附到的整窗矩形）
	maskRect = D2D1::RectF(0.f, 0.f, 0.f, 0.f);
	poly.push_back(D2D1::Point2F((float)pos.x, (float)pos.y));
	syncPoly();
}

void CutMask::addPolyPoint(POINT pos)
{
	if (poly.empty()) return;
	const auto& last = poly.back();
	const float dx = (float)pos.x - last.x, dy = (float)pos.y - last.y;
	// 隔着太近的点留着只会让路径变长、让 hitTest 变慢，视觉上一模一样
	if (dx * dx + dy * dy < polyStep * polyStep) return;
	poly.push_back(D2D1::Point2F((float)pos.x, (float)pos.y));
	syncPoly();
}

void CutMask::endPoly()
{
	if (!isPoly()) {
		clearPoly();
		return;
	}
	// 末点离起点不足一个采样步长，就是绕回来时多按下的那一点，去掉它
	if (poly.size() > 3) {
		const auto& first = poly.front();
		const auto& last = poly.back();
		const float dx = last.x - first.x, dy = last.y - first.y;
		if (dx * dx + dy * dy < polyStep * polyStep) poly.pop_back();
	}
	syncPoly();
	win->refresh();
}

void CutMask::clearPoly()
{
	poly.clear();
	syncPoly();     // 内部已经重算了 maskRect 与标签
	win->refresh();
}

bool CutMask::pointInPoly(POINT pos) const
{
	// 从待测点往右打一条射线，穿过奇数条边就在内部
	bool inside{ false };
	const float px = (float)pos.x, py = (float)pos.y;
	for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
	{
		const auto& a = poly[j];
		const auto& b = poly[i];
		const bool straddles = (a.y > py) != (b.y > py);
		if (straddles && px < (b.x - a.x) * (py - a.y) / (b.y - a.y) + a.x) {
			inside = !inside;
		}
	}
	return inside;
}

void CutMask::movePoly(const float dx, const float dy)
{
	for (auto& pt : poly) {
		pt.x += dx;
		pt.y += dy;
	}
}

void CutMask::syncPoly()
{
	polyGeom.Reset();
	ringGeom.Reset();
	if (!isPoly()) {
		// 没有手绘路径时 maskRect 必须归零：否则"画了一半又改画矩形"会留下上一次的旧框，
		// hasRect() 也会跟着误判成还有选区
		if (poly.empty()) maskRect = D2D1::RectF(0.f, 0.f, 0.f, 0.f);
		makeLayout();
		return;
	}
	float l{ FLT_MAX }, t{ FLT_MAX }, r{ -FLT_MAX }, b{ -FLT_MAX };
	for (auto& pt : poly)
	{
		l = std::min(l, pt.x); r = std::max(r, pt.x);
		t = std::min(t, pt.y); b = std::max(b, pt.y);
	}
	maskRect = D2D1::RectF(l, t, r, b);
	auto factory = Ling::D2D::get()->d2dFactory;
	auto makeGeom = [&factory, this](const bool withOuterRect, ID2D1PathGeometry** out) {
		if (FAILED(factory->CreatePathGeometry(out))) return false;
		Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
		if (FAILED((*out)->Open(sink.GetAddressOf()))) return false;
		// 偶数-奇数填充：整块窗口一个 figure、多边形一个 figure，重叠的部分"填了两次"
		// 等于被挖掉 —— 不用真去做几何布尔运算，也不关心两者的旋向
		if (withOuterRect) {
			sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
			sink->BeginFigure(D2D1::Point2F(0.f, 0.f), D2D1_FIGURE_BEGIN_FILLED);
			sink->AddLine(D2D1::Point2F(win->w, 0.f));
			sink->AddLine(D2D1::Point2F(win->w, win->h));
			sink->AddLine(D2D1::Point2F(0.f, win->h));
			sink->EndFigure(D2D1_FIGURE_END_CLOSED);
		}
		sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
		sink->BeginFigure(poly[0], D2D1_FIGURE_BEGIN_FILLED);
		for (size_t i = 1; i < poly.size(); i++) sink->AddLine(poly[i]);
		sink->EndFigure(D2D1_FIGURE_END_CLOSED);
		sink->Close();
		return true;
	};
	makeGeom(true, ringGeom.GetAddressOf());
	makeGeom(false, polyGeom.GetAddressOf());
	makeLayout();
}

ComPtr<ID2D1PathGeometry> CutMask::makePolyGeom(int offsetX, int offsetY) const
{
	ComPtr<ID2D1PathGeometry> geom;
	if (!isPoly()) return geom;
	auto factory = Ling::D2D::get()->d2dFactory;
	if (FAILED(factory->CreatePathGeometry(geom.GetAddressOf()))) return geom;
	Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(geom->Open(sink.GetAddressOf()))) { geom.Reset(); return geom; }
	sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
	// 顶点平移到"以选区左上角为原点"的坐标系：蒙层只覆盖 0,0 - 宽,高
	sink->BeginFigure(D2D1::Point2F(poly[0].x - (float)offsetX, poly[0].y - (float)offsetY),
		D2D1_FIGURE_BEGIN_FILLED);
	for (size_t i = 1; i < poly.size(); i++) {
		sink->AddLine(D2D1::Point2F(poly[i].x - (float)offsetX, poly[i].y - (float)offsetY));
	}
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	sink->Close();
	return geom;
}

MaskHit CutMask::hitTest(POINT pos) const
{
	if (!hasRect()) return MaskHit::None;
	// 手绘选区没有"边"和"角"可言，只有内外：落在里面就是整体拖动，外面一律不动它
	if (isPoly()) return pointInPoly(pos) ? MaskHit::Inside : MaskHit::None;
	const auto& r = maskRect;
	const float px = (float)pos.x, py = (float)pos.y;
	// 选区的四条边各自延长，把整个窗口切成九块：中间那块就是选区自己，
	// 落在里面一律整体拖动；外面八块各对应一个方位，落在哪块就调哪条边/哪个角
	const int col = px < r.left ? 0 : (px > r.right ? 2 : 1);
	const int row = py < r.top ? 0 : (py > r.bottom ? 2 : 1);
	static constexpr MaskHit grid[3][3]{
		{ MaskHit::TopLeft,    MaskHit::Top,    MaskHit::TopRight },
		{ MaskHit::Left,       MaskHit::Inside, MaskHit::Right },
		{ MaskHit::BottomLeft, MaskHit::Bottom, MaskHit::BottomRight },
	};
	return grid[row][col];
}

void CutMask::startAdjust(POINT pos)
{
	adjustHit = hitTest(pos);
	adjustStartRect = maskRect;
	adjustPressPos = pos;
	// 抓边或抓角：按下这一下就把对应的边挪到光标位置。
	// 内部整体拖动不能这么干 —— 那会让选区瞬间跳到光标为中心的位置。
	if (adjustHit != MaskHit::None && adjustHit != MaskHit::Inside) {
		adjust(pos);
	}
}

void CutMask::adjust(POINT pos)
{
	if (adjustHit == MaskHit::None) return;
	auto r = adjustStartRect;
	const float px = (float)pos.x, py = (float)pos.y;
	if (adjustHit == MaskHit::Inside) {
		// 整体平移，尺寸不变，夹在宿主客户区内
		const float rw = r.right - r.left, rh = r.bottom - r.top;
		const float left = std::clamp(r.left + px - adjustPressPos.x, 0.f, win->w - rw);
		const float top = std::clamp(r.top + py - adjustPressPos.y, 0.f, win->h - rh);
		if (isPoly()) {
			const float dx = left - maskRect.left, dy = top - maskRect.top;
			if (dx == 0.f && dy == 0.f) return;
			movePoly(dx, dy);
			syncPoly();
			win->refresh();
			return;
		}
		r = D2D1::RectF(left, top, left + rw, top + rh);
	}
	else {
		const float cx = std::clamp(px, 0.f, win->w);
		const float cy = std::clamp(py, 0.f, win->h);
		switch (adjustHit)
		{
		case MaskHit::Left: r.left = cx; break;
		case MaskHit::Right: r.right = cx; break;
		case MaskHit::Top: r.top = cy; break;
		case MaskHit::Bottom: r.bottom = cy; break;
		case MaskHit::TopLeft: r.left = cx; r.top = cy; break;
		case MaskHit::TopRight: r.right = cx; r.top = cy; break;
		case MaskHit::BottomRight: r.right = cx; r.bottom = cy; break;
		case MaskHit::BottomLeft: r.left = cx; r.bottom = cy; break;
		default: break;
		}
		// 拖过头（比如左边越过右边）时归一化，接下来就自然变成"在拖另一条边"
		const float l = std::min(r.left, r.right), rr = std::max(r.left, r.right);
		const float t = std::min(r.top, r.bottom), b = std::max(r.top, r.bottom);
		r = D2D1::RectF(l, t, rr, b);
		// 塌到最小尺寸以下时，把动着的那条边推回来，不动锚定的那条
		const bool moveLeft = adjustHit == MaskHit::Left || adjustHit == MaskHit::TopLeft || adjustHit == MaskHit::BottomLeft;
		const bool moveTop = adjustHit == MaskHit::Top || adjustHit == MaskHit::TopLeft || adjustHit == MaskHit::TopRight;
		if (r.right - r.left < minSize) {
			if (moveLeft) r.left = r.right - minSize;
			else r.right = r.left + minSize;
		}
		if (r.bottom - r.top < minSize) {
			if (moveTop) r.top = r.bottom - minSize;
			else r.bottom = r.top + minSize;
		}
	}
	if (r.left == maskRect.left && r.top == maskRect.top &&
		r.right == maskRect.right && r.bottom == maskRect.bottom) return;
	maskRect = r;
	makeLayout();
	win->refresh();
}

void CutMask::paint(ID2D1DeviceContext* ctx)
{
	if (!layout || !hasRect()) return;
	if (isPoly() && ringGeom && polyGeom) {
		ctx->FillGeometry(ringGeom.Get(), brushBg.Get());
		ctx->DrawGeometry(polyGeom.Get(), brushBorder.Get(), strokeWidth);
	}
	else {
		ctx->FillRectangle(D2D1::RectF(0.f, 0.f, win->w, maskRect.top), brushBg.Get());
		ctx->FillRectangle(D2D1::RectF(0.f, maskRect.bottom, win->w, win->h), brushBg.Get());
		ctx->FillRectangle(D2D1::RectF(0.f, maskRect.top, maskRect.left, maskRect.bottom), brushBg.Get());
		ctx->FillRectangle(D2D1::RectF(maskRect.right, maskRect.top, win->w, maskRect.bottom), brushBg.Get());
		auto halfStrokeWidth{ strokeWidth / 2.f };
		ctx->DrawRectangle(D2D1::RectF(maskRect.left - halfStrokeWidth, maskRect.top - halfStrokeWidth, maskRect.right + halfStrokeWidth, maskRect.bottom + halfStrokeWidth), brushBorder.Get(), strokeWidth);
	}
	if (hideLabel) return;
	ctx->FillRectangle(layoutRect, brushBg.Get());
	ctx->DrawTextLayout({ layoutRect.left+ paddingMargin, layoutRect.top+ paddingMargin }, layout.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}
