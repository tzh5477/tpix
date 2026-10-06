#include "pch.h"
#include <cmath>
#include "Canvas.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeArrow.h"

using Microsoft::WRL::ComPtr;

namespace {
	// 细箭头（arrowStyle 2）那一档的箭杆是一条直画到箭尖的线，头部是开口 V。
	// 开口 V 的顶点必须尖：拿两笔 DrawLine 拼的话，各自的平头端帽会在箭尖处糊出一个平口，
	// 看着是钝的 —— 改成一条折线 + 斜接连接（miter），由 D2D 把顶点接成尖角
	ID2D1StrokeStyle* miterStyle()
	{
		// 整进程只建一次并故意不释放：这一份到进程退出前一直要用，而释放点落在 D2D 设备
		// 销毁之后是自找麻烦（设备没了，样式对象还挂着它的引用）
		static ID2D1StrokeStyle* style{ nullptr };
		if (!style) {
			Ling::D2D::get()->d2dFactory->CreateStrokeStyle(
				D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
					D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10.f,
					D2D1_DASH_STYLE_SOLID, 0.f),
				nullptr, 0, &style);
		}
		return style;
	}

	// 往 sink 里加一条"有厚度的线段"（一个矩形）。细箭头的命中几何拿它拼出来：
	// 杆一段、两翼各一段。调用方把填充模式设成 WINDING，
	// 几段叠着才不会按"重叠即挖掉"互相打洞（ALTERNATE 默认就是这么干的）
	void addThickLine(ID2D1GeometrySink* sink, const D2D1_POINT_2F& a, const D2D1_POINT_2F& b, const float half)
	{
		const auto dx{ b.x - a.x }, dy{ b.y - a.y };
		const auto len{ std::sqrt(dx * dx + dy * dy) };
		if (len < 0.01f) return;
		const D2D1_POINT_2F n{ -dy / len * half, dx / len * half };
		sink->BeginFigure(D2D1::Point2F(a.x + n.x, a.y + n.y), D2D1_FIGURE_BEGIN_FILLED);
		sink->AddLine(D2D1::Point2F(b.x + n.x, b.y + n.y));
		sink->AddLine(D2D1::Point2F(b.x - n.x, b.y - n.y));
		sink->AddLine(D2D1::Point2F(a.x - n.x, a.y - n.y));
		sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	}

	// 开口 V 的尺寸：沿走向长 9.7w、半展 3w（顶角约 34°）—— 与线条端点那一档（ShapeLine 的
	// paintMark，照 FSCapture 端点预览逐像素量出来的 9.7w / 3w）完全对齐，箭头组件与端点看着才是
	// 同一套箭头。原来这里是 4.5w / 1.8w，头只有端点的一半长，薄杆时 V 小得看不出来。
	// back 是折线拐点相对箭尖的回收量：斜接长度 = 半线宽 / sin(半顶角)，
	// 补上这一节，描出来的**外**角才正好落在箭尖上（内角落在 2×back 处，那就是开口有多深）
	struct HeadGeom { float len, half, back; };
	HeadGeom headGeom(const float w)
	{
		const auto len{ 9.7f * w }, half{ 3.f * w };
		const auto sinHalf{ half / std::sqrt(len * len + half * half) };
		return { len, half, w / (2.f * sinHalf) };
	}

	// 细箭头（2）的头部：箭杆停在 V 的内顶点（调用方画），这里补那个开口 V ——
	// ShapeArrow::paintLineStyle 与下拉里的样例共用它
	void strokeHead(ID2D1DeviceContext* ctx, const D2D1_POINT_2F& tip, const D2D1_POINT_2F& dir,
		const float w, ID2D1Brush* brush)
	{
		// 开口 V：折线从一翼画到另一翼、中间经过回收过的拐点，再按杆厚描出来。
		// 顶点那一圈交给斜接（miter），两翼末端留平头（就是被切齐的那一刀）
		const auto g{ headGeom(w) };
		const D2D1_POINT_2F n{ -dir.y, dir.x };
		const D2D1_POINT_2F a{ tip.x - dir.x * g.len + n.x * g.half, tip.y - dir.y * g.len + n.y * g.half };
		const D2D1_POINT_2F b{ tip.x - dir.x * g.len - n.x * g.half, tip.y - dir.y * g.len - n.y * g.half };
		ComPtr<ID2D1PathGeometry> geo;
		if (FAILED(Ling::D2D::get()->d2dFactory->CreatePathGeometry(geo.GetAddressOf()))) return;
		ComPtr<ID2D1GeometrySink> sink;
		if (FAILED(geo->Open(sink.GetAddressOf()))) return;
		sink->BeginFigure(a, D2D1_FIGURE_BEGIN_HOLLOW);
		sink->AddLine(D2D1::Point2F(tip.x - dir.x * g.back, tip.y - dir.y * g.back));
		sink->AddLine(b);
		sink->EndFigure(D2D1_FIGURE_END_OPEN);
		sink->Close();
		ctx->DrawGeometry(geo.Get(), brush, w, miterStyle());
	}

	// 箭头的轮廓。两种用法：0/1/3 三档直接填它，2 那一档拿它做命中判定（"有厚度的带子"那几段）。
	// 画在图上与下拉里的样例共用这一份顶点（见 ShapeArrow::paintSample）
	void buildOutline(ID2D1GeometrySink* sink, const D2D1_POINT_2F& tail, const D2D1_POINT_2F& tip,
		const float size, const int style)
	{
		float dx = tip.x - tail.x;
		float dy = tip.y - tail.y;
		float length = sqrtf(dx * dx + dy * dy);
		if (length < 1.f)
		{
			sink->BeginFigure(tail, D2D1_FIGURE_BEGIN_FILLED);
			sink->EndFigure(D2D1_FIGURE_END_CLOSED);
			return;
		}
		float ux = dx / length;
		float uy = dy / length;
		float vx = -uy;
		float vy = ux;
		float v1 = size / 4.0f;        // 箭杆半宽
		float v2 = size * 2.0f / 3.0f; // 箭头半宽
		if (style == 0) {
			// 普通箭头：平口尾、箭杆从头到尾一样粗，头上再接一个三角（pixpin 默认那种）。
			// 与尖尾那版的差别只在尾部 —— 那边是从 tail 一个尖点展开，箭杆是楔形的
			float baseX = tip.x - (size + v1) * ux;
			float baseY = tip.y - (size + v1) * uy;
			sink->BeginFigure({ tail.x + v1 * vx, tail.y + v1 * vy }, D2D1_FIGURE_BEGIN_FILLED);
			sink->AddLine({ baseX + v1 * vx, baseY + v1 * vy });
			sink->AddLine({ baseX + v2 * vx, baseY + v2 * vy });
			sink->AddLine(tip);
			sink->AddLine({ baseX - v2 * vx, baseY - v2 * vy });
			sink->AddLine({ baseX - v1 * vx, baseY - v1 * vy });
			sink->AddLine({ tail.x - v1 * vx, tail.y - v1 * vy });
		}
		else if (style == 1) {
			// 尖尾箭头：尾部收成一个点，箭杆由细到粗
			sink->BeginFigure(tail, D2D1_FIGURE_BEGIN_FILLED);
			sink->AddLine({ tip.x - size * ux - v1 * vx, tip.y - size * uy - v1 * vy });
			sink->AddLine({ tip.x - (size + v1) * ux - v2 * vx, tip.y - (size + v1) * uy - v2 * vy });
			sink->AddLine(tip);
			sink->AddLine({ tip.x - (size + v1) * ux + v2 * vx, tip.y - (size + v1) * uy + v2 * vy });
			sink->AddLine({ tip.x - size * ux + v1 * vx, tip.y - size * uy + v1 * vy });
		}
	else if (style == 2) {
		// 细箭头画出来的样子见 strokeHead（描边），这份几何只做命中判定：
		// 换成"杆一段 + 两翼各一段"那些有厚度的带子。
		// WINDING 填充：几段叠在一起才不会互相挖洞
		const auto w{ size * 0.25f };
		sink->SetFillMode(D2D1_FILL_MODE_WINDING);
		const auto g{ headGeom(w) };
		const D2D1_POINT_2F vertex{ tip.x - ux * g.back, tip.y - uy * g.back };
		addThickLine(sink, vertex,
			D2D1::Point2F(tip.x - ux * g.len - uy * g.half, tip.y - uy * g.len + ux * g.half), w * 0.5f);
		addThickLine(sink, vertex,
			D2D1::Point2F(tip.x - ux * g.len + uy * g.half, tip.y - uy * g.len - ux * g.half), w * 0.5f);
		addThickLine(sink, tail, tip, w * 0.5f);
		// 必须从这里返回：三条带子各自 Begin / End 过了，再落到函数末尾那个共用的 EndFigure 上，
		// 就是"没有打开的图形却收尾"，sink 当场变成非法状态，Close 之后整个几何是空的 ——
		// GetBounds 回 (0,0,0,0)、FillContainsPoint 恒 false。
		// 表现是这一档只有两头的夹点能选中，杆上、头上点哪儿都没反应
		//（作者报的"点箭头线条无法选中，只能点头部或尾部"）。
		// 这一档不经过 path 画面（见 paintLineStyle），所以一直没别的症状暴露它
		return;
	}
	else {
		// 凹口实心：两翼向后掠，后缘的中段朝箭尖凹进去一刀（棱角分明）。箭杆只走到
		// "凹口点"，翅膀从那里斜着往外后方展开到翼尖 —— 凹口就是这两段之间那块缺口
		const auto head{ size };              // 翼尖离箭尖多远
		// 凹口点离箭尖多远。这个值不能小：两翼的面积 = ½·|head·v1 − notch·v2|，
		// 而"凹口点朝箭尖那条连线"的斜率是 v1/notch、"翅膀外缘"的斜率是 v2/head ——
		// notch 取 0.35·size 时两者正好都约 0.7，两条边几乎平行，两翼退化成零面积的小片
		// （默认线宽下每翼约 1 平方像素），整支箭画出来就是一根线加一个几乎看不见的尖，
		// 填充状态更是什么都看不出（作者报的"第 4 个箭头太细，像是直线"）。
		// 0.8 与线条端点那档燕尾同一路数（后缘只朝箭尖凹进去一小截），两翼这才立得住
		const auto notch{ size * 0.8f };
			const float hx{ tip.x - ux * notch }, hy{ tip.y - uy * notch };
			const float bx{ tip.x - ux * head }, by{ tip.y - uy * head };
			sink->BeginFigure({ tail.x + v1 * vx, tail.y + v1 * vy }, D2D1_FIGURE_BEGIN_FILLED);
			sink->AddLine({ hx + v1 * vx, hy + v1 * vy });
			sink->AddLine({ bx + v2 * vx, by + v2 * vy });
			sink->AddLine(tip);
			sink->AddLine({ bx - v2 * vx, by - v2 * vy });
			sink->AddLine({ hx - v1 * vx, hy - v1 * vy });
			sink->AddLine({ tail.x - v1 * vx, tail.y - v1 * vy });
		}
		sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	}
}

