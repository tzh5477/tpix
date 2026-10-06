#include "pch.h"
#include "Canvas.h"
#include "Tool/ToolSub.h"
#include "ShapeLineBase.h"

using Microsoft::WRL::ComPtr;

ShapeLineBase::ShapeLineBase(Canvas* win) :ShapeBase(win), draggers{
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0)}
{
	auto d2d = Ling::D2D::get();
	// 圆头圆角描边。本项目没有 App::getRoundStrokeStyle 那样的全局缓存，
	// 就每个 shape 自己持一个 —— StrokeStyle 是不可变的轻对象，代价可以忽略
	d2d->d2dFactory->CreateStrokeStyle(
		D2D1::StrokeStyleProperties(
			D2D1_CAP_STYLE_ROUND,    // 起点线帽：圆角
			D2D1_CAP_STYLE_ROUND,    // 终点线帽：圆角
			D2D1_CAP_STYLE_ROUND,    // 虚线端点（如有）
			D2D1_LINE_JOIN_ROUND,    // 线段连接处：圆角
			8.f,                     // miterLimit
			D2D1_DASH_STYLE_SOLID,
			0.f
		),
		nullptr, 0, roundStyle.GetAddressOf());
}

ShapeLineBase::~ShapeLineBase()
{

}

void ShapeLineBase::paintDragger(ID2D1DeviceContext* ctx)
{
	for (auto& dragger : draggers)
	{
		// 选中的填白、悬停的留空：光标掠过一串元素时能分出改样式会作用到谁。
		// 先填后描：描边是压在矩形边线中线上的，先描再填会把内半边盖掉，线看着只剩外半截
		if (win->selected == this) ctx->FillRectangle(dragger, brushDraggerFill.Get());
		ctx->DrawRectangle(dragger, brushDragger.Get(), win->getDpi());
	}
}

void ShapeLineBase::mouseDrag(const float x, const float y)
{
	// Shift 按下时不再追加顶点，而是拖动端点 —— 效果是一条直线；松开 Shift 就是自由画
	bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	if (hoverDraggerIndex == 0) {
		if (shiftDown) {
			auto& p = linePoints[0];
			p.x = x;
			p.y = y;
		}
		else {
			linePoints.insert(linePoints.begin(), { x,y });
		}
		makePath();
	}
	else if (hoverDraggerIndex == 1) {
		if (shiftDown) {
			auto& p = linePoints[linePoints.size() - 1];
			p.x = x;
			p.y = y;
		}
		else {
			linePoints.push_back({ x,y });
		}
		makePath();
	}
	else if (hoverDraggerIndex == 8) {
		auto spanX{ x - pressX };
		auto spanY{ y - pressY };
		for (auto& p:linePoints)
		{
			p.x += spanX;
			p.y += spanY;
		}
		makePath();
		pressX = x;
		pressY = y;
	}
}

void ShapeLineBase::mouseDown(const float x, const float y)
{
	if (hoverDraggerIndex == -1) { //首次创建
		linePoints.push_back({ x,y });
		if (GetKeyState(VK_SHIFT) & 0x8000) {
			// 直线模式需要两个点，末点跟着鼠标走
			linePoints.push_back({ x,y });
		}
		makePath();
		hoverDraggerIndex = 1;
		win->refresh();
	}
	else if (hoverDraggerIndex == 8) {
		pressX = x;
		pressY = y;
	}
}

void ShapeLineBase::mouseUp(const float x, const float y)
{
	makeDraggers();
}

void ShapeLineBase::mouseMove(const float x, const float y)
{
	hoverDraggerIndex = -1;
	hitDraggers(x, y);
	if (hoverDraggerIndex == -1) {
		hitTest({ x,y });
	}
}

void ShapeLineBase::setCursor()
{
	if (hoverDraggerIndex == 8) {
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	}
}

void ShapeLineBase::hitDraggers(const float x, const float y)
{
	if (isInRect(draggers[0], x, y)) {
		hoverDraggerIndex = 0;
	}
	else if (isInRect(draggers[1], x, y)) {
		hoverDraggerIndex = 1;
	}
}

