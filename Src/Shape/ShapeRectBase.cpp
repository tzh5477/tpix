#include "pch.h"
#include <cmath>
#include <algorithm>
#include "Canvas.h"
#include "Tool/ToolSub.h"
#include "ShapeRectBase.h"

using Microsoft::WRL::ComPtr;

namespace {
	constexpr float kPi{ 3.14159265358979323846f };
	float toRad(const float deg) { return deg * kPi / 180.f; }

	// 拖 0~7 号手柄时，那个"固定不动的参考点"该取哪一号手柄的位置。
	//
	// 四个角取对角（i + 4）。四条边中点不能取对角 —— 边中点的对角是**另一条边的中点**，
	// 它压根不是新矩形的角：拿它当锚点，新矩形的那一维会整体偏半格（拖下边中点往上收，
	// 左上角会右移半个宽度；拖上边中点、右边中点分别往左 / 往上偏）。这正是作者报的
	// "拖这 4 个点会导致图形位置偏移"。
	//
	// 正确取法是"贴着不动那条边的角"：上中→右下、右中→左下、下中→左上、左中→右上，
	// 正好是 (i + 3) % 8（与对角那条只差一格）。这样 mouseDrag 里
	// anchor + du·u + dv·v 量出来的矩形，两个角都是真角，被拖的那条边动、对边与两侧岿然不动
	int anchorHandleIndex(const int i)
	{
		return i % 2 == 1 ? (i + 3) % 8 : (i + 4) % 8;
	}
}

ShapeRectBase::ShapeRectBase(Canvas* win) : ShapeBase(win), draggers(17, D2D1::RectF(0, 0, 0, 0))
{
}

ShapeRectBase::~ShapeRectBase()
{
}

D2D1_POINT_2F ShapeRectBase::rectCenter() const
{
	return { (rect.left + rect.right) / 2.f, (rect.top + rect.bottom) / 2.f };
}

D2D1_POINT_2F ShapeRectBase::toWorld(const D2D1_POINT_2F& p) const
{
	if (angle == 0.f) return p;
	return rotatePoint(p, rectCenter(), angle);
}

D2D1_MATRIX_3X2_F ShapeRectBase::setRotateTransform(ID2D1DeviceContext* ctx) const
{
	D2D1_MATRIX_3X2_F prev{};
	ctx->GetTransform(&prev);
	if (angle != 0.f) {
		// 旋转排在内层（矩阵左乘 = 先缩放再旋转），所以中心要用缩放后的坐标
		ctx->SetTransform(prev * D2D1::Matrix3x2F::Rotation(angle, transformPoint(ctx, rectCenter())));
	}
	return prev;
}

const wchar_t* ShapeRectBase::styleGroup() const
{
	return kind == Kind::Ellipse ? L"ellipse" : L"rect";
}

float ShapeRectBase::norm360(const float deg)
{
	auto v = fmodf(deg, 360.f);
	if (v < 0.f) v += 360.f;
	return v;
}

D2D1_POINT_2F ShapeRectBase::ellipsePoint(const float deg, const float scale) const
{
	auto rad = toRad(deg);
	return { cx + rx * scale * cosf(rad), cy + ry * scale * sinf(rad) };
}

bool ShapeRectBase::inNotch(const float deg) const
{
	if (notchSweep <= 0.5f) return false;
	// 缺角是 [notchStart, notchStart + notchSweep]，两头顶着算 —— 边界上那一点点
	// 让给缺角侧，免得沿切边的命中时有时无
	return norm360(deg - notchStart) <= notchSweep;
}

float ShapeRectBase::radiusHome() const
{
	auto minWH = std::min(rect.right - rect.left, rect.bottom - rect.top);
	// 初始位置随图形尺寸走（小图形不该被几枚手柄压满），但最小要让开角上那个八向手柄，
	// 两枚图标叠在一起谁也点不准
	return std::max(minWH * 0.22f, draggerSize * 1.2f);
}

float ShapeRectBase::radiusMax() const
{
	auto minWH = std::min(rect.right - rect.left, rect.bottom - rect.top);
	return std::max(0.f, minWH / 2.f - radiusHome());
}