ShapeArrow::ShapeArrow(Canvas* win) :ShapeBase(win), draggers{
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0) }
{
	auto toolSub = win->getToolSub();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(), brush.GetAddressOf());
	// 滑块值当箭头尺寸用。太小的话箭头画出来只有几个像素，看不出形状
	arrowSize = toolSub->getSliderVal() * 4.f;
	arrowStyle = toolSub->arrowStyle;
	isFill = toolSub->isArrowFill;
}

ShapeArrow::~ShapeArrow()
{

}

// 箭头尺寸是按 path 画出来的，改了就得按新 arrowSize 重画一次几何
void ShapeArrow::applyStyle()
{
	auto toolSub = win->getToolSub();
	brush->SetColor(toolSub->getSelectedColor());
	arrowSize = toolSub->getSliderVal() * 4.f;
	isFill = toolSub->isArrowFill;
	if (path) makeArrow();
}

// 档位（第几档箭头样式）。与 applyStyle 分开的理由见 ShapeBase::applyToolStyle ——
// 滚轮调粗细、点填充开关走的都是 applyStyle，带上这一句就会把形状一起换掉
void ShapeArrow::applyToolStyle()
{
	arrowStyle = win->getToolSub()->arrowStyle;
	if (path) makeArrow();
}

// 「选择对象」框选要用。path 是 buildOutline 建出来的整支箭的轮廓（杆 + 头），
// 四档都建 —— 细箭头那一档画的时候走 strokeHead 描边，但轮廓几何照样在，
// 本来就是那一档的命中几何（见 buildOutline 的 style 2 分支）
bool ShapeArrow::getShapeBounds(D2D1_RECT_F& out) const
{
	if (!path) return false;
	D2D1_RECT_F b{};
	if (FAILED(path->GetBounds(nullptr, &b))) return false;
	// 轮廓是"面"，画的却有描边：细箭头那档笔宽是 arrowSize * 0.25。
	// 往外让出半个笔宽，真正画出来的像素才全落在框里
	const float pad{ arrowSize * 0.25f };
	out = D2D1::RectF(b.left - pad, b.top - pad, b.right + pad, b.bottom + pad);
	return true;
}

