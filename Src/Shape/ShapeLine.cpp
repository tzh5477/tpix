#include "pch.h"
#include <cmath>
#include <algorithm>
#include "Canvas.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeLine.h"

using Microsoft::WRL::ComPtr;

namespace {
	constexpr float kPi{ 3.14159265358979323846f };

	// 从 a 指向 b 的单位向量。两点重合时回一个朝右的兜底向量，免得出 NaN 把几何画飞
	D2D1_POINT_2F unitDir(const D2D1_POINT_2F& a, const D2D1_POINT_2F& b)
	{
		auto dx{ b.x - a.x }, dy{ b.y - a.y };
		auto len = std::sqrt(dx * dx + dy * dy);
		if (len < 0.01f) return { 1.f, 0.f };
		return { dx / len, dy / len };
	}

	float distance(const D2D1_POINT_2F& a, const D2D1_POINT_2F& b)
	{
		auto dx{ b.x - a.x }, dy{ b.y - a.y };
		return std::sqrt(dx * dx + dy * dy);
	}

	// 直角折线吸附的两个阈值（底图像素）：手抖不到 kMinStep 就不算"走了一步"，
	// 垂直方向攒够 kTurnStep 才认定用户拐了弯
	constexpr float kMinStep{ 5.f };
	constexpr float kTurnStep{ 8.f };
}

ShapeLine::ShapeLine(Canvas* win) :ShapeLineBase(win)
{
	auto toolSub = win->getToolSub();
	auto color = toolSub->getSelectedColor();
	if (toolSub->isLineTransparent) {
		color.a = 0.5f;
	}
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(color, brush.GetAddressOf());
	// 线宽 / 类型 / 端点 / 线型都走 applyStyle 那一套，省得两处各写一遍（那里还会建描边样式）
	applyStyle();
}

ShapeLine::~ShapeLine()
{

}

// 线宽、半透明、线条类型、两端形状、线条样式全在 ToolSub 上，改一样就整份重取。
// 半透明开关也在这里 —— 颜色带 alpha 时要连它一起重算
void ShapeLine::applyStyle()
{
	auto toolSub = win->getToolSub();
	auto color = toolSub->getSelectedColor();
	if (toolSub->isLineTransparent) {
		color.a = 0.5f;
	}
	brush->SetColor(color);
	strokeWidth = toolSub->getSliderVal();
	auto wasOrtho{ isOrtho() };
	kind = (Kind)toolSub->lineKind;
	lineStyle = (Style)toolSub->lineStyle;
	endIndex = toolSub->lineEnd;
	makeStrokeStyle();
	// 类型从"普通线条"切到"直角折线"：拿已画的那串顶点重新吸附一遍。反向不做 ——
	// 原始的自由轨迹已经丢了，还原不回去，只能保持现状。作者要的"改完同步到选中的那一笔"
	// 只有这一半有结果，另一半（切回普通线条）本来就是"没有可改的东西"
	if (isOrtho() && !wasOrtho) snapExisting();
}

// 已经画好的自由线条改成直角折线：它的 linePoints 就是一串鼠标位置，正好是 snapTrail 要的
// 输入，借 trail 递进去重吸附一遍即可
void ShapeLine::snapExisting()
{
	if (linePoints.size() < 2) return;
	trail = linePoints;
	snapTrail();
	trail.clear();
	makePath();
	// 顶点动了，两端的夹点得跟着挪，不然拖端点会拖空
	makeDraggers();
}

bool ShapeLine::isOrtho() const
{
	return kind == Kind::Ortho;
}

ShapeLine::EndPair ShapeLine::ends() const
{
	// 顺序 = config.json 里 line/end 的落盘值，也是「端点」下拉里的顺序：
	// 无 / 末端实心箭头 / 起点圆点+末端实心箭头 / 末端细箭头 / 起点圆点+末端细箭头 /
	// 末端圆点 / 起点圆点 / 两端圆点 / 两端实心箭头 / 两端细箭头。
	// 三种端点标记（实心箭头 / 细箭头 / 圆点）的两两组合，加上"起点有、末端有"叠出来的这些
	static const EndPair table[]{
		{ EndMark::None,  EndMark::None  },
		{ EndMark::None,  EndMark::Arrow },
		{ EndMark::Dot,   EndMark::Arrow },
		{ EndMark::None,  EndMark::Thin  },
		{ EndMark::Dot,   EndMark::Thin  },
		{ EndMark::None,  EndMark::Dot   },
		{ EndMark::Dot,   EndMark::None  },
		{ EndMark::Dot,   EndMark::Dot   },
		{ EndMark::Arrow, EndMark::Arrow },
		{ EndMark::Thin,  EndMark::Thin  },
	};
	if (endIndex < 0 || endIndex >= (int)std::size(table)) return { EndMark::None, EndMark::None };
	return table[endIndex];
}

