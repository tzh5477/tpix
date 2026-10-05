#include "pch.h"
#include "App.h"
#include "Util.h"
#include "History.h"
#include "Canvas.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeText.h"

using Microsoft::WRL::ComPtr;

ShapeText::ShapeText(Canvas* win) :ShapeBase(win), borderPadding{ 6.f * win->getDpi() }
{
	setAttr();
	// 虚线框：2 实 2 虚，与 2.4.25 一致
	float dashes[] = { 2.f, 2.f };
	Ling::D2D::get()->d2dFactory->CreateStrokeStyle(
		D2D1::StrokeStyleProperties(
			D2D1_CAP_STYLE_FLAT,
			D2D1_CAP_STYLE_FLAT,
			D2D1_CAP_STYLE_ROUND,
			D2D1_LINE_JOIN_MITER,
			10.f,
			D2D1_DASH_STYLE_CUSTOM,
			0.f
		),
		dashes, ARRAYSIZE(dashes), dashedStrokeStyle.GetAddressOf());
}

ShapeText::~ShapeText()
{

}

D2D1_POINT_2F ShapeText::center() const
{
	return { (rect.left + rect.right) / 2.f, (rect.top + rect.bottom) / 2.f };
}

void ShapeText::fitRectToText()
{
	if (!textLayout) return;
	DWRITE_TEXT_METRICS metrics{};
	if (FAILED(textLayout->GetMetrics(&metrics))) return;
	auto w = metrics.width + borderPadding * 2.f;
	auto h = metrics.height + borderPadding * 2.f;
	// 空文本时度量是 0，框会缩成一个点；留一个手柄大小的最小尺寸
	auto minSize = borderPadding * 2.f;
	rect.right = rect.left + (w > minSize ? w : minSize);
	rect.bottom = rect.top + (h > minSize ? h : minSize);
}

