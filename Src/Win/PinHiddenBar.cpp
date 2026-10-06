#include "pch.h"
#include <algorithm>
#include "WinPin.h"
#include "../Tool/ToolMain.h"
#include "../Tool/ToolSub.h"
#include "PinHiddenBar.h"

namespace {
	// 左、上各一扇窗。两边各算各的：贴图归哪条边是拖到哪儿松手决定的，
	// 常出现"左边藏两张、顶边藏一张"，少哪边就不建哪扇窗
	std::unique_ptr<PinHiddenBar> edgeBars[2];

	// 藏起来的贴图按创建顺序轮转取色。之所以不在"藏的那一刻"按第几个藏的去取：
	// 那样放掉中间一张，后面几张的颜色会跟着往前挪一位，同一张图前后不是一个颜色。
	// 八种够用了 —— 同时藏九张的情况本身就说明该换个用法了。
	// 用 const 而不是 constexpr：Ling::Color 的构造不是 constexpr，摆进常量表达式表格里编不过
	const Ling::Color barColors[] = {
		0xE53935ff, 0xFF8F00ff, 0xFDD835ff, 0x43A047ff,
		0x00ACC1ff, 0x1E88E5ff, 0x8E24AAff, 0xD81B60ff,
	};
}

PinHiddenBar::PinHiddenBar(WinPin::BarEdge edge) : Ling::WinBase(), edge(edge)
{
	// 屏幕左上角。用主显示器的工作区而不是 (0,0)：任务栏要是在顶上停靠，
	// 条正好压在它下面，看不见也点不着
	RECT wa{};
	if (SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0)) {
		barX = wa.left;
		barY = wa.top;
	}
	// 顶边那条往右让开一条竖排的宽度：两扇窗都贴着左上角，不让开就叠在同一个角上，
	// 后建的那扇会把先建那扇的开头几像素盖住（topmost 组内后建者在上）
	if (isHorizontal()) barX += (int)((barThick + pad * 2) * dpi);
	x = barX;
	y = barY;
	// 先给个"一条"的尺寸，真正的尺寸由 rebuild 按条数算；这里要的是让 CreateWindowEx
	// 别拿 0 宽高建窗（那会连 WM_MOUSEMOVE 都收不到）
	setSize((isHorizontal() ? barLong : barThick) + pad * 2,
		(isHorizontal() ? barThick : barLong) + pad * 2);
	// WS_EX_NOACTIVATE：这条要一直挂在屏幕角上，绝不能因为它把用户手上的焦点抢走。
	// WS_EX_TOPMOST：贴图窗口本身也是 topmost，不跟上的话全屏应用一起就被压掉了
	createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP);
}

PinHiddenBar::~PinHiddenBar()
{
}

void PinHiddenBar::onCreated()
{
	// 整扇窗口就是几根头发丝，不要底色也不要边框。
	// 左边那条自上而下竖着排（水平居中）；顶边那条自左向右横着排（垂直居中）
	body->setBg(0);
	body->setFlexDirection(isHorizontal() ? Ling::FlexDirection::Row : Ling::FlexDirection::Column);
	body->setAlignItems(Ling::Align::Center);
	body->setPadding(pad);
	onMouseMove.add([this](POINT pos) { this->onMove(pos); });
	onTimer.add([this](UINT id) { this->onTimerCB(id); });
	// 条数由 rebuild 定，这里不 show —— 一条都没有的时候不该在屏幕角上留一扇空窗
}

void PinHiddenBar::onMinMaxInfo(MINMAXINFO* mmi)
{
	// 本窗口只有几十像素，Ling 默认的最小跟踪尺寸是 800x600，不放开就建不出这么小的窗
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}

void PinHiddenBar::sync()
{
	for (int i = 0; i < 2; ++i) {
		auto edge = (WinPin::BarEdge)i;
		if (!edgeBars[i]) {
			if (WinPin::getHiddenPins(edge).empty()) continue;
			edgeBars[i].reset(new PinHiddenBar(edge));
		}
		edgeBars[i]->rebuild();
	}
}