void ShapeLine::makeStrokeStyle()
{
	auto dash{ D2D1_DASH_STYLE_SOLID };
	switch (lineStyle) {
	case Style::Dash: dash = D2D1_DASH_STYLE_DASH; break;
	case Style::Dot: dash = D2D1_DASH_STYLE_DOT; break;
	case Style::DashDot: dash = D2D1_DASH_STYLE_DASH_DOT; break;
	case Style::DashDotDot: dash = D2D1_DASH_STYLE_DASH_DOT_DOT; break;
	default: break;   // 实线与波浪都按实线描：波浪另画一条正弦路径
	}
	Ling::D2D::get()->d2dFactory->CreateStrokeStyle(
		D2D1::StrokeStyleProperties(
			D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
			D2D1_LINE_JOIN_ROUND, 8.f, dash, 0.f),
		nullptr, 0, strokeStyle.ReleaseAndGetAddressOf());
}

void ShapeLine::mouseDown(const float x, const float y)
{
	// 直角折线的新建这一笔：起点先落下来，几何随后由鼠标轨迹现算（见 mouseDrag）。
	// 基类那套"每个鼠标位置塞一个顶点"是自由画的记法，直角族不能用 —— 记下来的是斜的轨迹
	if (isOrtho() && hoverDraggerIndex == -1) {
		trail.clear();
		trail.push_back({ x, y });
		creating = true;
	}
	ShapeLineBase::mouseDown(x, y);
}

void ShapeLine::mouseDrag(const float x, const float y)
{
	if (creating && isOrtho()) {
		trail.push_back({ x, y });
		snapTrail();
		makePath();
		return;
	}
	ShapeLineBase::mouseDrag(x, y);
}

void ShapeLine::mouseUp(const float x, const float y)
{
	trail.clear();
	creating = false;
	ShapeLineBase::mouseUp(x, y);
}

// 直角折线：把这次拖拽的鼠标轨迹压成一条横平竖直的折线。
// 分两种情况（作者定的规矩）：
//   - 轨迹没拐弯：拖成什么方向就出什么线 —— 横拖出横线、竖拖出竖线、斜拖出斜线。
//     所以先判"直不直"，直的把首尾直接连起来，不吸附
//   - 轨迹拐了弯：整条吸附成横平竖直 —— 沿轨迹走，走到"转向垂直方向"的那一点就落一个
//     90° 拐点，于是出来的折线只有横竖两向，一条斜的都没有
void ShapeLine::snapTrail()
{
	if (trail.size() < 2) return;
	auto p0{ trail.front() }, p1{ trail.back() };
	auto len = distance(p0, p1);
	if (len < kMinStep) {
		linePoints.assign(1, p0);   // 还没真拖开，等位移够大再定形
		return;
	}
	// 直不直看"轨迹上的点到首尾连线的最大垂距"：手抖一两像素、缩放后更小，
	// 阈值取线长的 4% 与 4 个底图像素里的大者
	auto vx{ p1.x - p0.x }, vy{ p1.y - p0.y };
	auto tol = std::max(4.f, len * 0.04f);
	bool straight{ true };
	for (auto& p : trail) {
		auto dev = std::abs((p.x - p0.x) * vy - (p.y - p0.y) * vx) / len;
		if (dev > tol) { straight = false; break; }
	}
	if (straight) {
		linePoints.assign({ p0, p1 });
		return;
	}
	// 拐过弯：沿轨迹逐点看"这一段往哪走"。轴向未定就攒位移，定了之后跟增量比 ——
	// 垂直方向的增量超过沿轴方向的增量、且攒够一截，就认定用户在这一带拐了个直角
	linePoints.clear();
	linePoints.push_back(p0);
	auto v{ p0 };                 // 当前折线的末顶点
	auto prev{ p0 };
	auto lastAlong{ p0 };         // 上一步"还在沿轴走"的位置 —— 拐点就落在它身上
	int axis{ 0 };                // 这一段往哪走：0 未定 / 1 横 / 2 竖
	float warmUp{ 0.f };          // 还没定轴时攒的位移
	float turnRun{ 0.f };         // 垂直方向连续走了多远
	for (size_t i = 1; i < trail.size(); ++i) {
		auto p{ trail[i] };
		auto dx{ p.x - prev.x }, dy{ p.y - prev.y };
		prev = p;
		if (axis == 0) {
			warmUp += std::abs(dx) + std::abs(dy);
			if (warmUp < kMinStep) continue;
			// 定轴：从当前顶点看出去，哪一维走得远就算哪一维
			axis = std::abs(p.x - v.x) >= std::abs(p.y - v.y) ? 1 : 2;
			turnRun = 0.f;
			lastAlong = p;
			continue;
		}
		auto dAlong = axis == 1 ? std::abs(dx) : std::abs(dy);
		auto dPerp = axis == 1 ? std::abs(dy) : std::abs(dx);
		if (dPerp <= dAlong) {
			lastAlong = p;
			turnRun = 0.f;
			continue;
		}
		turnRun += dPerp;
		if (turnRun < kTurnStep) continue;
		// 拐点落在上一段那条轴上（沿轴走到拐弯前最后一步的位置），另一维从这里起算
		auto corner = axis == 1 ? D2D1::Point2F(lastAlong.x, v.y) : D2D1::Point2F(v.x, lastAlong.y);
		if (std::abs(corner.x - v.x) + std::abs(corner.y - v.y) > kMinStep) {
			linePoints.push_back(corner);
			v = corner;
		}
		axis = 0;
		warmUp = 0.f;
		turnRun = 0.f;
		lastAlong = p;
	}
	// 收尾：从最后一个顶点沿它那条轴走到鼠标所在的轴线上。末点因此不一定落在光标正下方 ——
	// 吸附成横平竖直之后，端点只能在这条线自己的轴线上
	auto p{ trail.back() };
	if (axis == 1) linePoints.push_back({ p.x, v.y });
	else if (axis == 2) linePoints.push_back({ v.x, p.y });
	else if (linePoints.size() < 2) linePoints.push_back(p);
}