void ShapeText::paint(ID2D1DeviceContext* ctx)
{
	if (isEditing) {
		// 编辑中文字由 TextBox 自己那层画，这里不重复画（重复的话会和它错位、还会糊在一起）。
		// 它的宽高跟着文本内容长，顺手把框的矩形同步过来 —— 本函数在 yoga 排布之后才跑，取到的是本帧的值。
		// TextBox 的 x/y/w/h 是窗口坐标（物理像素），rect 存的是底图坐标，差一个缩放倍数
		auto tb = win->getTextBox();
		auto s = win->getScale();
		rect = D2D1::RectF(tb->x / s, tb->y / s, (tb->x + tb->w) / s, (tb->y + tb->h) / s);
		return;
	}
	// makeTextLayout 要等编辑结束才跑，这之前可能先来一次 paint
	if (!textLayout) return;
	D2D1_MATRIX_3X2_F prev{};
	ctx->GetTransform(&prev);
	if (angle != 0.f) {
		// 旋转叠在当前变换之后（矩阵左乘 = 先缩放再旋转），所以中心要用缩放后的坐标
		ctx->SetTransform(prev * D2D1::Matrix3x2F::Rotation(angle, transformPoint(ctx, center())));
	}
	ctx->DrawTextLayout({ rect.left + borderPadding, rect.top + borderPadding },
		textLayout.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
	if (angle != 0.f) ctx->SetTransform(prev);
}

void ShapeText::paintDragger(ID2D1DeviceContext* ctx)
{
	// 虚线框画在这里而不是 paint 里：导出图片走的是离屏 paint(ctx)，画在那边会被存进图里。
	// 本函数只在 hover 且没按下鼠标时调，正好是该显示提示框的时候。
	// 手柄位置在基类里一次算完（按外接框的右下角，与右上角的 × 对称）。原来这里还要
	// 把方框中心再绕中心转 angle，现在不用了：手柄属于"外接框"而不是文字本身，
	// 转过之后它仍待在框的右下角，三个角上的按钮才对得齐
	updateRotateDragger();
	D2D1_MATRIX_3X2_F prev{};
	ctx->GetTransform(&prev);
	if (angle != 0.f) {
		ctx->SetTransform(prev * D2D1::Matrix3x2F::Rotation(angle, transformPoint(ctx, center())));
	}
	ctx->DrawRectangle(rect, textBrush.Get(), win->getDpi(), dashedStrokeStyle.Get());
	ctx->SetTransform(prev);
	// 手柄没跟着上面的变换转（见上），这里直接按它自己的坐标画
	paintRotateHandle(ctx);
}

void ShapeText::mouseDrag(const float x, const float y)
{
	if (hoverDraggerIndex == 9) {
		auto c = center();
		// 手柄静止时挂在右下角，鼠标方向减掉静止方向（rotateRestAngle）才是这次转过的角度。
		// 顺时针为正
		angle = rotateAngleAt(c, x, y);
		return;
	}
	if (hoverDraggerIndex != 8) return;
	auto spanX{ x - pressX };
	auto spanY{ y - pressY };
	rect.left += spanX;
	rect.right += spanX;
	rect.top += spanY;
	rect.bottom += spanY;
	pressX = x;
	pressY = y;
}

void ShapeText::mouseDown(const float x, const float y)
{
	if (hoverDraggerIndex == -1) { //首次创建
		// 文字的起笔点就是按下点，虚线框比它外扩一个 borderPadding
		rect = D2D1::RectF(x - borderPadding, y - borderPadding, x + borderPadding, y + borderPadding);
		hoverDraggerIndex = 1;
		startEdit();
	}
	else if (hoverDraggerIndex == 1) { //点在框里：继续编辑
		pressX = x;
		pressY = y;
		startEdit();
	}
	else if (hoverDraggerIndex == 8) { //点在边框上：结束编辑，准备拖动
		pressX = x;
		pressY = y;
		finishEdit();
	}
	else if (hoverDraggerIndex == 9) { //点在旋转手柄上：记下起点，转由 mouseDrag 跟手算
		// 编辑中先退出：编辑期间角度被归零记在 editAngle 里，不退出的话这里转出来的角度
		// 会在下一次 finishEdit 时被 editAngle 盖回去，等于白转
		if (isEditing) finishEdit();
		pressX = x;
		pressY = y;
	}
	else if (hoverDraggerIndex == 0) { //点在框外：结束编辑
		finishEdit();
	}
}

void ShapeText::mouseMove(const float x, const float y)
{
	hoverDraggerIndex = -1;
	// 手柄位置是 paintDragger 里算的，而它只在 hover 时才跑；这里先补算一次，
	// 免得刚把鼠标移上去的那一帧拿着上一次的旧位置判不中
	updateRotateDragger();
	if (isInRect(rotateDragger, x, y)) {
		hoverDraggerIndex = 9;
		return;
	}
	// 转过之后框的可点区域也跟着转了，得把鼠标点逆着角度转回来再判 ——
	// 否则框转了、能点中的那块还留在原处
	auto p = unrotatePoint(D2D1::Point2F(x, y), center(), angle);
	auto lx = p.x, ly = p.y;
	auto half{ borderPadding / 2.f + win->getDpi() };//多给一个 dpi，让判定范围宽松点
	if (lx >= rect.left - half && lx <= rect.right + half && ly >= rect.top - half && ly <= rect.bottom + half)
	{
		// 四条边带：挨着任一条边都算"要拖边框"。
		// 注意上边那条 —— 这里以前笔误写成 ly >= rect.top + half，于是框内凡是
		// ly 比 top+half 大的（也就是中间那大片）全被判成边框，点文字中间走的是
		// "拖框"，永远进不了编辑态。表现就是"选中了文本却改不了它的内容"
		if (lx <= rect.left + half || lx >= rect.right - half || ly <= rect.top + half || ly >= rect.bottom - half) {
			hoverDraggerIndex = 8;
		}
		else {
			hoverDraggerIndex = 1;
		}
	}
	else if (isEditing) {
		// 编辑中即使鼠标移出去了也要让自己保持 shapeHover，
		// 这样下一次点击才会走到自己的 mouseDown（结束编辑），而不是新建一个文本
		hoverDraggerIndex = 0;
	}
}
// 滚轮调字号。只在光标停在文字上时收得到（WinPin 把滚轮转给 shapeHover），
// 与矩形/序号那几个"滚轮调尺寸"是同一套用法
void ShapeText::mouseWheel(const float x, const float y, const short delta)
{
	// 一格走两个逻辑像素：字号值域是 10~60，一格的步子太小得滚很多下
	auto step{ 2.f * win->getDpi() };
	auto next = fontSize + (delta < 0 ? -step : step);
	// 夹到滑块的値域里，返回的就是最终生效的字号（物理像素）；顶到头了直接返回
	auto applied = win->getToolSub()->setShapeSliderVal(L"text", next);
	if (applied == fontSize) return;
	fontSize = applied;
	if (isEditing) {
		// 编辑中文字由 TextBox 画，它收逻辑像素，中间隔着缩放与 dpi 两个换算
		win->getTextBox()->setFontSize(fontSize * win->getScale() / win->getDpi());
	}
	else {
		makeTextLayout();
		fitRectToText();
	}
	win->refresh();
}

void ShapeText::setCursor()
{
	if (hoverDraggerIndex == 9) {
		SetCursor(LoadCursor(nullptr, IDC_CROSS));
	}
	else if (hoverDraggerIndex == 8) {
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	}
	else if (hoverDraggerIndex == 1) {
		SetCursor(LoadCursor(nullptr, IDC_IBEAM));
	}
	else if (hoverDraggerIndex == 0) {
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
	}
}

void ShapeText::startEdit()
{
	if (isEditing) return;
	isEditing = true;
	// 每次进入编辑都跟当前工具栏走：改了颜色/字号再点已有文本，就是要按新样式改
	setAttr();
	// 编辑中的文字是 TextBox 那个真控件画的，D2D 的变换管不到它（缩放当年就撞过同一堵墙，
	// 旋转更没法靠乘一个数糊弄过去）。所以进编辑先把角度归零，退出时再转回去：
	// 代价是编辑的这一瞬间文字会摆正，换来的是不必改 Ling 给 TextBox 加旋转
	editAngle = angle;
	angle = 0.f;
	auto tb = win->getTextBox();
	auto d = win->getDpi();
	// rect 是底图坐标，TextBox 是挂在窗口上的真控件、收的是逻辑像素，
	// 所以要先乘上缩放倍数（窗口坐标）再除以 dpi（逻辑像素）
	auto s = win->getScale();
	tb->setPosition(Ling::Edge::Left, rect.left * s / d);
	tb->setPosition(Ling::Edge::Top, rect.top * s / d);
	// 顺手把布局坐标也写成本 shape 的矩形：TextBox 刚显示出来时 x/y/w/h 还是上一次的旧值，
	// 而紧跟着到来的鼠标事件要靠 isPosIn 判断该不该失焦，拿旧值会把刚拿到的焦点当场弹掉。
	// 下一帧 yoga 会用同样的位置把它们覆盖回来。
	tb->x = rect.left * s;
	tb->y = rect.top * s;
	tb->w = (rect.right - rect.left) * s;
	tb->h = (rect.bottom - rect.top) * s;
	tb->setColor(Ling::Color(colorValue));
	tb->setCaretColor(Ling::Color(colorValue));
	// 内边距也得跟着倍数走：borderPadding 是底图空间的，而 TextBox 的 padding 是窗口空间的
	// （Ling 默认 6.f 逻辑像素），不乘倍数的话提交后文字会比编辑时偏出去 borderPadding*(scale-1)
	tb->setPadding(borderPadding * s / d);
	// fontSize 是底图上的物理像素，setFontSize 收逻辑像素（内部再乘 dpi），中间还差一个缩放倍数
	tb->setFontSize(fontSize * s / d);
	tb->setBold(isBold);
	tb->setItalic(isItalic);
	tb->setFontFamily(fontFamily);
	tb->setText(text);
	tb->show();
	// 订阅放在 setText 之后：setText 自己也会触发 onTextChanged，不用理那一次
	textChangedTok = tb->onTextChanged.add([this](Ling::TextBox*, const std::wstring& val) {
		text = val;
		// 框会随文本长大，虚线框跟着重画；rect 在 paint 里从 TextBox 的实际尺寸同步
		win->refresh();
	});
	focusTok = tb->onFocusChanged.add([this](Ling::TextBox*, bool focused) {
		// 点到别处、按 ESC、窗口失焦都会走到这儿
		if (!focused) finishEdit();
	});
	win->setEditingShape(this);
	tb->focus();
	win->refresh();
}

void ShapeText::finishEdit()
{
	if (!isEditing) return;
	isEditing = false;
	auto tb = win->getTextBox();
	// 先摘订阅：下面的 blur 会再触发一次 onFocusChanged，不摘就会重入
	tb->onTextChanged.remove(textChangedTok);
	tb->onFocusChanged.remove(focusTok);
	textChangedTok = {};
	focusTok = {};
	text = tb->getText();
	tb->blur();
	tb->hide();
	win->setEditingShape(nullptr);
	angle = editAngle;
	makeTextLayout();
	// 编辑期间框是从 TextBox 的实际尺寸同步来的，与 D2D 量出来的文字尺寸可能有零点几像素的差，
	// 收工时以 layout 的度量为准重算一次，虚线框与命中判定才和画出来的文字严格对齐
	fitRectToText();
	win->refresh();
	if (text.empty()) {
		// 空文本不留痕：点一下没输入就走开，不该在 history 里攒一堆看不见的 shape。
		// 不能在这儿直接删 —— 本函数是从 shape 自己的事件回调里调进来的，删了后面还要用 this。
		Ling::App::get()->dq.TryEnqueue([w = win, self = this]() {
			w->history->removeShape(self);
		});
	}
}

void ShapeText::applyStyle()
{
	// 非编辑态也要生效：选中一段已经写完的文字再改工具条上的颜色 / 字号 / 字体，
	// 改的就是它。以前这里只认编辑态，选中态改样式一点反应没有，
	// 只能靠"再点进去重新编辑一次"把工具条的配置带进来
	setAttr();
	makeTextLayout();
	if (isEditing) {
		auto tb = win->getTextBox();
		tb->setColor(Ling::Color(colorValue));
		tb->setCaretColor(Ling::Color(colorValue));
		tb->setFontSize(fontSize * win->getScale() / win->getDpi());
		tb->setBold(isBold);
		tb->setItalic(isItalic);
		tb->setFontFamily(fontFamily);
	}
	else {
		// 字号变了框的大小就得跟着走，否则虚线框与文字对不上，命中判定也跟着偏
		fitRectToText();
	}
	win->refresh();
}

void ShapeText::makeTextLayout()
{
	// 不折行，宽高都放开，与 TextBox 的 autoSize 一致
	textLayout = Ling::D2D::get()->makeTextLayout(text, fontSize);
	if (!textLayout) return;
	textLayout->SetFontWeight(isBold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, { 0, INT_MAX });
	textLayout->SetFontStyle(isItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, { 0, INT_MAX });
	// 字体名要在建完 layout 之后单独设：Ling 的 makeTextLayout 用的是系统字体集合，
	// 系统字体换族名在同一集合内就能换（自定义字体才需要换 format，见 D2D::getTextFormat）
	if (!fontFamily.empty()) textLayout->SetFontFamilyName(fontFamily.c_str(), { 0, INT_MAX });
}

void ShapeText::setAttr()
{
	auto toolSub = win->getToolSub();
	colorValue = toolSub->getSelectedColorValue();
	color = toolSub->getSelectedColor();
	// getSliderVal 返回的已经是物理像素
	fontSize = toolSub->getSliderVal();
	isBold = toolSub->isTextBold;
	isItalic = toolSub->isTextItalic;
	fontFamily = toolSub->getFontFamily();
	Ling::D2D::get()->deviceContext->CreateSolidColorBrush(color, textBrush.ReleaseAndGetAddressOf());
}

bool ShapeText::getShapeBounds(D2D1_RECT_F& out) const
{
	// 转过的文字按"看得见的那一块"给动作图标定位，不然 × 会飘在虚线框外面
	out = rotatedBounds(rect, angle);
	return true;
}