void ShapeArrow::paint(ID2D1DeviceContext* ctx)
{
	// makeArrow 要等第一次 mouseDown 才建 path，这之前可能先来一次 paint
	if (!path) return;
	// 细箭头那一档是描出来的（没有可填充的面），填充开关在它上面没有意义
	if (arrowStyle == 2) {
		paintLineStyle(ctx);
		return;
	}
	if (isFill) {
		ctx->FillGeometry(path.Get(), brush.Get());
	}
	else {
		// 空心：描的就是这个实心轮廓，**笔宽必须用这条箭头的线宽（滑块值）**。
		// 原来这里传的是 win->getDpi()（一个逻辑像素）：一条 8 像素粗的箭头描出来成了一圈
		// 发丝，整支箭看着变成另一种细箭头（作者报的"点填充 / 取消填充之后箭头样式就变了"）。
		// arrowSize 是滑块值的 4 倍（见构造函数），所以这里算出来的正是滑块的线宽 ——
		// 与 ShapeRectBase 的空心分支用 strokeWidth 是同一条规矩
		ctx->DrawGeometry(path.Get(), brush.Get(), arrowSize * 0.25f);
	}
}

// 细箭头（2）：箭杆是一条从起点直画到箭尖的线，头部是开口 V。
// 杆厚 = 滑块值本身（arrowSize 是滑块值的 4 倍，所以这里乘 0.25）——
// 与线条端点那档"细箭头"同一个口径：**一条宽 w 的线 + 一个 9.7w 长的开口 V**。
// 于是它跟线条的细箭头是同一支箭，下拉里那一格画的也是这个比例。
// 以前这里乘 0.5（杆厚 = 2×滑块值＝块箭头那一族的杆厚），头跟着杆翻倍，
// 下拉里看着就是"第 3 格的头比别的大一圈"（作者报的"第 3 个箭头太大"）
// 头部交给 strokeHead（匿名 namespace），下拉里的样例与它共用一份
bool ShapeArrow::paintLineStyle(ID2D1DeviceContext* ctx)
{
	const auto dx{ endX - startX }, dy{ endY - startY };
	const auto len{ std::sqrt(dx * dx + dy * dy) };
	if (len < 1.f) return false;
	const auto w{ arrowSize * 0.25f };
	const auto ux{ dx / len }, uy{ dy / len };
	// 箭杆停在开口 V 的内顶点（离箭尖回退 back），不一直画到箭尖：箭杆的端帽是平头，
	// 画到箭尖的话那一小截平口正好压在 V 的外角上，把尖顶切成 4~5 像素宽的钝边
	// （FSCapture 同一档量出来箭尖只有 1 像素）。回退 back 之后杆的平口整段落在 V 的描边里，
	// 最外侧就只剩 V 的斜接外角 —— 那才是"尖尖头"
	// 回退量夹在半个杆长以内：拖出来的极短箭头（杆还没回退量长）时别把杆反向画出去
	const auto back{ std::min(headGeom(w).back, len * 0.5f) };
	ctx->DrawLine(D2D1::Point2F(startX, startY),
		D2D1::Point2F(endX - ux * back, endY - uy * back), brush.Get(), w);
	strokeHead(ctx, D2D1::Point2F(endX, endY), D2D1::Point2F(ux, uy), w, brush.Get());
	return true;
}