void ShapeLine::paint(ID2D1DeviceContext* ctx)
{
	// makePath 要等第一次 mouseDown 才建 path，这之前可能先来一次 paint
	if (!path) return;
	if (lineStyle == Style::Wave) {
		auto wave = makeWaveGeometry();
		if (wave) {
			// 波浪本身就是一条描出来的曲线，不再叠虚线样式；两端装饰照旧
			ctx->DrawGeometry(wave.Get(), brush.Get(), strokeWidth);
			paintEnds(ctx);
			return;
		}
	}
	ctx->DrawGeometry(path.Get(), brush.Get(), strokeWidth, strokeStyle.Get());
	paintEnds(ctx);
}

// 波浪线：D2D 的虚线样式里没有波浪，只能把折线重采样成正弦路径。
// 沿折线累计弧长采样（所以拐点两侧的相位连得上，不会在拐角处跳一下），
// 振幅与波长都按线宽走 —— 线越粗波越大，不然细线上一坨、粗线上看不出波动
ComPtr<ID2D1PathGeometry> ShapeLine::makeWaveGeometry() const
{
	if (linePoints.size() < 2) return nullptr;
	auto w = std::max(strokeWidth, 1.f);
	auto amp{ w * 1.2f };
	auto wave{ std::max(8.f, w * 5.f) };
	constexpr float step{ 2.f };
	auto total{ 0.f };
	for (size_t i = 1; i < linePoints.size(); ++i) total += distance(linePoints[i - 1], linePoints[i]);
	if (total < 1.f) return nullptr;

	ComPtr<ID2D1PathGeometry> geo;
	if (FAILED(Ling::D2D::get()->d2dFactory->CreatePathGeometry(geo.GetAddressOf()))) return nullptr;
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(geo->Open(sink.GetAddressOf()))) return nullptr;
	size_t seg{ 1 };
	float segStart{ 0.f }, segLen{ distance(linePoints[0], linePoints[1]) };
	float s{ 0.f };
	bool started{ false };
	while (true) {
		while (seg + 1 < linePoints.size() && s > segStart + segLen) {
			segStart += segLen;
			++seg;
			segLen = distance(linePoints[seg - 1], linePoints[seg]);
		}
		auto t = segLen > 0.01f ? std::clamp((s - segStart) / segLen, 0.f, 1.f) : 0.f;
		auto a{ linePoints[seg - 1] }, b{ linePoints[seg] };
		auto dir = unitDir(a, b);
		auto off = amp * std::sin(s * 2.f * kPi / wave);
		D2D1_POINT_2F p{ a.x + (b.x - a.x) * t - dir.y * off, a.y + (b.y - a.y) * t + dir.x * off };
		if (!started) {
			sink->BeginFigure(p, D2D1_FIGURE_BEGIN_HOLLOW);
			started = true;
		}
		else sink->AddLine(p);
		if (s >= total) break;
		s = std::min(total, s + step);
	}
	sink->EndFigure(D2D1_FIGURE_END_OPEN);
	sink->Close();
	return geo;
}