bool ShapeRectBase::getShapeBounds(D2D1_RECT_F& out) const
{
	// 转过之后外接框也跟着放大：动作图标摆的是"看得见的那一块"的外面
	out = rotatedBounds(rect, angle);
	return true;
}

D2D1_POINT_2F ShapeRectBase::handleLocalPoint(const int i) const
{
	auto w{ rect.right - rect.left }, h{ rect.bottom - rect.top };
	switch (i) {
	case 1: return { rect.left + w / 2.f, rect.top };
	case 2: return { rect.right, rect.top };
	case 3: return { rect.right, rect.top + h / 2.f };
	case 4: return { rect.right, rect.bottom };
	case 5: return { rect.left + w / 2.f, rect.bottom };
	case 6: return { rect.left, rect.bottom };
	case 7: return { rect.left, rect.top + h / 2.f };
	default: return { rect.left, rect.top };
	}
}

// 扇形 / 环形：外弧 + （内弧）拼成一条闭合路径。弧用折线近似（2 度一段）——
// D2D 的 AddArc 要求把超过 180 度的弧拆成几段才不会画歪，而这里的 sweep 是连着变的，
// 拆段的边界每帧都在动。折线在半径几百像素时的偏差远小于一个像素，肉眼看不出来
ComPtr<ID2D1PathGeometry> ShapeRectBase::makePieGeometry() const
{
	ComPtr<ID2D1PathGeometry> geo;
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(Ling::D2D::get()->d2dFactory->CreatePathGeometry(geo.GetAddressOf()))) return geo;
	if (FAILED(geo->Open(sink.GetAddressOf()))) return geo;
	// 要画的是"缺角之外的那一段"：从 notchStart 扫过一周、停在缺角的起点
	auto start{ notchStart + notchSweep };
	auto sweep{ 360.f - notchSweep };
	auto steps{ std::max(1, (int)ceilf(sweep / 2.f)) };
	auto ring{ innerRatio > 0.001f };
	if (ring) sink->BeginFigure(ellipsePoint(start, innerRatio), D2D1_FIGURE_BEGIN_FILLED);
	else sink->BeginFigure(D2D1::Point2F(cx, cy), D2D1_FIGURE_BEGIN_FILLED);
	sink->AddLine(ellipsePoint(start, 1.f));
	for (int i = 1; i <= steps; i++) sink->AddLine(ellipsePoint(start + sweep * i / steps, 1.f));
	if (ring) {
		sink->AddLine(ellipsePoint(start + sweep, innerRatio));
		for (int i = steps - 1; i >= 0; i--) sink->AddLine(ellipsePoint(start + sweep * i / steps, innerRatio));
	}
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	sink->Close();
	return geo;
}

void ShapeRectBase::paint(ID2D1DeviceContext* ctx)
{
	// 外层变换（屏幕上是缩放、导出时是单位阵）要保住，旋转排在内层 ——
	// 与 ShapeText 同一套写法，导出因此不必另走一条路
	auto prev{ setRotateTransform(ctx) };
	if (kind == Kind::Ellipse) {
		if (!isPie()) {
			// 完整圆没有缺角，直接用 D2D 的椭圆（比拿折线拼出来的更干净）
			D2D1_ELLIPSE e = D2D1::Ellipse({ cx, cy }, rx, ry);
			if (isFill) ctx->FillEllipse(e, brush.Get());
			else ctx->DrawEllipse(e, brush.Get(), strokeWidth);
		}
		else if (auto geo = makePieGeometry()) {
			if (isFill) ctx->FillGeometry(geo.Get(), brush.Get());
			else ctx->DrawGeometry(geo.Get(), brush.Get(), strokeWidth);
		}
	}
	else if (radius > 0.5f) {
		// 半径再大就被 D2D 按半宽半高夹住，画出来的角会突然变样；这里先自己夹一道
		auto r{ std::min(radius, std::min(rect.right - rect.left, rect.bottom - rect.top) / 2.f) };
		auto rr = D2D1::RoundedRect(rect, r, r);
		if (isFill) ctx->FillRoundedRectangle(rr, brush.Get());
		else ctx->DrawRoundedRectangle(rr, brush.Get(), strokeWidth);
	}
	else if (isFill) {
		ctx->FillRectangle(rect, brush.Get());
	}
	else {
		ctx->DrawRectangle(rect, brush.Get(), strokeWidth);
	}
	ctx->SetTransform(prev);
}

