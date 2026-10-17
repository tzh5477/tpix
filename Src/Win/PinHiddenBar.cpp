#include "pch.h"
#include <algorithm>
#include "WinPin.h"
#include "../Tool/ToolMain.h"
#include "../Tool/ToolSub.h"
#include "../Lang.h"
#include "PinHiddenBar.h"
#include "PinHoverPreview.h"

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

	// hover 预览浮层整进程只有一枚，两条边共用 —— 鼠标只有一只，不存在"两边同时 hover"；
	// 少一扇窗也少一个可能被人忘掉的句柄
	std::unique_ptr<PinHoverPreview> hoverPreview;
	PinHoverPreview* ensurePreview()
	{
		if (!hoverPreview) hoverPreview = std::make_unique<PinHoverPreview>();
		return hoverPreview.get();
	}
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
	// 点某一条 = 放回桌面 / 弹菜单。条本身只有 4 像素厚，命中取"最近的那一条"（见 barIndexAt）
	onMouseDown.add([this](POINT pos, bool isRight) { this->handleDown(pos, isRight); });
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
	hoverPreview.reset();
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
	// 条数要重排了，悬记着的那一格不再可靠：待 peek 与浮层预览一并收掉，下次 hover 从头来
	hoverIdx = -1;
	killTimer(dwellId);
	hidePreview();
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
	// Ling 把"离开窗口"也报进这条事件，坐标给的是 INT_MAX。这会儿把预览收掉、待 peek 那一格
	// 忘掉 —— 已经露出来的整窗不管：收不收回去看的是"鼠标还在不在那张图 / 两条工具条上"，
	// 由那条 250ms 的复核定时器判（见 onTimerCB）
	if (pos.x == INT_MAX) {
		hoverIdx = -1;
		killTimer(dwellId);
		// 不直接收：光标可能正挪到浮层上（条与浮层之间隔着 4px 缝），交给复核定时器判 ——
		// 那样才能"单击浮层缩略图恢复贴图"。光标真的离开条和浮层两边，定时器再收
		startPvWatch();
		return;
	}
	auto idx = barIndexAt(pos);
	if (idx < 0) return;
	// 还停在同一格就什么都不做：预览由 showPreview 自己维持，换格才重画
	if (idx == hoverIdx) return;
	hoverIdx = idx;
	killTimer(dwellId);
	auto pins = WinPin::getHiddenPins(edge);
	if (idx >= (int)pins.size()) return;
	// 若已有整窗露着（防御性：当前仅拖出等其它路径可能触发），换格即换图
	if (peek) {
		hidePreview();
		reveal(idx);
		return;
	}
	// hover 一格：在条旁边弹这格的缩略预览（安静）。不自动升级成整窗 peek —— 整窗交给单击「放回桌面」。
	// 落点复用 calcPeekPos —— 预览与（若存在）回显摆的是同一个位置
	showPreview(idx, pins[(size_t)idx]);
}

// hover 一格时先给出的那个安静的小预览。见 PinHoverPreview 上的说明
void PinHiddenBar::showPreview(int index, WinPin* pin)
{
	auto* pv = ensurePreview();
	// 浮层整进程一枚、两条边共用：每次唤起都把 owner 指回当前这条，单击恢复才找得到正确的条
	pv->owner = this;
	pv->showPreview(pin, calcPeekPos(index), dpi);
	// 浮层起来了：它的存活期改由复核定时器看着（光标在条或浮层上就留），
	// 不再走"条一离开就收" —— 否则光标从条挪到浮层必经那条 4px 缝，浮层会被提前藏掉
	startPvWatch();
}

void PinHiddenBar::hidePreview()
{
	// 没建过就别建 —— ensurePreview 会凭空造出一扇窗，而这里只是要"如果没有就啥也不做"
	if (hoverPreview) hoverPreview->hidePreview();
	// 浮层收了，复查它的定时器也没存在的必要
	killTimer(pvTickId);
}

