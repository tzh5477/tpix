#include "pch.h"
#include <cmath>
#include <algorithm>
#include "Canvas.h"
#include "Tool/ToolSub.h"
#include "ShapeRectBase.h"

namespace {
	constexpr float kPi{ 3.14159265358979323846f };
	float toRad(const float deg) { return deg * kPi / 180.f; }
}

ShapeRectBase::ShapeRectBase(Canvas* win) : ShapeBase(win), draggers(9, D2D1::RectF(0, 0, 0, 0))
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

void ShapeRectBase::paint(ID2D1DeviceContext* ctx)
{
	// 外层变换（屏幕上是缩放、导出时是单位阵）要保住，旋转排在内层 ——
	// 与 ShapeText 同一套写法，导出因此不必另走一条路
	auto prev{ setRotateTransform(ctx) };
	if (kind == Kind::Ellipse) {
		// 完整圆没有缺角，直接用 D2D 的椭圆（比拿折线拼出来的更干净）
		D2D1_ELLIPSE e = D2D1::Ellipse({ cx, cy }, rx, ry);
		if (isFill) ctx->FillEllipse(e, brush.Get());
		else ctx->DrawEllipse(e, brush.Get(), strokeWidth);
	}
	else if (isFill) {
		ctx->FillRectangle(rect, brush.Get());
	}
	else {
		ctx->DrawRectangle(rect, brush.Get(), strokeWidth);
	}
	ctx->SetTransform(prev);
}

void ShapeRectBase::paintDragger(ID2D1DeviceContext* ctx)
{
	// 手柄位置每帧重算：rect 可能刚被拖过，而 paintDragger 不一定排在 mouseDrag 之后
	makeDraggers();
	updateRotateHandle();
	auto prev{ setRotateTransform(ctx) };
	auto dpi = win->getDpi();
	for (int i = 0; i <= 7; i++) {
		// 选中的填白、悬停的留空：光标掠过一串元素时能分出改样式会作用到谁。
		// 先填后描：描边是压在矩形边线中线上的，先描再填会把内半边盖掉，线看着只剩外半截
		if (win->selected == this) ctx->FillRectangle(draggers[i], brushDraggerFill.Get());
		ctx->DrawRectangle(draggers[i], brushDragger.Get(), dpi);
	}
	ctx->SetTransform(prev);
	// 旋转手柄的坐标已经是屏幕坐标（见 updateRotateHandle），不能再跟着上面的变换转一遍
	paintRotateHandle(ctx);
}

void ShapeRectBase::updateRotateHandle()
{
	ShapeBase::updateRotateDragger(rect);
	if (angle == 0.f) return;
	auto half{ draggerSize / 2.f };
	auto h = D2D1::Point2F((rotateDragger.left + rotateDragger.right) / 2.f,
		(rotateDragger.top + rotateDragger.bottom) / 2.f);
	auto p = rotatePoint(h, rectCenter(), angle);
	rotateDragger = D2D1::RectF(p.x - half, p.y - half, p.x + half, p.y + half);
}

void ShapeRectBase::mouseDrag(const float x, const float y)
{
	if (hoverDraggerIndex == HitRotate) {
		// 手柄静止时挂在右下角，鼠标方向减掉静止方向（rotateRestAngle）才是这次转过的角度
		angle = rotateAngleAt(rectCenter(), x, y);
		return;
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
	if (hoverDraggerIndex == HitRotate) {
		// 旋转按"当前位形 + 鼠标位置"现算，按下这一下只要把尺寸记下来就够
		pressX = x;
		pressY = y;
		return;
	}
	// 0~7：记下对角那个手柄此刻的屏幕坐标，拖的过程中它固定不动（见 mouseDrag）
	anchorWorld = toWorld(handleLocalPoint((hoverDraggerIndex + 4) % 8));
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
	updateRotateHandle();
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
	brush->SetColor(toolSub->getSelectedColor());
	strokeWidth = toolSub->getSliderVal();
	isFill = kind == Kind::Ellipse ? toolSub->isEllipseFill : toolSub->isRectFill;
}

void ShapeRectBase::paintActionIcon(ID2D1DeviceContext* ctx, const int i, const D2D1_POINT_2F& c, const float rad)
{
	if (i != 0) return;
	// 画的是"点一下会变成的形状"：现在是矩形就画个圆，反之画个方框。
	// 用线画而不是字形 —— 图标字体里有没有现成的码位靠猜，短文本又得跟着语言包走
	auto k{ rad * 0.55f };
	if (kind == Kind::Rect) {
		ctx->DrawEllipse(D2D1::Ellipse(c, k, k), brushDragger.Get(), win->getDpi());
	}
	else {
		ctx->DrawRectangle(D2D1::RectF(c.x - k, c.y - k, c.x + k, c.y + k), brushDragger.Get(), win->getDpi());
	}
}

void ShapeRectBase::onAction(const int i)
{
	if (i != 0) return;
	kind = kind == Kind::Rect ? Kind::Ellipse : Kind::Rect;
	// 几何、颜色、线宽全部留着 —— 换的只是"怎么画、怎么命中"。
	// toolId 故意不动：它是"这一笔当初是哪个工具画的"，工具条改样式、WinPin 换工具
	// 时清选中态都按它筛。翻成"ellipse"的话，正选着它的这一刻调颜色反而落不到它身上。
	// 互转之后要跟着换的是线宽那一组，由 styleGroup() 按 kind 现取
	syncFromRect();
	win->refresh();
}

void ShapeRectBase::hitDraggers(const float x, const float y)
{
	for (int i = 0; i <= 7; i++) {
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
		if (r > 1.f + band || r < 1.f - band) return;
		hoverDraggerIndex = HitBody;
		return;
	}
	if (x >= rect.left - half && x <= rect.right + half && y >= rect.top - half && y <= rect.bottom + half)
	{
		if (x <= rect.left + half || x >= rect.right - half || y >= rect.top + half || y >= rect.bottom - half) {
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
	// 顺序不能动，mouseDown / mouseDrag 里的语义按它写（对角 = 索引 + 4）
	draggers[0] = box(rect.left, rect.top);
	draggers[1] = box(rect.left + w / 2, rect.top);
	draggers[2] = box(rect.right, rect.top);
	draggers[3] = box(rect.right, rect.top + h / 2);
	draggers[4] = box(rect.right, rect.bottom);
	draggers[5] = box(rect.left + w / 2, rect.bottom);
	draggers[6] = box(rect.left, rect.bottom);
	draggers[7] = box(rect.left, rect.top + h / 2);
}