void ShapeRectBase::paintDot(ID2D1DeviceContext* ctx, const D2D1_RECT_F& box, const bool withCenter) const
{
	if (box.right <= box.left) return;
	auto c = D2D1::Point2F((box.left + box.right) / 2.f, (box.top + box.bottom) / 2.f);
	auto r{ (box.right - box.left) / 2.f };
	ctx->FillEllipse(D2D1::Ellipse(c, r, r), brushDraggerFill.Get());
	ctx->DrawEllipse(D2D1::Ellipse(c, r, r), brushDragger.Get(), win->getDpi());
	// 中间点一点的那枚 = "转"（扇区的方向由它定），与另外两枚空心点分开
	if (withCenter) {
		ctx->FillEllipse(D2D1::Ellipse(c, r * 0.3f, r * 0.3f), brushDragger.Get());
	}
}

void ShapeRectBase::paintDragger(ID2D1DeviceContext* ctx)
{
	// 手柄位置每帧重算：rect 可能刚被拖过，而 paintDragger 不一定排在 mouseDrag 之后。
	// 摆在未旋转的外接框右下角（基类按 getShapeBounds 算），不跟着图形转 ——
	// 手柄属于"外接框"，元素转过之后它仍待在框的右下角，三个角上的按钮才对得齐
	makeDraggers();
	updateRotateDragger();
	auto prev{ setRotateTransform(ctx) };
	auto dpi = win->getDpi();
	for (int i = 0; i <= 7; i++) {
		// 选中的填白、悬停的留空：光标掠过一串元素时能分出改样式会作用到谁。
		// 先填后描：描边是压在矩形边线中线上的，先描再填会把内半边盖掉，线看着只剩外半截
		if (win->selected == this) ctx->FillRectangle(draggers[i], brushDraggerFill.Get());
		ctx->DrawRectangle(draggers[i], brushDragger.Get(), dpi);
	}
	// 内部那几枚（圆角 / 扇区）只在选中时现身：它们调的是"这个元素自己的参数"，
	// 光标只是路过时冒出来，会和八向手柄混在一起分不清谁是谁。
	// 图片那一族没有这两样东西，整段跳过（见 hasInnerHandles）
	if (win->selected == this && hasInnerHandles()) {
		if (kind == Kind::Rect) {
			for (int i = HitRadiusTL; i <= HitRadiusBL; i++) paintDot(ctx, draggers[i], false);
		}
		else {
			// 完整圆只有一枚（拖它才切出扇形）；切出缺角之后内径那一枚才出现
			if (isPie()) {
				paintDot(ctx, draggers[HitInner], false);
				paintDot(ctx, draggers[HitNotchStart], true);
			}
			paintDot(ctx, draggers[HitNotchEnd], false);
		}
	}
	ctx->SetTransform(prev);
	// 手柄没跟着上面的变换转（见上），这里直接按它自己的坐标画
	paintRotateHandle(ctx);
}

// 手柄位置由基类按外接框右下角一次算完（与右上角的 × 对称），这里不必再跟着图形转 ——
// 手柄属于"外接框"而不是图形本身，元素转过之后它仍待在框的右下角