void PinHiddenBar::dispose()
{
	for (auto& b : edgeBars) {
		if (b) b->close();
		b.reset();
	}
}

void PinHiddenBar::rebuild()
{
	auto pins = WinPin::getHiddenPins(edge);
	// 露着的那张可能已经被关掉了。关窗是先销毁窗口、下一轮消息循环才把它从表里摘掉，
	// 而本函数也可能正好被那次摘除叫过来 —— 所以只可能是"它已经不在藏着的那批里了"：
	// 清指针，绝不去解引用它（那可能是已经析构掉的对象）
	if (peek && std::find(pins.begin(), pins.end(), peek) == pins.end()) {
		peek = nullptr;
		killTimer(tickId);
	}
	body->removeAllChildren();
	bars.clear();
	if (pins.empty()) {
		conceal();
		hide();
		return;
	}
	for (size_t i = 0; i < pins.size(); ++i) {
		auto node = body->makeChild<Ling::Node>();
		// 长边 30、厚 4。横排时这两条掉个个儿：条是横着摆的，自左向右接下去
		node->setWidth(isHorizontal() ? barLong : barThick);
		node->setHeight(isHorizontal() ? barThick : barLong);
		// 条只有 4 逻辑像素厚，绝不能被 yoga 当成"空间不够"压掉
		node->setFlexShrink(0.f);
		node->setBg(barColors[pins[i]->getBarColorIndex() % std::size(barColors)]);
		// 最后一条不留缝，否则窗口末了白出 2 像素
		if (i + 1 < pins.size()) {
			if (isHorizontal()) node->setMarginRight(gapW);
			else node->setMarginBottom(gapW);
		}
		bars.push_back(node);
	}
	// 顺着排的方向随条数增长，另一向固定成一条的厚度
	auto longSide = barLong * (float)pins.size() + gapW * (float)(pins.size() - 1) + pad * 2;
	auto thickSide = barThick + pad * 2;
	if (isHorizontal()) setSize(longSide, thickSide);
	else setSize(thickSide, longSide);
	setPosition(barX, barY);
	// 尺寸没变时不会有 WM_SIZE，也就没人排这一次；新加的条得自己排一遍才算得出位置
	if (body) layout();
	show();
}

// 光标落在第几条上。条只有 4 逻辑像素宽，严格按它自己的范围判定等于要求用户拿鼠标
// 去点一根头发丝；本窗口本身就只有"条 + 内边距"那么大，所以直接取最近的那一条。
// 竖排之后比的是纵坐标
int PinHiddenBar::barIndexAt(POINT pos) const
{
	if (bars.empty()) return -1;
	int best = 0;
	float bestD = -1.f;
	for (size_t i = 0; i < bars.size(); ++i) {
		auto center = isHorizontal() ? bars[i]->x + bars[i]->w / 2.f
			: bars[i]->y + bars[i]->h / 2.f;
		auto d = isHorizontal() ? std::abs((float)pos.x - center)
			: std::abs((float)pos.y - center);
		if (bestD < 0.f || d < bestD) {
			bestD = d;
			best = (int)i;
		}
	}
	return best;
}

void PinHiddenBar::onMove(POINT pos)
{
	// Ling 把"离开窗口"也报进这条事件，坐标给的是 INT_MAX。这会儿什么都不做：
	// 收不收回去看的是"鼠标还在不在那张图 / 两条工具条上"，由复核定时器判，见 onTimerCB
	if (pos.x == INT_MAX) return;
	auto idx = barIndexAt(pos);
	if (idx >= 0) reveal(idx);
}

