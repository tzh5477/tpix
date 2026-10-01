#include "pch.h"
#include <algorithm>
#include <cmath>
#include "WinBall.h"
#include "../BallAction.h"
#include "../Setting.h"
#include "../Lang.h"
#include "../App.h"
#include "../PinSource.h"
#include "../SelectPopup.h"
#include "../Tip.h"
#include "WinCap.h"
#include "WinDelay.h"
#include "WinHistory.h"
#include "WinOverlay.h"
#include "WinSetting.h"

namespace {
	std::unique_ptr<WinBall> ballIns;
	// 接管窗口过程的那一跳。一个进程只有一枚球，所以这两个指针放文件级静态就够
	WNDPROC ballOrigProc{ nullptr };
	WinBall* ballPtr{ nullptr };

	// 逻辑像素。细线是"贴着屏幕边的一条红杠"，展开后每个图标占一个方块
	constexpr float lineLen{ 100.f };      // 细线长度
	constexpr float lineThick{ 5.f };      // 细线粗细
	constexpr float itemSize{ 32.f };      // 展开后每个图标的格子
	constexpr uint32_t lineColor{ 0xE4383Cff };
	// 延时收起的定时器 id。要避开宿主窗口里其他人在用的（Tip 用 0x5100，
	// ToolVideo 用 100，Ling 的 TextBox 从 0x4200 起）
	constexpr UINT menuTimerId{ 0x5200 };

	LRESULT CALLBACK ballProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (msg == WM_DROPFILES && ballPtr) {
			ballPtr->onDropFiles(reinterpret_cast<HDROP>(wParam));
			return 0;
		}
		return CallWindowProc(ballOrigProc, hwnd, msg, wParam, lParam);
	}
}

WinBall::WinBall()
{
	auto setting = Setting::get();
	edge = static_cast<Edge>(std::clamp(static_cast<int>(setting->getToolNum(L"ball", L"edge", 1.f)), 0, 2));
	auto savedAlong = setting->getToolNum(L"ball", L"along", -1.f);
	const auto wa = workArea();
	// 沿边方向的中点：第一次取屏幕正中间（贴右边而不是压到任务栏通知区那一带），
	// 之后取上次拖到的位置
	along = savedAlong >= 0 ? static_cast<int>(savedAlong)
		: (edge == Edge::Top ? (wa.left + wa.right) / 2 : (wa.top + wa.bottom) / 2);
	// 折叠态的尺寸先定下来，createNativeWindow 会照着摆。真正贴边对齐在 onCreated 里
	// 由 applyGeometry 统一做 —— 那时才知道自己最终落在哪块屏幕上
	if (edge == Edge::Top) { w = lineLen * dpi; h = lineThick * dpi; }
	else { w = lineThick * dpi; h = lineLen * dpi; }
	x = edge == Edge::Left ? wa.left : (edge == Edge::Right ? wa.right - static_cast<int>(w) : along - static_cast<int>(w) / 2);
	y = edge == Edge::Top ? wa.top : along - static_cast<int>(h) / 2;
	onMouseDown.add([this](POINT pos, BOOL isRight) { this->onDown(pos, isRight); });
	onMouseMove.add([this](POINT pos) { this->onMove(pos); });
	onMouseUp.add([this](POINT, BOOL) { this->onUp(); });
	// 弹层开着的时候鼠标移出去先不收（见 onMove），靠这个定时器等它关了再收
	onTimer.add([this](UINT id) {
		if (id != menuTimerId) return;
		killTimer(menuTimerId);
		if (!SelectPopup::isOpen() && !isMouseIn) collapse();
		});
	// 同 WinPin：DPI 变了系统会先擅自缩放窗口，等它把建议矩形应用完（紧随而来的 WM_SIZE）再掰回来
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (!dpiChanged) return;
		dpiChanged = false;
		applyGeometry();
		});
}

WinBall::~WinBall()
{
}

void WinBall::init()
{
	if (ballIns) return;
	if (!Setting::get()->getToolFlag(L"ball", L"show", false)) return;
	auto ptr = new WinBall();
	ballIns.reset(ptr);
	ballPtr = ptr;
	ptr->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
}