void ShapeRectBase::mouseDrag(const float x, const float y)
{
	switch (hoverDraggerIndex)
	{
	case HitRotate:
		angle = rotateAngleAt(rectCenter(), x, y);
		return;
	case HitRadiusTL:
	case HitRadiusTR:
	case HitRadiusBR:
	case HitRadiusBL:
	{
		// 手柄往图形里拖得越深圆角越大：半径就是它离那个角点往里让开的距离。
		// 拖回角上（甚至拖到外面）就是方角 —— 用两份分量里小的那个，
		// 这样斜着拖也不会把角拖成"一边大一边小"的怪形状
		auto p = unrotatePoint({ x, y }, rectCenter(), angle);
		float inward{ 0.f };
		switch (hoverDraggerIndex)
		{
		case HitRadiusTL: inward = std::min(p.x - rect.left, p.y - rect.top); break;
		case HitRadiusTR: inward = std::min(rect.right - p.x, p.y - rect.top); break;
		case HitRadiusBR: inward = std::min(rect.right - p.x, rect.bottom - p.y); break;
		default:          inward = std::min(p.x - rect.left, rect.bottom - p.y); break;
		}
		radius = std::clamp(inward - radiusHome(), 0.f, radiusMax());
		return;
	}
	case HitInner:
	{
		// 往中心拖 = 实心扇形；往外拖 = 环形。位置就是"离圆心多远"，与画出来的内圈一致
		auto p = unrotatePoint({ x, y }, rectCenter(), angle);
		auto nx{ rx > 0.f ? (p.x - cx) / rx : 0.f };
		auto ny{ ry > 0.f ? (p.y - cy) / ry : 0.f };
		innerRatio = std::clamp(sqrtf(nx * nx + ny * ny), 0.f, 0.95f);
		return;
	}
	case HitNotchStart:
	{
		// 这枚是"把缺角整个转过去"：跨度不变，两条边一起走 —— 就是扇形的朝向
		auto p = unrotatePoint({ x, y }, rectCenter(), angle);
		notchStart = norm360(atan2f(p.y - cy, p.x - cx) * 180.f / kPi);
		return;
	}
	case HitNotchEnd:
	{
		// 这枚改的是缺角有多大（也就是扇形还剩多少）。停在起始边上就是完整圆
		auto p = unrotatePoint({ x, y }, rectCenter(), angle);
		auto deg = atan2f(p.y - cy, p.x - cx) * 180.f / kPi;
		// 留一点点：正好 360 时那两条边重合，看着像画错了
		notchSweep = std::min(norm360(deg - notchStart), 359.f);
		return;
	}
	}
	if (hoverDraggerIndex == HitBody) {
		auto w = rect.right - rect.left;
		auto h = rect.bottom - rect.top;
		rect.left = x - pressX;
		rect.top = y - pressY;
		rect.right = rect.left + w;
		rect.bottom = rect.top + h;
		syncFromRect();
		return;
	}
	if (hoverDraggerIndex < 0 || hoverDraggerIndex > 7) return;
	// 手柄拖动：被拖的那个角跟着鼠标走，对角那个点纹丝不动。
	// 做法是把"鼠标"与"固定的对角点"都投影到元素自己的两条轴上（u 沿局部 x、v 沿局部 y），
	// 由这两个投影直接定出矩形 —— 转过角度的矩形也就跟着对了。
	// 别改成"就地改 rect 的四条边"：那样锚点会随 rect 中心漂走，转过的元素一拖就飘
	auto ux{ cosf(toRad(angle)) }, uy{ sinf(toRad(angle)) };
	auto vx{ -sinf(toRad(angle)) }, vy{ cosf(toRad(angle)) };
	auto du = (x - anchorWorld.x) * ux + (y - anchorWorld.y) * uy;
	auto dv = (x - anchorWorld.x) * vx + (y - anchorWorld.y) * vy;
	// 上下两枚只改高度、左右两枚只改宽度：另一维保持按下时的尺寸。
	// 锚点在图形的哪一侧决定了这里的正负号（锚点在对边，往另一侧量过去）
	switch (hoverDraggerIndex)
	{
	case 1: du = -pressW; break;
	case 5: du = pressW; break;
	case 3: dv = -pressH; break;
	case 7: dv = pressH; break;
	}
	if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
		auto m = std::max(fabsf(du), fabsf(dv));
		du = du < 0.f ? -m : m;
		dv = dv < 0.f ? -m : m;
	}
	// 世界矩形的两个对角点 = 锚点 与 锚点 + du·u + dv·v，局部 rect 就是它转回轴对齐的样子：
	// 边长 |du| × |dv|、中心仍是这两点的中点
	auto mid = D2D1::Point2F(anchorWorld.x + (du * ux + dv * vx) / 2.f,
		anchorWorld.y + (du * uy + dv * vy) / 2.f);
	auto hw{ fabsf(du) / 2.f }, hh{ fabsf(dv) / 2.f };
	rect = D2D1::RectF(mid.x - hw, mid.y - hh, mid.x + hw, mid.y + hh);
	syncFromRect();
}