// 下拉里的一格：一条等长的样例箭头，铺满整格。
// 4 档原来各挑一个符号（"—▶" / "➔" / "—→" / "➤"），字宽不一样所以长短不一，
// 而且符号的形状跟画到图上的那个箭头对不上（作者报的"没有标识度、各线条长短不一"）
void ShapeArrow::paintSample(ID2D1DeviceContext* ctx, const D2D1_RECT_F& rect,
	const int styleIndex, const float strokeW, ID2D1Brush* brush)
{
	// 与 ShapeLine 的样例同一套夹法（格窄了就把线细下来）。clamp 的除数按"最宽的
	// 那一档"算：细箭头的头长 9.7w（w = 这一步的 strokeW）+ 左右各 2.5w 的余量 ≈ 14.7w，
	// 取 16 留一点空。列表里那格宽 120 逻辑像素起，这条永远不生效；
	// 只有工具条上那枚 42 逻辑像素宽的按钮会被真的夹到
	// 之后按图上的比例换成箭头尺寸 —— 那边的 arrowSize 是"块箭头杆厚的 2 倍"
	// （杆半宽 = arrowSize/4，画出来的杆正好 arrowSize/2 厚）
	const auto w{ std::max(1.f, std::min(strokeW, (rect.right - rect.left) / 16.f)) };
	const auto size{ w * 4.f };
	const auto pad{ 2.5f * w };
	const auto y{ (rect.top + rect.bottom) / 2.f };
	const D2D1_POINT_2F tail{ rect.left + pad, y };
	const D2D1_POINT_2F tip{ std::max(rect.left + pad, rect.right - pad), y };
	const auto style = styleIndex >= 0 && styleIndex <= 3 ? styleIndex : 0;
	if (style == 2) {
		// 这一档图上就是描出来的：一根线 + 头上的开口 V。线的粗细取 w ——
		// 与 ShapeLine 的端点预览同一个口径（那边也是"线宽 = 这一格的 w"），
		// 于是箭头下拉里这一格与线条端点下拉里的细箭头画出来是同一支箭。
		// 箭杆与图上画法一样停在 V 的内顶点，免得平口端帽把箭尖切成钝边（理由见 paintLineStyle）
		ctx->DrawLine(tail, D2D1::Point2F(tip.x - headGeom(w).back, tip.y), brush, w);
		strokeHead(ctx, tip, D2D1::Point2F(1.f, 0.f), w, brush);
		return;
	}
	ComPtr<ID2D1PathGeometry> geo;
	if (FAILED(Ling::D2D::get()->d2dFactory->CreatePathGeometry(geo.GetAddressOf()))) return;
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(geo->Open(sink.GetAddressOf()))) return;
	buildOutline(sink.Get(), tail, tip, size, style);
	sink->Close();
	ctx->FillGeometry(geo.Get(), brush);
}

