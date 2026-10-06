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

ShapeNumber::ShapeNumber(Canvas* win, bool preview) :ShapeBase(win), draggers{
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0) },
	// 半径就是工具栏滑块的值（物理像素），跟别的工具的"线宽"是同一个滑块。
	// 拖拽/滚轮改过之后会回写给滑块（见 ToolSub::setShapeSliderVal），所以后面新建的序号沿用同一大小，
	// 关掉应用再打开也还是这个大小 —— 值存在 config.json 的 toolPin.number.radius 里
	r{ win->getToolSub()->getSliderVal() },
	// 编号取自工具条上那个输入框，取完就自增并回填 —— 所以第一笔是 1、第二笔是 2。
	// 想从别的数起，直接改输入框（见 ToolSub::numberNext），而每次重新进入标号工具都会回到 1。
	// preview 那个实例不领号：它还没落下，推进计数会让真正落下的那一笔跳号
	val{ preview ? win->getToolSub()->peekNumberVal() : win->getToolSub()->takeNumberVal() },
	// 描述文本默认落在圆圈的右下方：转折点比圆心低 0.9r，引线就有了一段看得见的斜线。
	// 与圆心同高的话那一段会退化成一条直线，跟"折线引线"就不是一个样子了
	descDx{ r + descGap() },
	descDy{ r * 0.9f }
{
	auto toolSub = win->getToolSub();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(), brush.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0XFFFFFF), brushText.GetAddressOf());
	setAttr();
}

ShapeNumber::~ShapeNumber()
{
}

std::unique_ptr<ShapeBase> ShapeNumber::clone(const float dx, const float dy) const
{
	return cloneSelf(*this, dx, dy);
}

void ShapeNumber::fixupCopy()
{
	// 复制出来的一份永远不在编辑态。那两个订阅句柄是挂在 Canvas 那个共用 TextBox 上的，
	// 照抄过来会让新的一份以为自己在编辑，收尾时还会去摘别人家的订阅
	isEditing = false;
	textChangedTok = {};
	focusTok = {};
	// 画刷重建一份（圈 / 方框的底色 + 圈里的白字）：ComPtr 拷过来是同一支，
	// 改一方的颜色会连另一方一起改
	auto d2d = Ling::D2D::get();
	if (brush) {
		auto color = brush->GetColor();
		d2d->deviceContext->CreateSolidColorBrush(color, brush.ReleaseAndGetAddressOf());
	}
	if (brushText) {
		auto color = brushText->GetColor();
		d2d->deviceContext->CreateSolidColorBrush(color, brushText.ReleaseAndGetAddressOf());
	}
}

void ShapeNumber::translate(const float dx, const float dy)
{
	cx += dx;
	cy += dy;
	// 路径、描述引线（descJoint 也读 cx/cy）、四个动作按钮都按圆心现算；
	// 三个夹点原本只在 mouseUp 里重算，这里直接借它走一遍
	makePath();
	makeTextLayout();
	mouseUp(cx, cy);
}

// 鼠标还没落笔时，把"将要落下的那个编号"画在光标处。样式取工具条当前那一份，
// 所以它就是最终效果的预演；不落进 history、也不推进计数（见 ctor 的 preview）
void ShapeNumber::previewAt(const float x, const float y, const int previewVal)
{
	val = previewVal;
	cx = x;
	cy = y;
	// applyStyle 会把颜色 / 外圈取一遍再按新位置重建几何与文字，一次到位
	applyStyle();
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
	// 圆圈里只放编号本身。描述文本另排一份 layout 摆在圆圈外面（见 paintDesc）
	return serializeVal(val, numStyle);
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
	// 描述文本：它排在圆圈外面，宽度不设限（画多宽由文字自己决定，横线按它的实际宽度画），
	// 字号与编号同大。空描述就没有这一份
	layoutDesc.Reset();
	if (!customText.empty()) layoutDesc = Ling::D2D::makeTextLayout(customText, r);
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
	paintDesc(ctx);
}