void ShapeRectBase::mouseDown(const float x, const float y)
{
	// 首次创建：rect 从起笔点长出来，锚点就是它
	if (hoverDraggerIndex == -1) {
		rect = D2D1::RectF(x, y, x, y);
		hoverDraggerIndex = firstDraggerIndex();
		anchorWorld = D2D1::Point2F(x, y);
		pressW = pressH = 0.f;
		syncFromRect();
		return;
	}
	if (hoverDraggerIndex == HitBody) {
		pressX = x - rect.left;
		pressY = y - rect.top;
		return;
	}
	pressW = rect.right - rect.left;
	pressH = rect.bottom - rect.top;
	if (hoverDraggerIndex >= HitRotate) {
		// 旋转与那几枚内部手柄都在 mouseDrag 里按"当前位形 + 鼠标位置"现算，
		// 按下这一下只要把尺寸记下来就够
		pressX = x;
		pressY = y;
		return;
	}
	// 0~7：记下对角那个手柄此刻的屏幕坐标，拖的过程中它固定不动（见 mouseDrag）。
	// 四条边中点取的是旁边那个角而不是对角那枚手柄，理由见 anchorHandleIndex
	anchorWorld = toWorld(handleLocalPoint(anchorHandleIndex(hoverDraggerIndex)));
}

void ShapeRectBase::mouseUp(const float x, const float y)
{
	makeDraggers();
}

void ShapeRectBase::mouseMove(const float x, const float y)
{
	hoverDraggerIndex = -1;
	// 手柄位置是 paintDragger 里算的，而它只在 hover 时才跑；这里先补算一次，
	// 免得刚把鼠标移上去的那一帧拿着上一次的旧位置判不中
	updateRotateDragger();
	if (isInRect(rotateDragger, x, y)) {
		hoverDraggerIndex = HitRotate;
		return;
	}
	// 框转过之后能点中的那块也跟着转，把鼠标点逆着角度转回来再判
	auto p = unrotatePoint({ x, y }, rectCenter(), angle);
	hitDraggers(p.x, p.y);
	if (hoverDraggerIndex == -1) {
		hitBody(p.x, p.y);
	}
}

void ShapeRectBase::setCursor()
{
	switch (hoverDraggerIndex)
	{
	case 0: case 4: SetCursor(LoadCursor(nullptr, IDC_SIZENWSE)); return;
	case 1: case 5: SetCursor(LoadCursor(nullptr, IDC_SIZENS)); return;
	case 2: case 6: SetCursor(LoadCursor(nullptr, IDC_SIZENESW)); return;
	case 3: case 7: SetCursor(LoadCursor(nullptr, IDC_SIZEWE)); return;
	case HitBody: SetCursor(LoadCursor(nullptr, IDC_SIZEALL)); return;
	case HitRotate: SetCursor(LoadCursor(nullptr, IDC_CROSS)); return;
	default:
		if (hoverDraggerIndex >= HitRadiusTL && hoverDraggerIndex <= HitRadiusBL) {
			SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
		}
		else if (hoverDraggerIndex >= HitInner) {
			SetCursor(LoadCursor(nullptr, IDC_CROSS));
		}
		return;
	}
}

