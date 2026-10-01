#include "pch.h"
#include "App.h"
#include "Canvas.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "History.h"
#include "ShapeNumber.h"

using Microsoft::WRL::ComPtr;

namespace {
	// 字母序号走 Excel 列名那套进位法：1->a、26->z、27->aa。没有 0，也从不像 roman 那样用减号形式
	std::wstring toAlpha(int val, bool upper)
	{
		std::wstring result;
		while (val > 0)
		{
			auto n = (val - 1) % 26;
			result.insert(result.begin(), (wchar_t)(upper ? L'A' : L'a') + n);
			val = (val - 1) / 26;
		}
		return result;
	}
	std::wstring toRoman(int val)
	{
		// 罗马数字用 ASCII 字母拼，不用 U+2160 那批符号 —— 后者在微软雅黑里不一定有字形
		static const std::pair<int, const wchar_t*> table[]{
			{1000,L"M"},{900,L"CM"},{500,L"D"},{400,L"CD"},{100,L"C"},{90,L"XC"},
			{50,L"L"},{40,L"XL"},{10,L"X"},{9,L"IX"},{5,L"V"},{4,L"IV"},{1,L"I"}
		};
		std::wstring result;
		for (auto& [num, sym] : table)
		{
			while (val >= num) {
				result += sym;
				val -= num;
			}
		}
		return result;
	}
	const wchar_t* chineseDigits[]{ L"零",L"一",L"二",L"三",L"四",L"五",L"六",L"七",L"八",L"九" };
	std::wstring toChinese(int val)
	{
		std::wstring result;
		if (val < 10) return chineseDigits[val];
		if (val == 10) return L"十";
		if (val < 20) return L"十" + std::wstring(chineseDigits[val - 10]);
		if (val < 100) {
			auto tens = val / 10;
			result = chineseDigits[tens] + std::wstring(L"十");
			if (val % 10) result += chineseDigits[val % 10];
			return result;
		}
		// 三位以上基本用不上，拼不出也不该猜，原样给阿拉伯数字让用户看得懂
		return std::to_wstring(val);
	}
}

ShapeNumber::ShapeNumber(Canvas* win) :ShapeBase(win), draggers{
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0) },
	// 半径就是工具栏滑块的值（物理像素），跟别的工具的"线宽"是同一个滑块。
	// 拖拽/滚轮改过之后会回写给滑块（见 ToolSub::setShapeSliderVal），所以后面新建的序号沿用同一大小，
	// 关掉应用再打开也还是这个大小 —— 值存在 config.json 的 toolPin.number.radius 里
	r{ win->getToolSub()->getSliderVal() },
	val{ getNextVal(win) }
{
	auto toolSub = win->getToolSub();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(), brush.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0XFFFFFF), brushText.GetAddressOf());
	setAttr();
}

int ShapeNumber::getNextVal(Canvas* win)
{
	int maxVal{ 0 };
	for (auto& shape : win->history->shapes) {
		auto number = dynamic_cast<ShapeNumber*>(shape.get());
		if (number && !number->isUndo && number->val > maxVal) {
			maxVal = number->val;
		}
	}
	return maxVal + 1;
}

ShapeNumber::~ShapeNumber()
{
}

std::wstring ShapeNumber::serializeVal(const int val, const NumStyle style)
{
	switch (style)
	{
	case NumStyle::AlphaLower: return toAlpha(val, false);
	case NumStyle::AlphaUpper: return toAlpha(val, true);
	case NumStyle::Roman: return toRoman(val);
	case NumStyle::Chinese: return toChinese(val);
	default: return std::to_wstring(val);
	}
}

void ShapeNumber::setAttr()
{
	auto toolSub = win->getToolSub();
	numStyle = static_cast<NumStyle>(toolSub->numberStyle);
	ringStyle = static_cast<RingStyle>(toolSub->numberRing);
	isFill = toolSub->isNumberFill;
	// 画刷建一次就够，后续只换颜色；brushText 恒为白色（填底色时数字用）
	colorValue = toolSub->getSelectedColorValue();
	brush->SetColor(Ling::Color(colorValue).getD2DColor());
}

void ShapeNumber::applyStyle()
{
	setAttr();
	makePath();
	makeTextLayout();
}

std::wstring ShapeNumber::displayText()
{
	return customText.empty() ? serializeVal(val, numStyle) : customText;
}

// 把序号排到 2r × 2r 的方框里居中，字号取 r（直径的一半），刚好填满圆
void ShapeNumber::makeTextLayout()
{
	auto d2d = Ling::D2D::get();
	auto text = displayText();
	d2d->dwriteFactory->CreateTextLayout(text.data(), (UINT32)text.length(),
		d2d->baseTextFormat.Get(), r * 2, r * 2, layoutText.ReleaseAndGetAddressOf());
	if (!layoutText) return;
	layoutText->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
	layoutText->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	layoutText->SetFontSize(r, { 0, (UINT32)text.length() });
}