void ShapeLineBase::makeDraggers()
{
	if (linePoints.empty()) return;
	auto half{ draggerSize / 2 };
	auto& start = linePoints[0];
	draggers[0].left = start.x - half;
	draggers[0].top = start.y - half;
	draggers[0].right = start.x + half;
	draggers[0].bottom = start.y + half;
	auto& end = linePoints[linePoints.size() - 1];
	draggers[1].left = end.x - half;
	draggers[1].top = end.y - half;
	draggers[1].right = end.x + half;
	draggers[1].bottom = end.y + half;
}

void ShapeLineBase::makePath()
{
	buildPath(linePoints);
}

void ShapeLineBase::buildPath(const std::vector<D2D1_POINT_2F>& pts)
{
	if (pts.empty()) return;
	auto d2d = Ling::D2D::get();
	// ReleaseAndGetAddressOf 而不是 GetAddressOf：后者不放旧对象，自由画时每个鼠标事件漏一个几何体
	d2d->d2dFactory->CreatePathGeometry(path.ReleaseAndGetAddressOf());
	if (!path) return;
	ComPtr<ID2D1GeometrySink> sink;
	path->Open(sink.GetAddressOf());
	sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
	if (pts.size() > 1) {
		sink->AddLines(&pts[1], (UINT32)(pts.size() - 1));
	}
	sink->EndFigure(D2D1_FIGURE_END_OPEN);
	sink->Close();
}

float  ShapeLineBase::pointToSegmentDistance(const D2D1_POINT_2F& p, const D2D1_POINT_2F& a, const D2D1_POINT_2F& b)
{
	float abx = b.x - a.x, aby = b.y - a.y;
	float apx = p.x - a.x, apy = p.y - a.y;
	float ab2 = abx * abx + aby * aby;
	float t = (ab2 == 0.0f) ? 0.0f : (apx * abx + apy * aby) / ab2;
	t = std::max(0.0f, std::min(1.0f, t));  // 限制在线段范围内
	float cx = a.x + t * abx;
	float cy = a.y + t * aby;
	float dx = p.x - cx, dy = p.y - cy;

	return std::sqrtf(dx * dx + dy * dy);
}

void ShapeLineBase::hitTest(const D2D1_POINT_2F& mousePos)
{
	// 只有笔画边缘那一圈算命中：粗笔画（马赛克、橡皮擦尤其明显）整片都能拖的话，
	// 就没法在已有笔画上面再画一笔了 —— 鼠标一按下会变成拖动旧元素。
	// 矩形模式本来就是只有边框附近才响应，这里跟它对齐。
	// 内圈多少由 hitInnerLimit 定：折线族默认照上面的理由挖掉半个夹点宽，
	// 线条那一支覆写成 0（它不存在"在笔画内部起笔"的用法）
	float outer = strokeWidth * 0.5f + win->getDpi(); //外沿保持原来的判定范围
	float inner = hitInnerLimit(outer);
	// 得先在所有线段里取最小距离再判断：自交的笔画里，某一段的边缘可能正好压在另一段的
	// 内部，那种位置视觉上是在笔画里面，逐段判断会误判成边缘
	float minDist{ FLT_MAX };
	for (size_t i = 0; i + 1 < linePoints.size(); ++i) {
		minDist = std::min(minDist, pointToSegmentDistance(mousePos, linePoints[i], linePoints[i + 1]));
	}
	if (minDist <= outer && minDist >= inner) {
		hoverDraggerIndex = 8;
	}
}

float ShapeLineBase::hitInnerLimit(const float outer) const
{
	// 往里让出半个夹点算内部。细笔画算出来是负数，等于整条线都能拖
	return outer - draggerSize / 2;
}

// 「选择对象」框选要用。量的是点列，不是 path —— path 是照 shaftPoints 建的，
// 线条那一档带标记的两头往回收了一截（见 ShapeLine::shaftPoints），拿它量会把箭头漏在框外
bool ShapeLineBase::getShapeBounds(D2D1_RECT_F& out) const
{
	if (linePoints.empty()) return false;
	float l{ linePoints[0].x }, r{ l }, t{ linePoints[0].y }, b{ t };
	for (auto& p : linePoints) {
		l = std::min(l, p.x);
		r = std::max(r, p.x);
		t = std::min(t, p.y);
		b = std::max(b, p.y);
	}
	// 笔画有宽度：往外让出半个线宽，画出来的像素才全落在框里
	const float pad{ boundsPad() };
	out = D2D1::RectF(l - pad, t - pad, r + pad, b + pad);
	return true;
}

float ShapeLineBase::boundsPad() const
{
	return strokeWidth * 0.5f;
}