void WinBall::dispose()
{
	ballPtr = nullptr;
	ballIns.reset();
}

bool WinBall::hasBall()
{
	return ballIns != nullptr;
}

void WinBall::toggle()
{
	if (hasBall()) {
		Setting::get()->setToolFlag(L"ball", L"show", false);
		dispose();
	}
	else {
		Setting::get()->setToolFlag(L"ball", L"show", true);
		init();
	}
}

void WinBall::reload()
{
	if (!ballIns) return;
	// 重建前先收回折叠态：改勾选的时候鼠标多半在设置窗口那边，
	// 留着展开态会一直挂在屏幕上不收
	ballIns->expanded = false;
	ballIns->rebuildBody();
}

void WinBall::onCreated()
{
	// 悬浮球不能出现在自己的截图里。excludeFromCapture 是静态的 —— 本函数跑在 App 构造
	// 期间（App::init 里的 app.reset 还没执行），那时 App::get() 还是空的
	App::excludeFromCapture(hwnd);
	// 拖放用 WM_DROPFILES：不必为了 OLE 的 IDropTarget 去动 Ling，代价是只认文件、
	// 不认浏览器里拖出来的那一块裸位图。提权运行时，普通权限的资源管理器发来的这条消息
	// 会被 UIPI 拦掉，所以放行它（连同拖放实现里有时用到的 WM_COPYDATA）
	DragAcceptFiles(hwnd, TRUE);
	ChangeWindowMessageFilterEx(hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
	ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
	ballOrigProc = reinterpret_cast<WNDPROC>(GetWindowLongPtr(hwnd, GWLP_WNDPROC));
	SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ballProc));
	tip = std::make_unique<Tip>(this);
	buildBody();
	applyGeometry();
	show();
}

void WinBall::buildBody()
{
	if (!body) return;
	// 细线永远贴着屏幕那条边，图标排在它里侧：竖排时一左一右，横排时一上一下
	body->removeAllChildren();
	body->setFlexDirection(edge == Edge::Top ? Ling::FlexDirection::Column : Ling::FlexDirection::Row);
	body->setAlignItems(Ling::Align::Center);
	body->setJustifyContent(edge == Edge::Right ? Ling::Justify::End : Ling::Justify::Start);

	lineBox = nullptr;
	itemBox = nullptr;
	itemBtns.clear();

	auto makeLine = [this]() {
		lineBox = body->makeChild<Ling::Node>();
		if (edge == Edge::Top) lineBox->setSize(lineLen, lineThick);
		else lineBox->setSize(lineThick, lineLen);
		lineBox->setBg(lineColor);
		lineBox->setBorderRadius(lineThick / 2.f);
		};
	auto makeItems = [this]() {
		itemIds = ballParseActions(Setting::get()->getToolStr(L"ball", L"actions", ballDefaultActions()));
		if (itemIds.empty()) return;   // 一项都没勾：那这条细线就只是个摆设，点了也没东西
		itemBox = body->makeChild<Ling::Node>();
		itemBox->setBg(0xFAFAFAF7);
		itemBox->setBorderRadius(6.f);
		itemBox->setBorder(1.f, 0xDEDEDEFF);
		itemBox->setPadding(3.f);
		itemBox->setFlexDirection(edge == Edge::Top ? Ling::FlexDirection::Row : Ling::FlexDirection::Column);
		for (auto& id : itemIds) {
			auto def = ballFindAction(id);
			auto btn = itemBox->makeChild<Ling::Button>();
			btn->setSize(itemSize, itemSize);
			btn->setFontFamily(L"icon");
			btn->setFontSize(17.f);
			btn->setText(def->icon);
			btn->setBg(0);
			btn->setHoverBg(0xE8F0FEFF);
			btn->setBorderRadius(4.f);
			btn->setColor(0x444444FF);
			btn->setHoverColor(0x1677FFFF);
			bindTip(btn, Lang::get(def->nameKey));
			btn->setId(id);
			btn->onClick.add([this, id](Ling::Button*) { runAction(id); });
			itemBtns.push_back(btn);
		}
		};

	if (edge == Edge::Right) { makeItems(); makeLine(); }
	else { makeLine(); makeItems(); }

	if (!expanded && itemBox) itemBox->hide();
}

