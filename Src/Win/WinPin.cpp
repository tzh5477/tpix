#include "pch.h"
#include <algorithm>
#include "../Canvas.h"
#include "../Tool/ToolMain.h"
#include "../Tool/ToolSub.h"
#include "../Shape/ShapeBase.h"
#include "../Shape/ShapeText.h"
#include "../Shape/ShapeNumber.h"
#include "../Shape/ShapeWatermark.h"
#include "WinPin.h"
#include "WinCap.h"
#include "History.h"
#include "../App.h"
#include "../Util.h"
#include "../Update.h"
#include "../Setting.h"
#include "../ShotHistory.h"
#include "WinWatermarkPanel.h"
#include "WinWatermarkText.h"

using namespace Microsoft::WRL;
using namespace winrt::Windows::Data::Json;
namespace {
	std::vector<std::unique_ptr<WinPin>> winPins;
	// 下一个可用的贴图组号。0 留给"不成组"，所以从 1 起
	int nextGroupId{ 1 };

	int clampPos(float val, float size, int min, int max)
	{
		auto upper = max - static_cast<int>(size);
		if (upper < min) upper = min;
		auto result = static_cast<int>(val);
		if (result < min) result = min;
		if (result > upper) result = upper;
		return result;
	}

	// 枚举显示器的回调。写成静态函数而不是无捕获 lambda：MONITORENUMPROC 是 CALLBACK
	//（__stdcall），lambda 转出来的函数指针是 __cdecl，只有 x64 下两者才碰巧一致
	BOOL CALLBACK collectMonitor(HMONITOR, HDC, LPRECT rect, LPARAM data)
	{
		reinterpret_cast<std::vector<RECT>*>(data)->push_back(*rect);
		return TRUE;
	}

	// 贴图属性回写。四个 setter 都是公开接口，所以这个帮忙的可以待在匿名 namespace 里
	void restoreProps(WinPin* pin, JsonObject obj)
	{
		pin->setOpacity((float)obj.GetNamedNumber(L"opacity", 1.0));
		pin->setRounded(obj.GetNamedBoolean(L"round", false));
		pin->setLocked(obj.GetNamedBoolean(L"lock", false));
		pin->setPinTitle(std::wstring{ obj.GetNamedString(L"title", L"") });
	}
}

WinPin::WinPin(int x, int y, int w, int h, const std::vector<BYTE>* data, const std::wstring& initToolId)
	: Ling::WinBase(), drawing{ std::make_unique<Canvas>(this) }
{
	this->x = x;
	this->y = y;
	this->w = (float)w;
	this->h = (float)h;
	if (data) {
		// 外部像素建底图。马赛克那两个会把它当取样源，属性与 getCutImg() 出来的保持一致
		D2D1_BITMAP_PROPERTIES1 props{};
		props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
		props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
		props.dpiX = 96.0f;
		props.dpiY = 96.0f;
		Ling::D2D::get()->deviceContext->CreateBitmap(D2D1::SizeU(w, h), data->data(), w * 4, &props, drawing->screenImg.GetAddressOf());
	}
	else {
		drawing->screenImg = WinCap::get()->getCutImg();
	}
	toolMain = std::make_unique<ToolMain>(this);
    toolSub = std::make_unique<ToolSub>(this);
	// 预选工具排在两条工具条都建好之后：selectTool 会按工具配出 ToolSub 的内容再重排整组
	if (!initToolId.empty()) {
		toolMain->selectTool(initToolId);
	}
	layoutTools();
	onMoved.add([this]() { layoutTools(); });
	// DPI 变了（用户改了缩放比例，或者窗口被拖到缩放比例不同的显示器上）：系统会按新旧缩放比
	// 把窗口整体放大一圈，但贴图窗口的尺寸是钉死在底图像素上的 —— 底图按原始像素画，
	// shape 的坐标也都是相对底图的物理像素，跟着缩放只会让窗口比图大一圈：
	// 右边、下边多出一条空白，边框看着比左上两边粗（描边居中，左上那半截被窗口边裁掉了），
	// 工具条也会按虚高的宽高往右下偏。所以等系统把建议矩形应用完（紧随而来的 WM_SIZE）
	// 再把尺寸掰回底图大小，并重排工具条。位置不能在 onDpiChanged 里改 ——
	// 那个事件在系统建议矩形生效之前触发，改了马上被覆盖
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (!dpiChanged) return;
		dpiChanged = false;
		applyWinSize();
		layoutTools();
	});
	onMouseDown.add([this](POINT pos, BOOL isRight) {this->onDown(pos, isRight);});
	onMouseMove.add([this](POINT pos) {this->onMove(pos);});
	onMouseUp.add([this](POINT pos, BOOL isRight) {this->onUp(pos, isRight);});
	// Ling 传进来的是已经换算成滚动距离的 space（一格 = 60 逻辑像素 × dpi），
	// shape 只关心方向，这里按符号还原成 ±WHEEL_DELTA
	onMouseWheel.add([this](POINT pos, float space) {
		// 缩略图上滚一下先还原：这时候缩放没有画面反馈，看着像没反应
		if (isThumb) setThumbMode(false);
		// Ling 的滚轮事件不带修饰键状态，自己查：按住 Ctrl 是缩放窗口，不是调 shape
		if (GetKeyState(VK_CONTROL) & 0x8000) {
			// 一格 10%，按当前倍数等比走，放大和缩小的手感才对称
			applyScale(scale * (space > 0 ? 1.1f : 1.f / 1.1f), pos);
			return;
		}
		if (!drawing->shapeHover) return;
		auto imgPos = toImgPos(pos);
		drawing->shapeHover->mouseWheel((float)imgPos.x, (float)imgPos.y, space > 0 ? (short)WHEEL_DELTA : (short)-WHEEL_DELTA);
	});
	onTimer.add([this](UINT id) {this->onTimerCB(id);});
	onKeyDown.add([this](UINT key) {this->onKey(key);});
	onDestroy.add([this]() { this->onClosed(); });
}

// WinBase::close() 里 DestroyWindow 之后同步触发 onDestroy，所以这个函数很可能是从
// ToolMain 的按钮回调里一路调进来的（点了 close 按钮）。此时 ToolMain::onClick 还在栈上，
// 而 toolMain 是 WinPin 的成员 —— 在这里直接把自己从 winPins 里擦掉就是 use-after-free。
// 因此：窗口句柄立即销毁（用户马上看到界面消失），C++ 对象的释放推迟到下一轮消息循环。
void WinPin::onClosed()
{
	// 防止 close() 被走两遍（比如按钮和快捷键先后触发）时排两次销毁
	if (isClosed) return;
	isClosed = true;
	if (editingShape) editingShape->finishEditing();
	// 竖排浮层与内容弹窗是独立顶层窗口，不随 toolSub 一起死。
	// 不收的话它们会孤零零留在屏幕上：那上面的滑块调的是一个已经关掉的窗口的水印样式，
	// 拖上去什么也不会发生，看着像程序卡了
	WinWatermarkPanel::close();
	WinWatermarkText::close();
	// 先收起附属窗口，再让出 hover 指针 —— shapeHover 指向 history 里的元素，
	// 而 history 现在归 drawing 所有（与 WinPin 同生共死），留着悬空指针没意义
	if (toolSub) toolSub->close();
	if (toolMain) toolMain->close();
	drawing->shapeHover = nullptr;
	drawing->selected = nullptr;
	editingShape = nullptr;
	// screenImg / canvas / drawing 都是成员（canvas 挂在 body 的子节点上），随下面这次 erase 一并释放
	Ling::App::get()->dq.TryEnqueue([this]() {
		std::erase_if(winPins, [this](const std::unique_ptr<WinPin>& p) { return p.get() == this; });
		// 用完即走模式下，最后一个贴图窗口关掉就退出进程，不驻留在系统里。
		// 贴图可以同时开好几个（标注、长截图各来一张），所以得等它们都没了才退
		if (winPins.empty()) {
			if (Ling::App::get()->args[L"--auto-quit"] == L"true") {
				Ling::App::get()->quit(0);
			}
			else {
				Update::checkLater(); //只剩托盘图标了，顺便查一下更新
			}
		}
	});
}

bool WinPin::hasWindow()
{
	return !winPins.empty();
}

void WinPin::dispose()
{
	winPins.clear();
}