// 滚滚轮 = 调线宽，与序号那边用滚轮调大小是一回事。填充图形没有边框可调，直接不管。
// 马赛克与擦除不参与：它们的滑块调的是笔刷块大小，与线宽不是一回事
void ShapeRectBase::mouseWheel(const float x, const float y, const short delta)
{
	if (!allowShapeToggle || isFill) return;
	// 一格一个逻辑像素。上下限交给 ToolSub 那张滑块值域表夹，用它夹完的返回值 ——
	// 线宽与工具栏滑块因此永远是同一个数，也滚不出滑块能表达的范围。
	// 组名按 kind 取而不是按 toolId：矩形与圆互转之后要改的是它现在那一组
	auto next = strokeWidth + (delta < 0 ? -win->getDpi() : win->getDpi());
	auto applied = win->getToolSub()->setShapeSliderVal(styleGroup(), next);
	if (applied == strokeWidth) return;   //已经顶到值域的头了，不用重画
	strokeWidth = applied;
	win->refresh();
}

// 颜色 / 线宽 / 填充都是构造那一刻的快照（取法与构造函数里一模一样），
// 不重取的话改样式对已经画出去的图形毫无作用
void ShapeRectBase::applyStyle()
{
	// 马赛克与擦除的画刷是按画面算出来的（马赛克位图 / 底图），套上工具条的颜色
	// 就把它们涂掉了 —— 它们原本就没有 applyStyle，这里同样得跳过去
	if (!allowShapeToggle) return;
	auto toolSub = win->getToolSub();
	// 颜色、线宽、填充三样都取"面板此刻显示的那个工具"的那一份，口径一致（见 ToolSub::getCurrentFill）。
	// 填充原来按 kind 取（椭圆读 isEllipseFill、矩形读 isRectFill），而 kind 会被元素上那枚动作图标
	// 翻转（见 onAction：那里刻意不动 toolId，所以面板不会跟着换）。于是矩形工具画出的矩形转成圆
	// 之后，面板仍是矩形面板、显示的是 isRectFill，按 kind 却去读一份没人动过的 isEllipseFill ——
	// 那枚开关就按不动它。
	// 实测（A/B：只把下面这一行换回旧写法，其余不动）——矩形工具画矩形→点元素左下角那枚图标转成圆
	// →点面板「填充」：旧版图形纹丝不动（仍实心，覆盖 0.772），新版按面板的值变空心（0.024）
	brush->SetColor(toolSub->getSelectedColor());
	strokeWidth = toolSub->getSliderVal();
	isFill = toolSub->getCurrentFill();
}

// 批量旋转：绕外部中心刚体转。位置那一步走 translate（马赛克 / 擦除那几个覆写的会顺手
// 把位图重算一遍），角度这一步只改 angle —— 马赛克的取样区域与角度无关，paint 时统一叠旋转
void ShapeRectBase::rotateBy(const float deg, const D2D1_POINT_2F& center)
{
	auto rc = rectCenter();
	auto nr = rotatePoint(rc, center, deg);
	auto dx{ nr.x - rc.x }, dy{ nr.y - rc.y };
	// center 就是自己的中心时（WinPin 的批量旋转现在逐个传"它自己的中心"）
	// 位移恒为 0，这一步纯属白跑 —— 马赛克 / 擦除的 translate 还会顺手把位图重建一遍
	if (dx != 0.f || dy != 0.f) translate(dx, dy);
	angle += deg;
}

void ShapeRectBase::paintActionIcon(ID2D1DeviceContext* ctx, const int i, const D2D1_POINT_2F& c,
	const float rad, ID2D1Brush* brush, const float strokeW)
{
	if (i != 0) return;
	// 画的是"点一下会变成的形状"：现在是矩形就画个圆，反之画个方框。
	// 用线画而不是字形 —— 图标字体里有没有现成的码位靠猜，短文本又得跟着语言包走。
	// 笔与笔宽由基类传进来（它要拿同一份几何先描一遍白边，见 paintIconHaloed）
	auto k{ rad * 0.55f };
	if (kind == Kind::Rect) {
		ctx->DrawEllipse(D2D1::Ellipse(c, k, k), brush, strokeW);
	}
	else {
		ctx->DrawRectangle(D2D1::RectF(c.x - k, c.y - k, c.x + k, c.y + k), brush, strokeW);
	}
}