void ShapeLine::paintEnds(ID2D1DeviceContext* ctx)
{
	if (linePoints.size() < 2) return;
	auto pair = ends();
	paintEnd(ctx, linePoints.front(), endDir(false), pair.start);
	paintEnd(ctx, linePoints.back(), endDir(true), pair.end);
}

// 端点标记朝哪：从端点沿折线往回让够"箭头自己那么长"的一段（4×线宽），拿那两个点的连线当走向。
//
// 为什么不直接用最后两个顶点 —— 自由画的末端常常是一小撮原地抖动：收笔时的手一抖、或者抓着
// 端点夹点往外拖出来的那一截，方向跟整条线能差出几十度。照着它画，箭头就横在线上（作者报的
// "箭头会弯折"）。这一小撮本来就被箭头自己盖着，按箭头长度往回让一段正好跳过它。
// 直线段上让与不让结果一样；直角折线的端点只要那一条腿长过这个基准，就仍落在同一条腿上
D2D1_POINT_2F ShapeLine::endDir(const bool atEnd) const
{
	auto base{ 4.f * std::max(strokeWidth, 1.f) };
	auto n{ (int)linePoints.size() };
	auto i{ atEnd ? n - 1 : 0 };
	auto step{ atEnd ? -1 : 1 };
	auto tip{ linePoints[i] };
	// 叫 away 而不是 far：windef.h 把 far / near / huge 定义成了空宏，叫 far 会被预处理器吃掉
	auto away{ tip };
	auto acc{ 0.f };
	while (acc < base) {
		auto next{ i + step };
		if (next < 0 || next >= n) break;
		acc += distance(linePoints[i], linePoints[next]);
		i = next;
		away = linePoints[i];
	}
	return unitDir(away, tip);   // away → tip 就是朝外的走向
}

void ShapeLine::paintEnd(ID2D1DeviceContext* ctx, const D2D1_POINT_2F& tip,
	const D2D1_POINT_2F& dir, const EndMark mark)
{
	if (mark == EndMark::None) return;
	auto w = std::max(strokeWidth, 1.f);
	if (mark == EndMark::Dot) {
		// 圆点得比线粗一圈才看得出是个"端点"，不然跟线头融成一坨
		auto r{ w * 1.4f };
		ctx->FillEllipse(D2D1::Ellipse(tip, r, r), brush.Get());
		return;
	}
	// 箭头沿走向的长度 / 根部半宽：实心那档短而宽，细的那档长而窄，两档一眼能分开
	auto len = (mark == EndMark::Arrow ? 4.f : 5.f) * w;
	auto half = (mark == EndMark::Arrow ? 1.7f : 1.f) * w;
	D2D1_POINT_2F base{ tip.x - dir.x * len, tip.y - dir.y * len };
	// 走向的法线（把走向转 90°）
	D2D1_POINT_2F a{ base.x - dir.y * half, base.y + dir.x * half };
	D2D1_POINT_2F b{ base.x + dir.y * half, base.y - dir.x * half };
	if (mark == EndMark::Arrow) {
		ComPtr<ID2D1PathGeometry> geo;
		if (FAILED(Ling::D2D::get()->d2dFactory->CreatePathGeometry(geo.GetAddressOf()))) return;
		ComPtr<ID2D1GeometrySink> sink;
		if (FAILED(geo->Open(sink.GetAddressOf()))) return;
		sink->BeginFigure(tip, D2D1_FIGURE_BEGIN_FILLED);
		sink->AddLine(a);
		sink->AddLine(b);
		sink->EndFigure(D2D1_FIGURE_END_CLOSED);
		sink->Close();
		ctx->FillGeometry(geo.Get(), brush.Get());
	}
	else {
		// 细箭头 = 开口的 V，两笔描边
		ctx->DrawLine(tip, a, brush.Get(), w);
		ctx->DrawLine(tip, b, brush.Get(), w);
	}
}

// 滚滚轮 = 调线宽，与矩形 / 箭头那边是同一回事（Canvas 只在光标停在图形身上时才把滚轮转过来）。
// 一格一个逻辑像素，上下限交给 ToolSub 那张滑块值域表夹 —— 线宽与工具条滑块因此永远是同一个数
void ShapeLine::mouseWheel(const float x, const float y, const short delta)
{
	auto next = strokeWidth + (delta < 0 ? -win->getDpi() : win->getDpi());
	auto applied = win->getToolSub()->setShapeSliderVal(L"line", next);
	if (applied == strokeWidth) return;   //已经顶到值域的头了，不用重画
	strokeWidth = applied;
	win->refresh();
}