// 贴图持久化：退出时把每张贴图合成一张 PNG 存进数据目录 pin/，
// 落点 / 尺寸 / 属性写进 config.json。下次启动 restoreAll 按原样摆回去。
// 顺序上 saveAll 必须在 dispose 之前 —— dispose 一跑位图就没了
void WinPin::saveAll()
{
	auto arr = Setting::get()->getPins();
	arr.Clear();
	auto keep = Setting::get()->getRestorePins() && !winPins.empty();
	if (keep)
	{
		auto dir = Setting::get()->getDataPath() / L"pin";
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		// 本轮写出的文件名。清理排在写盘之后（见函数末尾）—— 恢复出来的动图，源文件
		// 就在这个目录里，先清目录等于把要拷的源自己删了，第二次退出后动图会退化成静态图
		std::vector<std::wstring> written;
		int index{ 0 };
		for (auto& pin : winPins)
		{
			auto imgSize = pin->getImgSize();
			if (imgSize.width == 0 || imgSize.height == 0) continue;
			// 动图存原始文件 —— 存的是当帧的话重启后就静止了；
			// 原文件没了（比如用户删了）才退回存当前这一帧
			auto base = std::format(L"{}", index);
			std::wstring savedName;
			bool isAnim{ false };
			if (pin->hasAnim() && !pin->animSrc.empty()) {
				auto animName = base + std::filesystem::path(pin->animSrc).extension().wstring();
				auto dst = dir / animName;
				std::error_code ec2;
				// 恢复出来的贴图，animSrc 就在本目录里，编号没变时源与目的是同一个文件，
				// 而标准规定那种情况 copy_file 必须报错 —— 先认一下"已经在位"
				if (std::filesystem::equivalent(pin->animSrc, dst, ec2)
					|| std::filesystem::copy_file(pin->animSrc, dst,
						std::filesystem::copy_options::overwrite_existing, ec2)) {
					savedName = animName;
					isAnim = true;
				}
			}
			if (savedName.empty()) {
				std::vector<BYTE> pixels;
				D2D1_SIZE_U size{};
				if (!pin->getImagePixels(pixels, size)) continue;
				savedName = base + L".png";
				if (!Util::saveToFile((dir / savedName).wstring(),
					(int)size.width, (int)size.height, pixels.data())) continue;
			}
			++index;
			written.push_back(savedName);
			JsonObject obj;
			obj.SetNamedValue(isAnim ? L"anim" : L"img", JsonValue::CreateStringValue(savedName));
			obj.SetNamedValue(L"x", JsonValue::CreateNumberValue((double)pin->x));
			obj.SetNamedValue(L"y", JsonValue::CreateNumberValue((double)pin->y));
			obj.SetNamedValue(L"w", JsonValue::CreateNumberValue((double)imgSize.width));
			obj.SetNamedValue(L"h", JsonValue::CreateNumberValue((double)imgSize.height));
			obj.SetNamedValue(L"opacity", JsonValue::CreateNumberValue((double)pin->opacity));
			obj.SetNamedValue(L"round", JsonValue::CreateBooleanValue(pin->isRounded));
			obj.SetNamedValue(L"lock", JsonValue::CreateBooleanValue(pin->isLocked));
			obj.SetNamedValue(L"title", JsonValue::CreateStringValue(pin->pinTitle));
			obj.SetNamedValue(L"group", JsonValue::CreateNumberValue((double)pin->groupId));
			arr.Append(obj);
		}
		// 上一次留下的文件与这次的编号对不上，留着只会越积越多，所以本轮没写到的都删掉。
		// 先收集再删 —— 边枚举边删目录项，没枚举到的可能被跳过
		std::vector<std::filesystem::path> stale;
		std::error_code ec2;
		for (auto& entry : std::filesystem::directory_iterator(dir, ec2)) {
			auto name = entry.path().filename().wstring();
			if (std::find(written.begin(), written.end(), name) == written.end()) {
				stale.push_back(entry.path());
			}
		}
		for (auto& path : stale) {
			std::filesystem::remove_all(path, ec2);
		}
		keep = arr.Size() > 0;
	}
	Setting::get()->setPins(arr);
}

void WinPin::finishRestore(JsonObject obj)
{
	auto pin = winPins.back().get();
	restoreProps(pin, obj);
	pin->groupId = (int)obj.GetNamedNumber(L"group", 0.0);
	if (pin->groupId >= nextGroupId) nextGroupId = pin->groupId + 1;
}

void WinPin::restoreAll()
{
	if (!Setting::get()->getRestorePins()) return;
	auto dir = Setting::get()->getDataPath() / L"pin";
	for (auto&& value : Setting::get()->getPins())
	{
		auto obj = value.GetObject();
		auto x = (int)obj.GetNamedNumber(L"x", 0.0);
		auto y = (int)obj.GetNamedNumber(L"y", 0.0);
		// 动图：原始文件还在就按帧序列恢复，解不出来（文件坏了）就当这条不存在
		auto animName = std::wstring{ obj.GetNamedString(L"anim", L"") };
		if (!animName.empty()) {
			auto animPath = (dir / animName).wstring();
			std::vector<AnimFrame> frames;
			if (AnimImage::load(animPath, frames)) {
				initFromAnim(x, y, animPath, frames);
				finishRestore(obj);
			}
			continue;
		}
		auto name = std::wstring{ obj.GetNamedString(L"img", L"") };
		if (name.empty()) continue;
		std::vector<BYTE> data;
		DWORD w{ 0 }, h{ 0 };
		if (!Util::loadImageBytes((dir / name).wstring(), data, w, h)) continue;
		initFromData(x, y, (int)w, (int)h, data);
		finishRestore(obj);
	}
}

D2D1_SIZE_U WinPin::getImgSize() const
{
	if (!drawing->screenImg) return D2D1::SizeU(0, 0);
	// 要的是像素数，所以问 GetPixelSize 而不是 GetSize（后者返回的是按位图自身 dpi 折算的 DIP）
	return drawing->screenImg->GetPixelSize();
}