void WinBall::rebuildBody()
{
	// Tip 是绑在按钮上的，按钮跟着节点树一起销毁，所以提示也得重建
	tip = std::make_unique<Tip>(this);
	buildBody();
	applyGeometry();
}

void WinBall::applyGeometry()
{
	const auto wa = workArea();
	const auto n = static_cast<float>(itemIds.size());
	const auto thick = lineThick * dpi;
	const auto item = itemSize * dpi;
	// 展开条沿边方向的长度。图标不多时至少和细线一样长，否则细线会把面板顶成一根长条
	const auto band = std::max(lineLen, n * itemSize) * dpi;
	int pw{ 0 }, ph{ 0 };
	if (edge == Edge::Top) {
		pw = static_cast<int>(std::lround(expanded ? band : lineLen * dpi));
		ph = static_cast<int>(std::lround(expanded ? thick + item : thick));
	}
	else {
		pw = static_cast<int>(std::lround(expanded ? thick + item : thick));
		ph = static_cast<int>(std::lround(expanded ? band : lineLen * dpi));
	}
	int px{ 0 }, py{ 0 };
	if (edge == Edge::Top) {
		px = clampAlong(along) - pw / 2;
		py = wa.top;
		px = std::clamp(px, wa.left, std::max(wa.left, wa.right - pw));
	}
	else {
		px = edge == Edge::Left ? wa.left : wa.right - pw;
		py = clampAlong(along) - ph / 2;
		py = std::clamp(py, wa.top, std::max(wa.top, wa.bottom - ph));
	}
	w = static_cast<float>(pw);
	h = static_cast<float>(ph);
	x = px;
	y = py;
	SetWindowPos(hwnd, nullptr, px, py, pw, ph, SWP_NOZORDER | SWP_NOACTIVATE);
	refresh();
}

int WinBall::clampAlong(const int value) const
{
	const auto wa = workArea();
	if (edge == Edge::Top) return std::clamp(value, wa.left, wa.right);
	return std::clamp(value, wa.top, wa.bottom);
}

WinBall::Area WinBall::workArea() const
{
	MONITORINFO mi{ .cbSize = sizeof(MONITORINFO) };
	// 构造函数里 hwnd 还没建，只能按主显示器算；onCreated 之后才问得出自己落在哪块屏幕
	auto monitor = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
		: MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
	if (!GetMonitorInfo(monitor, &mi)) {
		mi.rcWork = RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
	}
	return Area{ static_cast<int>(mi.rcWork.left), static_cast<int>(mi.rcWork.top),
		static_cast<int>(mi.rcWork.right), static_cast<int>(mi.rcWork.bottom) };
}

void WinBall::expand()
{
	if (expanded || !itemBox) return;
	expanded = true;
	// 先把窗口撑大再让图标参与布局：反过来的话它们会先挤在细线那么点地方里重排一遍，
	// 看着就是闪一下
	applyGeometry();
	itemBox->show();
}

void WinBall::collapse()
{
	if (!expanded) return;
	expanded = false;
	if (itemBox) itemBox->hide();
	applyGeometry();
}

void WinBall::onMove(POINT pos)
{
	// Ling 在鼠标离开窗口时发一个 (INT_MAX, INT_MAX)，正好当"移出去了"用
	if (pos.x == INT_MAX) {
		if (isMouseDown) return;
		// 弹层（右键图标弹出的方向 / 秒数列表）还开着就先不收：收起会挪动本窗口，
		// 而弹层订阅了宿主的移动事件、一挪就自己关了 —— 用户刚点开的列表会凭空消失。
		// 等它关掉再由定时器收，中间最多差 200ms
		if (SelectPopup::isOpen()) {
			setTimer(200, menuTimerId);
			return;
		}
		collapse();
		return;
	}
	if (isMouseDown) {
		// 光标一步没挪也会来 WM_MOUSEMOVE，所以跟按下点比一下再算拖动
		if (!hasDragged && (pos.x != pressPos.x || pos.y != pressPos.y)) {
			hasDragged = true;
			// 拖动时把图标收掉：面板连着图标一起跟着鼠标在屏幕上飘很碍眼，
			// 只留那条细线反而看得出来是在挪
			if (itemBox) itemBox->hide();
		}
		if (hasDragged) {
			setPosition(dragWinX + pos.x - pressPos.x, dragWinY + pos.y - pressPos.y);
		}
		return;
	}
	if (!expanded) expand();
}