void PinHiddenBar::startPvWatch()
{
	// 浮层存活期复核定时器：光标在书签条或浮层上就留着，都不在才收。
	// 重复 setTimer 同一个 id 等于续期，hover 换格时反复调也安全
	setTimer(pvTickMs, pvTickId);
}

// 浮层被单击时由它回调：执行"把那张贴图放回桌面" —— 与 handleDown 的左键完全同义。
// 浮层是独立顶层窗、压在书签条旁边，光标压在浮层上时书签条收不到点击，所以这条入口专门接它
void PinHiddenBar::restoreFromPreview(WinPin* pin)
{
	killTimer(pvTickId);
	hoverIdx = -1;
	// owner 指回的是这一条，恢复后这扇条可能被 sync 重建甚至销毁，先解绑免得悬空指针
	if (hoverPreview) hoverPreview->owner = nullptr;
	hidePreview();
	// 它正被 hover 露着的话，先让 conceal 把它挪回原位再放回 —— "点一下就回到原来的地方"；
	// 没露着就直接放回（x/y 本来就是藏之前那一份）
	if (peek == pin) conceal();
	// setHidden(false) 会走 PinHiddenBar::sync() 重建本窗（藏着的都放回了可能就没这条了），
	// 之后 this 可能失效 —— 所以它必须放最后，后面不再碰 this 的成员
	// restoreFromBar = "只放回原位，不进编辑态"（工具条不请回来，左键直接拖图）
	pin->restoreFromBar();
}

// 光标是不是压在书签条本身上（判断浮层要不要留）
bool PinHiddenBar::pvCursorOverBar() const
{
	if (!hwnd) return false;
	POINT p{};
	GetCursorPos(&p);
	return WindowFromPoint(p) == hwnd;
}

// 光标是不是压在浮层预览窗上（判断浮层要不要留）
bool PinHiddenBar::pvCursorOverPreview() const
{
	if (!hoverPreview || !hoverPreview->hwnd) return false;
	POINT p{};
	GetCursorPos(&p);
	return WindowFromPoint(p) == hoverPreview->hwnd;
}

// 左键点某一条 = 把那一张放回桌面（落回藏之前的位置）；右键 = 上下文菜单。
// 这条是"隐藏/放回"里唯一一个"点一下就回来"的入口 —— 原来只有"hover 露出来再拖出去"，
// 那个手感要求太高（拖到一半还会被 800ms 的复核定时器收回去）
void PinHiddenBar::handleDown(POINT pos, bool isRight)
{
	auto idx = barIndexAt(pos);
	if (idx < 0) return;
	auto pins = WinPin::getHiddenPins(edge);
	if (idx >= (int)pins.size()) return;
	auto target = pins[(size_t)idx];
	if (!isRight) {
		// 预览 / 待 peek 都先收掉：图马上要回到桌面上了，角上不该留着一张悬空的小图
		killTimer(dwellId);
		hidePreview();
		hoverIdx = -1;
		// 它正被 hover 露着的话，先让 conceal 把它挪回原位再放回 —— 于是"点一下就回到原来的地方"；
		// 没露着就直接放回（它这会儿的 x/y 本来就是藏之前那一份）
		if (peek == target) conceal();
		// 单击书签 = 只把图放回原位展示，**不进编辑态**（作者要求）；要标注按空格
		target->restoreFromBar();
		return;
	}
	killTimer(dwellId);
	hidePreview();
	hoverIdx = -1;
	enum : UINT { R_RESTORE = 1, R_COPY, R_SAVE, R_GROUP_SHOW, R_CLOSE };
	auto menu = CreatePopupMenu();
	AppendMenu(menu, MF_STRING, R_RESTORE, Lang::get(L"tool.barMenuRestore").data());
	AppendMenu(menu, MF_STRING, R_COPY, Lang::get(L"tool.copy").data());
	AppendMenu(menu, MF_STRING, R_SAVE, Lang::get(L"tool.pinMenuSave").data());
	// 成组的贴图才有"整组放回"。单张仍是单张书签格 —— 一格代表一组，点下去该放谁就成了歧义
	if (target->getGroupId() != 0) {
		AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
		AppendMenu(menu, MF_STRING, R_GROUP_SHOW, Lang::get(L"tool.pinMenuGroupShow").data());
	}
	AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenu(menu, MF_STRING, R_CLOSE, Lang::get(L"tool.close").data());
	auto cmd = Ling::App::get()->popupMenu(menu);
	switch (cmd) {
	case R_RESTORE:
		if (peek == target) conceal();
		// 与左键同义：放回原位，不进编辑态
		target->restoreFromBar();
		break;
	case R_COPY: target->copyImageToClipboard(); break;
	case R_SAVE: target->saveToFile(); break;      // 成功即关窗
	case R_GROUP_SHOW:
	{
		// 把同组、且散在两条边上的成员一起放回。先收进一个临时表再动 ——
		// setHidden(false) 会走 PinHiddenBar::sync() 重建本窗口的 bars
		auto g = target->getGroupId();
		std::vector<WinPin*> same;
		for (int i = 0; i < 2; ++i) {
			for (auto* p : WinPin::getHiddenPins((WinPin::BarEdge)i)) {
				if (p->getGroupId() == g) same.push_back(p);
			}
		}
		for (auto* p : same) p->setHidden(false);
	}
	break;
	case R_CLOSE: target->close(); break;
	default: break;
	}
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
	leaveAt = 0;
	setTimer(tickMs, tickId);
}