void ShapeNumber::paint(ID2D1DeviceContext* ctx)
{
	// makeTextLayout 要等第一次 mouseDown 才跑，这之前可能先来一次 paint
	if (!layoutText) return;
	// 没有外圈时也就没有底色可填，此时文字必须改用画笔本身的颜色 —— 白字画在浅色底上会看不见
	auto hasRing = ringStyle != RingStyle::None;
	auto hasFill = isFill && hasRing;
	if (path) {
		if (hasFill) {
			ctx->FillGeometry(path.Get(), brush.Get());
		}
		else {
			ctx->DrawGeometry(path.Get(), brush.Get(), win->getDpi());
		}
	}
	ctx->DrawTextLayout({ cx - r,cy - r }, layoutText.Get(),
		hasFill ? brushText.Get() : brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void ShapeNumber::paintDragger(ID2D1DeviceContext* ctx)
{
	if (isWheel) return;
	for (auto& dragger : draggers)
	{
		// 选中的填实、悬停的留空：光标掠过一串元素时能分出改样式会作用到谁
		ctx->DrawRectangle(dragger, brushDragger.Get(), win->getDpi());
		if (win->selected == this) ctx->FillRectangle(dragger, brushDragger.Get());
	}
}

void ShapeNumber::setValAndPush(const int newVal)
{
	val = newVal;
	ShapeNumber* conflict{ nullptr };
	for (auto& shape : win->history->shapes)
	{
		auto number = dynamic_cast<ShapeNumber*>(shape.get());
		if (number && number != this && !number->isUndo && number->val == val) {
			conflict = number;
			break;
		}
	}
	// 撞号的那个顶到新值的下一位；它再撞上别的就继续顶，直到全表不重号
	if (conflict) conflict->setValAndPush(newVal + 1);
}

void ShapeNumber::onKey(UINT key)
{
	if (isEditing) return;
	// 小键盘的 +/- 是另外两个虚拟键，主键盘上是 OEM_PLUS / OEM_MINUS
	if (key == VK_F2) {
		startEdit();
		return;
	}
	int delta{ 0 };
	if (key == VK_OEM_PLUS || key == VK_ADD) delta = 1;
	else if (key == VK_OEM_MINUS || key == VK_SUBTRACT) delta = -1;
	else return;
	auto next = val + delta;
	if (next < 1) return;
	setValAndPush(next);
	// 级联会动到别的序号的数字，它们的 layout 也得重建
	for (auto& shape : win->history->shapes)
	{
		auto number = dynamic_cast<ShapeNumber*>(shape.get());
		if (number && !number->isUndo) number->makeTextLayout();
	}
	win->refresh();
}

void ShapeNumber::mouseDrag(const float x, const float y)
{
	if (hoverDraggerIndex == 0) {
		auto spanX{ x - pressX };
		auto spanY{ y - pressY };
		cx += spanX;
		cy += spanY;
		makePath();
		pressX = x;
		pressY = y;
	}
	else if (hoverDraggerIndex == 1) {
		angle = -atan2f(y - cy, x - cx) * 180.f / 3.14159265358979323846f;
		makePath();
	}
	else if (hoverDraggerIndex == 2) {
		auto dx{ x - cx };
		auto dy{ y - cy };
		r = sqrtf(dx * dx + dy * dy);
		auto minR{ 8.f * win->getDpi() };
		if (r < minR) r = minR;
		// 用它夹好的返回值：半径不能超出工具栏滑块的值域，否则滑块显示的就不是真实大小了
		r = win->getToolSub()->setShapeSliderVal(L"number", r);
		makePath();
		makeTextLayout();
	}
}

void ShapeNumber::mouseDown(const float x, const float y)
{
	if (hoverDraggerIndex == -1) { //首次创建
		cx = x;
		cy = y;
		pressX = cx;
		pressY = cy;
		hoverDraggerIndex = 0;
		makePath();
		makeTextLayout();
		win->refresh();
	}
	else if (hoverDraggerIndex >= 0) {
		pressX = x;
		pressY = y;
	}
}

void ShapeNumber::mouseUp(const float x, const float y)
{
	auto half{ draggerSize / 2 };
	draggers[0].left = cx - half;
	draggers[0].top = cy - half;
	draggers[0].right = cx + half;
	draggers[0].bottom = cy + half;

	draggers[1].left = tip.x - half;
	draggers[1].top = tip.y - half;
	draggers[1].right = tip.x + half;
	draggers[1].bottom = tip.y + half;

	draggers[2].left = mid.x - half;
	draggers[2].top = mid.y - half;
	draggers[2].right = mid.x + half;
	draggers[2].bottom = mid.y + half;
}

void ShapeNumber::mouseMove(const float x, const float y)
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
	else if (isInRect(draggers[2], x, y))
	{
		hoverDraggerIndex = 2;
	}
	if (isWheel) {
		isWheel = false;
		mouseUp(x, y);
		win->refresh();
	}
}

void ShapeNumber::mouseWheel(const float x, const float y, const short delta)
{
	isWheel = true;
	if (delta < 0) {
		if (r <= 6.f * win->getDpi()) return;
		r--;
	}
	else {
		r++;
	}
	r = win->getToolSub()->setShapeSliderVal(L"number", r);
	makePath();
	makeTextLayout();
	win->refresh();
}

void ShapeNumber::setCursor()
{
	if (hoverDraggerIndex >= 0) {
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	}
}

D2D1_POINT_2F ShapeNumber::localPoint(const float degrees)
{
	float radians = degrees * 3.14159265358979323846f / 180.f;
	return D2D1::Point2F(r * cosf(radians), -r * sinf(radians));
}

D2D1_POINT_2F ShapeNumber::transformPoint(const D2D1_POINT_2F& point)
{
	float radians = -angle * 3.14159265358979323846f / 180.f;
	float cosValue = cosf(radians);
	float sinValue = sinf(radians);
	return D2D1::Point2F(
		cx + point.x * cosValue - point.y * sinValue,
		cy + point.x * sinValue + point.y * cosValue
	);
}

void ShapeNumber::makePath()
{
	auto d2d = Ling::D2D::get();
	if (ringStyle == RingStyle::None) {
		// 只要数字本身。指向箭头的那个夹点仍在老位置上悬着，但没有东西可画，
		// 用户此刻也不需要转方向
		path.Reset();
		tip = transformPoint(D2D1::Point2F(r + r / 3.f, 0.f));
		mid = transformPoint(localPoint(180.f));
		return;
	}
	// ReleaseAndGetAddressOf 而不是 GetAddressOf：后者不放旧对象，拖动时每个鼠标事件漏一个几何体
	d2d->d2dFactory->CreatePathGeometry(path.ReleaseAndGetAddressOf());
	ComPtr<ID2D1GeometrySink> sink;
	path->Open(sink.GetAddressOf());
	tip = transformPoint(D2D1::Point2F(r + r / 3.f, 0.f));
	if (ringStyle == RingStyle::Circle) {
		auto start = transformPoint(localPoint(10.f));
		mid = transformPoint(localPoint(180.f));
		auto end = transformPoint(localPoint(350.f));
		sink->BeginFigure(start, D2D1_FIGURE_BEGIN_FILLED);
		sink->AddArc(D2D1::ArcSegment(mid, D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
		sink->AddArc(D2D1::ArcSegment(end, D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
		sink->AddLine(tip);
		sink->AddLine(start);
	}
	else {
		// 方框：半边长取 r 的 0.8，外圈与圆版的视觉面积接近。
		// 起点是离箭头最近的那个角，把离箭头最近的那条边让出去给尾巴（与圆版对齐）
		auto h = r * 0.8f;
		auto p1 = transformPoint(D2D1::Point2F(h, -h));
		auto p2 = transformPoint(D2D1::Point2F(-h, -h));
		auto p3 = transformPoint(D2D1::Point2F(-h, h));
		auto p4 = transformPoint(D2D1::Point2F(h, h));
		mid = p3;
		sink->BeginFigure(p1, D2D1_FIGURE_BEGIN_FILLED);
		sink->AddLine(p2);
		sink->AddLine(p3);
		sink->AddLine(p4);
		sink->AddLine(tip);
		sink->AddLine(p1);
	}
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	sink->Close();
}

void ShapeNumber::startEdit()
{
	if (isEditing) return;
	isEditing = true;
	auto tb = win->getTextBox();
	auto d = win->getDpi();
	auto s = win->getScale();
	// tb 的位置与字号收逻辑像素，而 cx/cy/r 都是底图上的物理像素，中间隔着缩放与 dpi 两个换算
	tb->setPosition(Ling::Edge::Left, (cx - r) * s / d);
	tb->setPosition(Ling::Edge::Top, (cy - r) * s / d);
	tb->setFontSize(r * s / d);
	tb->setColor(Ling::Color(colorValue));
	tb->setCaretColor(Ling::Color(colorValue));
	tb->setText(customText);
	tb->show();
	// 订阅放在 setText 之后：setText 自己也会触发 onTextChanged，不用理那一次
	textChangedTok = tb->onTextChanged.add([this](Ling::TextBox*, const std::wstring& val) {
		customText = val;
		makeTextLayout();
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

void ShapeNumber::finishEdit()
{
	if (!isEditing) return;
	isEditing = false;
	auto tb = win->getTextBox();
	// 先摘订阅：下面的 blur 会再触发一次 onFocusChanged，不摘就会重入
	tb->onTextChanged.remove(textChangedTok);
	tb->onFocusChanged.remove(focusTok);
	textChangedTok = {};
	focusTok = {};
	customText = tb->getText();
	tb->blur();
	tb->hide();
	win->setEditingShape(nullptr);
	makeTextLayout();
	win->refresh();
}