void WinBall::onDown(POINT pos, bool isRight)
{
	if (isRight) {
		if (auto* btn = itemAt(pos)) {
			showActionMenu(btn->id, btn);
			return;
		}
		showBallMenu();
		return;
	}
	// 点在图标上：这一下是"用这个功能"，交给按钮自己的点击回调，不当拖动
	if (itemAt(pos)) return;
	isMouseDown = true;
	hasDragged = false;
	pressPos = pos;
	dragWinX = x;
	dragWinY = y;
	SetCapture(hwnd);
}

void WinBall::onUp()
{
	if (!isMouseDown) return;
	isMouseDown = false;
	ReleaseCapture();
	if (!hasDragged) {
		// 只是点了一下细线没拖：等价于展开（有些人不习惯"鼠标扫过去就弹东西"）
		expand();
		return;
	}
	if (itemBox) itemBox->show();
	snapAndSave();
}

Ling::Button* WinBall::itemAt(POINT pos) const
{
	if (!expanded) return nullptr;
	for (auto* btn : itemBtns) {
		if (btn->isPosIn(pos)) return btn;
	}
	return nullptr;
}

void WinBall::snapAndSave()
{
	const auto wa = workArea();
	// 拿窗口中心判离哪条边最近。下边不参与 —— 悬浮球贴在任务栏那一带没有意义
	const auto cx = x + w / 2.f;
	const auto cy = y + h / 2.f;
	const auto dLeft = cx - wa.left;
	const auto dRight = wa.right - cx;
	const auto dTop = cy - wa.top;
	if (dTop <= dLeft && dTop <= dRight) edge = Edge::Top;
	else edge = (dLeft <= dRight) ? Edge::Left : Edge::Right;
	along = static_cast<int>(std::lround(edge == Edge::Top ? cx : cy));
	along = clampAlong(along);
	expanded = false;
	storePosition();
	// 细线贴哪条边、图标往哪排都是建节点时定死的，换了边就得整棵重来
	rebuildBody();
}

void WinBall::storePosition()
{
	auto setting = Setting::get();
	setting->setToolNum(L"ball", L"edge", static_cast<float>(static_cast<int>(edge)));
	setting->setToolNum(L"ball", L"along", static_cast<float>(along));
}

void WinBall::bindTip(Ling::Button* btn, const std::wstring& name)
{
	// 气泡不能往屏幕外弹：贴顶边时挂到按钮下面，贴右边（图标在细线左侧）时挂到左边
	auto side = Tip::Side::Right;
	if (edge == Edge::Top) side = Tip::Side::Below;
	else if (edge == Edge::Right) side = Tip::Side::Left;
	btn->onEnter.add([this, btn, name, side](Ling::Button*) {
		// Node 的 x/y/w/h 是布局算完的窗口内绝对坐标（物理像素）
		auto ax = btn->x + btn->w / 2.f;
		auto ay = btn->y + btn->h;
		if (side == Tip::Side::Left) { ax = btn->x; ay = btn->y + btn->h / 2.f; }
		else if (side == Tip::Side::Right) { ax = btn->x + btn->w; ay = btn->y + btn->h / 2.f; }
		POINT p{ static_cast<LONG>(ax), static_cast<LONG>(ay) };
		ClientToScreen(hwnd, &p);
		const auto gap = 4.f * dpi;
		auto sx = static_cast<float>(p.x);
		auto sy = static_cast<float>(p.y);
		if (side == Tip::Side::Below) sy += gap;
		else if (side == Tip::Side::Left) sx -= gap;
		else sx += gap;
		tip->showAt(btn, sx, sy, name, side);
		});
	btn->onLeave.add([this](Ling::Button* b) { tip->hide(b); });
}