void ShapeRectBase::onAction(const int i)
{
	if (i != 0) return;
	kind = kind == Kind::Rect ? Kind::Ellipse : Kind::Rect;
	// 几何、颜色、线宽、圆角、扇区全部留着 —— 换的只是"怎么画、怎么命中"。
	// toolId 故意不动：它是"这一笔当初是哪个工具画的"，工具条改样式、WinPin 换工具
	// 时清选中态都按它筛。翻成"ellipse"的话，正选着它的这一刻调颜色反而落不到它身上。
	// 互转之后要跟着换的是线宽那一组，由 styleGroup() 按 kind 现取
	syncFromRect();
	win->refresh();
}

// ---- 复制（见 ShapeBase::cloneSelf）----
// 画刷重建一份：ComPtr 拷过来是同一支画刷，改一方的颜色会连另一方一起改。
// 马赛克 / 擦除那几支派生类自己的画刷不在这儿：它们要么是按画面算出来的位图刷
//（不可变，共享无妨），要么在 translate 里跟着几何重算
void ShapeRectBase::fixupCopy()
{
	if (!brush) return;
	auto color = brush->GetColor();
	Ling::D2D::get()->deviceContext->CreateSolidColorBrush(color, brush.ReleaseAndGetAddressOf());
}

void ShapeRectBase::translate(const float dx, const float dy)
{
	rect.left += dx;
	rect.right += dx;
	rect.top += dy;
	rect.bottom += dy;
	// 椭圆那套派生量（cx/cy/rx/ry）与所有手柄都按 rect 现算，改完 rect 走这两条就够
	syncFromRect();
	makeDraggers();
}

void ShapeRectBase::hitDraggers(const float x, const float y)
{
	for (int i = 0; i <= 7; i++) {
		if (isInRect(draggers[i], x, y)) {
			hoverDraggerIndex = i;
			return;
		}
	}
	// 内部那几枚只在选中时才算数（也只在选中时才画，见 paintDragger）。
	// 没有内部手柄的那一族（图片）直接到此为止 —— 与 paintDragger 同一条判断
	if (win->selected != this || !hasInnerHandles()) return;
	if (kind == Kind::Rect) {
		for (int i = HitRadiusTL; i <= HitRadiusBL; i++) {
			if (isInRect(draggers[i], x, y)) {
				hoverDraggerIndex = i;
				return;
			}
		}
		return;
	}
	// 完整圆时只有"切缺角"那一枚（内径那枚要等切出扇形才出现）
	if (isPie() && isInRect(draggers[HitInner], x, y)) {
		hoverDraggerIndex = HitInner;
		return;
	}
	for (int i = HitNotchStart; i <= HitNotchEnd; i++) {
		if (isInRect(draggers[i], x, y)) {
			hoverDraggerIndex = i;
			return;
		}
	}
}

void ShapeRectBase::hitBody(const float x, const float y)
{
	// 填充图形没有可见边框，边缘命中区用固定宽度（2 逻辑像素）即可，
	// 别跟滑块线宽走 —— 填充时滑块那套值跟这条边没有任何关系
	auto half{ isFill ? 2.f * win->getDpi() : strokeWidth / 2.f + win->getDpi() };//多个一个dpi，让范围更大点
	if (kind == Kind::Ellipse) {
		// 椭圆：归一化成单位圆再看半径，斜边与直角边一视同仁
		auto nx{ rx > 0.f ? (x - cx) / rx : 0.f };
		auto ny{ ry > 0.f ? (y - cy) / ry : 0.f };
		auto r = sqrtf(nx * nx + ny * ny);
		// 命中带的宽度折算到单位圆上（两个半径取平均，扁椭圆也不会一带子宽一带子窄）
		auto rr{ (rx + ry) / 2.f };
		auto band{ rr > 0.f ? half / rr : 0.f };
		if (r > 1.f + band) return;
		// 内边界：实心图形（含实心扇形）就是贴着外沿那一圈带子 —— 内部整块都能抓的话，
		// 一个铺满大半张图的填充椭圆会让里面的空白处再也点不到，别的标注就没法画了。
		// 环形（innerRatio > 0）时中间是空的，从内圈到外沿这一整圈才都是图，抓哪儿都算
		auto hole{ innerRatio > 0.02f ? innerRatio : 0.f };
		if (r < std::max(hole, 1.f - band)) return;
		// 缺角那一块也不在图里
		if (inNotch(atan2f(y - cy, x - cx) * 180.f / kPi)) return;
		hoverDraggerIndex = HitBody;
		return;
	}
	if (x >= rect.left - half && x <= rect.right + half && y >= rect.top - half && y <= rect.bottom + half)
	{
		if (x <= rect.left + half || x >= rect.right - half || y <= rect.top + half || y >= rect.bottom - half) {
			hoverDraggerIndex = HitBody;
		}
	}
}