void ShapeArrow::paintDragger(ID2D1DeviceContext* ctx)
{
	for (auto& dragger : draggers)
	{
		// 选中的填白、悬停的留空：光标掠过一串元素时能分出改样式会作用到谁。
		// 先填后描：描边是压在矩形边线中线上的，先描再填会把内半边盖掉，线看着只剩外半截
		if (win->selected == this) ctx->FillRectangle(dragger, brushDraggerFill.Get());
		ctx->DrawRectangle(dragger, brushDragger.Get(), win->getDpi());
	}
}

void ShapeArrow::mouseDrag(const float x, const float y)
{
	// Shift 约束成八方向。与 ShapeRect 一致，用 GetKeyState 而不是消息里的 modifiers
	bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	if (hoverDraggerIndex == 0) {
		if (shiftDown) {
			constrainToEightDirections(endX, endY, x, y, startX, startY);
		}
		else {
			startX = x;
			startY = y;
		}
		makeArrow();
	}
	else if (hoverDraggerIndex == 1) {
		if (shiftDown) {
			constrainToEightDirections(startX, startY, x, y, endX, endY);
		}
		else {
			endX = x;
			endY = y;
		}
		makeArrow();
	}
	else if (hoverDraggerIndex == 8) {
		auto spanX{ x - pressX };
		auto spanY{ y - pressY };
		startX += spanX;
		startY += spanY;
		endX += spanX;
		endY += spanY;
		makeArrow();
		pressX = x;
		pressY = y;
	}
}

void ShapeArrow::mouseDown(const float x, const float y)
{
	if (hoverDraggerIndex == -1) { //首次创建
		startX = x;
		startY = y;
		endX = x;
		endY = y;
		makeArrow();
		hoverDraggerIndex = 1;
	}
	else if (hoverDraggerIndex == 8) {
		pressX = x;
		pressY = y;
	}
}

void ShapeArrow::mouseUp(const float x, const float y)
{
	auto half{ draggerSize / 2 };
	draggers[0].left = startX - half;
	draggers[0].top = startY - half;
	draggers[0].right = startX + half;
	draggers[0].bottom = startY + half;

	draggers[1].left = endX - half;
	draggers[1].top = endY - half;
	draggers[1].right = endX + half;
	draggers[1].bottom = endY + half;
}