// 摆位 = 贴着第 index 条线：左条图放线右侧、图顶对齐线顶；顶条图放线下方、图左对齐线左。
// 缝 4 逻辑像素。
// （钳位不在这里做：要用贴图的 w/h，本函数只有 index，钳进 reveal）
POINT PinHiddenBar::calcPeekPos(int index) const
{
	const auto px = (float)barX, py = (float)barY;
	// 条线不是贴窗口原点画的：body 有 pad 内边距，条的真实起点 = 窗口原点 + pad
	// （漏掉它，图会比线高出一个 pad、贴线的缝也会被吃掉）
	const auto padPx = pad * dpi;
	const auto thick = barThick * dpi;
	const auto step = (barLong + gapW) * dpi;
	const auto gap = (int)std::lround(peekGap * dpi);
	// 条的真实起点 + 单条厚度 + 缝，就是图贴边的那一侧
	return isHorizontal()
		? POINT{ (int)std::lround(px + padPx + index * step), (int)std::lround(py + padPx + thick) + gap }
		: POINT{ (int)std::lround(px + padPx + thick) + gap, (int)std::lround(py + padPx + index * step) };
}

void PinHiddenBar::reveal(int index)
{
	auto pins = WinPin::getHiddenPins(edge);
	if (index < 0 || index >= (int)pins.size()) return;
	auto target = pins[(size_t)index];
	if (peek == target) return;
	conceal();
	peek = target;
	// 原位与摆位都记在这一次（conceal 时条可能已重建，序号不再可靠，所以要存）
	prevX = target->x;
	prevY = target->y;
	auto p = calcPeekPos(index);
	// 大图钳回工作区：图比屏幕还宽 / 高时，摆位不能开出边外
	RECT wa{};
	SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
	peekX = std::clamp((int)p.x, (int)wa.left, std::max((int)wa.left, (int)(wa.right - target->w)));
	peekY = std::clamp((int)p.y, (int)wa.top, std::max((int)wa.top, (int)(wa.bottom - target->h)));
	target->setPosition(peekX, peekY);
	target->peek(true);
	// show() 会把那张贴图提到 topmost 组的最前（组内后显示的在上），正好压住本窗口。
	// 条被压在底下就再也 hover 不到了，所以这里把它提回来
	SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	setTimer(tickMs, tickId);
}

void PinHiddenBar::conceal()
{
	killTimer(tickId);
	if (peek) {
		// 先藏后挪：可见态挪窗口会让原位在两步之间闪一帧（DWM 合成间隙），
		// 所以必须 hide() 之后再动位置 —— 与 reveal 的"先挪后 show"镜像
		bool atPlace = peek->x == peekX && peek->y == peekY;
		peek->peek(false);
		// 还停在摆位上 = 用户没拖过 → 挪回原位，「显示」按钮与既往行为一致；
		// 拖走了就就地藏 —— 窗口自己的 x/y 就是新原位，拖动手感不丢
		if (atPlace) peek->setPosition(prevX, prevY);
		peek = nullptr;
	}
}

bool PinHiddenBar::isOverPeek() const
{
	if (!peek || !peek->hwnd) return false;
	POINT cur{};
	GetCursorPos(&cur);
	auto hit = WindowFromPoint(cur);
	// 光标压在贴图本身、它的两条工具条、或者本窗口上，都算"还在这一套里"。
	// 少了工具条那一段，用户从条上移到工具条去点按钮的半路上图就没了
	return hit == hwnd || hit == peek->hwnd
		|| (peek->toolMain && hit == peek->toolMain->hwnd)
		|| (peek->toolSub && hit == peek->toolSub->hwnd);
}

void PinHiddenBar::onTimerCB(UINT id)
{
	if (id != tickId) return;
	if (!peek) {
		killTimer(tickId);
		return;
	}
	// 正拖着 / 正在标注 / 正在编辑文字时一概不收 —— 那种时候用户手上正拿着这张图，
	// 拖到哪儿它就跟到哪儿，光标一直在图上，本来也不会走到这里
	if (peek->isBusy() || isOverPeek()) return;
	conceal();
}