// 矩形族里除了椭圆，都没有 rect 之外的几何，这条留空就是它们的"什么都不用干"
void ShapeRectBase::syncFromRect()
{
	cx = (rect.left + rect.right) / 2.f;
	cy = (rect.top + rect.bottom) / 2.f;
	rx = (rect.right - rect.left) / 2.f;
	ry = (rect.bottom - rect.top) / 2.f;
}

int ShapeRectBase::firstDraggerIndex() const
{
	return kind == Kind::Ellipse ? 0 : 4;
}

void ShapeRectBase::makeDraggers()
{
	auto half{ draggerSize / 2 };
	auto w{ rect.right - rect.left }, h{ rect.bottom - rect.top };
	auto box = [half](float px, float py) {
		return D2D1::RectF(px - half, py - half, px + half, py + half);
	};
	auto none = D2D1::RectF(0, 0, 0, 0);
	// 0~7 八向：顺序不能动，mouseDown / mouseDrag 里的语义按它写（对角 = 索引 + 4）
	draggers[0] = box(rect.left, rect.top);
	draggers[1] = box(rect.left + w / 2, rect.top);
	draggers[2] = box(rect.right, rect.top);
	draggers[3] = box(rect.right, rect.top + h / 2);
	draggers[4] = box(rect.right, rect.bottom);
	draggers[5] = box(rect.left + w / 2, rect.bottom);
	draggers[6] = box(rect.left, rect.bottom);
	draggers[7] = box(rect.left, rect.top + h / 2);
	// 圆角手柄：四个角沿对角线往里让开"初始位置 + 当前半径"，正好落在圆角弧的起止点上
	auto off{ radiusHome() + radius };
	draggers[HitRadiusTL] = kind == Kind::Rect ? box(rect.left + off, rect.top + off) : none;
	draggers[HitRadiusTR] = kind == Kind::Rect ? box(rect.right - off, rect.top + off) : none;
	draggers[HitRadiusBR] = kind == Kind::Rect ? box(rect.right - off, rect.bottom - off) : none;
	draggers[HitRadiusBL] = kind == Kind::Rect ? box(rect.left + off, rect.bottom - off) : none;
	// 扇区那三枚：内径那一枚落在"缺角起点"那条边上、离圆心 innerRatio 倍半径处
	// （内径为 0 时正好在圆心）；另外两枚分别落在缺角的两条边与外弧的交点上
	if (kind == Kind::Ellipse) {
		auto start = notchStart, end = notchStart + notchSweep;
		if (isPie()) {
			draggers[HitInner] = box(ellipsePoint(start, innerRatio).x, ellipsePoint(start, innerRatio).y);
			draggers[HitNotchStart] = box(ellipsePoint(start, 1.f).x, ellipsePoint(start, 1.f).y);
			draggers[HitNotchEnd] = box(ellipsePoint(end, 1.f).x, ellipsePoint(end, 1.f).y);
		}
		else {
			// 完整圆只有一枚，摆在圆内（0.55 倍半径）而不是外弧上 ——
			// 外弧上正压着"整体拖动"的命中带，小点摆那儿会和拖图形抢鼠标
			draggers[HitInner] = none;
			draggers[HitNotchStart] = none;
			draggers[HitNotchEnd] = box(ellipsePoint(end, 0.55f).x, ellipsePoint(end, 0.55f).y);
		}
	}
	else {
		draggers[HitInner] = none;
		draggers[HitNotchStart] = none;
		draggers[HitNotchEnd] = none;
	}
}