void WinBall::runAction(const std::wstring& id)
{
	// 图标一点就走，先把展开条收掉：接下来多半是全屏的截图窗口，
	// 留着它反而浮在最上面挡着
	collapse();
	if (id == L"cap") WinCap::init();
	else if (id == L"long") WinCap::init(L"long");
	else if (id == L"video") WinCap::init(L"video");
	else if (id == L"ocr") WinCap::init(L"ocr");
	else if (id == L"qr") WinCap::init(L"qr");
	else if (id == L"pinDirect") WinCap::init(L"pin");
	else if (id == L"delay") WinDelay::start(
		std::clamp(static_cast<int>(Setting::get()->getToolNum(L"ball", L"delaySec", 1.f)), 1, 10));
	else if (id == L"pinClip") PinSource::fromClipboard();
	else if (id == L"pinFile") PinSource::fromFile(hwnd);
	else if (id == L"pinOlder") PinSource::pinNextOlder();
	else if (id == L"pinClipOlder") PinSource::pinNextOlderClip();
	else if (id == L"history") WinHistory::init();
	else if (id == L"ruler") WinOverlay::toggle(OverlayMode::Ruler);
	else if (id == L"crosshair") WinOverlay::toggle(OverlayMode::Crosshair);
	else if (id == L"focus") WinOverlay::toggle(OverlayMode::Focus);
	else if (id == L"setting") WinSetting::init();
	else if (id == L"hide") hideSelf();
}

void WinBall::showActionMenu(const std::wstring& id, Ling::Button* anchor)
{
	auto setting = Setting::get();
	// 带参数的两项：左键按当前设置直接跑，想换参数就在图标上点右键
	if (id == L"long") {
		auto horizontal = setting->getLongHorizontal();
		SelectPopup::show(this, anchor,
			{ Lang::get(L"long.vertical"), Lang::get(L"long.horizontal") },
			horizontal ? 1 : 0,
			[](int idx) { Setting::get()->setLongHorizontal(idx == 1); });
	}
	else if (id == L"delay") {
		std::vector<std::wstring> items;
		for (int i = 1; i <= 10; ++i) items.push_back(std::to_wstring(i) + Lang::get(L"setting.sec"));
		auto cur = std::clamp(static_cast<int>(setting->getToolNum(L"ball", L"delaySec", 1.f)), 1, 10);
		SelectPopup::show(this, anchor, items, cur - 1, [](int idx) {
			Setting::get()->setToolNum(L"ball", L"delaySec", static_cast<float>(idx + 1));
			});
	}
}

void WinBall::showBallMenu()
{
	enum : UINT { menuSetting = 1, menuHide };
	auto menu = CreatePopupMenu();
	AppendMenu(menu, MF_STRING, menuSetting, Lang::get(L"ball.setting").data());
	AppendMenu(menu, MF_STRING, menuHide, Lang::get(L"ball.hide").data());
	auto id = Ling::App::get()->popupMenu(menu);
	if (id == menuSetting) WinSetting::init();
	else if (id == menuHide) hideSelf();
}

void WinBall::hideSelf()
{
	Setting::get()->setToolFlag(L"ball", L"show", false);
	// 本函数正跑在鼠标事件的回调里，直接 dispose 就是把自己从栈底下抽掉。
	// 同 WinPin::onClosed：推迟到下一轮消息循环再放
	Ling::App::get()->dq.TryEnqueue([]() { WinBall::dispose(); });
}

void WinBall::onDropFiles(HDROP drop)
{
	auto count = DragQueryFile(drop, 0xFFFFFFFF, nullptr, 0);
	for (UINT i = 0; i < count; ++i)
	{
		wchar_t buf[MAX_PATH]{};
		if (DragQueryFile(drop, i, buf, MAX_PATH) == 0) continue;
		// 非图片（目录、txt…）由 PinSource 自己认出来什么都不做
		PinSource::fromPath(std::wstring{ buf });
	}
	DragFinish(drop);
}
