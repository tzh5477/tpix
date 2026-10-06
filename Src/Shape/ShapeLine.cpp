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

	// 两端形状的十档组合。顺序 = config.json 里 line/end 的落盘值，也是「端点」下拉里的顺序：
	// 无 / 末端实心箭头 / 起点圆点+末端实心箭头 / 末端细箭头 / 起点圆点+末端细箭头 /
	// 末端圆点 / 起点圆点 / 两端圆点 / 两端实心箭头 / 两端细箭头 ——
	// 由三种标记（实心箭头 / 细箭头 / 圆点）的两两组合加"一头有"叠出来。
	// ShapeLine::ends() 与下拉预览的 paintEndSample 都读它这一份
	using EndPair = std::pair<ShapeLine::EndMark, ShapeLine::EndMark>;
	const EndPair kEndTable[]{
		{ ShapeLine::EndMark::None,  ShapeLine::EndMark::None  },
		{ ShapeLine::EndMark::None,  ShapeLine::EndMark::Arrow },
		{ ShapeLine::EndMark::Dot,   ShapeLine::EndMark::Arrow },
		{ ShapeLine::EndMark::None,  ShapeLine::EndMark::Thin  },
		{ ShapeLine::EndMark::Dot,   ShapeLine::EndMark::Thin  },
		{ ShapeLine::EndMark::None,  ShapeLine::EndMark::Dot   },
		{ ShapeLine::EndMark::Dot,   ShapeLine::EndMark::None  },
		{ ShapeLine::EndMark::Dot,   ShapeLine::EndMark::Dot   },
		{ ShapeLine::EndMark::Arrow, ShapeLine::EndMark::Arrow },
		{ ShapeLine::EndMark::Thin,  ShapeLine::EndMark::Thin  },
	};

	// 线条样式 -> D2D 的虚线样式。波浪不在里面：D2D 没有波浪，它是另画的一条正弦路径
	D2D1_DASH_STYLE dashOf(const ShapeLine::Style style)
	{
		switch (style) {
		case ShapeLine::Style::Dash: return D2D1_DASH_STYLE_DASH;
		case ShapeLine::Style::Dot: return D2D1_DASH_STYLE_DOT;
		case ShapeLine::Style::DashDot: return D2D1_DASH_STYLE_DASH_DOT;
		case ShapeLine::Style::DashDotDot: return D2D1_DASH_STYLE_DASH_DOT_DOT;
		default: return D2D1_DASH_STYLE_SOLID;   // 实线与波浪都按实线描
		}
	}

	// 波浪的两个比例：振幅与波长都按线宽走 —— 线越粗波越大，不然细线上一坨、粗线上看不出波动。
	// makeWaveGeometry 与下拉预览共用
	float waveAmp(const float w) { return w * 1.2f; }
	float waveLen(const float w) { return std::max(8.f, w * 5.f); }

	// 带标记的那一头，线要沿走向往里让出这么多（倍线宽）。理由见 ShapeLine::shaftPoints
	constexpr float kMarkInset{ 2.f };

	// 把折线的一头沿走向往里缩 len：按弧长量，不够的顶点整段丢掉，落点落在某一段中间就插一个点。
	// atEnd 为 true 缩尾、false 缩首。整条线还没 len 长就原样留着 —— 那种线本来也没地方放标记
	void trimTail(std::vector<D2D1_POINT_2F>& pts, const bool atEnd, const float len)
	{
		if (pts.size() < 2) return;
		auto total{ 0.f };
		for (size_t i = 1; i < pts.size(); ++i) total += distance(pts[i - 1], pts[i]);
		if (total <= len + 1.f) return;
		auto rest{ len };
		while (pts.size() > 2) {
			auto a{ atEnd ? pts[pts.size() - 2] : pts[0] };
			auto b{ atEnd ? pts.back() : pts[1] };
			auto segLen{ distance(a, b) };
			if (segLen > rest) break;
			rest -= segLen;
			if (atEnd) pts.pop_back();
			else pts.erase(pts.begin());
		}
		auto a{ atEnd ? pts[pts.size() - 2] : pts[0] };
		auto b{ atEnd ? pts.back() : pts[1] };
		auto segLen{ distance(a, b) };
		if (segLen < 0.01f) return;
		auto t{ rest / segLen };
		// 落点要从"被削掉的那一头"朝另一头走 t 段。注意削尾与削首时 a、b 的里外身份是反的：
		// 削尾时 a 是内侧点、b 是外侧点（pts.back()），削首时外侧点反而是 a（pts[0]）。
		// 两边共用 b + (a-b)·t 的话，削首那支会把首点搬到**尾点**旁边，整条线只剩末尾一小截 ——
		// 直角折线的"直"分支只留两个顶点（见 snapTrail），一进 while 就是两个点，
		// 于是首尾各带一个标记的线整根消失（作者报的"2 个双向箭头没有绘制连线"）
		D2D1_POINT_2F p{ atEnd
			? D2D1_POINT_2F{ b.x + (a.x - b.x) * t, b.y + (a.y - b.y) * t }
			: D2D1_POINT_2F{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t } };
		if (atEnd) pts.back() = p;
		else pts[0] = p;
	}

	// 描边样式（实线 / 虚线族）。整进程只建一次并故意不释放，同 ShapeArrow 里的 miterStyle。
	// 端帽与图上那条线同一套（圆头），所以画出来的点 / 划两头都是圆的
	ID2D1StrokeStyle* dashStroke(const D2D1_DASH_STYLE dash)
	{
		static ID2D1StrokeStyle* cache[5]{};
		auto i{ (int)dash };
		if (i < 0 || i > 4) i = 0;
		if (!cache[i]) {
			Ling::D2D::get()->d2dFactory->CreateStrokeStyle(
				D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
					D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 8.f, dash, 0.f),
				nullptr, 0, &cache[i]);
		}
		return cache[i];
	}

	// 把一串折线重采样成一条正弦路径 —— 图上那条波浪线与下拉里的预览共用它。
	// 沿折线累计弧长采样，所以拐点两侧的相位连得上，不会在拐角处跳一下
	ComPtr<ID2D1PathGeometry> wavePath(const std::vector<D2D1_POINT_2F>& pts, const float w)
	{
		if (pts.size() < 2) return nullptr;
		auto amp{ waveAmp(w) };
		auto wave{ waveLen(w) };
		constexpr float step{ 2.f };
		auto total{ 0.f };
		for (size_t i = 1; i < pts.size(); ++i) total += distance(pts[i - 1], pts[i]);
		if (total < 1.f) return nullptr;

		ComPtr<ID2D1PathGeometry> geo;
		if (FAILED(Ling::D2D::get()->d2dFactory->CreatePathGeometry(geo.GetAddressOf()))) return nullptr;
		ComPtr<ID2D1GeometrySink> sink;
		if (FAILED(geo->Open(sink.GetAddressOf()))) return nullptr;
		size_t seg{ 1 };
		float segStart{ 0.f }, segLen{ distance(pts[0], pts[1]) };
		float s{ 0.f };
		bool started{ false };
		while (true) {
			while (seg + 1 < pts.size() && s > segStart + segLen) {
				segStart += segLen;
				++seg;
				segLen = distance(pts[seg - 1], pts[seg]);
			}
			auto t = segLen > 0.01f ? std::clamp((s - segStart) / segLen, 0.f, 1.f) : 0.f;
			auto a{ pts[seg - 1] }, b{ pts[seg] };
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

	// ---- 下拉里那一格的骨架。两档共用：线一律铺满整格、每个枚举值一样长 ----
	// 原来那几行符号（"———" / "—▶" / "➔"）各是各的字宽，于是"直线"那一行很长、
	// 带箭头的那几行很短（作者报的"各线条长短不一"），而符号本身也看不出箭头长什么样
	// 样例线的粗细：调用方给的粗了、或格子窄了（工具条上那三个按钮只有列表里那格的三分之一宽）
	// 就细下来。标记长度是 9.7w，不夹一下的话窄格子里箭头会把整条线吃掉
	float sampleW(const D2D1_RECT_F& rect, const float strokeW)
	{
		return std::max(1.f, std::min(strokeW, (rect.right - rect.left) / 32.f));
	}
	// 样例线的走向（水平、居中）与两端。pad 按线宽走：圆点半径是 1.85w，线两端得让它落得下
	struct SampleSpan { float left, right, y; };
	SampleSpan sampleSpan(const D2D1_RECT_F& rect, const float w)
	{
		auto pad{ 2.5f * w };
		return { rect.left + pad, std::max(rect.left + pad, rect.right - pad), (rect.top + rect.bottom) / 2.f };
	}

	// 一端的标记。图上那条线与下拉里的预览共用它。比例照 FSCapture 的端点预览逐像素量出来
	// （拿它的放大图量，再除上当时的线宽 w）：沿走向长 9.7w；实心那档是平底三角、半宽 3.5w，
	// 细的那档是燕尾、半宽 3w、后缘凹口深 1.7w。两档尺寸几乎一样，区别就在这条底边上
	void paintMark(ID2D1DeviceContext* ctx, const D2D1_POINT_2F& tip, const D2D1_POINT_2F& dir,
		const ShapeLine::EndMark mark, const float w, ID2D1Brush* brush)
	{
		if (mark == ShapeLine::EndMark::None) return;
		if (mark == ShapeLine::EndMark::Dot) {
			// 圆点得比线粗一圈才看得出是个"端点"，不然跟线头融成一坨
			const auto r{ w * 1.85f };
			ctx->FillEllipse(D2D1::Ellipse(tip, r, r), brush);
			return;
		}
		const auto len{ 9.7f * w };
		const auto half{ (mark == ShapeLine::EndMark::Arrow ? 3.5f : 3.f) * w };
		const D2D1_POINT_2F base{ tip.x - dir.x * len, tip.y - dir.y * len };
		// 走向的法线（把走向转 90°）
		const D2D1_POINT_2F a{ base.x - dir.y * half, base.y + dir.x * half };
		const D2D1_POINT_2F b{ base.x + dir.y * half, base.y - dir.x * half };
		ComPtr<ID2D1PathGeometry> geo;
		if (FAILED(Ling::D2D::get()->d2dFactory->CreatePathGeometry(geo.GetAddressOf()))) return;
		ComPtr<ID2D1GeometrySink> sink;
		if (FAILED(geo->Open(sink.GetAddressOf()))) return;
		sink->BeginFigure(tip, D2D1_FIGURE_BEGIN_FILLED);
		sink->AddLine(a);
		if (mark == ShapeLine::EndMark::Thin) {
			// 燕尾：后缘的中点朝尖端让出凹口深，收出一个缺口
			const auto notch{ 1.7f * w };
			sink->AddLine(D2D1_POINT_2F{ base.x + dir.x * notch, base.y + dir.y * notch });
		}
		sink->AddLine(b);
		sink->EndFigure(D2D1_FIGURE_END_CLOSED);
		sink->Close();
		ctx->FillGeometry(geo.Get(), brush);
	}
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
	// 外观（颜色 / 线宽）走 applyStyle，档位（线型 / 端点 / 线条类型）走 applyToolStyle ——
	// 两处各写一遍迟早对不上。顺序有讲究：档位那一步会按档位重建 path
	//（有标记的一头要往里缩，见 shaftPoints），得排在最后
	applyStyle();
	applyToolStyle();
}

ShapeLine::~ShapeLine()
{

}

std::unique_ptr<ShapeBase> ShapeLine::clone(const float dx, const float dy) const
{
	return cloneSelf(*this, dx, dy);
}

// 外观：颜色、半透明、线宽。滚轮调粗细也走这里，所以它不能顺手改档位
//（档位在 applyToolStyle 里，理由见 ShapeBase::applyToolStyle）
void ShapeLine::applyStyle()
{
	auto toolSub = win->getToolSub();
	auto color = toolSub->getSelectedColor();
	if (toolSub->isLineTransparent) {
		color.a = 0.5f;
	}
	brush->SetColor(color);
	strokeWidth = toolSub->getSliderVal();
	// 缩进的基准是线宽（见 shaftPoints），线粗了收进去的那一截也要跟着长
	makePath();
}

// 档位：线条类型、线条样式、两端形状。只有用户真去动那三个下拉时才套过来
void ShapeLine::applyToolStyle()
{
	auto toolSub = win->getToolSub();
	auto wasOrtho{ isOrtho() };
	kind = (Kind)toolSub->lineKind;
	lineStyle = (Style)toolSub->lineStyle;
	endIndex = toolSub->lineEnd;
	makeStrokeStyle();
	// 类型从"普通线条"切到"直角折线"：拿已画的那串顶点重新吸附一遍。反向不做 ——
	// 原始的自由轨迹已经丢了，还原不回去，只能保持现状。作者要的"改完同步到选中的那一笔"
	// 只有这一半有结果，另一半（切回普通线条）本来就是"没有可改的东西"
	// path 也要重排：它现在跟着端点档位与线宽走（有标记的那一头缩进去一截，见 shaftPoints），
	// 换了档位不重建的话缩进还是上一档的
	if (isOrtho() && !wasOrtho) snapExisting();
	else makePath();
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
	// 表在匿名 namespace 里那一个 kEndTable：下拉里的预览也读它，两处各列一份的话
	// 迟早会不一样（顺序就是 config.json 里 line/end 的落盘值，动了它旧配置就串味）
	if (endIndex < 0 || endIndex >= (int)std::size(kEndTable)) return { EndMark::None, EndMark::None };
	return kEndTable[endIndex];
}

void ShapeLine::makeStrokeStyle()
{
	Ling::D2D::get()->d2dFactory->CreateStrokeStyle(
		D2D1::StrokeStyleProperties(
			D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
			D2D1_LINE_JOIN_ROUND, 8.f, dashOf(lineStyle), 0.f),
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

// 画出来的那条线用哪串点：两端有标记的那一头往回收 kMarkInset 倍线宽。
//
// 为什么收：线是用**圆头端帽**描的，而端帽是"以端点为圆心、半径 = 线宽一半"的半个圆。
// 端点又正好是箭头的顶点，于是那半个圆从尖上鼓出来。细箭头在顶点附近只有 34°，
// 两侧比端帽窄得多，完全盖不住它 —— 看着就是"箭尖上顶着一个圆点"（作者报的
// "去掉箭头顶点的圆点，我要尖尖头，不要圆头"）。线缩进 2 倍线宽之后端帽整个落进箭头里面，
// 顶点就只剩箭头自己那个尖角。linePoints 本身不能动：端点朝向、夹点、标记落点都按它算
std::vector<D2D1_POINT_2F> ShapeLine::shaftPoints() const
{
	auto pts{ linePoints };
	auto pair = ends();
	auto inset{ kMarkInset * std::max(strokeWidth, 1.f) };
	if (pair.second != EndMark::None) trimTail(pts, true, inset);
	if (pair.first != EndMark::None) trimTail(pts, false, inset);
	return pts;
}

void ShapeLine::makePath()
{
	buildPath(shaftPoints());
}

// 折线族默认把笔画正中挖掉半个夹点宽（理由见 ShapeLineBase::hitTest：粗笔画整片能拖的话，
// 就没法在已有笔画上面再画一笔）。线条不挖 —— 它的线宽上限是 60 逻辑像素，挖掉之后
// 只有紧贴边缘的一圈点得中，杆上点哪儿都没反应（作者报的"点箭头线条无法选中，
// 只能点两头的夹点"）。线条本来也没有"在笔画内部起笔"这种用法
float ShapeLine::hitInnerLimit(const float outer) const
{
	return 0.f;
}

// 端点那几档标记在端点两侧支出去多少：实心箭头半宽 3.5 倍线宽，圆点半径 1.85 倍
// （比例见 paintMark 那一组常数）—— 取大的那个。标记都画在 linePoints 的端点上，
// 不往两端外伸，所以往外让 3.5 倍就够把整支箭圈进外接框
float ShapeLine::boundsPad() const
{
	return 3.5f * std::max(strokeWidth, 1.f);
}

// 波浪线：D2D 的虚线样式里没有波浪，只能把折线重采样成正弦路径 ——
// 几何在匿名 namespace 的 wavePath 里，下拉里那条波浪与它共用一份
ComPtr<ID2D1PathGeometry> ShapeLine::makeWaveGeometry() const
{
	return wavePath(shaftPoints(), std::max(strokeWidth, 1.f));
}

void ShapeLine::paintEnds(ID2D1DeviceContext* ctx)
{
	if (linePoints.size() < 2) return;
	auto pair = ends();
	paintEnd(ctx, linePoints.front(), endDir(false), pair.first);
	paintEnd(ctx, linePoints.back(), endDir(true), pair.second);
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

// 一端的标记就是 paintMark 那一个函数 —— 与「端点」下拉里画的是同一份几何
void ShapeLine::paintEnd(ID2D1DeviceContext* ctx, const D2D1_POINT_2F& tip,
	const D2D1_POINT_2F& dir, const EndMark mark)
{
	paintMark(ctx, tip, dir, mark, std::max(strokeWidth, 1.f), brush.Get());
}

void ShapeLine::paintEndSample(ID2D1DeviceContext* ctx, const D2D1_RECT_F& rect,
	const int endIndex, const float strokeW, ID2D1Brush* brush)
{
	const auto w{ sampleW(rect, strokeW) };
	const auto span{ sampleSpan(rect, w) };
	auto pair = endIndex >= 0 && endIndex < (int)std::size(kEndTable)
		? kEndTable[endIndex] : EndPair{ EndMark::None, EndMark::None };
	ctx->DrawLine(D2D1::Point2F(span.left, span.y), D2D1::Point2F(span.right, span.y),
		brush, w, dashStroke(D2D1_DASH_STYLE_SOLID));
	// 走向朝外：起点那头的标记指向左、末端指向右（与图上的 endDir 一致）
	paintMark(ctx, { span.left, span.y }, { -1.f, 0.f }, pair.first, w, brush);
	paintMark(ctx, { span.right, span.y }, { 1.f, 0.f }, pair.second, w, brush);
}

void ShapeLine::paintStyleSample(ID2D1DeviceContext* ctx, const D2D1_RECT_F& rect,
	const int styleIndex, const float strokeW, ID2D1Brush* brush)
{
	const auto w{ sampleW(rect, strokeW) };
	const auto span{ sampleSpan(rect, w) };
	const auto style = (Style)(styleIndex >= 0 && styleIndex <= (int)Style::DashDotDot ? styleIndex : 0);
	if (style == Style::Wave) {
		// 波浪不是描出来的：它是按弧长重采样出来的一条正弦路径，与图上那条走同一个函数
		std::vector<D2D1_POINT_2F> pts{ { span.left, span.y }, { span.right, span.y } };
		auto geo = wavePath(pts, w);
		if (geo) ctx->DrawGeometry(geo.Get(), brush, w);
		return;
	}
	ctx->DrawLine(D2D1::Point2F(span.left, span.y), D2D1::Point2F(span.right, span.y),
		brush, w, dashStroke(dashOf(style)));
}


// 滚滚轮 = 调线宽，与矩形 / 箭头那边是同一回事（Canvas 只在光标停在图形身上时才把滚轮转过来）。
// 一格一个逻辑像素，上下限交给 ToolSub 那张滑块值域表夹 —— 线宽与工具条滑块因此永远是同一个数
void ShapeLine::mouseWheel(const float x, const float y, const short delta)
{
	auto next = strokeWidth + (delta < 0 ? -win->getDpi() : win->getDpi());
	auto applied = win->getToolSub()->setShapeSliderVal(L"line", next);
	if (applied == strokeWidth) return;   //已经顶到值域的头了，不用重画
	strokeWidth = applied;
	// 缩进的基准是线宽（见 shaftPoints），线粗了收进去的那一截也要跟着长
	makePath();
	win->refresh();
}