void PinHiddenBar::conceal()
{
	// 收回 = 这一轮 hover 结束：待 peek 清零，浮层预览一并收掉。
	// 不清的话，紧接着回到同一格会被"还停在同一格"那条早退挡掉，图再也 peeek 不出来
	killTimer(dwellId);
	hoverIdx = -1;
	hidePreview();
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
	// 浮层存活期复核：只在浮层还开着时才有意义。光标压在书签条或浮层上就留着；
	// 两边都不在（挪到桌面 / 别处）→ 收掉浮层并停定时器
	if (id == pvTickId) {
		if (!hoverPreview || !hoverPreview->hwnd) { killTimer(pvTickId); return; }
		if (pvCursorOverBar() || pvCursorOverPreview()) return;
		hidePreview();
		killTimer(pvTickId);
		return;
	}
	if (id != tickId) return;
	if (!peek) {
		killTimer(tickId);
		return;
	}
	// 用户把这张图从条上拖走了 —— 他就是要它留在桌面上。这一刻就"放回来"（isHidden 置假，
	// 两条工具条一并请回来），不回原位、也不等满 800ms 再收回边上：拖出来又被自动收回去，
	// 正是作者报的那条。判"拖过没有"看的是位置还与本次回显的摆位对不对得上。
	// 只在没拿着它的时候判：拖着的那几跳位置本来就在变，那时候收手是不对的
	if (!peek->isBusy()
		&& std::max(std::abs((float)(peek->x - peekX)), std::abs((float)(peek->y - peekY))) > dragOutSlop) {
		auto pin = peek;
		peek = nullptr;
		killTimer(tickId);
		pin->setHidden(false);
		return;
	}
	// 在场判定：鼠标压在图 / 两条工具条 / 本条上，或图正被拖着 / 正在编辑 —— 都视作在场
	if (peek->isBusy() || isOverPeek()) {
		leaveAt = 0;
		return;
	}
	// 第一跳发现离开 → 记时刻；连续离开满 800ms 才收回。中途回来 leaveAt 清零，重新计时
	if (leaveAt == 0) {
		leaveAt = GetTickCount();
		return;
	}
	if (GetTickCount() - leaveAt >= peekGraceMs) conceal();
}