// 描述文本的引线：圆周 -> 斜线 -> 转折点 -> 横线（文字压在横线上）。
// 以前是一条直线从圆周直接连到文字末端，线正好从文字身上穿过去，字和线糊在一起
void ShapeNumber::paintDesc(ID2D1DeviceContext* ctx)
{
	if (!layoutDesc) return;
	// 编辑中整段引线都不画：文字此刻由 TextBox 自己那一层画，引线还挂在老位置上
	// 会从输入框底下穿过去，看着像把输入框划了一道（用户要求：编辑完才显示折线）
	if (isEditing) return;
	DWRITE_TEXT_METRICS metrics{};
	layoutDesc->GetMetrics(&metrics);
	auto joint = descJoint();
	auto textX{ descTextX(metrics.width) };
	auto farX{ descLineFar(metrics.width) };
	// 斜线的起点取圆周上朝着转折点的那一点，线因此从圆边出发、不会插进圈里
	auto dx{ joint.x - cx }, dy{ joint.y - cy };
	auto len{ sqrtf(dx * dx + dy * dy) };
	D2D1_POINT_2F from{ cx, cy };
	if (len > 0.001f) {
		from.x += dx / len * r;
		from.y += dy / len * r;
	}
	ctx->DrawLine(from, joint, brush.Get(), win->getDpi());
	ctx->DrawLine(joint, D2D1::Point2F(farX, joint.y), brush.Get(), win->getDpi());
	ctx->DrawTextLayout({ textX, joint.y - metrics.height }, layoutDesc.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

float ShapeNumber::descTextX(float textW) const
{
	// 落点在圆心右侧：文字排在转折点右边；拖到左侧就整段翻过去，连横线一起镜像
	return descDx >= 0.f ? cx + descDx + descLead() : cx + descDx - descLead() - textW;
}

float ShapeNumber::descLineFar(float textW) const
{
	// 横线末端从文字再往外伸一个 descLead：文字两侧都压在线上，看着才像"压在一条引线上"
	return descDx >= 0.f ? descTextX(textW) + textW + descLead() : descTextX(textW) - descLead();
}

D2D1_POINT_2F ShapeNumber::descTextPos() const
{
	DWRITE_TEXT_METRICS metrics{};
	if (layoutDesc) layoutDesc->GetMetrics(&metrics);
	// 还没写描述（第一次按 A）时量不到高度，按字号估一行 —— 否则输入框会落在比文字
	// 低一行的地方，敲下第一个字才跳到最终位置
	else metrics.height = r;
	return D2D1::Point2F(descTextX(metrics.width), descJoint().y - metrics.height);
}

D2D1_RECT_F ShapeNumber::descHandleRect() const
{
	auto half{ draggerSize / 2 };
	auto joint = descJoint();
	// 抓手摆在转折点上：那儿既不在文字里、也不在圈里，任何长度 / 任何一侧都点得到
	return D2D1::RectF(joint.x - half, joint.y - half, joint.x + half, joint.y + half);
}

D2D1_RECT_F ShapeNumber::descTextRect() const
{
	DWRITE_TEXT_METRICS metrics{};
	if (layoutDesc) layoutDesc->GetMetrics(&metrics);
	auto pos = descTextPos();
	// 四周各放 2px 余量：贴着字形边缘点很难一次点中
	return D2D1::RectF(pos.x - 2.f, pos.y - 2.f, pos.x + metrics.width + 2.f, pos.y + metrics.height + 2.f);
}

D2D1_RECT_F ShapeNumber::editHitRect() const
{
	// 四个动作按钮伸到圆心外 (r + btnR + gap)·√2/2 + btnR ≈ 1.64r，取 1.8r 再留一点余量。
	// 开启编辑的那一下点击落在 A 按钮上（圆圈的右下角），必须落在命中区里
	auto rad{ r * 1.8f + draggerSize };
	D2D1_RECT_F box{ cx - rad, cy - rad, cx + rad, cy + rad };
	// 输入框那一块也算进来：它才是编辑器真正待的地方，两处一起框住就万无一失
	DWRITE_TEXT_METRICS metrics{};
	if (layoutDesc) layoutDesc->GetMetrics(&metrics);
	else metrics.height = r;
	auto pos = descTextPos();
	auto right{ pos.x + metrics.width }, bottom{ pos.y + metrics.height };
	if (pos.x < box.left) box.left = pos.x;
	if (pos.y < box.top) box.top = pos.y;
	if (right > box.right) box.right = right;
	if (bottom > box.bottom) box.bottom = bottom;
	return box;
}

void ShapeNumber::paintDragger(ID2D1DeviceContext* ctx)
{
	if (isWheel) return;
	// 圆心那个控制点不画：它正好压在数字上，选中时把编号挡得看不清。
	// 徽章本身照样能拖 —— 命中判定还在 draggers[0] 上（见 mouseMove），
	// 选中与否由圆圈外那几个动作按钮显示
	if (hasTail()) {
		// 无尾的样式没有"指向"可言（见 hasTail），只有带尾的才有那两个控制点。
		// 选中的填白、悬停的留空：光标掠过一串元素时能分出改样式会作用到谁。
		// 先填后描：描边是压在矩形边线中线上的，先描再填会把内半边盖掉，线看着只剩外半截
		for (size_t i = 1; i <= 2; i++) {
			if (win->selected == this) ctx->FillRectangle(draggers[i], brushDraggerFill.Get());
			ctx->DrawRectangle(draggers[i], brushDragger.Get(), win->getDpi());
		}
	}
	// 描述文本落点的控制点：没写描述就没有可拖的东西
	if (layoutDesc) {
		auto handle = descHandleRect();
		if (win->selected == this) ctx->FillRectangle(handle, brushDraggerFill.Get());
		ctx->DrawRectangle(handle, brushDragger.Get(), win->getDpi());
	}
	// 圆圈外那四个动作按钮。paintDragger 只会为选中或悬停的序号调用（见 WinPin::layout），
	// 所以走到这儿就说明该显示它们
	paintOpBtn(ctx, valuePlus, OpBtn::Plus);
	paintOpBtn(ctx, valueMinus, OpBtn::Minus);
	paintOpBtn(ctx, valueRemove, OpBtn::Remove);
	paintOpBtn(ctx, valueText, OpBtn::Text);
}

void ShapeNumber::updateValueBtns()
{
	// 四个按钮摆在圆圈外的四个斜角上，恒不跟着 angle 转 —— 它们是"点这里改这个号"的动作，
	// 不是指向图上的某个位置，转了反而不知道该点哪儿（pixpin 也是这么摆的）。
	// 半径取 0.45r：再大就把旁边的标注压住了，再小又点不准
	auto btnR{ r * 0.45f };
	auto gap{ btnR * 0.5f };
	auto box = [btnR](float px, float py) {
		return D2D1::RectF(px - btnR, py - btnR, px + btnR, py + btnR);
	};
	// 中心到按钮中心的距离：斜向走 (r + btnR + gap) 正好让按钮贴在圆周外
	auto d{ (r + btnR + gap) * 0.70710678f };
	valuePlus = box(cx - d, cy - d);      // 左上：编号 +1
	valueMinus = box(cx - d, cy + d);     // 左下：编号 −1
	valueRemove = box(cx + d, cy - d);    // 右上：删掉这个编号
	valueText = box(cx + d, cy + d);      // 右下：加一段描述文本
}

void ShapeNumber::paintOpBtn(ID2D1DeviceContext* ctx, const D2D1_RECT_F& box, OpBtn kind)
{
	auto c = D2D1::Point2F((box.left + box.right) / 2.f, (box.top + box.bottom) / 2.f);
	auto rad{ (box.right - box.left) / 2.f };
	// 先垫一层白圆再描边：按钮是直接压在底图上的，没有这层会和底图糊在一起
	ctx->FillEllipse(D2D1::Ellipse(c, rad, rad), brushDraggerFill.Get());
	ctx->DrawEllipse(D2D1::Ellipse(c, rad, rad), brushDragger.Get(), win->getDpi());
	// 线宽取控制点那个尺度。+ / − / × 都在圆里用线画，只有 A 是字形
	auto arm{ rad * 0.55f };
	auto stroke{ draggerSize * 0.15f };
	if (kind == OpBtn::Text) {
		// 字号取按钮直径的 0.7，字形不随半径变就复用上一份
		auto fontSize{ rad * 1.4f };
		if (!layoutBtnText || btnTextSize != fontSize) {
			btnTextSize = fontSize;
			layoutBtnText = Ling::D2D::makeTextLayout(L"A", fontSize, rad * 2, rad * 2);
			layoutBtnText->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
			layoutBtnText->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		}
		ctx->DrawTextLayout({ c.x - rad, c.y - rad }, layoutBtnText.Get(), brushDragger.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
		return;
	}
	if (kind == OpBtn::Plus || kind == OpBtn::Minus) {
		// 横线恒有，+ 再加一条竖线
		ctx->DrawLine({ c.x - arm, c.y }, { c.x + arm, c.y }, brushDragger.Get(), stroke);
		if (kind == OpBtn::Plus) ctx->DrawLine({ c.x, c.y - arm }, { c.x, c.y + arm }, brushDragger.Get(), stroke);
	}
	else {
		// × 就是"+"转 45 度，即两条斜线
		auto k{ arm * 0.70710678f };
		ctx->DrawLine({ c.x - k, c.y - k }, { c.x + k, c.y + k }, brushDragger.Get(), stroke);
		ctx->DrawLine({ c.x - k, c.y + k }, { c.x + k, c.y - k }, brushDragger.Get(), stroke);
	}
}

// 编号加减：本号走一格，比它大的编号全部跟着走一格。
// 于是"在中间插一个号 / 撤掉一个号"之后，后面的编号仍是一段连续的 —— 与 pixpin 一致。
// 不能用"撞号就顶"那套写法：减号方向上顶来顶去还是原来的组合，等于空转
void ShapeNumber::bumpVal(int delta)
{
	auto next = val + delta;
	if (next < 1) return;
	auto from = val;
	for (auto& shape : win->history->shapes)
	{
		auto number = dynamic_cast<ShapeNumber*>(shape.get());
		if (!number || number->isUndo) continue;
		if (number == this) number->val = next;
		else if (number->val > from) number->val += delta;
		// 编号变了，文字跟着重排（后面的编号在这个循环里也是一个个就地改完再排的）
		number->makeTextLayout();
	}
	// 顺移会动到"最大的那个号"：往下减时最大号也跟着退一格，工具条上那个待用编号要跟着退，
	// 否则下一笔会跳过刚空出来的号（见 ToolSub::syncNumberVal）
	win->getToolSub()->syncNumberVal();
	win->refresh();
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
	bumpVal(delta);
}

void ShapeNumber::mouseDrag(const float x, const float y)
{
	// 描述文本的落点可以随便拖：横线从圆周指向它，文字仍压在横线上（见 paintDesc）。
	// 几何不依赖它，所以不用重排，画的时候按新偏移现算
	if (hoverDraggerIndex == HitDesc) {
		descDx += x - pressX;
		descDy += y - pressY;
		pressX = x;
		pressY = y;
		return;
	}
	// 动作按钮是一下就见效的动作，没有可拖的东西
	if (hoverDraggerIndex >= HitPlus) return;
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
	// 四个动作按钮按下即生效，不进拖拽：它们没有"拖大拖小"的语义，
	// 一旦走进下面那条分支就会把 pressX/pressY 记下来，鼠标一动按钮跟着飘。
	// 描述文本的落点是可拖的（HitDesc），要放它过去记按下点
	if (hoverDraggerIndex >= HitPlus && hoverDraggerIndex != HitDesc) {
		switch (hoverDraggerIndex) {
		case HitPlus: bumpVal(1); break;
		case HitMinus: bumpVal(-1); break;
		case HitRemove:
			// 不能在这儿直接删 —— 本函数正是从这个 shape 自己的回调里调进来的，
			// 删了后面还要用 this。排到消息队列下一轮回调里删（同 ShapeText::finishEdit）
			Ling::App::get()->dq.TryEnqueue([w = win, self = this]() {
				w->history->removeShape(self);
			});
			break;
		case HitText: startEdit(); break;
		}
		return;
	}
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

bool ShapeNumber::hitInside(const float x, const float y) const
{
	// 编号文字排在 2r×2r 的方框里（见 makeTextLayout），外圈也正好是这个方框的内切圆 ——
	// 所以这一个方框同时是"文字的范围"和"圈的范围"。再往外让半个控制点，
	// 鼠标压在圈线上也该算数
	auto pad{ draggerSize * 0.5f };
	return x >= cx - r - pad && x <= cx + r + pad && y >= cy - r - pad && y <= cy + r + pad;
}

void ShapeNumber::mouseMove(const float x, const float y)
{
	hoverDraggerIndex = -1;
	// 动作按钮与描述落点都在圆圈外面，先判它们；没命中再看那几个几何控制点。
	// tip / mid 只有带尾的样式才有（见 hasTail），无尾时它们不参与命中
	if (isInRect(valuePlus, x, y))
	{
		hoverDraggerIndex = HitPlus;
	}
	else if (isInRect(valueMinus, x, y))
	{
		hoverDraggerIndex = HitMinus;
	}
	else if (isInRect(valueRemove, x, y))
	{
		hoverDraggerIndex = HitRemove;
	}
	else if (isInRect(valueText, x, y))
	{
		hoverDraggerIndex = HitText;
	}
	else if (layoutDesc && isInRect(descTextRect(), x, y))
	{
		// 点描述文字本身就是"改这段话"：与按 A / F2 同一条路
		hoverDraggerIndex = HitText;
	}
	else if (layoutDesc && isInRect(descHandleRect(), x, y))
	{
		hoverDraggerIndex = HitDesc;
	}
	// 圈 / 方块内部整片都算命中：直接问 hitInside，不再只认圆心那一个小方框 ——
	// 圈画得挺大却非得点正中心才选得中的问题就在这儿
	else if (hitInside(x, y))
	{
		hoverDraggerIndex = 0;
	}
	else if (hasTail() && isInRect(draggers[1], x, y))
	{
		hoverDraggerIndex = 1;
	}
	else if (hasTail() && isInRect(draggers[2], x, y))
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
	// 动作按钮是"点一下"的，给手型；其余控制点都是"拖"的，给四向箭头
	if (hoverDraggerIndex >= HitPlus) {
		SetCursor(LoadCursor(nullptr, IDC_HAND));
	}
	else if (hoverDraggerIndex >= 0) {
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

bool ShapeNumber::hasTail() const
{
	// 只有这两种"指向某处"的样式带尾巴；无尾的圆 / 方才是 pixpin 那种纯圈号
	return ringStyle == RingStyle::CircleArrow || ringStyle == RingStyle::SquareArrow;
}

// 「选择对象」框选要用。范围与 mouseMove 的命中范围对齐：圈 + 带尾时的尾巴尖 +
// 追加的描述文字那一段（描述文字本来就点得中，见 HitDesc）。
// r 是 mouseDown 才落定的，在那之前返回 false —— 框选时它是个还没成形的元素
bool ShapeNumber::getShapeBounds(D2D1_RECT_F& out) const
{
	if (r <= 0.f) return false;
	out = D2D1::RectF(cx - r, cy - r, cx + r, cy + r);
	if (hasTail()) {
		out.left = std::min(out.left, tip.x);
		out.right = std::max(out.right, tip.x);
		out.top = std::min(out.top, tip.y);
		out.bottom = std::max(out.bottom, tip.y);
	}
	if (!customText.empty()) {
		auto d = descTextRect();
		out.left = std::min(out.left, d.left);
		out.right = std::max(out.right, d.right);
		out.top = std::min(out.top, d.top);
		out.bottom = std::max(out.bottom, d.bottom);
	}
	return true;
}

void ShapeNumber::makePath()
{
	auto d2d = Ling::D2D::get();
	if (ringStyle == RingStyle::None) {
		// 只要数字本身。tip / mid 仍按老位置算出来：它们的 dragger 既不画也不响应（见 hasTail），
		// 但 mouseUp 里还会照写一遍，不赋值就是读到垃圾
		path.Reset();
		tip = transformPoint(D2D1::Point2F(r + r / 3.f, 0.f));
		mid = transformPoint(localPoint(180.f));
		updateValueBtns();
		return;
	}
	// ReleaseAndGetAddressOf 而不是 GetAddressOf：后者不放旧对象，拖动时每个鼠标事件漏一个几何体
	d2d->d2dFactory->CreatePathGeometry(path.ReleaseAndGetAddressOf());
	ComPtr<ID2D1GeometrySink> sink;
	path->Open(sink.GetAddressOf());
	tip = transformPoint(D2D1::Point2F(r + r / 3.f, 0.f));
	auto tail = hasTail();
	// 半径控制点就挂在"离尾巴最远的那一点"上（带尾时是 180 度位置）。
	// 无尾时没有半径控制点（见 hasTail），这里仍然算出来：mouseUp 会照写一遍 dragger
	mid = transformPoint(localPoint(180.f));
	if (ringStyle == RingStyle::Circle || ringStyle == RingStyle::CircleArrow) {
		if (tail) {
			// 带尾的：从 10 度开口，两段小弧绕过上方，再把尾巴接出去
			auto start = transformPoint(localPoint(10.f));
			auto bend = transformPoint(localPoint(180.f));
			auto end = transformPoint(localPoint(350.f));
			sink->BeginFigure(start, D2D1_FIGURE_BEGIN_FILLED);
			sink->AddArc(D2D1::ArcSegment(bend, D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
			sink->AddArc(D2D1::ArcSegment(end, D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
			sink->AddLine(tip);
		}
		else {
			// 整圆：三段各 120 度的弧。不用两段 180 度拼 —— 半圆那两个候选弧一样大，
			// SMALL / LARGE 正好落在分界上，画成哪半边全看实现怎么挑
			auto start = transformPoint(localPoint(90.f));
			auto p2 = transformPoint(localPoint(210.f));
			auto p3 = transformPoint(localPoint(330.f));
			sink->BeginFigure(start, D2D1_FIGURE_BEGIN_FILLED);
			for (auto& to : { p2, p3, start }) {
				sink->AddArc(D2D1::ArcSegment(to, D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
			}
		}
	}
	else {
		// 方框：半边长取 r 的 0.8，外圈与圆版的视觉面积接近。
		// 起点是离箭头最近的那个角，把离箭头最近的那条边让出去给尾巴（与圆版对齐）；
		// 无尾时就是一个闭合的矩形，不留缺口
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
		if (tail) sink->AddLine(tip);
	}
	// 收尾必须在分支外面：图没 EndFigure 就 Close()，D2D 会直接报错，
	// 整个几何体作废 —— 圆画不出来，数字又按"有底色"画成白的，序号就整个不见了
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	sink->Close();
	updateValueBtns();
}

void ShapeNumber::startEdit()
{
	if (isEditing) return;
	isEditing = true;
	auto tb = win->getTextBox();
	auto d = win->getDpi();
	auto s = win->getScale();
	// tb 的位置与字号收逻辑像素，而 cx/cy/r 都是底图上的物理像素，中间隔着缩放与 dpi 两个换算。
	// 框就落在描述文本该出现的地方（圆圈右侧那条横线的末端），敲进去的字与收工后画出来的位置一致
	auto pos = descTextPos();
	tb->setPosition(Ling::Edge::Left, pos.x * s / d);
	tb->setPosition(Ling::Edge::Top, pos.y * s / d);
	tb->setFontSize(r * s / d);
	tb->setColor(Ling::Color(colorValue));
	tb->setCaretColor(Ling::Color(colorValue));
	tb->setText(customText);
	// 命中矩形先按"整个序号 + 描述那一块"铺开。开编辑的是 A 按钮那一下点击，它在圆圈边上，
	// 而输入框按所见即所得摆在描述文字的位置 —— 两块并不重叠。命中区要是只算输入框本身，
	// 这一下点击的 TextBox::onDown（它订阅得比 WinPin 晚，排在整套派发之后）就会按"点在框外"
	// 把刚打开的编辑器当场关掉，表现正是"单击 A 没反应"。
	// 下一帧 yoga 会照 setPosition 把 x/y/w/h 覆盖回真实位置，所以这只是给这一下点击用的
	auto hit = editHitRect();
	tb->x = hit.left * s;
	tb->y = hit.top * s;
	tb->w = (hit.right - hit.left) * s;
	tb->h = (hit.bottom - hit.top) * s;
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