void WinPin::applyWinSize()
{
	auto sz = getImgSize();
	if (!hwnd || sz.width == 0 || sz.height == 0) return;
	auto vs = viewScale();
	auto newW = std::max(1, static_cast<int>(std::lround(sz.width * vs)));
	auto newH = std::max(1, static_cast<int>(std::lround(sz.height * vs)));
	w = static_cast<float>(newW);
	h = static_cast<float>(newH);
	// 不走 setSize：它收的是逻辑像素、内部还要乘一遍 dpi，而这里的宽高本来就是物理像素
	SetWindowPos(hwnd, nullptr, 0, 0, newW, newH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
}

POINT WinPin::toImgPos(const POINT& pos) const
{
	auto vs = viewScale();
	if (vs == 1.f) return pos;
	return POINT{ static_cast<LONG>(std::lround(pos.x / vs)), static_cast<LONG>(std::lround(pos.y / vs)) };
}

float WinPin::clampScale(float v) const
{
	auto sz = getImgSize();
	if (sz.width == 0 || sz.height == 0) return v;
	// 上限跟着底图大小走：窗口边长再大，swap chain 那块显存也吃不消，人也看不过来；
	// 但至少要能回到 1 倍，所以外面再 max 一下
	auto maxScale = std::max(1.f, std::min(8.f, 16000.f / std::max(sz.width, sz.height)));
	return std::clamp(v, 0.1f, maxScale);
}

void WinPin::syncScale(float newScale)
{
	auto clamped = clampScale(newScale);
	if (std::abs(clamped - scale) < 0.0001f) return;
	scale = clamped;
	// 收成缩略图的成员只记倍数：窗口尺寸此刻由 thumbScale 说了算，
	// 等它退出缩略图时 applyWinSize 会按这个新倍数重算
	if (isThumb) return;
	applyWinSize();
	scaleTip = Ling::D2D::get()->makeTextLayout(std::format(L"{}%", static_cast<int>(std::lround(scale * 100.f))), 11.f * dpi);
	setTimer(800, 101);
	layoutTools();
	refresh();
}

// 组内其他成员跟着 src 挪 dx/dy。贴图组要的就是"拖一张等于拖整组"
void WinPin::syncGroupPos(WinPin* src, int dx, int dy)
{
	if (!src || src->groupId == 0) return;
	for (auto& pin : winPins)
	{
		if (pin.get() == src || pin->groupId != src->groupId) continue;
		pin->setPosition(pin->x + dx, pin->y + dy);
	}
}

void WinPin::toggleGroupAll()
{
	// 有任何一个成组就整体解散，否则把当前所有贴图并为一组。
	// 不做"部分成组"：组的语义是"这几张一起动"，半组半不组解释不清
	auto grouped = std::any_of(winPins.begin(), winPins.end(),
		[](const std::unique_ptr<WinPin>& p) { return p->groupId != 0; });
	if (grouped) {
		for (auto& pin : winPins) pin->groupId = 0;
		return;
	}
	auto id = nextGroupId++;
	for (auto& pin : winPins) pin->groupId = id;
}

// 缩略图模式：窗口缩成一枚小图，位置不变。与 Ctrl+M 那条贴边细条是两种收法，
// 细条只剩一条边看不见内容，缩略图还能看清贴的是什么
void WinPin::setThumbMode(bool on)
{
	if (on == isThumb) return;
	auto sz = getImgSize();
	if (!hwnd || sz.width == 0 || sz.height == 0) return;
	if (on && isMinimized) setMinimized(false);   // 两种收法互斥，先退出细条再缩
	isThumb = on;
	if (on) {
		// 窗口尺寸要变，编辑中的文字先收尾 —— 它的位置是按当前倍率算死的
		if (editingShape) editingShape->finishEditing();
		// hover 的夹点也是按当前倍率画的，留着会画到缩略图框外面去
		drawing->shapeHover = nullptr;
		drawing->selected = nullptr;
		savedX = x;
		savedY = y;
		savedW = static_cast<int>(w);
		savedH = static_cast<int>(h);
		// 按长边塞进一个固定大小的框里，短边等比；图本来就比框小就不放大
		constexpr float boxW{ 160.f }, boxH{ 120.f };
		thumbScale = std::min(1.f, std::min(boxW * dpi / sz.width, boxH * dpi / sz.height));
		applyWinSize();
		// 工具条比缩略图本身还大，收起来；还原时再请回来
		if (IsWindowVisible(toolMain->hwnd)) {
			toolsHiddenByThumb = true;
			toolMain->hide();
			toolSub->hideTools();
		}
	}
	else {
		thumbScale = 0.f;
		// 按当前 scale 重算窗口尺寸：成组的贴图被同步缩放过的话，scale 在这期间变过
		applyWinSize();
		SetWindowPos(hwnd, nullptr, savedX, savedY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		if (toolsHiddenByThumb) {
			toolsHiddenByThumb = false;
			// hideTools 把 ToolSub 的内容一起作废了，得按当前工具重建一遍才出得来。
			// 原先这里只认 pin 面板，别的工具（序号、颜色、字体那一堆）从缩略图还原之后
			// 子工具条就再也回不来了 —— 正是"工具栏自己藏起来不见了"的一种
			toolMain->refreshToolSub();
			toolMain->show();
		}
		layoutTools();
	}
	refresh();
}

void WinPin::alignToEdge(UINT key)
{
	// 收起来的话先还原：细条 / 缩略图的 w/h 不是图的尺寸，贴边贴的是那条边
	if (isMinimized) setMinimized(false);
	if (isThumb) setThumbMode(false);
	MONITORINFO mi{ .cbSize = sizeof(MONITORINFO) };
	if (!GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
	auto& wa = mi.rcWork;
	auto newX{ x }, newY{ y };
	// 一律走 clampPos：贴图放大后可能比屏幕还宽，直接减出来会是负数（把窗口甩到屏幕外）
	switch (key) {
	case VK_LEFT:   newX = clampPos((float)wa.left, w, wa.left, wa.right); break;
	case VK_RIGHT:  newX = clampPos((float)wa.right, w, wa.left, wa.right); break;
	case VK_UP:     newY = clampPos((float)wa.top, h, wa.top, wa.bottom); break;
	case VK_DOWN:   newY = clampPos((float)wa.bottom, h, wa.top, wa.bottom); break;
	default: return;
	}
	auto dx = newX - x, dy = newY - y;
	setPosition(newX, newY);
	syncGroupPos(this, dx, dy);
}

// 搬到相邻显示器（dir = -1 往左、+1 往右），保持在本显示器内的相对位置
void WinPin::moveToMonitor(int dir)
{
	if (isMinimized) setMinimized(false);
	if (isThumb) setThumbMode(false);
	std::vector<RECT> monitors;
	EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&monitors));
	if (monitors.size() < 2) return;
	std::sort(monitors.begin(), monitors.end(), [](const RECT& a, const RECT& b) { return a.left < b.left; });
	MONITORINFO cur{ .cbSize = sizeof(MONITORINFO) };
	auto monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	if (!monitor || !GetMonitorInfo(monitor, &cur)) return;
	int index{ 0 };
	for (size_t i = 0; i < monitors.size(); ++i) {
		if (monitors[i].left == cur.rcMonitor.left && monitors[i].top == cur.rcMonitor.top) {
			index = (int)i;
			break;
		}
	}
	auto next = std::clamp(index + dir, 0, (int)monitors.size() - 1);
	if (next == index) return;
	// 用整屏矩形而不是工作区：贴图本来就可以压在任务栏上，没必要替用户躲
	auto& from = cur.rcMonitor;
	auto& to = monitors[next];
	auto newX = clampPos((float)(to.left + (x - from.left)), w, to.left, to.right);
	auto newY = clampPos((float)(to.top + (y - from.top)), h, to.top, to.bottom);
	auto dx = newX - x, dy = newY - y;
	setPosition(newX, newY);
	syncGroupPos(this, dx, dy);
}

void WinPin::applyScale(float newScale, POINT anchor)
{
	auto sz = getImgSize();
	if (sz.width == 0 || sz.height == 0) return;
	newScale = clampScale(newScale);
	if (std::abs(newScale - scale) < 0.0001f) return;
	// 编辑中的文字是 TextBox（真控件）画的，缩放期间它的位置、字号都得跟着重算，
	// 与其在缩放过程里一路同步，不如先收尾把文字交回 ShapeText 自己画 —— 之后它就跟着一起缩了
	if (editingShape) editingShape->finishEditing();
	// anchor 底下那个底图上的点，缩放前后都要停在光标下：屏幕坐标 = 窗口原点 + 底图点 × 倍数
	auto imgX = anchor.x / scale;
	auto imgY = anchor.y / scale;
	scale = newScale;
	applyWinSize();
	// setPosition 内部会 SetWindowPos，随后的 WM_MOVE 会带出 onMoved -> layoutTools
	setPosition(x + anchor.x - static_cast<int>(std::lround(imgX * scale)),
		y + anchor.y - static_cast<int>(std::lround(imgY * scale)));
	scaleTip = Ling::D2D::get()->makeTextLayout(std::format(L"{}%", static_cast<int>(std::lround(scale * 100.f))), 11.f * dpi);
	// 停手 800 毫秒后由定时器把倍数提示收掉。同一个 id 再调一次 SetTimer 就是重新计时，
	// 所以连续滚动期间它一直不会触发
	setTimer(800, 101);
	layoutTools();
	refresh();
	// 成组的贴图跟着一起缩放：一组图钉在屏幕上，放大一张而另外几张不动就对不齐了
	for (auto& pin : winPins)
	{
		if (pin.get() != this && pin->groupId != 0 && pin->groupId == groupId) {
			pin->syncScale(newScale);
		}
	}
}

// 画在窗口右上角，半透明底 + 白字，与 CutMask 上那个坐标标签一个路子。
// 调用方要先把缩放变换收回去：这是窗口装饰，不跟着图一起放大
void WinPin::paintScaleTip(ID2D1DeviceContext* ctx)
{
	if (!scaleTip || !brushTipBg) return;
	DWRITE_TEXT_METRICS tm{};
	if (FAILED(scaleTip->GetMetrics(&tm))) return;
	auto pad = 3.f * dpi;
	auto margin = 5.f * dpi;
	D2D1_RECT_F bgRect{ w - margin - tm.width - pad * 2, margin, w - margin, margin + tm.height + pad * 2 };
	// 图小到装不下提示时，贴着左边画，别画到窗口外面去
	if (bgRect.left < margin) bgRect.left = margin;
	ctx->FillRectangle(bgRect, brushTipBg.Get());
	ctx->DrawTextLayout({ bgRect.left + pad, bgRect.top + pad }, scaleTip.Get(), brushTipText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void WinPin::paintTitle(ID2D1DeviceContext* ctx)
{
	if (!titleLayout || !brushTipBg) return;
	DWRITE_TEXT_METRICS tm{};
	if (FAILED(titleLayout->GetMetrics(&tm))) return;
	auto pad = 4.f * dpi;
	// 贴着边框内侧画，标题条底部留一道 1px 的分隔，与边框区分开
	D2D1_RECT_F bar{ 1.f, 1.f, w - 1.f, 1.f + tm.height + pad * 2 };
	ctx->FillRectangle(bar, brushTipBg.Get());
	ctx->DrawTextLayout({ bar.left + pad, bar.top + pad }, titleLayout.Get(), brushTipText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void WinPin::setOpacity(float v)
{
	// 不透明度低于 20% 基本看不见了，压住下限，免得一档调完图就"消失"
	opacity = std::clamp(v, 0.2f, 1.f);
	// 窗口本身没有背景，整棵合成树就是 body 这一层，调它的不透明度即可全窗生效
	body->visual.Opacity(opacity);
}

void WinPin::setRounded(bool on)
{
	// 状态也记下来：持久化的 round 与 pin 面板的开关都读它，
	// 只改外观不记账的话，圆角既存不进去、面板上也永远显示为关
	isRounded = on;
	body->setBorderRadius(on ? 8.f : 0.f);
	refresh();
}

void WinPin::setLocked(bool on)
{
	// 锁定只拦"对图本身的操作"：不许拖动窗口、不许画、不许改 shape。
	// 工具条是独立窗口，仍然可点，所以解锁这条路永远是通的
	isLocked = on;
}

void WinPin::setMouseThrough(bool on)
{
	// WS_EX_TRANSPARENT 让命中测试直接穿过去。开着的时候图上什么都点不到，
	// 只能从工具条上关掉 —— 所以同样依赖"工具条是独立窗口"这一点
	isThrough = on;
	auto ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
	if (on) ex |= WS_EX_TRANSPARENT;
	else ex &= ~WS_EX_TRANSPARENT;
	SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex);
}

bool WinPin::hasDrawTool() const
{
	return toolMain && !toolMain->curId.empty() && toolMain->curId != L"pin";
}

// 水印工具一点开就把水印铺满整张图，不用再点一下截图区域（ToolSub 上的文字 / 字号 / 间距
// 改一处就重画一遍，它本来就是每次 paint 现读工具条）。
// 水印是"整张图一层"的东西，已经有了就不再加第二层 —— 点它自己的按钮、还是从截图窗口上
// 那个水印按钮直达进来，都只该有一层
void WinPin::ensureWatermark()
{
	if (!drawing || !drawing->history) return;
	for (auto& shape : drawing->history->shapes) {
		if (dynamic_cast<ShapeWatermark*>(shape.get())) return;
	}
	auto sz = drawing->getImgSize();
	drawing->history->createShape(L"watermark", (int)(sz.width / 2), (int)(sz.height / 2));
	refresh();
}

void WinPin::updateNumberPreview(const POINT& imgPos)
{
	// 只在标号工具下预览，而且只预览"落在空白处"的那一下 —— 光标压在已有元素上时，
	// 这一下是选中它（见 onDown），预览一个将要落下的号会误导
	auto sz = drawing->getImgSize();
	if (toolMain->curId != L"number" || imgPos.x < 0 || imgPos.y < 0
		|| imgPos.x >= (LONG)sz.width || imgPos.y >= (LONG)sz.height) {
		hideNumberPreview();
		return;
	}
	if (!numberPreview) numberPreview = std::make_unique<ShapeNumber>(drawing.get(), true);
	numberPreview->previewAt((float)imgPos.x, (float)imgPos.y, toolSub->peekNumberVal());
	numberPreviewOn = true;
	refresh();
}

void WinPin::hideNumberPreview()
{
	if (!numberPreviewOn) return;
	numberPreviewOn = false;
	refresh();
}

void WinPin::setPinTitle(const std::wstring& t)
{
	pinTitle = t;
	titleLayout.Reset();
	if (!t.empty()) {
		// 标题画在窗口物理像素坐标系里（不跟 scale 变换），字号按 dpi 放大才不会在缩放屏上变小
		titleLayout = Ling::D2D::makeTextLayout(t, 13.f * dpi);
	}
	refresh();
}


// 把 ToolMain / ToolSub 摆到 WinPin 周围，始终靠 WinPin 右对齐，并尽量留在屏幕可视区内。
// 三种模式，按 ToolMain+ToolSub 的总高度决定（与 curId 是否为空无关，避免选中按钮时整组跳动）：
//   bottom : WinPin 下方，自上而下 ToolMain -> ToolSub
//   top    : 下方空间不足时改到 WinPin 上方，自上而下 ToolMain -> ToolSub -> WinPin
//   overlay: 上下都不足时覆盖在 WinPin 右下角，整组贴 WinPin 底边
// ToolSub 只在 curId 非空时显示，此时 ToolMain 上移为它腾出空间；ToolSub 永远紧贴 ToolMain 下方，
// 所以它那个朝上的小箭头在三种模式下都不需要翻转。
void WinPin::layoutTools()
{
	if (!toolMain || !toolSub) return;
	// WinPin 的 hwnd 此时可能还没创建（本函数会在构造期调用），所以用矩形而不是窗口句柄找显示器。
	RECT winRect{ x, y, x + static_cast<int>(w), y + static_cast<int>(h) };
	MONITORINFO mi{ .cbSize = sizeof(MONITORINFO) };
	auto monitor = MonitorFromRect(&winRect, MONITOR_DEFAULTTONEAREST);
	if (!monitor || !GetMonitorInfo(monitor, &mi)) {
		auto [sx, sy, sw, sh] = App::get()->getScreenArea();
		mi.rcWork = RECT{ sx, sy, sx + sw, sy + sh };
	}
	auto& wa = mi.rcWork;

	const auto gap = 5.f * dpi;              // 工具栏与 WinPin 之间的间距
	const auto subGap = ToolSub::mainGap;    // ToolMain 与 ToolSub 之间的间距
	const auto mainH = toolMain->h;
	const auto subH = toolSub->getDesiredHeight();
	const auto groupH = mainH + subGap + subH;
	const bool showSub = !toolMain->curId.empty() && toolSub->hasContent();
	// ToolSub 实际显示时 ToolMain 才上移让出它的位置（有些绘图按钮暂时还没有子工具栏）
	const auto usedH = showSub ? groupH : mainH;

	float mainY;
	// 工具条是落在 WinPin 外面还是压在里面。压在里面（overlay）时要额外把工具条提到最前，
	// 见本函数末尾 —— 那底下盖着的是 WinPin 自己
	bool overlay;
	if (winRect.bottom + gap + groupH <= wa.bottom) {         // bottom
		mainY = winRect.bottom + gap;
		overlay = false;
	}
	else if (winRect.top - gap - groupH >= wa.top) {          // top
		mainY = winRect.top - gap - usedH;
		overlay = false;
	}
	else {                                                    // overlay
		mainY = winRect.bottom - gap - usedH;
		overlay = true;
	}
	auto mainX = static_cast<float>(winRect.right) - toolMain->w;
	// 垂直方向按整组当前高度裁剪，避免 WinPin 超出工作区时把 ToolSub 挤到屏幕外
	toolMain->setPosition(clampPos(mainX, toolMain->w, wa.left, wa.right), clampPos(mainY, usedH, wa.top, wa.bottom));
	if (showSub) {
		toolSub->updatePosition(wa);
	}
	else {
		toolSub->hideTools();
	}
	// 重叠模式下工具条压在底图里头。而本窗口的 hwnd 是在两条工具条之后才建的
	//（ToolMain / ToolSub 在 WinPin 构造函数里就 createNativeWindow 了），
	// topmost 组内后建者在上 —— 不提一次的话整条工具条被底图盖得干干净净，
	// 全屏贴图必然如此，用户看着就是"工具栏自己藏起来了"。
	// 只在重叠状态**变化**时动手：本函数在拖动窗口 / 调尺寸时每个鼠标事件都要跑一遍，
	// 而 SetWindowPos 是同步打进窗口管理器的，白调一次就是白等一次
	if (overlay != toolsOverlay) {
		toolsOverlay = overlay;
		if (overlay) raiseTools();
	}
	// 换了工具就把选中态收掉：选中的那一笔是上一个工具留下的，留着它会让工具条上的
	// 样式改动（WinPin::onToolStyleChanged）落到它身上。水印最典型 —— 它是铺满整张图的
	// 一层，一直挂着选中态的话，后面随便调个颜色都作用在它身上
	if (drawing && drawing->selected && drawing->selected->toolId != toolMain->curId) {
		drawing->selected = nullptr;
	}
}

void WinPin::raiseTools()
{
	// 只动 Z 序：位置尺寸归 layoutTools 管，激活状态更不能碰
	//（一激活本窗口就收 WM_KILLFOCUS，正在编辑的文字会被打断）
	if (toolMain && IsWindow(toolMain->hwnd)) {
		SetWindowPos(toolMain->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
	if (toolSub && IsWindow(toolSub->hwnd) && IsWindowVisible(toolSub->hwnd)) {
		SetWindowPos(toolSub->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
}

bool WinPin::isToolsVisible() const
{
	return toolMain && IsWindowVisible(toolMain->hwnd);
}

// 工具栏整组显隐（空格键）。与右键收起**不是**一回事：右键是"专注看图"，顺带把画笔也放掉；
// 空格只是把面板收起来 / 请回来，手里选着的工具、ToolSub 上的设置一概保持原样。
// 所以这里只动窗口，不碰 curId
void WinPin::setToolsVisible(bool on)
{
	if (!toolMain) return;
	if (!on) {
		toolMain->hide();
		toolSub->hideTools();
		return;
	}
	// 缩略图 / 贴边细条这两种收法本来就没给工具条留位置，先还原再谈显示
	if (isThumb) setThumbMode(false);
	if (isMinimized) setMinimized(false);
	// hideTools 把 ToolSub 的内容一起作废了，得按当前工具重建一遍才出得来
	toolMain->refreshToolSub();
	toolMain->show();
	layoutTools();
}

WinPin::~WinPin()
{
}

void WinPin::init(int x, int y, int w, int h, const std::wstring& toolId)
{
	auto ptr = new WinPin(x, y, w, h, nullptr, toolId);
	std::unique_ptr<WinPin> winPin{ ptr };
	ptr->createNativeWindow(WS_EX_TOPMOST| WS_EX_TOOLWINDOW, WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_POPUP);
	winPins.push_back(std::move(winPin));
}

void WinPin::initFromData(int x, int y, int w, int h, std::vector<BYTE>& data)
{
	auto ptr = new WinPin(x, y, w, h, &data);
	std::unique_ptr<WinPin> winPin{ ptr };
	ptr->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_POPUP);
	winPins.push_back(std::move(winPin));
}

void WinPin::initFromAnim(int x, int y, const std::wstring& src, std::vector<AnimFrame>& frames)
{
	if (frames.empty()) return;
	auto& first = frames[0];
	auto ptr = new WinPin(x, y, (int)first.w, (int)first.h, &first.pixels);
	std::unique_ptr<WinPin> winPin{ ptr };
	ptr->animSrc = src;
	ptr->frames = std::move(frames);
	ptr->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_POPUP);
	winPins.push_back(std::move(winPin));
	// 定时器要 hwnd，所以开播排在窗口创建之后。新建的窗口刚压进 winPins，back() 就是它
	winPins.back()->setAnimPlaying(true);
}

// 动图换帧只换底图像素：窗口尺寸、shapes、缩放倍数都不动 ——
// 走 swapImage 那套会把标注清掉、把缩放打回 1 倍，播起来就是一路闪
void WinPin::showFrame(int index)
{
	if (index < 0 || index >= (int)frames.size()) return;
	auto& frame = frames[index];
	if (!drawing->screenImg) return;
	auto sz = drawing->screenImg->GetPixelSize();
	if (sz.width != frame.w || sz.height != frame.h) return;
	drawing->screenImg->CopyFromMemory(nullptr, frame.pixels.data(), frame.w * 4);
	refresh();
}

void WinPin::setAnimPlaying(bool on)
{
	if (!hasAnim()) return;
	animPlaying = on;
	// 每帧的停留时间不一样，所以定时器不能设一次管到底：到点先撤，按下一帧的延时重设
	killTimer(102);
	if (on) setTimer(frames[frameIndex].delayMs, 102);
}

void WinPin::toggleAnim()
{
	setAnimPlaying(!animPlaying);
}

void WinPin::onCreated()
{
    disableBorderRadius();
    auto d2d = Ling::D2D::get();
    // 画布铺满窗口，走 swap chain（双缓冲）后端，避免拖动 shape 时整帧闪烁
    canvas = body->makeChild<Ling::Canvas>();
    canvas->enableSwapChain();
    canvas->setSizePercent(100.f, 100.f);
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x1677ff), borderBrush.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.46f), brushTipBg.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushTipText.GetAddressOf());
    show();
    // 本窗口的 hwnd 是刚刚才建的，压在两条工具条之上（见 layoutTools 末尾）。
    // 构造期那次 layoutTools 已经把 toolsOverlay 算出来了，但那时本窗口还没建窗口、提也没用，
    // 到这里补一次，工具条才真的浮在全屏底图上面
    if (toolsOverlay) raiseTools();
}

void WinPin::layout()
{
    Ling::WinBase::layout();
    if (!drawing->screenImg || !canvas) return;
    auto ctx = canvas->startPaint();
    if (!ctx) return;
    ctx->Clear(0);
    auto sz = drawing->screenImg->GetSize();
    D2D1_RECT_F destRect = D2D1::RectF(0, 0, sz.width, sz.height);
    // 底图和 shape 都是按底图像素画的，放大缩小整个交给这个变换，
    // 笔宽、夹点跟着一起缩 —— 鼠标坐标进来时也除掉了倍数，所以命中判定天然对得上。
    // 倍数取 viewScale：缩略图模式下窗口被缩成小图，画的时候也得跟着缩，否则只剩左上角一块
    auto vs = viewScale();
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(vs, vs));
    ctx->DrawBitmap(drawing->screenImg.Get(), destRect);
	for (auto& shape : drawing->history->shapes)
	{
		if (!shape->isUndo) {
			shape->paint(ctx);
		}
	}
	// 标号工具的 hover 预览压在标注上面：它是"马上要落下的这一笔"，本来就不该被别的元素盖住。
	// 缩略图 / 细条态下不画 —— 那时候的变换与光标位置对不上
	if (numberPreviewOn && numberPreview && !isThumb && !isMinimized) {
		numberPreview->paint(ctx);
	}
	// 手柄是"这里能拖动"的提示，所以悬停的那个要画；选中的那个画得更重（实心），
	// 好让人看得出改样式会作用到谁。两者同时指着一个元素时只画一次
	if (!isMouseDown) {
		if (drawing->selected && drawing->selected != drawing->shapeHover) {
			drawing->selected->paintDragger(ctx);
		}
		if (drawing->shapeHover) {
			drawing->shapeHover->paintDragger(ctx);
		}
		// 选中元素外侧那几枚动作图标（右上 × / 左上动作图标，右下那枚旋转手柄
		// 由它自己的 paintDragger 画）。只给"当前选中"的那一个画：
		// 鼠标掠过一串元素时每个都冒一枚 ×，反而看不出改样式 / 删除会作用到谁
		if (drawing->selected) {
			drawing->selected->paintActionBtns(ctx);
		}
	}
	// 蓝边框和倍数提示属于窗口装饰，不跟着图缩放：变换收回来，按窗口坐标画。
	// 边框也因此从"底图矩形"改成"窗口矩形"，任何倍数下都是 2*dpi 粗
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	ctx->DrawRectangle(D2D1::RectF(0.f, 0.f, w, h), borderBrush.Get(), 2*dpi);
	paintTitle(ctx);
	paintScaleTip(ctx);
    canvas->finishPaint();
}

void WinPin::onMinMaxInfo(MINMAXINFO* mmi)
{
	auto [x, y, w, h] = App::get()->getScreenArea();
	mmi->ptMaxPosition.x = x;
	mmi->ptMaxPosition.y = y;
	mmi->ptMaxSize.x = w;
	mmi->ptMaxSize.y = h;
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
	// Ctrl+滚轮放大后窗口可以比屏幕大（超出的部分自然被裁掉）。默认的最大跟踪尺寸只有
	// 主显示器那么大，不放开的话 SetWindowPos 会被系统按住，放大就到此为止了
	mmi->ptMaxTrackSize.x = 20000;
	mmi->ptMaxTrackSize.y = 20000;
}

void WinPin::onDown(POINT pos, BOOL isRight)
{
	// 锁定后图上什么都不许动。右键单独放开：收/放工具条是解锁的入口，
	// 全拦了的话锁死的贴图就只能靠任务栏找回工具条
	if (isLocked && !isRight) return;
	// 缩略图上点一下就还原。放在所有分支之前：这一下是"把图放回来"，
	// 不是画画也不是拖窗。lastDownTime 清零是免得紧接着的第二下被认成双击（双击 = 复制关窗）
	if (isThumb) {
		setThumbMode(false);   // 工具条由 setThumbMode 自己按"是不是它收的"请回来
		lastDownTime = 0;
		return;
	}
	// 编辑文本时，落在文本框里的点击整个交给 TextBox（它自己订阅了窗口的鼠标事件）。
	// 这里不能抢先 SetCapture / 置 isMouseDown，否则拖选文本会被当成拖 shape。
	if (editingShape && textBox && textBox->isPosIn(pos)) return;
	// 图上有一下真实的点击了：预览的那个"将要落下的号"该让位给真的那一笔
	// （右键收起工具条、双击导出也一样，这会儿图上不该再飘着一个影子）
	hideNumberPreview();
	if (isRight) {
		// 右键在"有工具条"和"只剩图"这两个状态之间来回切。
		// 藏着的时候（上一次右键收起来的）就把它请回来。位置先重排一遍：
		// 藏着的这段时间里窗口可能被 Ctrl+滚轮缩放过，工具条的落点跟着变了
		if (!isToolsVisible()) {
			layoutTools();
			toolMain->refreshToolSub();
			toolMain->show();
			return;
		}
		// 显示着：清掉画笔选中态，把两条工具条一起收起来，只剩图本身。
		// cancelSelect 里已经顺手隐藏了 ToolSub 并重排整组，但它在 curId 本来就空时会提前返回，
		// 所以 ToolSub 这一下自己再收一次，右键的效果与当时选没选画笔无关。
		// 左键点一下（抬手时，见 onUp）也能把 ToolMain 请回来。
		// 空格键是另一套：它只收放面板，不动画笔（见 setToolsVisible）
		toolMain->cancelSelect();
		toolMain->hide();
		toolSub->hideTools();
		return;
	}
	// 双击判定得自己做，做法同 WinCap::onDown：Ling 的窗口类没带 CS_DBLCLKS，
	// WM_LBUTTONDBLCLK 根本不会来，只能拿系统的双击间隔和双击判定框自己认。
	// 与 WinCap 唯一的不同是这里比屏幕坐标而不是客户区坐标：拖动贴图窗口时窗口跟着光标走，
	// 抓住的那一点始终停在光标下，光标的客户区坐标几乎不变 —— 用客户区坐标会把
	// "拖一下松手再拖一下"当成双击，图和标注就这么被复制走关掉了。
	// 而拖动必然意味着光标在屏幕上真的移动过，屏幕坐标能把这种情况分开
	POINT screenPos{};
	GetCursorPos(&screenPos);
	auto now = GetTickCount64();
	bool isDblClick = (now - lastDownTime <= GetDoubleClickTime())
		&& std::abs(screenPos.x - lastDownPos.x) <= GetSystemMetrics(SM_CXDOUBLECLK)
		&& std::abs(screenPos.y - lastDownPos.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
	lastDownTime = now;
	lastDownPos = screenPos;
	// 双击 = Ctrl+C：把图连标注一起送进剪切板并关窗，选着画笔也一样（等价于按 Ctrl+C，
	// 手里拿着什么工具都不该影响这个手势）。要在下面所有分支之前处理：
	// 这一下既不是画画也不是拖窗，不该留下 capture、更不该新建 shape。
	// 编辑文字时不算：双击归文本框（选中单词），点在框外才会走到这里
	if (isDblClick && !editingShape) {
		// 前半段那一下点击是这个手势的一部分，它顺手放下的元素（只有序号是按一下就成形的，
		// 别的都在抬手时按"没画出东西"清掉了）不该被带进剪切板
		if (prevPressCreatedShape) drawing->history->undo();
		copyToClipboard();
		return;
	}
	// 选中元素外侧那几枚按钮：右上角的 × 是删掉，左上角是元素自己的动作图标（矩形/圆用它互转）。
	// 要赶在下面 SetCapture / 建新元素之前 —— 这些按钮摆在外接矩形之外，
	// 不拦的话这一下会被当成"点空白"，反手又落一个新元素。
	// 也赶在"给正在编辑的那个收尾"之前：编辑器开着时这些按钮同样该点得动
	{
		auto hitPos = toImgPos(pos);
		if (drawing->selected) {
			auto idx = drawing->selected->hitActionBtn((float)hitPos.x, (float)hitPos.y);
			if (idx >= 0) {
				drawing->selected->onActionBtn(idx);
				return;
			}
		}
	}
	// 点在文本框外：先把正在编辑的那一个收尾，再往下派发。
	// 必须赶在这儿做，不能等 TextBox 自己那一层 —— TextBox 的 onMouseDown 订阅比本窗口晚
	// （它是懒建的），它那一下失焦排在下面这一整套派发之后：等它跑起来时新的一笔已经建好、
	// 新的编辑也已经开了，两个 finishEdit 叠着跑，会把刚写的描述文字一起冲掉
	//（表现就是"单击 A 没反应"，而且上一个序号的描述还会被清空）
	if (editingShape) {
		editingShape->finishEditing();
		// 这一下只是"关掉编辑器"：落在空白处就到此为止，不该顺手再落一个新元素
		if (!drawing->shapeHover) return;
	}
	// 记的是按下点在窗口内的偏移（客户区坐标），拖动时用它把抓住的那一点保持在光标下
	pressPos.x = pos.x;
	pressPos.y = pos.y;
	isMouseDown = true;
	hasDragged = false;
	SetCapture(hwnd);
	// 没选画笔，或只开着贴图属性面板（都画不了），左键是拖窗口，拖的时候把工具条收起来
	if (!hasDrawTool()) {
		toolMain->hide();
		return;
	}
	// 以下都是交给 shape 的坐标，一律换算成底图像素（拖窗口那条路仍用窗口坐标）
	auto imgPos = toImgPos(pos);
	if (drawing->shapeHover) {
		// 点在已有元素上：这一下建立选中。选中态独立于悬停，移开鼠标也不会丢
		drawing->selected = drawing->shapeHover;
		drawing->shapeCur = nullptr; //改的是已有元素，不参与空元素判定
		drawing->shapeHover->mouseDown((float)imgPos.x, (float)imgPos.y);
		return;
	}
	// 点在空白处：取消选中。下面新建的这笔如果只是单击，抬手时空笔判定会把它自己收掉
	drawing->selected = nullptr;
	drawing->shapeHover = drawing->history->createShape(toolMain->curId, imgPos.x, imgPos.y);
	drawing->shapeCur = drawing->shapeHover;
}

void WinPin::onMove(POINT pos)
{
	// 缩略图上不做 hover / 命中：点一下是"还原"，夹点也没有地方摆
	if (isThumb) return;
	// 收成细条时鼠标一碰就展开 —— 这是细条唯一的展开方式
	if (isMinimized) {
		setMinimized(false);
		return;
	}
	if (isLocked) return;   // 锁定时不给 hover 高亮，也不给拖动
	// 同 onDown：文本框里的移动归 TextBox（拖选、滚动条 hover），不参与 shape 的 hover 判定
	if (editingShape && textBox && textBox->isPosIn(pos)) return;
	// 拖窗口用的是窗口坐标（pressPos 也是），只有交给 shape 的才换算成底图像素
	auto imgPos = toImgPos(pos);
	if (isMouseDown) {
		if (!hasDrawTool()) {
			auto newX = x + pos.x - pressPos.x;
			auto newY = y + pos.y - pressPos.y;
			auto dx = newX - x, dy = newY - y;
			setPosition(newX, newY);
			// 成组的贴图跟着一起挪，整组的相对位置不变
			syncGroupPos(this, dx, dy);
			return;
		}
		else if(drawing->shapeHover) {
			// 光标一步没挪也会来 WM_MOUSEMOVE，所以跟按下点比一下再算拖动
			if (pos.x != pressPos.x || pos.y != pressPos.y) hasDragged = true;
			drawing->shapeHover->mouseDrag((float)imgPos.x, (float)imgPos.y);
			refresh();
		}
	}
	else
	{
		if (!hasDrawTool()) {
			// 画笔被收起来了（右键 / 关掉工具），预览也要跟着收
			hideNumberPreview();
			return;
		}
		int i{ (int)(drawing->history->shapes.size() - 1) };
		for (; i >= 0; i--)
		{
			auto cur = drawing->history->shapes[i].get();
			if (cur->isUndo) continue;
			cur->mouseMove((float)imgPos.x, (float)imgPos.y);
			if (cur->hoverDraggerIndex >= 0) {
				if (drawing->shapeHover != cur) {
					drawing->shapeHover = cur;
					setTimer(800, 100);
					refresh();
				}
				// 落在已有元素上，这一下是选中它：没有"将要落下的号"可预览
				hideNumberPreview();
				return;
			}
		}
		if (drawing->shapeHover) {
			drawing->shapeHover = nullptr;
		}
		updateNumberPreview(imgPos);
	}
}

void WinPin::onUp(POINT pos, BOOL isRight)
{
	// 右键按下时什么都没抓（既没置 isMouseDown 也没 SetCapture，见 onDown），抬手也就没什么要收的。
	// 更要紧的是不能往下走：下面那条"拖窗结束"的路会把 ToolMain 显示出来，
	// 而右键刚刚才把它收起来 —— 一按一放就等于什么都没做
	if (isRight) return;
	isMouseDown = false;
	ReleaseCapture();
	auto justCreated = drawing->shapeCur;
	drawing->shapeCur = nullptr;
	// 这一下按下有没有新建出一个留得住的元素：紧接着来第二下凑成双击时要把它撤掉（见 onDown）
	prevPressCreatedShape = false;
	if (!hasDrawTool()) {
		// 没选画笔，这一下要么是拖完窗口（按新位置重排工具条），要么只是点了一下 ——
		// 两种情况都把 ToolMain 显示出来：拖动期间它是藏着的，右键之后它也是藏着的，
		// 左键点一下就是"我还要用工具条"。ToolSub 由 curId 驱动，这会儿仍然不该出来，
		// layoutTools 里已经管了
		layoutTools();
		toolMain->show();
	}
	else if (drawing->shapeHover) {
		// 新建的这一笔按下马上弹起，什么也没画出来：直接丢掉，
		// 也省了 mouseUp 里的收尾开销（马赛克那边要把 GPU 像素读回内存，不该为一个要删的元素白做）
		if (drawing->shapeHover == justCreated && !hasDragged && !drawing->shapeHover->isValidWithoutDrag()) {
			drawing->history->removeShape(drawing->shapeHover); //它会顺手清掉 drawing->shapeHover 并刷新
			return;
		}
		prevPressCreatedShape = (drawing->shapeHover == justCreated);
		auto imgPos = toImgPos(pos);
		drawing->shapeHover->mouseUp((float)imgPos.x, (float)imgPos.y);
		// 画完的这一笔保持选中，紧接着就能改它的样式。上面空笔那条路已经 return 了，
		// 走到这里的都是留在图上的
		drawing->selected = drawing->shapeHover;
		refresh();
		setTimer(800, 100);
	}

}

void WinPin::onTimerCB(UINT id)
{
	if (id == 101) { //缩放停手了，收掉右上角的倍数提示
		killTimer(101);
		scaleTip = nullptr;
		refresh();
		return;
	}
	if (id == 102) {   // 动图：翻到下一帧，并按这一帧自己的延时重新起表
		killTimer(102);
		frameIndex = (frameIndex + 1) % (int)frames.size();
		showFrame(frameIndex);
		if (animPlaying && hasAnim()) setTimer(frames[frameIndex].delayMs, 102);
		return;
	}
	if (id != 100) return;
	if (!drawing->shapeHover) {
		refresh();
		killTimer(100);
	}
}

Ling::TextBox* WinPin::getTextBox()
{
	if (textBox) return textBox;
	// 建在 canvas 之后：Composition 的子 visual 按插入顺序叠放，文本框要盖在截图上面。
	// 绝对定位，位置由 ShapeText 按自己的矩形指定，不参与 body 的 flex 排布。
	textBox = body->makeChild<Ling::TextBox>();
	textBox->setPositionType(Ling::Position::Absolute);
	// 不折行、尺寸跟着文字长，与 2.4.25 的文本窗口一致
	textBox->setAutoSize(true);
	// 背景、边框都不画：编辑中看到的就是最终效果，那圈虚线框由 ShapeText 自己画
	textBox->hide();
	return textBox;
}

void WinPin::setEditingShape(ShapeBase* shape)
{
	editingShape = shape;
}

// ---- CanvasHost ----
// 前四个都在头文件里内联了：就一条取值，跟着成员声明放一起更好读
const std::wstring& WinPin::curToolId() const
{
	return toolMain->curId;
}

void WinPin::requestRefresh()
{
	refresh();
}

History* WinPin::getHistory() const
{
	return drawing->history.get();
}

void WinPin::onToolStyleChanged()
{
	// 优先级：正在编辑的文本 > 选中的元素。两者都没有就什么都不改 ——
	// 这条链路以前只认 editingShape，选中态没有单独的载体，选中的矩形族
	// 连 applyStyle 都没实现，颜色永远是构造那一刻的快照
	auto target = editingShape ? editingShape : drawing->selected;
	if (!target) return;
	target->applyStyle();
	refresh();
}

// ToolSub 上的编号样式 / 外圈样式切换之后：图上已经画着的序号要跟着换样子，
// 而不是等下一次新画的才生效。全量重排的代价可以忽略 —— 一张图上序号通常是个位数
void WinPin::refreshNumberShapes()
{
	for (auto& shape : drawing->history->shapes)
	{
		auto number = dynamic_cast<ShapeNumber*>(shape.get());
		if (number) number->applyStyle();
	}
	refresh();
}

// 「应用到全部」：把工具条当前样式套到图上同工具的所有标注。
// 只认同类 —— 颜色是按工具各存一份的，跨类型套会让文字、序号被矩形的颜色污染
void WinPin::applyStyleToAllShapes()
{
	for (auto& shape : drawing->history->shapes)
	{
		if (shape->isUndo) continue;
		if (shape->toolId == toolMain->curId) shape->applyStyle();
	}
	refresh();
}

void WinPin::onKey(UINT key)
{
	if (isLocked) return;
	// 编辑文本时所有按键都归 TextBox：否则 Ctrl+C 复制的是截图、回车会保存并关窗、
	// Delete 删掉的是整个 shape、ESC 直接把窗口关了。ESC 结束编辑由 TextBox 自己处理。
	// 这一句必须排在派发之前 —— 序号拿到 F2 会去开它自己那份描述编辑框，而共用的那个
	// TextBox 上还挂着当前这一个的订阅，两个编辑叠在一起就会把正在写的文字冲掉
	if (editingShape) return;
	// 选中某个元素时先把按键交给它：序号用 +/- 改编号、F2 编辑序号里的文字。
	// 这几个键不与下面的全局快捷键冲突，所以不用抢返回值
	if (drawing->shapeHover) drawing->shapeHover->onKey(key);
	bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
	bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
	// Ctrl+Alt+左右：搬到相邻显示器；Alt+方向：贴到当前显示器的那条边。
	// 两者都带方向键，所以先判组合更多的那个
	if (ctrl && alt && (key == VK_LEFT || key == VK_RIGHT)) {
		moveToMonitor(key == VK_LEFT ? -1 : 1);
	}
	else if (alt && (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN)) {
		alignToEdge(key);
	}
	else if (ctrl && key == 'T') {      // Ctrl+T：缩略图模式 / 还原
		setThumbMode(!isThumb);
	}
	else if (ctrl && key == 'Z') {
		drawing->history->undo();
	}
	else if (ctrl && key == 'Y') {
		drawing->history->redo();
	}
	else if (ctrl && key == 'C') {
		copyToClipboard();
	}
	else if (ctrl && key == 'S') {
		saveToFile();
	}
	else if (key == VK_RETURN) {
		copyToClipboard();
	}
	else if (key == VK_DELETE) {
		drawing->history->removeActiveShape();
	}
	else if (key == VK_PRIOR) {     // PageUp：往前翻历史截图（更早的那张）
		previewHistory(1);
	}
	else if (key == VK_NEXT) {      // PageDown：往回翻（更新的那张）
		previewHistory(-1);
	}
	else if (ctrl && key == 'M') {  // Ctrl+M：收成贴边细条 / 展开。悬停细条也会展开
		setMinimized(!isMinimized);
	}
	// 空格：显示 / 隐藏整组工具条。工具条被右键收掉、被缩略图收掉、或者在全屏贴图上被
	// 底图盖住看不见的时候，它都是"把工具条找回来"的那一下。
	// 只有"工具条正显示着"且"贴的是动图"时空格仍是老语义（播放 / 暂停）——
	// 那种情况要收工具条用右键
	else if (key == VK_SPACE) {
		if (hasAnim() && isToolsVisible()) toggleAnim();
		else setToolsVisible(!isToolsVisible());
	}
	else if (key == VK_ESCAPE) {
		close();
	}
}

// 翻历史截图：把底图换成历史里第 previewIndex + step 张（0 = 最新）。
// 旧图上的标注跟着作废 —— 换了底图，坐标就对不上了
void WinPin::previewHistory(int step)
{
	auto shots = ShotHistory::get();
	if (!shots) return;
	if (previewIndex < 0) previewIndex = 0;
	auto list = shots->list(ShotHistory::Source::Shot);
	if (list.empty()) return;
	auto next = std::clamp(previewIndex + step, 0, (int)list.size() - 1);
	if (next == previewIndex) return;
	std::vector<BYTE> data;
	int w{ 0 }, h{ 0 };
	if (!shots->loadImage(list[next], data, w, h)) return;
	if (!swapImage(data, w, h)) return;
	previewIndex = next;
}

// 换底图。共用构造函数里那套位图属性：ShapeMosaic / ShapeEraser 把它当取样源，
// 属性不一致的话马赛克会取错
bool WinPin::swapImage(const std::vector<BYTE>& data, const int w, const int h)
{
	if (w <= 0 || h <= 0 || data.empty()) return false;
	// 换了底图，动图的帧就对不上了（尺寸、内容都不是原来那张），一并丢掉停播。
	// shrink_to_fit 是必要的：几百帧的大图能占几十 MB，clear 只是把 size 归零
	killTimer(102);
	frames.clear();
	frames.shrink_to_fit();
	frameIndex = 0;
	animPlaying = false;
	animSrc.clear();
	// 正显示着 pin 面板的话，那个播放按钮已经没东西可播，重建一下把它去掉
	if (toolMain && toolMain->curId == L"pin") toolSub->showPinTools();
	D2D1_BITMAP_PROPERTIES1 props{};
	props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
	props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
	props.dpiX = 96.0f;
	props.dpiY = 96.0f;
	ComPtr<ID2D1Bitmap1> bmp;
	if (FAILED(Ling::D2D::get()->deviceContext->CreateBitmap(D2D1::SizeU(w, h), data.data(),
		w * 4, &props, bmp.GetAddressOf()))) return false;
	drawing->screenImg = bmp;
	scale = 1.f;
	drawing->history->shapes.clear();
	drawing->shapeHover = nullptr;
	drawing->selected = nullptr;
	editingShape = nullptr;
	applyWinSize();
	layoutTools();
	refresh();
	return true;
}

// 收成一条贴在屏幕左缘的细条。悬停（onMove）即展开，Ctrl+M 再收回去
void WinPin::setMinimized(bool on)
{
	if (on == isMinimized) return;
	if (on && isThumb) setThumbMode(false);   // 两种收法互斥，先把缩略图还原成整图再收细条
	isMinimized = on;
	if (on) {
		savedX = x;
		savedY = y;
		savedW = static_cast<int>(w);
		savedH = static_cast<int>(h);
		MONITORINFO mi{ sizeof(MONITORINFO) };
		GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
		auto stripW = static_cast<int>(std::lround(6 * dpi));
		w = static_cast<float>(stripW);
		h = static_cast<float>(savedH);
		SetWindowPos(hwnd, nullptr, mi.rcWork.left, savedY, stripW, savedH,
			SWP_NOZORDER | SWP_NOACTIVATE);
	}
	else {
		w = static_cast<float>(savedW);
		h = static_cast<float>(savedH);
		SetWindowPos(hwnd, nullptr, savedX, savedY, savedW, savedH,
			SWP_NOZORDER | SWP_NOACTIVATE);
		layoutTools();
	}
	refresh();
}

void WinPin::copyToClipboard()
{
	std::vector<BYTE> pixels;
	D2D1_SIZE_U size{};
	if (!getImagePixels(pixels, size)) return;
	Util::saveToClipboard((int)size.width, (int)size.height, pixels.data());
	close();
}

void WinPin::saveToFile()
{
	auto foregroundBeforeDialog = GetForegroundWindow();
	auto path = Util::resolveSavePath(hwnd);
	if (path.empty()) {   // 用户取消
		restoreWindowState(foregroundBeforeDialog);
		return;
	}
	std::vector<BYTE> pixels;
	D2D1_SIZE_U size{};
	if (!getImagePixels(pixels, size)) {
		restoreWindowState(foregroundBeforeDialog);
		return;
	}
	auto fmt = (Util::ImgFormat)Util::getSaveFormat();
	if (Util::saveToFile(path, (int)size.width, (int)size.height, pixels.data(), fmt)) {
		close();
	}
	else {
		restoreWindowState(foregroundBeforeDialog);
	}
}

// 另存为对话框关掉后会把 owner(hwnd) 变成活动窗口，WinPin 一被激活就会盖住 ToolMain。
// 这里把三个窗口重新压到 topmost，并把前台还给开对话框之前的那个窗口。
void WinPin::restoreWindowState(HWND foregroundBeforeDialog)
{
	SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	if (toolMain && toolMain->hwnd && IsWindowVisible(toolMain->hwnd)) {
		SetWindowPos(toolMain->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
	if (toolSub && toolSub->hwnd && IsWindowVisible(toolSub->hwnd)) {
		SetWindowPos(toolSub->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
	if (foregroundBeforeDialog && foregroundBeforeDialog != hwnd && IsWindowVisible(foregroundBeforeDialog)) {
		SetForegroundWindow(foregroundBeforeDialog);
	}
}

// 离屏把底图和 shape 合成到一张新位图上再读回像素。
// 不直接画到 drawing->screenImg 上：橡皮擦那两个是它的"原样"来源，马赛克那两个拿它取样，
// 一旦被 shape 覆写，之后再擦除/打码就会拿到已经画过的画面。
// 用 d2d->deviceContext 做离屏是安全的，SetTarget → BeginDraw → EndDraw → SetTarget(nullptr) 在本函数内闭环。
bool WinPin::getImagePixels(std::vector<BYTE>& pixels, D2D1_SIZE_U& size)
{
	// 尺寸一律取底图的像素尺寸，不用窗口的 w/h —— Ctrl+滚轮缩放改的是窗口，
	// 导出的图该始终是原始大小。调用方也得按这个尺寸解释 pixels，所以用出参交出去
	auto imgSize = getImgSize();
	if (imgSize.width == 0 || imgSize.height == 0) return false;
	// 编辑中的文字是 TextBox 自己那层画的，进不了下面这个离屏 target。
	// 先收尾，把文字交回 ShapeText 自己画，保存/复制出去的图才有它。
	if (editingShape) editingShape->finishEditing();
	size = imgSize;
	auto d2d = Ling::D2D::get();
	auto ctx = d2d->deviceContext.Get();

	D2D1_BITMAP_PROPERTIES1 targetProps{
		.pixelFormat{ D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED) },
		.dpiX{ 96.0f }, .dpiY{ 96.0f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_TARGET }
	};
	ComPtr<ID2D1Bitmap1> targetBmp;
	auto hr = ctx->CreateBitmap(size, nullptr, 0, &targetProps, targetBmp.GetAddressOf());
	if (FAILED(hr)) return false;

	ctx->SetTarget(targetBmp.Get());
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	ctx->BeginDraw();
	ctx->Clear(D2D1::ColorF(0, 0.0f));
	ctx->DrawBitmap(drawing->screenImg.Get(), D2D1::RectF(0.f, 0.f, (float)imgSize.width, (float)imgSize.height));
	for (auto& shape : drawing->history->shapes)
	{
		if (!shape->isUndo) {
			shape->paint(ctx);
		}
	}
	hr = ctx->EndDraw();
	// 解绑，下面 CopyFromBitmap 才能把它当 source 读
	ctx->SetTarget(nullptr);
	if (FAILED(hr)) return false;

	// GPU 上的 target 位图不能直接 Map，得先拷到一块带 CPU_READ 的位图上
	D2D1_BITMAP_PROPERTIES1 cpuProps{
		.pixelFormat{ targetBmp->GetPixelFormat() },
		.dpiX{ 96.0f }, .dpiY{ 96.0f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW }
	};
	ComPtr<ID2D1Bitmap1> cpuBmp;
	hr = ctx->CreateBitmap(size, nullptr, 0, &cpuProps, cpuBmp.GetAddressOf());
	if (FAILED(hr)) return false;
	hr = cpuBmp->CopyFromBitmap(nullptr, targetBmp.Get(), nullptr);
	if (FAILED(hr)) return false;
	D2D1_MAPPED_RECT mapped{};
	hr = cpuBmp->Map(D2D1_MAP_OPTIONS_READ, &mapped);
	if (FAILED(hr)) return false;
	// mapped.pitch 按 GPU 行对齐，可能大于 w*4；剪切板和 WIC 都要求紧凑步长，逐行紧缩
	const UINT32 rowBytes = size.width * 4;
	pixels.resize((size_t)rowBytes * size.height);
	for (UINT32 row = 0; row < size.height; ++row)
	{
		CopyMemory(pixels.data() + (size_t)row * rowBytes,
			mapped.bits + (size_t)row * mapped.pitch,
			rowBytes);
	}
	cpuBmp->Unmap();
	return true;
}

BOOL WinPin::setCursor()
{
	// 编辑文本时光标形状交给 TextBox 决定（文本区 I 形、滚动条箭头）。
	// 本函数覆写了基类且不调用它，TextBox 挂在 onCursor 上的那个订阅不会自己被触发，得手动发一次。
	if (editingShape) {
		bool handled{ false };
		onCursor(&handled);
		if (handled) return TRUE;
	}
	if (!hasDrawTool()) {
		// 画不了的时候是拖窗手势，给十字箭头（含 pin 面板开着的时候）
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
		return TRUE;
	}
	if (drawing->shapeHover) {
		drawing->shapeHover->setCursor();
		return TRUE;
	}
	if (toolMain->curId == L"text") {
		SetCursor(LoadCursor(nullptr, IDC_IBEAM));
	}
	else if (toolMain->curId == L"number") {
		// 标号落笔前会在光标处画一个"将要落下的编号"预览，十字光标正好压在它身上、
		// 把编号挡得看不清 —— 改回普通箭头，预览就是这一步唯一的位置提示
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
	}
	else {
		SetCursor(LoadCursor(nullptr, IDC_CROSS));
	}
	return TRUE;
}
