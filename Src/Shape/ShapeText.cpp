#include "pch.h"
#include "App.h"
#include "Util.h"
#include "History.h"
#include "Win/WinPin.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeText.h"

using Microsoft::WRL::ComPtr;

ShapeText::ShapeText(WinPin* win) :ShapeBase(win), borderPadding{ 6.f * win->dpi }
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

D2D1_POINT_2F ShapeText::rotatedPoint(const D2D1_POINT_2F& p)
{
	auto c = center();
	float radians = angle * 3.14159265358979323846f / 180.f;
	float cosValue = cosf(radians);
	float sinValue = sinf(radians);
	float dx = p.x - c.x, dy = p.y - c.y;
	return { c.x + dx * cosValue - dy * sinValue, c.y + dx * sinValue + dy * cosValue };
}

// 旋转中心不是 rect 的中心点本身：画布上可能还压着 WinPin 的缩放变换（Ctrl+滚轮），
// 而导出那条路走的是不带缩放的离屏画布。统一用当前矩阵把中心点映射过去，
// 两条路都不用各写一份换算
D2D1_POINT_2F ShapeText::transformCenter(ID2D1DeviceContext* ctx) const
{
	// ID2D1RenderTarget::GetTransform 返回 void、走出参，没有按值返回的重载
	D2D1_MATRIX_3X2_F m{};
	ctx->GetTransform(&m);
	auto c = center();
	return { c.x * m._11 + c.y * m._21 + m._31, c.x * m._12 + c.y * m._22 + m._32 };
}

void ShapeText::updateRotateDragger()
{
	auto half{ draggerSize / 2 };
	// 手柄摆在框正上方，比框顶再退一个手柄高度，转起来才不和框本身挤在一起
	auto offset = draggerSize * 2.f;
	auto p = rotatedPoint({ (rect.left + rect.right) / 2.f, rect.top - offset });
	rotateDragger = D2D1::RectF(p.x - half, p.y - half, p.x + half, p.y + half);
}

void ShapeText::paint(ID2D1DeviceContext* ctx)
{
	if (isEditing) {
		// 编辑中文字由 TextBox 自己那层画，这里不重复画（重复的话会和它错位、还会糊在一起）。
		// 它的宽高跟着文本内容长，顺手把框的矩形同步过来 —— 本函数在 yoga 排布之后才跑，取到的是本帧的值。
		// TextBox 的 x/y/w/h 是窗口坐标（物理像素），rect 存的是底图坐标，差一个缩放倍数
		auto tb = win->getTextBox();
		auto s = win->scale;
		rect = D2D1::RectF(tb->x / s, tb->y / s, (tb->x + tb->w) / s, (tb->y + tb->h) / s);
		return;
	}
	// makeTextLayout 要等编辑结束才跑，这之前可能先来一次 paint
	if (!textLayout) return;
	D2D1_MATRIX_3X2_F prev{};
	ctx->GetTransform(&prev);
	if (angle != 0.f) {
		// 旋转叠在当前变换之后（矩阵左乘 = 先缩放再旋转），所以中心要用缩放后的坐标
		ctx->SetTransform(prev * D2D1::Matrix3x2F::Rotation(angle, transformCenter(ctx)));
	}
	ctx->DrawTextLayout({ rect.left + borderPadding, rect.top + borderPadding },
		textLayout.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
	if (angle != 0.f) ctx->SetTransform(prev);
}

void ShapeText::paintDragger(ID2D1DeviceContext* ctx)
{
	// 虚线框画在这里而不是 paint 里：导出图片走的是离屏 paint(ctx)，画在那边会被存进图里。
	// 本函数只在 hover 且没按下鼠标时调，正好是该显示提示框的时候。
	updateRotateDragger();
	D2D1_MATRIX_3X2_F prev{};
	ctx->GetTransform(&prev);
	if (angle != 0.f) {
		ctx->SetTransform(prev * D2D1::Matrix3x2F::Rotation(angle, transformCenter(ctx)));
	}
	ctx->DrawRectangle(rect, textBrush.Get(), win->dpi, dashedStrokeStyle.Get());
	ctx->SetTransform(prev);
	// 手柄的坐标已经是转好之后的，不能再跟着上面的变换转一遍
	ctx->DrawRectangle(rotateDragger, textBrush.Get(), win->dpi);
	ctx->FillRectangle(rotateDragger, textBrush.Get());
}

void ShapeText::mouseDrag(const float x, const float y)
{
	if (hoverDraggerIndex == 9) {
		auto c = center();
		// 手柄静止时就在框的正上方，所以 dx=0、dy<0 对应 0 度，顺着屏幕顺时针为正
		angle = atan2f(x - c.x, -(y - c.y)) * 180.f / 3.14159265358979323846f;
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
	auto lx = x, ly = y;
	if (angle != 0.f) {
		auto c = center();
		float radians = -angle * 3.14159265358979323846f / 180.f;
		float cosValue = cosf(radians);
		float sinValue = sinf(radians);
		auto dx = x - c.x, dy = y - c.y;
		lx = c.x + dx * cosValue - dy * sinValue;
		ly = c.y + dx * sinValue + dy * cosValue;
	}
	auto half{ borderPadding / 2.f + win->dpi };//多给一个 dpi，让判定范围宽松点
	if (lx >= rect.left - half && lx <= rect.right + half && ly >= rect.top - half && ly <= rect.bottom + half)
	{
		if (lx <= rect.left + half || lx >= rect.right - half || ly >= rect.top + half || ly >= rect.bottom - half) {
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
	auto d = win->dpi;
	// rect 是底图坐标，TextBox 是挂在窗口上的真控件、收的是逻辑像素，
	// 所以要先乘上缩放倍数（窗口坐标）再除以 dpi（逻辑像素）
	auto s = win->scale;
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
	if (!isEditing) return;
	setAttr();
	auto tb = win->getTextBox();
	tb->setColor(Ling::Color(colorValue));
	tb->setCaretColor(Ling::Color(colorValue));
	tb->setFontSize(fontSize * win->scale / win->dpi);
	tb->setBold(isBold);
	tb->setItalic(isItalic);
	win->refresh();
}

void ShapeText::makeTextLayout()
{
	// 不折行，宽高都放开，与 TextBox 的 autoSize 一致
	textLayout = Ling::D2D::get()->makeTextLayout(text, fontSize);
	if (!textLayout) return;
	textLayout->SetFontWeight(isBold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, { 0, INT_MAX });
	textLayout->SetFontStyle(isItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, { 0, INT_MAX });
}

void ShapeText::setAttr()
{
	auto toolSub = win->toolSub.get();
	colorValue = toolSub->getSelectedColorValue();
	color = toolSub->getSelectedColor();
	// getSliderVal 返回的已经是物理像素
	fontSize = toolSub->getSliderVal();
	isBold = toolSub->isTextBold;
	isItalic = toolSub->isTextItalic;
	Ling::D2D::get()->deviceContext->CreateSolidColorBrush(color, textBrush.ReleaseAndGetAddressOf());
}