void ShapeArrow::mouseMove(const float x, const float y)
{
	hoverDraggerIndex = -1;
	if (isInRect(draggers[0], x, y))
	{
		hoverDraggerIndex = 0;
	}
	else if (isInRect(draggers[1], x, y))
	{
		hoverDraggerIndex = 1;
	}
	if (hoverDraggerIndex == -1)
	{
		if (!path) return;
		BOOL contains = FALSE;
		path->FillContainsPoint({ x, y }, nullptr, &contains);
		if (contains) hoverDraggerIndex = 8;
	}
}

// 光标停在两端的 dragger 或箭头身上（此时 Canvas 才把滚轮事件转过来）滚滚轮 = 调箭头大小，
// 与矩形/椭圆用滚轮调线宽是一回事。
void ShapeArrow::mouseWheel(const float x, const float y, const short delta)
{
	// arrowSize 是滑块值的 4 倍（见构造函数），所以换算回滑块那个尺度再交给 ToolSub 夹值，
	// 它返回的也是滑块尺度的物理像素，再乘回 4。一格走一个滑块刻度
	auto step{ 4.f * win->getDpi() };
	auto next = arrowSize + (delta < 0 ? -step : step);
	auto applied = win->getToolSub()->setShapeSliderVal(L"arrow", next / 4.f) * 4.f;
	if (applied == arrowSize) return;   //已经顶到值域的头了，不用重画
	arrowSize = applied;
	makeArrow();                        //形状是按 arrowSize 算出来的，得重建
	win->refresh();
}

void ShapeArrow::setCursor()
{
	SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
}

std::unique_ptr<ShapeBase> ShapeArrow::clone(const float dx, const float dy) const
{
	return cloneSelf(*this, dx, dy);
}

// 画刷重建一份：ComPtr 拷过来是同一支，改一方的颜色会连另一方一起改
void ShapeArrow::fixupCopy()
{
	if (!brush) return;
	auto color = brush->GetColor();
	Ling::D2D::get()->deviceContext->CreateSolidColorBrush(color, brush.ReleaseAndGetAddressOf());
}

void ShapeArrow::translate(const float dx, const float dy)
{
	startX += dx;
	startY += dy;
	endX += dx;
	endY += dy;
	makeArrow();
	// 两个夹点的重算原本只在 mouseUp 里做（那一下是"这一笔画完了"的收尾），
	// 复制出来的这一份没有那一下，照它的算法补一遍
	auto half{ draggerSize / 2 };
	draggers[0] = D2D1::RectF(startX - half, startY - half, startX + half, startY + half);
	draggers[1] = D2D1::RectF(endX - half, endY - half, endX + half, endY + half);
}

void ShapeArrow::makeArrow()
{
	auto d2d = Ling::D2D::get();
	// 每次都要按新的起终点重建，用 ReleaseAndGetAddressOf 放掉上一个，
	// 否则 GetAddressOf 只是覆盖指针，等于每拖一下漏一个 ID2D1PathGeometry
	d2d->d2dFactory->CreatePathGeometry(path.ReleaseAndGetAddressOf());
	ComPtr<ID2D1GeometrySink> sink;
	path->Open(sink.GetAddressOf());
	// 顶点全在 buildOutline 里（匿名 namespace），下拉里的样例与它共用一份
	buildOutline(sink.Get(), D2D1::Point2F(startX, startY), D2D1::Point2F(endX, endY), arrowSize, arrowStyle);
	sink->Close();
}

void ShapeArrow::constrainToEightDirections(const float anchorX, const float anchorY, const float mouseX, const float mouseY, float& targetX, float& targetY)
{
	float dx = mouseX - anchorX;
	float dy = mouseY - anchorY;
	float absX = fabsf(dx);
	float absY = fabsf(dy);
	if (absY <= absX * 0.41421356237f) {
		targetX = anchorX + dx;
		targetY = anchorY;
	}
	else if (absX <= absY * 0.41421356237f) {
		targetX = anchorX;
		targetY = anchorY + dy;
	}
	else {
		float span = absX > absY ? absX : absY;
		targetX = anchorX + (dx >= 0.f ? span : -span);
		targetY = anchorY + (dy >= 0.f ? span : -span);
	}
}
