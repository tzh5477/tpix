#include "pch.h"
#include <algorithm>
#include <cmath>
#include <thread>
#include "../Canvas.h"
#include "../Tool/ToolMain.h"
#include "../Tool/ToolSub.h"
#include "../Shape/ShapeBase.h"
#include "../Shape/ShapeText.h"
#include "../Shape/ShapeImage.h"
#include "../Shape/ShapeNumber.h"
#include "../Shape/ShapeWatermark.h"
#include "WinPin.h"
#include "WinCap.h"
// 只为了读 WinCap 上那份选区（常驻剪裁要知道"这张贴图占的是整屏原图的哪一块"）
#include "CutMask.h"
#include "PinHiddenBar.h"
#include "History.h"
#include "../App.h"
#include "../Lang.h"
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
	// 左上角隐藏条上那个颜色序号的分发器。按创建顺序发，所以同一张图从头到尾一个颜色，
	// 不会因为中途放掉别的贴图就换色（见 WinPin::getBarColorIndex）
	int nextBarColorIndex{ 0 };
	// ---- tpix 内部的对象剪贴板（Ctrl+C / Ctrl+X / Ctrl+V）----
	// 存的是 clone 出来的形状本体而不是指针：源窗口关掉之后这份还在，粘到别的贴图窗口也成立。
	// 刻意不用系统剪贴板 —— 那边在贴图窗口里是"复制整张图"的通道（Ctrl+C 无选中、双击、
	// 工具条「复制」都往那儿写），两套混在一起必然互相覆盖。
	// 每次粘贴都从这份原件再 clone 一份，所以可以连着粘好几次
	std::vector<std::unique_ptr<ShapeBase>> shapeClipboard;
	// 已经粘过几次。第一份就落在原处会跟原件完全重叠，看着像"什么都没发生"，按这个数错开落点
	int clipPasteCount{ 0 };

	int clampPos(float val, float size, int min, int max)
	{
		auto upper = max - static_cast<int>(size);
		if (upper < min) upper = min;
		auto result = static_cast<int>(val);
		if (result < min) result = min;
		if (result > upper) result = upper;
		return result;
	}

	// 矩形上那 8 个标记点的中心：四角 + 四边中点（0 左上、顺时针）。
	// 多选时每个对象按自己的外接框回显一圈，与单选时那 8 枚夹点的摆法同一套
	void rectHandleCenters(const D2D1_RECT_F& b, D2D1_POINT_2F(&c)[8])
	{
		auto mx{ (b.left + b.right) / 2.f }, my{ (b.top + b.bottom) / 2.f };
		c[0] = { b.left, b.top };
		c[1] = { mx, b.top };
		c[2] = { b.right, b.top };
		c[3] = { b.right, my };
		c[4] = { b.right, b.bottom };
		c[5] = { mx, b.bottom };
		c[6] = { b.left, b.bottom };
		c[7] = { b.left, my };
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
	// 隐藏条上那一条的颜色在这里就定下来，之后不再变
	barColorIndex = nextBarColorIndex++;
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
		auto winCap = WinCap::get();
		// getCutImg() 只给选区那一块，而常驻剪裁的采样点要能往外拖，得知道"框外还有什么"。
		// 于是把整屏原图和"当前显示的是它里面哪一块"一并记下来（见 ensureCropSource）。
		// 底图本身仍用 getCutImg()：手绘选区那圈透明外边是它抠出来的，自己切矩形会丢
		drawing->screenImg = winCap->getCutImg();
		srcImg = winCap->getScreenImg();
		// 与 getCutImg() 同一套取整：左上加尺寸，而不是各自截断右 / 下 —— 否则浮点选区
		// 可能让 cropRect 比刚拿到的那张底图宽出一两个像素，往后一拖就错位
		auto& r = winCap->cutMask->maskRect;
		const int cx = (int)r.left, cy = (int)r.top;
		const int cw = (int)(r.right - r.left), ch = (int)(r.bottom - r.top);
		cropRect = D2D1::RectU((UINT32)cx, (UINT32)cy, (UINT32)(cx + cw), (UINT32)(cy + ch));
		// 整屏原图与选区是同一套像素坐标（screenImg 抓的就是这一整屏），所以选区左上角
		// 就是"标注坐标 (0,0) 落在源图的哪儿"。imgOrigin 照旧留 {0,0}：底图与标注此刻是重合的
		cropBase = POINT{ static_cast<LONG>(cropRect.left), static_cast<LONG>(cropRect.top) };
	}
	toolMain = std::make_unique<ToolMain>(this);
    toolSub = std::make_unique<ToolSub>(this);
	// 预选工具排在两条工具条都建好之后：selectTool 会按工具配出 ToolSub 的内容再重排整组
	if (!initToolId.empty()) {
		toolMain->selectTool(initToolId);
	}
	layoutTools();
	// 窗口一挪就重排工具条。但**拖窗口期间要跳过**：那时两条工具条都收着（onDown 里 hide 了），
	// 重排纯属白做，而它内部要给两条工具条各来一次 SetWindowPos —— 实测单条 WM_MOUSEMOVE
	// 的 16ms 里有 8ms 花在这次白排上，鼠标消息根本来不及，窗口就落在光标后面一抖一抖地追。
	// 抬手时 onUp 会照常重排一次，所以跳过的不亏
	onMoved.add([this]() {
		if (winDragging) return;
		layoutTools();
	});
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
			// 滚过轮也是在拿 Ctrl 当修饰键（Ctrl+滚轮 = 缩放），同 onDown：
			// 抬手别再把这一轮 Ctrl 当成"空点一下"去收框选
			ctrlTapArmed = false;
			ctrlUsedAsModifier = true;
			// 一格 10%，按当前倍数等比走，放大和缩小的手感才对称
			applyScale(scale * (space > 0 ? 1.1f : 1.f / 1.1f), pos);
			return;
		}
		if (!drawing->shapeHover && drawing->multiSelected.empty()) return;
		auto imgPos = toImgPos(pos);
		auto delta = space > 0 ? (short)WHEEL_DELTA : (short)-WHEEL_DELTA;
		// 多选时一次滚到整批上。各组件自己的 mouseWheel 会先判"我这一样吃不吃滚轮"
		//（图片 / 填充图形 / 马赛克那几个直接早退）—— 不支持的自然就跳过了
		if (!drawing->multiSelected.empty()) {
			for (auto* s : drawing->multiSelected) s->mouseWheel((float)imgPos.x, (float)imgPos.y, delta);
			return;
		}
		drawing->shapeHover->mouseWheel((float)imgPos.x, (float)imgPos.y, delta);
	});
	onTimer.add([this](UINT id) {this->onTimerCB(id);});
	onKeyDown.add([this](UINT key) {this->onKey(key);});
	// 抬起也接一路：Ctrl 的"空点一下"要在这里收掉框选（见 onKeyRelease）
	onKeyUp.add([this](UINT key) {this->onKeyRelease(key);});
	// 被激活同样会把本窗口提到 topmost 同类的最前面（Alt+Tab、别的窗口让位给它……），
	// 这一条兜住所有"不是点出来的"激活，道理与 onDown 开头那次一样
	onFocus.add([this]() { this->raiseTools(); });
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
	// 后台识别线程可能还在跑（贴图刚建起来就被关掉是常事），先让它认的结果作废：
	// 它拿到的是裸 this，回填时不能再碰这个对象
	if (ocrAlive) *ocrAlive = false;
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
	// 藏着的贴图被复制 / 存盘关掉了，它那条书签得跟着消失。先把标志放掉再 sync：
	// sync 是按这个标志决定条数的，而对象要下一轮消息循环才从 winPins 里摘掉
	isHidden = false;
	PinHiddenBar::sync();
	// screenImg / canvas / drawing 都是成员（canvas 挂在 body 的子节点上），随下面这次 erase 一并释放
	Ling::App::get()->dq.TryEnqueue([this]() {
		std::erase_if(winPins, [this](const std::unique_ptr<WinPin>& p) { return p.get() == this; });
		// 真正摘掉了再对一次账：隐藏条那边存着"露出来的是哪一张"的裸指针，
		// 它就是在这次 sync 里发现那张已经不在了、把指针清掉的
		PinHiddenBar::sync();
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
	// 出来的坐标是"标注坐标系"里的，不是底图像素 —— 剪过一刀之后两者差一个
	// drawing->imgOrigin（见 Canvas.h）。shape 存的、认的、画的一律是前者，所以这里要加回来；
	// 而剪裁采样点量的是屏幕上的位置，它除回倍数就行，不加这个偏移（见 applyCropRect）
	auto vs = viewScale();
	const auto& o = drawing->imgOrigin;
	if (vs == 1.f) return POINT{ pos.x + o.x, pos.y + o.y };
	return POINT{ static_cast<LONG>(std::lround(pos.x / vs)) + o.x,
		static_cast<LONG>(std::lround(pos.y / vs)) + o.y };
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
	// pin 不是画笔：它只是"调这张贴图自己的属性"，鼠标该归拖窗口那条路
	return toolMain && !toolMain->curId.empty() && toolMain->curId != L"pin";
}

// ---- 「选择对象」----
// 它算"能画东西"的工具（见 hasDrawTool）：左键要留在画布上，不能落进"拖窗口"那条路；
// 区别只在于点中的是已有元素，不是新建一笔
bool WinPin::selecting() const
{
	// 「选择画布」之外都是"可以点选对象"的状态。它不再是一个要用户先按一下的模式 ——
	// 手里拿着画笔就默认开着（见头文件上的说明）
	return !canvasMode;
}

D2D1_RECT_F WinPin::marqueeRect() const
{
	return D2D1::RectF(
		(float)std::min(marqueeAnchor.x, marqueeCur.x),
		(float)std::min(marqueeAnchor.y, marqueeCur.y),
		(float)std::max(marqueeAnchor.x, marqueeCur.x),
		(float)std::max(marqueeAnchor.y, marqueeCur.y));
}

// 框选抬手：把外接框与选框相交的元素收进 Canvas::multiSelected。
// 判定是"相交"而不是"完全框住" —— 细长元素（线条 / 箭头）只要被框碰到就算，
// 要求整个框住的话得瞄得很准，反而是个负担
void WinPin::collectMarquee()
{
	// 只是点了一下空白（一步没拖）：选框退化成一个点，什么都不选。
	// 不判这一句的话，点在"外接框能容纳它、自身命中范围却不在那儿"的位置上也会被选中 ——
	// 空心矩形的内部、椭圆的四角都是这种地方，而那一下本该是"点空白取消选中"
	if (!hasDragged) return;
	const auto r = marqueeRect();
	for (auto& up : drawing->history->shapes) {
		auto shape = up.get();
		if (shape->isUndo) continue;
		// 没有外接框的元素不参与 —— 水印铺满整张图，算进来的话拉什么框都会把它一起选中
		D2D1_RECT_F b{};
		if (!shape->getShapeBounds(b)) continue;
		if (r.left > b.right || b.left > r.right || r.top > b.bottom || b.top > r.bottom) continue;
		drawing->multiSelected.push_back(shape);
	}
}

// 框选的两层提示：正在拉的那个选框，以及多选那一批各自的外框 + 8 个标记点。
// 两者都画在标注坐标系里（调用方没换过变换），所以跟着 Ctrl+滚轮缩放、剪裁一起走。
//
// 多选那一批画的是"每人都有一圈夹点"，但夹点只表示"它在这批里"、不参与交互 ——
// 拉夹点改大小、拖动、右上角那枚 × 都只作用于单选。整批要表达的是两件事：
// 这些会被一起挪 / 一起改样式 / 一起删（见画布右上角那三枚批量按钮）。
// 刻意不给每个对象都画动作图标：一整批每个都冒出四枚按钮，图上一片按钮，
// 反倒看不出哪个是"整批操作"、哪个是"只改这一个"
void WinPin::paintSelection(ID2D1DeviceContext* ctx)
{
	if (!borderBrush || !selectBrush || !brushSelWhite) return;
	if (marqueeOn) {
		auto r = marqueeRect();
		ctx->FillRectangle(r, selectBrush.Get());
		ctx->DrawRectangle(r, borderBrush.Get(), dpi);
	}
	// 标记点半边长。比单选那 8 枚夹点（draggerSize*0.5 = 3*dpi）小一圈：
	// 多选时一圈标记点只是"选中提示"，不需要那么好点，太大会把细小的标注整个盖住
	const float hr{ 2.6f * dpi };
	// 框住的对象一多，逐人一圈 8 个点就糊成一片，反倒看不清到底圈进了谁
	//（作者：超过 2 个只留框线）。这些点本来就是纯提示、不可交互 —— 批量只能整体挪 /
	// 整体转 / 整体删，没有"拖某个人的角改大小"这回事。<=2 时仍然给，两个对象时
	// 那圈点还能帮着确认"是哪两个"
	const bool withHandles{ drawing->multiSelected.size() <= 2 };
	for (auto* shape : drawing->multiSelected) {
		D2D1_RECT_F b{};
		if (!shape->getShapeBounds(b)) continue;
		// 框线与标记点都要**让到元素外面**画，别压在它自己的描边上。
		// 空心的矩形画的正是 getShapeBounds 那个矩形，蓝色框线落在红描边的中线上、
		// 8 个白点落在它的角与边中点上 —— 整个边框就被混成紫的了
		//（作者报的"ctrl 框选之后矩形边框线的颜色会变"）。
		// 让开三样：元素自己描边的半宽（shape->selectionGap）+ 框线自己的一半 + 同样宽的一条缝。
		// 椭圆族、填满的矩形、线条族那边 selectionGap 都是 0（理由见 ShapeBase::selectionGap）
		const float inset{ shape->selectionGap() + dpi };
		const D2D1_RECT_F f{ b.left - inset, b.top - inset, b.right + inset, b.bottom + inset };
		ctx->DrawRectangle(f, borderBrush.Get(), dpi);
		if (!withHandles) continue;
		D2D1_POINT_2F cs[8];
		rectHandleCenters(f, cs);
		for (auto& c : cs) {
			auto box = D2D1::RectF(c.x - hr, c.y - hr, c.x + hr, c.y + hr);
			ctx->FillRectangle(box, brushSelWhite.Get());
			ctx->DrawRectangle(box, borderBrush.Get(), dpi);
		}
	}
}

// ---- 多选那一批的批量操作 ----
D2D1_POINT_2F WinPin::multiSelectCenter() const
{
	// 整组的外接框中心。旋转绕它转，所以取的是"整批合起来"的中心而不是某一个的中心
	D2D1_RECT_F all{};
	bool has{ false };
	for (auto* shape : drawing->multiSelected) {
		D2D1_RECT_F b{};
		if (!shape->getShapeBounds(b)) continue;
		if (!has) { all = b; has = true; }
		else {
			all.left = std::min(all.left, b.left);
			all.top = std::min(all.top, b.top);
			all.right = std::max(all.right, b.right);
			all.bottom = std::max(all.bottom, b.bottom);
		}
	}
	if (!has) {
		auto img = getImgSize();
		return D2D1::Point2F((float)img.width / 2.f, (float)img.height / 2.f);
	}
	return D2D1::Point2F((all.left + all.right) / 2.f, (all.top + all.bottom) / 2.f);
}

// 画布右上角那三枚批量按钮。位置取**窗口坐标**（画在 setTransform(Identity) 之后那一层，
// 与剪裁采样点同一个坐标系）—— 它们属于"这扇窗"而不是底图，Ctrl+滚轮放大缩小时不该跟着跑
D2D1_RECT_F WinPin::batchBtnRect(const int i) const
{
	const float size{ 26.f * dpi }, gap{ 6.f * dpi }, margin{ 8.f * dpi };
	const float left0{ w - margin - (size * 3 + gap * 2) };
	const float left{ left0 + i * (size + gap) };
	return D2D1::RectF(left, margin, left + size, margin + size);
}

int WinPin::batchBtnAt(const POINT pos) const
{
	if (drawing->multiSelected.empty()) return -1;
	for (int i = 0; i < 3; ++i) {
		auto b = batchBtnRect(i);
		if ((float)pos.x > b.left && (float)pos.x < b.right
			&& (float)pos.y > b.top && (float)pos.y < b.bottom) return i;
	}
	return -1;
}

void WinPin::paintBatchButtons(ID2D1DeviceContext* ctx)
{
	if (drawing->multiSelected.empty() || !borderBrush || !brushSelWhite) return;
	for (int i = 0; i < 3; ++i) {
		auto box = batchBtnRect(i);
		auto c = D2D1::Point2F((box.left + box.right) / 2.f, (box.top + box.bottom) / 2.f);
		auto rad{ (box.right - box.left) / 2.f };
		// 停在"拖拽旋转"态时旋转那枚反白（蓝底 + 白图标），一眼看得出还开着
		const bool active{ i == 2 && batchRotateOn };
		auto bg = active ? borderBrush.Get() : brushSelWhite.Get();
		auto fg = active ? brushSelWhite.Get() : borderBrush.Get();
		ctx->FillRectangle(box, bg);
		ctx->DrawRectangle(box, borderBrush.Get(), dpi);
		const float strokeW{ dpi };
		if (i == 0) {
			// 垃圾桶：盖子一横 + 提手 + 桶身 + 一道竖棱。与「选择画布」那枚删除同一套几何
			auto k{ rad * 0.52f };
			ctx->DrawLine({ c.x - k, c.y - k * 0.52f }, { c.x + k, c.y - k * 0.52f }, fg, strokeW);
			ctx->DrawLine({ c.x - k * 0.42f, c.y - k * 0.86f }, { c.x + k * 0.42f, c.y - k * 0.86f }, fg, strokeW);
			ctx->DrawLine({ c.x - k * 0.76f, c.y - k * 0.18f }, { c.x - k * 0.56f, c.y + k * 0.86f }, fg, strokeW);
			ctx->DrawLine({ c.x - k * 0.56f, c.y + k * 0.86f }, { c.x + k * 0.56f, c.y + k * 0.86f }, fg, strokeW);
			ctx->DrawLine({ c.x + k * 0.56f, c.y + k * 0.86f }, { c.x + k * 0.76f, c.y - k * 0.18f }, fg, strokeW);
			ctx->DrawLine({ c.x, c.y - k * 0.1f }, { c.x, c.y + k * 0.7f }, fg, strokeW);
		}
		else if (i == 1) {
			// 复制：两枚叠着的方框，都只描边
			auto k{ rad * 0.42f }, off{ rad * 0.3f };
			ctx->DrawRectangle(D2D1::RectF(c.x - k - off, c.y - k - off, c.x + k - off, c.y + k - off), fg, strokeW);
			ctx->DrawRectangle(D2D1::RectF(c.x - k + off, c.y - k + off, c.x + k + off, c.y + k + off), fg, strokeW);
		}
		else {
			// 旋转：一段留口的圆弧 + 末端一支箭头（与单选那枚旋转手柄同一个意思）。
			// 圆弧用折线近似 —— 2D 的 AddArc 要把超过 180 度的弧拆段，不如折线直接
			const float r{ rad * 0.56f };
			auto pt = [&](float deg) {
				auto rd = deg * 3.14159265358979323846f / 180.f;
				return D2D1::Point2F(c.x + r * cosf(rd), c.y - r * sinf(rd));
			};
			const float a0{ -50.f }, sweep{ 285.f };
			ComPtr<ID2D1PathGeometry> arc;
			Ling::D2D::get()->d2dFactory->CreatePathGeometry(arc.GetAddressOf());
			ComPtr<ID2D1GeometrySink> sk;
			if (arc && SUCCEEDED(arc->Open(sk.GetAddressOf()))) {
				const int steps{ 24 };
				sk->BeginFigure(pt(a0), D2D1_FIGURE_BEGIN_HOLLOW);
				for (int k = 1; k <= steps; ++k) sk->AddLine(pt(a0 + sweep * k / steps));
				sk->EndFigure(D2D1_FIGURE_END_OPEN);
				// 末端箭头：沿该点的切向指出去，两腰落在切向的法向上。
				// 与圆弧共用一次 BeginDraw —— D2D 不支持嵌套，必须先把弧收进同一个 sink
				{
					auto rd = (a0 + sweep) * 3.14159265358979323846f / 180.f;
					auto bx = c.x + r * cosf(rd), by = c.y - r * sinf(rd);
					auto tx{ -sinf(rd) }, ty{ -cosf(rd) };
					auto nx{ -ty }, ny{ tx };
					auto h{ rad * 0.34f };
					sk->BeginFigure(D2D1::Point2F(bx + nx * h * 0.6f, by + ny * h * 0.6f), D2D1_FIGURE_BEGIN_FILLED);
					sk->AddLine(D2D1::Point2F(bx + tx * h, by + ty * h));
					sk->AddLine(D2D1::Point2F(bx - nx * h * 0.6f, by - ny * h * 0.6f));
					sk->EndFigure(D2D1_FIGURE_END_CLOSED);
				}
				sk->Close();
				ctx->DrawGeometry(arc.Get(), fg, strokeW);
			}
		}
	}
}

// 批量删除：与按 Delete 那条路同一套 —— 走 undoShapes（只打撤销标记），Ctrl+Y 能整批找回
void WinPin::batchDeleteShapes()
{
	if (drawing->multiSelected.empty()) return;
	auto batch = drawing->multiSelected;
	drawing->multiSelected.clear();
	// 整批没了，拖动 / 旋转态跟着收掉（旋转态里拖的是已经不存在的那些指针）
	clearBatchState();
	drawing->history->undoShapes(batch);
	refresh();
}

// 批量复制：就地再画一份、整体错开一点摆在旁边。与每个元素左上角那枚复制按钮
// （ShapeBase::onActionBtn）是同一套语义，只是这里一次做一整批。
// 刻意不做成"复制进剪贴板" —— 手上有 Ctrl+C，而这枚按钮在元素自己的复制按钮旁边，
// 两者行为不一致才奇怪。复制出来的这一批取代原来的选中，接着就能拖走
void WinPin::batchCopyShapes()
{
	if (drawing->multiSelected.empty()) return;
	auto src = drawing->multiSelected;   // 先存一份：addShape 会改 selected / shapeHover
	const float off{ 10.f * dpi };
	std::vector<ShapeBase*> made;
	for (auto* s : src) {
		// 传目标画布：原件可能来自别的贴图窗口（对象剪贴板里那些），换了宿主才能安全跑善后
		if (auto c = s->clone(off, off, drawing.get())) {
			if (auto* added = drawing->history->addShape(std::move(c))) made.push_back(added);
		}
	}
	// addShape 每收一份都把 selected 指过去，循环完停在最后一份上 —— 这里改成"整批选中"，
	// 与用户按 Ctrl 一个个加选出来的状态一致
	drawing->selected = nullptr;
	drawing->shapeHover = nullptr;
	drawing->multiSelected = made;
	clearBatchState();
	refresh();
}

void WinPin::toggleBatchRotate()
{
	batchRotateOn = !batchRotateOn;
	batchRotating = false;
	refresh();
}

void WinPin::clearBatchState()
{
	batchMoving = false;
	batchRotating = false;
	batchRotateOn = false;
}

// ---- 「选择画布」（「选择器」的第二个子模式）----
// 作用对象是底图 drawing->screenImg 的像素。刻意不走 swapImage：那条路是"整张图换掉了"，
// 会把标注一并清掉；这里只是把底图的某一块挪个位置，标注不该跟着没
bool WinPin::hasSel() const
{
	return selRect.right > selRect.left && selRect.bottom > selRect.top;
}

// 8 个采样点的中心：四角 + 四边中点。顺序与剪裁那 8 个点一致（0 左上、顺时针）
void WinPin::selHandleCenters(D2D1_POINT_2F (&centers)[8]) const
{
	const auto& r = selRect;
	auto mx{ (r.left + r.right) / 2.f }, my{ (r.top + r.bottom) / 2.f };
	centers[0] = { r.left, r.top };
	centers[1] = { mx, r.top };
	centers[2] = { r.right, r.top };
	centers[3] = { r.right, my };
	centers[4] = { r.right, r.bottom };
	centers[5] = { mx, r.bottom };
	centers[6] = { r.left, r.bottom };
	centers[7] = { r.left, my };
}

int WinPin::selHandleAt(const POINT& imgPos) const
{
	if (!hasSel()) return -1;
	D2D1_POINT_2F cs[8];
	selHandleCenters(cs);
	// 命中框比画出来的点大一圈（同剪裁采样点）：那么小的圆，差几个像素就点不着
	const float hitR{ 9.f * dpi };
	for (int i = 0; i < 8; ++i) {
		if (std::abs((float)imgPos.x - cs[i].x) <= hitR && std::abs((float)imgPos.y - cs[i].y) <= hitR) return i;
	}
	return -1;
}

// 选区上那两枚动作图标：0 复制摆在左上角外侧、1 删除摆在右上角外侧 ——
// 与 ShapeBase 的动作按钮同一套摆法，同一个编辑器里两处按钮的位置不至于各说各话
D2D1_RECT_F WinPin::selActionRect(const int i) const
{
	if (!hasSel() || i < 0 || i > 1) return D2D1::RectF(0.f, 0.f, 0.f, 0.f);
	const auto& r = selRect;
	// 图标半径与其它组件的动作按钮一致（ShapeBase 是 draggerSize(6*dpi) * 0.9）——
	// 原来是 9*dpi，压在同一张图上比别人的复制 / 删除按钮大了一圈（作者提的）
	const float rad{ 5.4f * dpi };
	// 离角多远：内边缘到角的距离与原来（20 - 9）保持一致，图标缩小之后仍贴在角上，
	// 不会因为变小而看着飘出去
	const float gap{ 16.4f * dpi };
	float cx = (i == 0) ? (r.left - gap) : (r.right + gap);
	float cy = r.top - gap;
	// 顶到画布边上就翻到内侧，否则被裁掉、点都点不到
	auto img = getImgSize();
	if (img.width > 0 && (cx - rad < 0.f || cx + rad > (float)img.width)) {
		cx = (i == 0) ? (r.left + gap) : (r.right - gap);
	}
	if (img.height > 0 && cy - rad < 0.f) cy = r.top + gap;
	return D2D1::RectF(cx - rad, cy - rad, cx + rad, cy + rad);
}

int WinPin::selActionAt(const POINT& imgPos) const
{
	if (!hasSel()) return -1;
	for (int i = 0; i < 2; ++i) {
		auto b = selActionRect(i);
		if (b.right <= b.left) continue;
		if ((float)imgPos.x > b.left && (float)imgPos.x < b.right
			&& (float)imgPos.y > b.top && (float)imgPos.y < b.bottom) return i;
	}
	return -1;
}

void WinPin::paintCanvasSelection(ID2D1DeviceContext* ctx)
{
	if (!hasSel()) return;
	const auto& r = selRect;
	// 搬运期间那块画面跟着选区走（底图上的原位此刻已经是白的，见 pickUpSelection）
	if (selFloat) ctx->DrawBitmap(selFloat.Get(), r);
	// 虚线框（作者要的 fasCapture 那种观感，原来是实线）
	ctx->DrawRectangle(r, borderBrush.Get(), dpi, selDashStyle.Get());
	// 选区上这几枚图标的尺寸按其它组件的动作按钮来（ShapeBase 的 draggerSize * 0.9 = 5.4*dpi），
	// 笔宽也照它那一套：先白描边打底、再上原色
	const float iconStroke{ dpi }, iconHalo{ 2.6f * dpi };
	// 8 个采样点：白底 + 蓝边，与剪裁采样点同一套观感
	D2D1_POINT_2F cs[8];
	selHandleCenters(cs);
	const float hr{ 4.f * dpi };
	for (auto& c : cs) {
		ctx->FillEllipse(D2D1::Ellipse(c, hr, hr), brushSelWhite.Get());
		ctx->DrawEllipse(D2D1::Ellipse(c, hr, hr), borderBrush.Get(), dpi);
	}
	// 中间那个四向箭头 = "可以拖走"。先白描一遍打底，压在图上才看得清。
	// 同样收到其它组件动作按钮那个大小（原来 7+3 个 dpi 的跨度，比别的按钮大了一圈）
	{
		auto c = D2D1::Point2F((r.left + r.right) / 2.f, (r.top + r.bottom) / 2.f);
		const float len{ 3.4f * dpi }, head{ 1.6f * dpi };
		auto cross = [&](ID2D1Brush* b, float w) {
			ctx->DrawLine({ c.x - len, c.y }, { c.x + len, c.y }, b, w);
			ctx->DrawLine({ c.x, c.y - len }, { c.x, c.y + len }, b, w);
		};
		ComPtr<ID2D1PathGeometry> mv;
		Ling::D2D::get()->d2dFactory->CreatePathGeometry(mv.GetAddressOf());
		ComPtr<ID2D1GeometrySink> sk;
		if (mv && SUCCEEDED(mv->Open(sk.GetAddressOf()))) {
			auto addTri = [&](D2D1_POINT_2F tip, D2D1_POINT_2F p1, D2D1_POINT_2F p2) {
				sk->BeginFigure(p1, D2D1_FIGURE_BEGIN_FILLED);
				sk->AddLine(tip);
				sk->AddLine(p2);
				sk->EndFigure(D2D1_FIGURE_END_CLOSED);
			};
			addTri({ c.x + len + head, c.y }, { c.x + len, c.y - head }, { c.x + len, c.y + head });
			addTri({ c.x - len - head, c.y }, { c.x - len, c.y + head }, { c.x - len, c.y - head });
			addTri({ c.x, c.y - len - head }, { c.x + head, c.y - len }, { c.x - head, c.y - len });
			addTri({ c.x, c.y + len + head }, { c.x - head, c.y + len }, { c.x + head, c.y + len });
			sk->Close();
			ctx->DrawGeometry(mv.Get(), brushSelWhite.Get(), iconHalo);
			ctx->FillGeometry(mv.Get(), borderBrush.Get());
			cross(brushSelWhite.Get(), iconHalo);
			cross(borderBrush.Get(), iconStroke);
		}
	}
	// 两枚动作图标：0 复制（两枚叠着的方框）、1 删除（垃圾桶）。画法与 ShapeBase 的动作按钮同一套
	for (int i = 0; i < 2; ++i) {
		auto box = selActionRect(i);
		if (box.right <= box.left) continue;
		auto cc = D2D1::Point2F((box.left + box.right) / 2.f, (box.top + box.bottom) / 2.f);
		auto rad{ (box.right - box.left) / 2.f };
		if (i == 1) {
			// 垃圾桶，而不是关闭按钮那个 ×（作者要的：两者摆在同一张图上要一眼分得开）。
			// 盖子一横 + 盖上提手 + 桶身（上宽下窄）+ 一道竖棱，六条线拼出来；
			// 这个尺寸下也就是勉强认得出，再省一条就成别的形状了
			auto k{ rad * 0.62f };
			auto draw = [&](ID2D1Brush* b, float w) {
				ctx->DrawLine({ cc.x - k, cc.y - k * 0.52f }, { cc.x + k, cc.y - k * 0.52f }, b, w);
				ctx->DrawLine({ cc.x - k * 0.42f, cc.y - k * 0.86f }, { cc.x + k * 0.42f, cc.y - k * 0.86f }, b, w);
				ctx->DrawLine({ cc.x - k * 0.76f, cc.y - k * 0.18f }, { cc.x - k * 0.56f, cc.y + k * 0.86f }, b, w);
				ctx->DrawLine({ cc.x - k * 0.56f, cc.y + k * 0.86f }, { cc.x + k * 0.56f, cc.y + k * 0.86f }, b, w);
				ctx->DrawLine({ cc.x + k * 0.56f, cc.y + k * 0.86f }, { cc.x + k * 0.76f, cc.y - k * 0.18f }, b, w);
				ctx->DrawLine({ cc.x, cc.y - k * 0.1f }, { cc.x, cc.y + k * 0.7f }, b, w);
			};
			draw(brushSelWhite.Get(), iconHalo);
			draw(borderBrush.Get(), iconStroke);
		}
		else {
			auto k{ rad * 0.46f }, off{ rad * 0.32f };
			auto draw = [&](ID2D1Brush* b, float w) {
				ctx->DrawRectangle(D2D1::RectF(cc.x - k - off, cc.y - k - off, cc.x + k - off, cc.y + k - off), b, w);
				ctx->DrawRectangle(D2D1::RectF(cc.x - k + off, cc.y - k + off, cc.x + k + off, cc.y + k + off), b, w);
			};
			draw(brushSelWhite.Get(), iconHalo);
			draw(borderBrush.Get(), iconStroke);
		}
	}
}

bool WinPin::writeScreenImg(const std::vector<BYTE>& px, const int w, const int h)
{
	if (w <= 0 || h <= 0 || px.size() < (size_t)w * 4 * h) return false;
	if (!drawing->screenImg) return false;
	D2D1_BITMAP_PROPERTIES1 props{};
	// 像素格式照抄现在的底图：截图那条路是 ALPHA_MODE_IGNORE、长图那条是 PREMULTIPLIED，
	// 写死一个就会跟 readBasePixels 读出来的排布对不上（填的白也会变成别的颜色）
	props.pixelFormat = drawing->screenImg->GetPixelFormat();
	props.dpiX = 96.0f;
	props.dpiY = 96.0f;
	props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
	ComPtr<ID2D1Bitmap1> bmp;
	if (FAILED(Ling::D2D::get()->deviceContext->CreateBitmap(D2D1::SizeU((UINT32)w, (UINT32)h),
		px.data(), (UINT32)w * 4, &props, bmp.GetAddressOf()))) return false;
	drawing->screenImg = bmp;
	return true;
}

void WinPin::pushCanvasUndo()
{
	int w{}, h{};
	if (!readBasePixels(canvasUndoPx, w, h)) {
		canvasUndoPx.clear();
		return;
	}
	canvasUndoW = w;
	canvasUndoH = h;
}

bool WinPin::restoreCanvasUndo()
{
	if (canvasUndoPx.empty() || canvasUndoW <= 0 || canvasUndoH <= 0) return false;
	if (!writeScreenImg(canvasUndoPx, canvasUndoW, canvasUndoH)) return false;
	canvasUndoPx.clear();
	canvasUndoW = canvasUndoH = 0;
	// 底图内容换了，之前认出来的词一个都对不上了
	clearOcr();
	startOcr();
	refresh();
	return true;
}

// 把选区那块画面从底图上取下来：CPU 一份（落回时用）+ GPU 一份（拖动期间预览），
// 原位填白。抬手时 dropSelection 再把它落到新位置 —— 于是"拖出去"的观感是内容跟着鼠标走
void WinPin::pickUpSelection()
{
	if (!hasSel() || selFloat) return;
	std::vector<BYTE> px;
	int w{}, h{};
	if (!readBasePixels(px, w, h)) return;
	const int l = std::max(0, (int)selRect.left), t = std::max(0, (int)selRect.top);
	const int r = std::min(w, (int)selRect.right), b = std::min(h, (int)selRect.bottom);
	if (r <= l || b <= t) return;
	const int sw = r - l, sh = b - t;
	selBlockW = sw;
	selBlockH = sh;
	selBlockPx.resize((size_t)sw * 4 * sh);
	for (int row = 0; row < sh; ++row) {
		CopyMemory(selBlockPx.data() + (size_t)row * sw * 4,
			px.data() + ((size_t)(t + row) * w + l) * 4, (size_t)sw * 4);
	}
	D2D1_BITMAP_PROPERTIES1 props{};
	props.pixelFormat = drawing->screenImg->GetPixelFormat();
	props.dpiX = 96.0f;
	props.dpiY = 96.0f;
	props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
	if (FAILED(Ling::D2D::get()->deviceContext->CreateBitmap(D2D1::SizeU((UINT32)sw, (UINT32)sh),
		selBlockPx.data(), (UINT32)sw * 4, &props, selFloat.GetAddressOf()))) {
		selFloat.Reset();
		return;
	}
	pushCanvasUndo();
	// 原位填白（作者定的）
	std::vector<BYTE> white((size_t)sw * 4, 0xFF);
	for (int row = t; row < b; ++row) {
		CopyMemory(px.data() + ((size_t)row * w + l) * 4, white.data(), white.size());
	}
	if (!writeScreenImg(px, w, h)) return;
	refresh();
}

// 把抠下来的画面落回底图的当前位置。超出画布的部分裁掉（作者定的：允许拖出，超出不留）
void WinPin::dropSelection()
{
	if (selBlockPx.empty() || selBlockW <= 0 || selBlockH <= 0) {
		selFloat.Reset();
		return;
	}
	std::vector<BYTE> px;
	int w{}, h{};
	if (!readBasePixels(px, w, h)) {
		selFloat.Reset();
		selBlockPx.clear();
		selBlockW = selBlockH = 0;
		return;
	}
	const int l = (int)selRect.left, t = (int)selRect.top;
	// 目标矩形与画布求交，逐行把源块里对应那一段整段拷过去
	const int x0 = std::max(0, l), x1 = std::min(w, l + selBlockW);
	const int y0 = std::max(0, t), y1 = std::min(h, t + selBlockH);
	if (x1 > x0 && y1 > y0) {
		for (int dy = y0; dy < y1; ++dy) {
			const int sx = x0 - l, sy = dy - t;
			CopyMemory(px.data() + ((size_t)dy * w + x0) * 4,
				selBlockPx.data() + ((size_t)sy * selBlockW + sx) * 4, (size_t)(x1 - x0) * 4);
		}
		if (!writeScreenImg(px, w, h)) return;
	}
	selFloat.Reset();
	selBlockPx.clear();
	selBlockW = selBlockH = 0;
	refresh();
}

void WinPin::deleteSelection()
{
	if (!hasSel()) return;
	std::vector<BYTE> px;
	int w{}, h{};
	if (!readBasePixels(px, w, h)) return;
	const int l = std::max(0, (int)selRect.left), t = std::max(0, (int)selRect.top);
	const int r = std::min(w, (int)selRect.right), b = std::min(h, (int)selRect.bottom);
	if (r <= l || b <= t) return;
	pushCanvasUndo();
	std::vector<BYTE> white((size_t)(r - l) * 4, 0xFF);
	for (int y = t; y < b; ++y) {
		CopyMemory(px.data() + ((size_t)y * w + l) * 4, white.data(), white.size());
	}
	if (!writeScreenImg(px, w, h)) return;
	// 删掉这一块就退出「选择画布」（作者定的）：这一趟画布上的活儿已经做完了，
	// 再留在那个模式里等着框下一块没有道理。复制 / 搬移两条路同理
	setCanvasMode(false);
	refresh();
}

void WinPin::copySelectionToClipboard()
{
	if (!hasSel()) return;
	std::vector<BYTE> px;
	int w{}, h{};
	if (!readBasePixels(px, w, h)) return;
	const int l = std::max(0, (int)selRect.left), t = std::max(0, (int)selRect.top);
	const int r = std::min(w, (int)selRect.right), b = std::min(h, (int)selRect.bottom);
	if (r <= l || b <= t) return;
	const int sw = r - l, sh = b - t;
	std::vector<BYTE> block((size_t)sw * 4 * sh);
	for (int row = 0; row < sh; ++row) {
		CopyMemory(block.data() + (size_t)row * sw * 4,
			px.data() + ((size_t)(t + row) * w + l) * 4, (size_t)sw * 4);
	}
	Util::saveToClipboard(sw, sh, block.data());
	showToast(Lang::get(L"tool.canvasCopied"));
	// 复制完退出「选择画布」，理由同 deleteSelection 末尾那条
	setCanvasMode(false);
}

void WinPin::canvasSelectDown(const POINT& imgPos)
{
	// 落在画布外时夹回边界：选区的起点一律在画布内，否则一按就从界外拉
	auto img = getImgSize();
	auto cx = std::clamp<LONG>(imgPos.x, 0, (LONG)img.width);
	auto cy = std::clamp<LONG>(imgPos.y, 0, (LONG)img.height);
	// 先认选区上那两枚动作图标（复制 / 删除）：它们摆在选区外的角上，
	// 不先认的话这一点会被当成"点空白"反手把选区清掉
	int act = selActionAt(imgPos);
	if (act == 0) {
		copySelectionToClipboard();
		return;
	}
	if (act == 1) {
		deleteSelection();
		return;
	}
	if (hasSel()) {
		int hh = selHandleAt(imgPos);
		if (hh >= 0) {
			selDrag = 3;
			selHandle = hh;
			return;
		}
		if ((float)imgPos.x > selRect.left && (float)imgPos.x < selRect.right
			&& (float)imgPos.y > selRect.top && (float)imgPos.y < selRect.bottom) {
			// 落在选区里：准备搬画面。真正抠图留到第一次移动（见 canvasSelectMove），
			// 不然在选区里点一下就把那块剪走了
			selDrag = 2;
			selDown = D2D1::Point2F((float)imgPos.x, (float)imgPos.y);
			selBaseLT = D2D1::Point2F(selRect.left, selRect.top);
			return;
		}
	}
	// 空白处：起一个新框，旧的先清掉
	selFloat.Reset();
	selBlockPx.clear();
	selBlockW = selBlockH = 0;
	selDrag = 1;
	selDown = D2D1::Point2F((float)cx, (float)cy);
	selRect = D2D1::RectF((float)cx, (float)cy, (float)cx, (float)cy);
	refresh();
}

void WinPin::canvasSelectMove(const POINT& imgPos)
{
	// 搬画面：刻意不夹边界 —— 作者定的语义是"允许拖出画布，超出的部分裁掉"，
	// 夹住了就永远拖不出去
	if (selDrag == 2) {
		// 第一次真拖动才把画面抠下来（那时才知道用户真要搬）。抠完这一步
		// 选区尺寸就定死成那块画面的尺寸，往后只平移
		if (!selFloat) pickUpSelection();
		if (selFloat) {
			const float dx = (float)imgPos.x - selDown.x, dy = (float)imgPos.y - selDown.y;
			const float nl = selBaseLT.x + dx, nt = selBaseLT.y + dy;
			selRect = D2D1::RectF(nl, nt, nl + (float)selBlockW, nt + (float)selBlockH);
		}
		refreshNow();
		return;
	}
	// 拉新框 / 改大小：光标夹在画布内，框不会拉到界外去
	auto img = getImgSize();
	const float cx = (float)std::clamp<LONG>(imgPos.x, 0, (LONG)img.width);
	const float cy = (float)std::clamp<LONG>(imgPos.y, 0, (LONG)img.height);
	if (selDrag == 1) {
		// 拉新框
		selRect = D2D1::RectF(std::min(selDown.x, cx), std::min(selDown.y, cy),
			std::max(selDown.x, cx), std::max(selDown.y, cy));
	}
	else if (selDrag == 3) {
		// 改大小：把被拉的那条边 / 那个角挪到光标，归一化之后仍是个正经矩形（拖过头就翻面）
		float l = selRect.left, t = selRect.top, r = selRect.right, b = selRect.bottom;
		switch (selHandle) {
		case 0: l = cx; t = cy; break;
		case 1: t = cy; break;
		case 2: r = cx; t = cy; break;
		case 3: r = cx; break;
		case 4: r = cx; b = cy; break;
		case 5: b = cy; break;
		case 6: l = cx; b = cy; break;
		case 7: l = cx; break;
		default: break;
		}
		selRect = D2D1::RectF(std::min(l, r), std::min(t, b), std::max(l, r), std::max(t, b));
	}
	refreshNow();
}

void WinPin::canvasSelectUp()
{
	if (selDrag == 2) {
		// 搬完了：把抠下来的画面落到新位置（选区跟着画面走，已经在那儿了）。
		// 真搬过一趟就退出「选择画布」（作者定的）—— 只按了一下没拖动的不算，
		// 那种情况下画面没动过，留在那个模式里等下一笔更顺手
		if (selFloat) {
			dropSelection();
			setCanvasMode(false);
		}
	}
	else if (selDrag == 1) {
		// 只点了一下、没拉出框的，当成"清掉选区"
		if (selRect.right - selRect.left < 2.f || selRect.bottom - selRect.top < 2.f) {
			selRect = D2D1::RectF(0.f, 0.f, 0.f, 0.f);
		}
	}
	selDrag = 0;
	selHandle = -1;
	refresh();
}

void WinPin::setCanvasMode(const bool on)
{
	// 已经是这个状态就什么都不做（幂等）。进出画布模式的两处调用方都会先问一次，
	// 重复一遍只是白清一次选中态、白重画一帧
	if (canvasMode == on) return;
	canvasMode = on;
	// 两套选中互不相干：画布模式下没有"选中的元素"（留着它的夹点 / 动作图标会跟选区抢鼠标），
	// 从画布模式出去时也要收干净 —— 那批是在"上一个语境"里挑出来的
	clearObjectSelection();
	hideNumberPreview();
	// 选区只属于「选择画布」：切走就收掉（正拖着的时候不收，等抬手自己收）
	if (!on && selDrag == 0) {
		selRect = D2D1::RectF(0.f, 0.f, 0.f, 0.f);
		selFloat.Reset();
		selBlockPx.clear();
		selBlockW = selBlockH = 0;
	}
	// 退出去时还得把这个工具本身也放掉：curId == selector 表示"选择画布被选中"，
	// 模式退了它就自相矛盾 —— hasDrawTool() 仍为真，紧接着点画布空白会去
	// "画一个选择器"（History 里根本没有这种元素，只会落一个空指针）。
	// 它没有"模式之外的形态"可回（撤掉「选择对象」之后就不用再留在里面挑元素了），
	// 所以直接取消选中，回到"手里没拿工具"的常态
	if (!on && toolMain && toolMain->curId == L"selector") toolMain->cancelSelect();
	refresh();
}

void WinPin::clearObjectSelection()
{
	if (!drawing) return;
	drawing->selected = nullptr;
	drawing->shapeHover = nullptr;
	drawing->multiSelected.clear();
	// 多选的拖动 / 旋转态跟着一起收（拖的就是刚清掉的那一批）
	clearBatchState();
}

// 藏进屏幕边上的那条书签条里（见 PinHiddenBar），再按一次就是放回来。
// 藏起来的是"这扇窗"：位置、底图、标注一个都不动，所以鼠标移回那条上时它能原样回来
void WinPin::setHidden(bool on, BarEdge edge)
{
	if (isClosed || isHidden == on) return;
	isHidden = on;
	if (on) {
		barEdge = edge;
		// 编辑器与剪裁采样点画的都是"贴图窗口里的东西"，窗口一藏它们就没上下文了
		if (editingShape) editingShape->finishEditing();
		// 藏在边上的这段时间不接鼠标，正在拉的那一刀也得放开，否则 capture 还挂在手上
		if (cropDragging()) endCropDrag();
		// 两条工具条是独立窗口，不跟着收就是浮在桌面上的一排按钮，点下去还不知道点的哪张图
		toolsWereVisible = isToolsVisible();
		toolMain->hide();
		toolSub->hideTools();
		hide();
	}
	else {
		show();
		// 藏起来之前工具条是开着的才请回来。这里不走 setToolsVisible：那个会把缩略图 /
		// 贴边细条一并还原，而"放回来"不该顺手改用户收图的形态
		if (toolsWereVisible) {
			toolMain->show();
			// hideTools 把 ToolSub 的内容一并作废了，按当前工具重建一遍才出得来。
			// curId 为空时它自己就返回（那时候本来也没有子面板）
			toolMain->refreshToolSub();
		}
		layoutTools();
		refresh();
	}
	PinHiddenBar::sync();
}

// hover 那条时"露一下"。只把窗口显出来，isHidden 不动 —— 条本身得一直留着，
// 否则鼠标一离开条就没了，而"离开就收回去"正是这一套的行为
void WinPin::peek(bool on)
{
	if (isClosed || !isHidden) return;
	if (on) {
		show();
		if (toolsWereVisible) {
			toolMain->show();
			toolMain->refreshToolSub();
		}
		layoutTools();
		// 工具条要压在贴图上（全屏贴图那种重叠摆法），而 show() 把本窗口提到了 topmost
		// 组的最前面 —— 不补一次，两条工具条整条被底图盖住
		raiseTools();
		refresh();
	}
	else {
		toolMain->hide();
		toolSub->hideTools();
		hide();
	}
}

bool WinPin::isBusy() const
{
	// 手上正拿着这张图：拖着窗口 / 正在画一笔 / 文字编辑器开着 / 正拉着剪裁采样点 / 正拖着选文字
	return isMouseDown || editingShape != nullptr || cropDragging() || selDragging;
}

std::vector<WinPin*> WinPin::getHiddenPins(BarEdge edge)
{
	std::vector<WinPin*> out;
	for (auto& pin : winPins) {
		if (pin->isHidden && pin->barEdge == edge) out.push_back(pin.get());
	}
	return out;
}

// 见头文件上的注释：判的是光标，不是窗口
std::optional<WinPin::BarEdge> WinPin::edgeAtCursor() const
{
	// 主显示器的工作区，与书签条摆的位置同源（见 PinHiddenBar 的构造函数）。
	// 多显示器只认主显示器那两条边 —— 条本来就只画在左上角那一个角上
	RECT wa{};
	if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0)) return {};
	// 容差取几个像素：光标被系统夹在屏幕边上不再往外走，光标贴到边上就是 0，
	// 但手抖留一两个像素也是"推到头了"
	constexpr int margin{ 4 };
	POINT cur{};
	if (!GetCursorPos(&cur)) return {};
	if (cur.x <= wa.left + margin) return BarEdge::Left;
	if (cur.y <= wa.top + margin) return BarEdge::Top;
	return {};
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
	// 比的是底图范围，而 imgPos 是标注坐标：剪过一刀之后两者差一个 imgOrigin
	const auto& o = drawing->imgOrigin;
	const auto imgX = imgPos.x - o.x, imgY = imgPos.y - o.y;
	if (toolMain->curId != L"number" || imgX < 0 || imgY < 0
		|| imgX >= (LONG)sz.width || imgY >= (LONG)sz.height) {
		hideNumberPreview();
		return;
	}
	if (!numberPreview) numberPreview = std::make_unique<ShapeNumber>(drawing.get(), true);
	numberPreview->previewAt((float)imgPos.x, (float)imgPos.y, toolSub->peekNumberVal());
	numberPreviewOn = true;
	// 这个影子是要"贴着光标"的，排队等 WM_PAINT 就会一直落在鼠标后面
	refreshNow();
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
	// 选中态 / 多选那一批的清理**不在这里做**。原来挂的是"退出选择模式"这个时机，而对象
	// 选择现在是常驻的，没有"退出"可挂；本函数在挪窗口 / 改尺寸时每个鼠标事件都要跑一遍，
	// 把清理挂在这儿等于每帧都清一次，刚框选出来的一批会当场没掉。
	// 改由两个明确的时机去收（都调 clearObjectSelection）：用户在工具条上换工具
	// （ToolMain::onClick）与取消画笔 / 右键 / ESC（ToolMain::cancelSelect）
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
	// 后台识别线程可能还在跑。它只认这个标志，不看对象在不在 —— 那个线程拿到的是裸 this，
	// 所以必须由这里（对象真没了之前）把标志放掉
	if (ocrAlive) *ocrAlive = false;
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
    // 边缘阴影交给 DWM（DwmExtendFrameIntoClientArea + DWMWA_NCRENDERING_POLICY，见 Ling 的
    // WinBase::enableShadow，倒数窗口那类小窗口也是这么来的）。之所以不自己画：那要在窗口四周
    // 留一圈 margin，底图与 shape 的坐标、命中、剪裁框、工具条的落点全都得跟着缩一圈，
    // 改动面比这一行大得多，还容易在缩放 / 贴边这些路子上漏一处。
    // 阴影画在窗口矩形**之外**，由合成器负责，所以窗口尺寸和上面那套坐标一概不动
    enableShadow();
    auto d2d = Ling::D2D::get();
    // 画布铺满窗口，走 swap chain（双缓冲）后端，避免拖动 shape 时整帧闪烁
    canvas = body->makeChild<Ling::Canvas>();
    canvas->enableSwapChain();
    canvas->setSizePercent(100.f, 100.f);
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x1677ff), borderBrush.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushSelWhite.GetAddressOf());
    // 「选择画布」选区的虚线笔型：2 实 2 虚（虚线长度按笔宽算，这里是 dpi），
    // 与 ShapeText 那圈虚线框同一套观感
    {
        float dashes[]{ 2.f, 2.f };
        d2d->d2dFactory->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f),
            dashes, ARRAYSIZE(dashes), selDashStyle.GetAddressOf());
    }
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.46f), brushTipBg.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushTipText.GetAddressOf());
    // 选中的词铺的那层蓝底。半透明：字还得看得清，不然选完不知道选的是哪几个字
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x1677ff, 0.34f), selectBrush.GetAddressOf());
    // 「贴图之后默认识别文本」：不等用户点「选文」，窗口一建起来就在后台认 ——
    // 那张图通常刚截下来，用户点开选文时结果基本已经在了，选起来是即时的
    startOcr();
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
    // 底图在"标注坐标系"里的位置：剪过一刀之后这张图从 imgOrigin 处开始铺
    //（shape 的坐标没被搬动，靠这个偏移让图和标注重新对齐）
    const auto& o = drawing->imgOrigin;
    D2D1_RECT_F destRect = D2D1::RectF((float)o.x, (float)o.y, (float)o.x + sz.width, (float)o.y + sz.height);
    // 底图和 shape 都是按底图像素画的，放大缩小整个交给这个变换，
    // 笔宽、夹点跟着一起缩 —— 鼠标坐标进来时也除掉了倍数，所以命中判定天然对得上。
    // 倍数取 viewScale：缩略图模式下窗口被缩成小图，画的时候也得跟着缩，否则只剩左上角一块。
    // 先平移再缩放（D2D 是行向量、左乘先作用），于是标注坐标系 → 窗口坐标一步到位
    auto vs = viewScale();
    ctx->SetTransform(D2D1::Matrix3x2F::Translation(-(float)o.x, -(float)o.y)
        * D2D1::Matrix3x2F::Scale(vs, vs));
	ctx->DrawBitmap(drawing->screenImg.Get(), destRect);
	for (auto& shape : drawing->history->shapes)
	{
		if (!shape->isUndo) {
			shape->paint(ctx);
		}
	}
	// 选文态：选中的那些词铺一层蓝底。画在标注之上、窗口装饰之下，而且变换还在标注坐标系里，
	// 所以它跟着 Ctrl+滚轮缩放、剪裁之后也仍贴在原来的字上
	if (textSelect) paintTextSelect(ctx);
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
	// 「选择对象」的框选框与多选外框。压在夹点与动作图标之上，也画在标注坐标系里 ——
	// 它们框的是元素，不是窗口
	paintSelection(ctx);
	// 「选择画布」的选区、采样点与搬运预览。同样在标注坐标系里，跟着缩放 / 剪裁走
	if (canvasSelecting()) paintCanvasSelection(ctx);
	// 蓝边框和倍数提示属于窗口装饰，不跟着图缩放：变换收回来，按窗口坐标画。
	// 边框也因此从"底图矩形"改成"窗口矩形"，任何倍数下都是 2*dpi 粗
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	ctx->DrawRectangle(D2D1::RectF(0.f, 0.f, w, h), borderBrush.Get(), 2*dpi);
	// 剪裁采样点压在边框上，任何时候都能拖（缩略图 / 细条 / 藏起来时除外，见 paintCropHandles）
	paintCropHandles(ctx);
	// 多选时画布右上角那三枚批量按钮（删除 / 复制 / 旋转）。同属窗口装饰，不进导出图
	paintBatchButtons(ctx);
	paintTitle(ctx);
	paintScaleTip(ctx);
	paintToast(ctx);
	if (textSelect) paintTextTip(ctx);
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
	// 上一按的残尾先清掉。Ctrl+单击那一支不 SetCapture（它不拖任何东西），
	// 按下之后把光标拖到窗口外再松手，onUp 根本不会来 —— 留着的这个标志会把
	// 下一次抬手整个吃掉（那一下本该选中元素 / 收尾空笔）。它只对同一次按放有效
	ctrlToggling = false;
	// 这一下按过鼠标，说明这一轮 Ctrl 是当修饰键使的（追加 / 框选），
	// 抬手时不该再被当成"空点一下 Ctrl"去收框选（见 onKeyUp）
	ctrlTapArmed = false;
	// 同上，而且要**一直记到 Ctrl 抬起来**才复位：按住 Ctrl 拖框选时系统会不停补
	// Ctrl 的 WM_KEYDOWN（自动重复），只清 ctrlTapArmed 的话下一条重复键就把它立回去了
	//（见 onKey 里那段说明）
	ctrlUsedAsModifier = true;
	// 同 ctrlToggling：上一次按的是批量按钮、抬手又没落在窗口里（没 SetCapture），
	// 标志就会留着 —— 在这里一并复位，免得吃掉下一次抬手
	batchBtnClicked = false;
	// 点上一下就把本窗口激活了（WM_MOUSEACTIVATE -> SetForegroundWindow），而激活会把这个
	// topmost 窗口提到同类的最前面 —— 于是它压住了自己的两条工具条。工具条通常落在窗口外面，
	// 看不出来；全屏贴图的 overlay 模式下工具条整条盖在底图里头，一点图就"工具条没了"，
	// 而且点不到（命中测试落在底图上）。layoutTools 里那次 raiseTools 只在重叠状态**变化**时跑，
	// 管不到"激活导致的重排"，所以每次点到图上都补一次
	raiseTools();
	// 多选那一批的三枚批量按钮（画在画布右上角，窗口坐标）。必须排在所有分支之前 ——
	// 它们盖在图上，不先拦的话这一下会被当成"点空白"，反手把整批选中收掉
	if (!isRight && !drawing->multiSelected.empty()) {
		if (auto idx = batchBtnAt(pos); idx >= 0) {
			batchBtnClicked = true;
			if (idx == 0) batchDeleteShapes();
			else if (idx == 1) batchCopyShapes();
			else toggleBatchRotate();
			return;
		}
		// 「拖拽旋转」态里，画布上按下就是"开始转"：中心在按下时按整组外接框定一次，
		// 拖动期间不再重算。与别的拖拽同一套收尾 —— SetCapture + isMouseDown，
		// 抬手那一下由 onUp 收（见那里的 batchRotating）
		if (batchRotateOn) {
			isMouseDown = true;
			hasDragged = false;
			SetCapture(hwnd);
			batchRotating = true;
			batchRotateCenter = multiSelectCenter();
			auto ip = toImgPos(pos);
			batchRotatePrevAngle = atan2f((float)ip.y - batchRotateCenter.y, (float)ip.x - batchRotateCenter.x)
				* 180.f / 3.14159265358979323846f;
			return;
		}
	}
	// 剪裁采样点：压在图的边界上，落在它上面就是"改这张图保留原图的哪一块"，不是画画也不是
	// 拖窗口。必须排在所有分支之前 —— 沿边线画一笔是很常见的动作，但用户瞄着采样点按下去
	// 只想改范围（fastcapture 那套就是这么定的）。右键让开：那一套是收放工具条
	if (!isRight) {
		auto handle = cropHandleAt(pos);
		if (handle >= 0) {
			startCropDrag(handle);
			return;
		}
	}
	// 选文态：左键这一下是"划选一段文字"，既不落笔（curId 已经放掉）也不拖窗口。
	// 整条判断必须排在 isLocked 之前 —— 锁的是"别改这张图"，在里面选字不算改图
	if (textSelect && !isRight) {
		auto imgPos = toImgPos(pos);
		// 双击 = 选中光标底下这一个词。要先认，否则按下的第一下已经先把它缩成一个插入点了
		if (takeDoubleClick()) {
			auto idx = wordIndexAt(imgPos);
			if (idx >= 0) { selAnchor = idx; selCur = idx + 1; }
			else { selAnchor = selCur = caretIndexAt(imgPos); }
			lastDownTime = 0;   // 第三下不该再凑成一次双击
		}
		else {
			selAnchor = selCur = caretIndexAt(imgPos);
		}
		selDragging = true;     // 双击之后接着拖也能继续扩选
		isMouseDown = true;
		hasDragged = false;
		SetCapture(hwnd);
		refresh();
		return;
	}
	// 选文态里右键先当"取消选中"：有选区就清掉，没有选区才轮到下面那套收放工具条
	if (textSelect && isRight && hasSelection()) {
		selAnchor = selCur = 0;
		refresh();
		return;
	}
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
	// 双击判定得自己做（Ling 的窗口类没带 CS_DBLCLKS，收不到 WM_LBUTTONDBLCLK）。
	// 为什么比的是屏幕坐标而不是客户区坐标，见 takeDoubleClick
	bool isDblClick = takeDoubleClick();
	// 双击 = Ctrl+C：把图连标注一起送进剪切板并关窗，选着画笔也一样（等价于按 Ctrl+C，
	// 手里拿着什么工具都不该影响这个手势）。要在下面所有分支之前处理：
	// 这一下既不是画画也不是拖窗，不该留下 capture、更不该新建 shape。
	// 编辑文字时不算：双击归文本框（选中单词），点在框外才会走到这里。
	// 「选择画布」下也不算：那一下双击会变成"复制整张图并关窗"，把正在框的选区一起带走
	if (isDblClick && !editingShape && !canvasSelecting()) {
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
	// 没选画笔，或只开着贴图属性面板（都画不了），左键是拖窗口，拖的时候把工具条收起来。
	// 收之前先记下它这会儿开着没有：拖到屏幕边线上会顺势把图藏起来，那一次"藏"发生在抬手时，
	// 工具条已经被这次按下收掉了，事后再问 isToolsVisible 一律是关 —— 记下的是"拖动之前"
	if (!hasDrawTool()) {
		toolsWereVisible = isToolsVisible();
		winDragging = true;
		toolMain->hide();
		return;
	}
	// 以下都是交给 shape 的坐标，一律换算成底图像素（拖窗口那条路仍用窗口坐标）
	auto imgPos = toImgPos(pos);
	// 「选择画布」：拉选区 / 搬画面 / 改大小 / 点选区上的复制删除。整条排在对象选择之前 ——
	// 它开着的整个期间画布上的左键都归底图选区，不参与对象选择
	//（capture / isMouseDown 上面已经置好了）
	if (canvasSelecting()) {
		clearObjectSelection();
		canvasSelectDown(imgPos);
		return;
	}
	// 从这儿往下手里都是拿着画笔的（hasDrawTool 上面问过），对象选择一律生效：
	// 点元素 = 选中它，点空白 = 用当前画笔落一笔。不必先切到"选择对象"
	//
	// Ctrl+单击：在选中那一批上做加减。Ctrl+拖动一次框出一二十个之后，想剔掉多选的
	// 那几个、或者补上漏掉的那一个，就靠这一下。必须排在下面那句 clear 之前 ——
	// 否则整批先被清空，这一下就成了"换成单选它"。
	// 用 Ctrl 而不是 Shift：Shift 在矩形 / 圆那边是"约束成正圆"（见 ShapeRectBase 的
	// mouseDrag），拿它当加减选的修饰键会和那个手势打架。
	// 空白处的 Ctrl+拖动仍然是框选（下面那条分支），两者不冲突：这一条要压在元素上，
	// 那一条要落在空白处
	if (drawing->shapeHover && (GetKeyState(VK_CONTROL) & 0x8000)) {
		auto& batch = drawing->multiSelected;
		// 上一次那个"单选"先并进来。用户心里的那一批常常就是从它开始的：
		// 先点一个（单选）、再 Ctrl 点第二个（想变成两个一起选）——
		// 不并进来的话第一个会被这一下放掉，看着就是"Ctrl 追选之后只剩下新点的那个"
		if (drawing->selected
			&& std::find(batch.begin(), batch.end(), drawing->selected) == batch.end()) {
			batch.push_back(drawing->selected);
		}
		auto it = std::find(batch.begin(), batch.end(), drawing->shapeHover);
		if (it != batch.end()) batch.erase(it);
		else batch.push_back(drawing->shapeHover);
		// 不建立单选：这一下加的是"整批"，选中态若换成它一个，外框与夹点就只盯住它了
		drawing->selected = nullptr;
		drawing->shapeCur = nullptr;
		ctrlToggling = true;
		refresh();
		return;
	}
	// 拖多选那一批里的任意一个 = 整批一起挪位置。必须排在下面那句 clear 之前 ——
	// 那一句会把整批清掉，之后就没得拖了。Ctrl 的那一支排在更前面：按着 Ctrl 压在
	// 同一批里的某个上，意思是"把它剔出去"，不该同时开始拖
	if (drawing->shapeHover
		&& std::find(drawing->multiSelected.begin(), drawing->multiSelected.end(), drawing->shapeHover)
			!= drawing->multiSelected.end()) {
		auto ip = toImgPos(pos);
		drawing->selected = nullptr;
		drawing->shapeCur = nullptr;   //挪的是已有元素，不参与空笔判定
		batchMoving = true;
		batchMoveLast = POINT{ ip.x, ip.y };
		return;
	}
	// 这一下按下就进入"单选 / 框选"了，上一轮框选那一批到此为止。不清的话
	// Delete 会连上次框的一起删（下面的分支会按需要重新填）
	drawing->multiSelected.clear();
	clearBatchState();
	if (drawing->shapeHover) {
		// 点在已有元素上：建立单选、把按下转给它 —— 于是拖它能挪位置、拉夹点能改大小。
		// 选中态独立于悬停，移开鼠标也不会丢。抬手时若一步没拖过才把工具条切到这个元素的
		// 工具上去（见 onUp）
		drawing->selected = drawing->shapeHover;
		drawing->shapeCur = nullptr; //改的是已有元素，不参与空元素判定
		drawing->shapeHover->mouseDown((float)imgPos.x, (float)imgPos.y);
		return;
	}
	// 点在空白处：取消选中。Ctrl+拖 = 框选；否则用当前画笔落新的一笔
	//（下面新建的这笔如果只是单击，抬手时空笔判定会把它自己收掉）
	drawing->selected = nullptr;
	if (GetKeyState(VK_CONTROL) & 0x8000) {
		// 框选。刻意不走 createShape —— 它在 History 里没有对应的元素，
		// 走了也只是白建一个空指针
		marqueeOn = true;
		marqueeAnchor = imgPos;
		marqueeCur = imgPos;
		refresh();
		return;
	}
	drawing->shapeHover = drawing->history->createShape(toolMain->curId, imgPos.x, imgPos.y);
	drawing->shapeCur = drawing->shapeHover;
}

void WinPin::onMove(POINT pos)
{
	// (INT_MAX, INT_MAX) 是 Ling 在"鼠标离开窗口"时补的哨兵移动（见 WinBase::mouseLeave）。
	// 它只对"没按着"的那半边有意义 —— 用来把 hover 高亮收掉。可要是**按着的时候**收到它，
	// 下面每一条拖拽路径都会拿着这个天文数字去算坐标：拖窗口那条最惨，newX = x + INT_MAX -
	// pressPos.x，系统再把坐标夹进 16 位（±32767），整张图直接飞到屏幕的另一头，接着在
	// 两端来回翻 —— 看起来就是"拖到一半突然疯狂抖动"。拖其他东西（剪裁点 / 夹点 / 选框）
	// 同样会算出垃圾坐标。所以按着的时候一律不认这条哨兵
	if (isMouseDown && pos.x == INT_MAX && pos.y == INT_MAX) return;
	// 正拉着剪裁采样点：换算成屏幕坐标再算新范围。窗口自己也在跟着挪 / 改尺寸，
	// 客户区坐标一路在变，只有屏幕坐标是稳的
	if (cropDragging()) {
		POINT screen{ pos.x, pos.y };
		ClientToScreen(hwnd, &screen);
		dragCropTo(screen);
		return;
	}
	// 选文态：按着就是在扩大 / 缩小选区（插入点跟着鼠标走）；没按着只是在图上划过，
	// 什么都不做 —— 所以这里也不必给词做 hover 高亮
	if (textSelect) {
		if (!selDragging) return;
		selCur = caretIndexAt(toImgPos(pos));
		refreshNow();
		return;
	}
	// 缩略图上不做 hover / 命中：点一下是"还原"，夹点也没有地方摆
	if (isThumb) return;
	// 收成细条时鼠标一碰就展开 —— 这是细条唯一的展开方式
	if (isMinimized) {
		setMinimized(false);
		return;
	}
	if (isLocked) return;   // 锁定时不给 hover 高亮，也不给拖动
	// 「选择画布」：这个子模式下鼠标只在画布上做三件事（拉框 / 搬画面 / 改大小），
	// 不参与元素 hover，也不往 shape 派发 —— 整条排在下面那些之前
	if (canvasSelecting()) {
		if (isMouseDown && selDrag) canvasSelectMove(toImgPos(pos));
		return;
	}
	// 同 onDown：文本框里的移动归 TextBox（拖选、滚动条 hover），不参与 shape 的 hover 判定
	if (editingShape && textBox && textBox->isPosIn(pos)) return;
	// 拖窗口用的是窗口坐标（pressPos 也是），只有交给 shape 的才换算成底图像素
	auto imgPos = toImgPos(pos);
	if (isMouseDown) {
		// Ctrl+单击的加减选那一下不该拖动任何东西（见 onDown）：它刻意没调 mouseDown，
		// 而下面那条 mouseDrag 拿的是元素上一次留下的 grab 偏移 —— 光标随手一动就会
		// 拿那个陈旧偏移把图形拖走。这正是"Ctrl 追加选中之后，被追加的那个位置下移了"
		if (ctrlToggling) return;
		// 批量拖动多选那一批。从按下点算增量而不是从"每个元素的起点"算 ——
		// 全是平移，逐帧增量叠起来与一次到位等价
		if (batchMoving) {
			auto dx = imgPos.x - batchMoveLast.x, dy = imgPos.y - batchMoveLast.y;
			if (dx != 0 || dy != 0) {
				hasDragged = true;
				for (auto* s : drawing->multiSelected) s->moveBy((float)dx, (float)dy);
				batchMoveLast = POINT{ imgPos.x, imgPos.y };
				refreshNow();
			}
			return;
		}
		// 拖拽旋转态里正按着：两帧鼠标方向角之差就是这一帧要转的量。
		// "转了多大角度"是绕整组外接框中心（batchRotateCenter）量出来的，但**落到每个元素上
		// 传的是它自己的中心** —— 也就是"各自原地转个角度"，与单选那枚旋转手柄同一条逻辑。
		// 原来传的是整组中心：那一批会绕着那个点公转，两个元素各自跑到新方位上
		//（作者报的"旋转后 2 个标注对象位置都变了、中心点偏移了原来的位置"）。
		// 逐个取自己的外接框中心而不是 rectCenter：getShapeBounds 已经把自身旋转算进去了，
		// 而"绕自己中心转"不改变外接框中心 —— 两者本来就是同一个点
		if (batchRotating) {
			auto now = atan2f((float)imgPos.y - batchRotateCenter.y, (float)imgPos.x - batchRotateCenter.x)
				* 180.f / 3.14159265358979323846f;
			auto d = now - batchRotatePrevAngle;
			if (d != 0.f) {
				hasDragged = true;
				for (auto* s : drawing->multiSelected) {
					D2D1_RECT_F b{};
					if (!s->getShapeBounds(b)) continue;   // 没有外接框的（水印）本来也不参与框选
					s->rotateBy(d, D2D1::Point2F((b.left + b.right) / 2.f, (b.top + b.bottom) / 2.f));
				}
				batchRotatePrevAngle = now;
				refreshNow();
			}
			return;
		}
		if (!hasDrawTool()) {
			auto newX = x + pos.x - pressPos.x;
			auto newY = y + pos.y - pressPos.y;
			auto dx = newX - x, dy = newY - y;
			// 光标一步没挪也会来 WM_MOUSEMOVE。抬手时要靠它区分"拖过"和"只是点了一下"：
			// 只有真拖过才轮得到"拖到屏幕边线上就藏起来"那一条
			if (pos.x != pressPos.x || pos.y != pressPos.y) hasDragged = true;
			// 位置没变就别再挪了。窗口自己挪完之后系统会补一条"光标没动"的 WM_MOUSEMOVE
			// （客户端坐标是相对窗口算的，窗口一动它就变），按上面那条公式算出来的正好是原位。
			// 实测每挪一步会多出三分之一次这种空挪，一次空挪照样要走一遍 WM_MOVE -> onMoved，
			// 白花几毫秒，还会把工具条那一串 SetWindowPos 再叫醒一次
			if (newX == x && newY == y) return;
			setPosition(newX, newY);
			// 成组的贴图跟着一起挪，整组的相对位置不变
			syncGroupPos(this, dx, dy);
			return;
		}
		// 正拉着框选：选框跟着光标走。hasDragged 的判法与别的拖拽同一条规矩 ——
		// 抬手时 collectMarquee 要用它区分"拉了个框"和"只是点了一下空白"
		if (marqueeOn) {
			if (pos.x != pressPos.x || pos.y != pressPos.y) hasDragged = true;
			marqueeCur = imgPos;
			refreshNow();
			return;
		}
		else if(drawing->shapeHover) {
			// 光标一步没挪也会来 WM_MOUSEMOVE，所以跟按下点比一下再算拖动
			if (pos.x != pressPos.x || pos.y != pressPos.y) hasDragged = true;
			drawing->shapeHover->mouseDrag((float)imgPos.x, (float)imgPos.y);
			refreshNow();
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
					refreshNow();
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
	// 拖窗口这一下到此结束。下面无论走哪条早退，拖窗口期间被跳过的那次工具条重排
	// 都不该再拖着了 —— 所以标志在最前面就放掉（见 winDragging / onMoved）
	winDragging = false;
	// 剪裁采样点：这一下只是把范围放稳，没有新建元素要收尾
	if (cropDragging()) {
		endCropDrag();
		return;
	}
	// 选文态：这一下只是把选区放下，除了 capture 没有别的东西要收（也没有新建的元素要清）
	if (textSelect) {
		isMouseDown = false;
		selDragging = false;
		ReleaseCapture();
		return;
	}
	// 右键按下时什么都没抓（既没置 isMouseDown 也没 SetCapture，见 onDown），抬手也就没什么要收的。
	// 更要紧的是不能往下走：下面那条"拖窗结束"的路会把 ToolMain 显示出来，
	// 而右键刚刚才把它收起来 —— 一按一放就等于什么都没做
	if (isRight) return;
	isMouseDown = false;
	ReleaseCapture();
	// 这一下按的是批量按钮（见 onDown）：动作在按下那一下已经做完了，
	// 抬手不该再走下面任何一条 —— 照常走的话会按光标底下那个元素建立单选
	if (batchBtnClicked) {
		batchBtnClicked = false;
		return;
	}
	// 「选择画布」：这一下只是把选区 / 搬移放稳，没有新建的元素要收尾
	if (selDrag) {
		canvasSelectUp();
		return;
	}
	// 批量拖动 / 拖拽旋转：这一下只是把位置 / 角度放稳，没有新建元素要收尾。
	// 旋转态刻意不一起收 —— 按钮还亮着，用户接着能再拖一次
	if (batchMoving || batchRotating) {
		batchMoving = false;
		batchRotating = false;
		return;
	}
	// Ctrl+单击的加减选（见 onDown）：这一下只改了框选那一批，既不建立单选、
	// 也没有新建元素要收尾，抬手不该往下走（下面那条路会把 selected 换成它一个）
	if (ctrlToggling) {
		ctrlToggling = false;
		return;
	}
	// 框选：抬手把选框里那批收下。没有新建的元素要收尾，也没有"空笔"要判定
	if (marqueeOn) {
		marqueeOn = false;
		collectMarquee();
		refresh();
		return;
	}
	auto justCreated = drawing->shapeCur;
	drawing->shapeCur = nullptr;
	// 这一下按下有没有新建出一个留得住的元素：紧接着来第二下凑成双击时要把它撤掉（见 onDown）
	prevPressCreatedShape = false;
	if (!hasDrawTool()) {
		// 拖到屏幕最左边 / 最顶上松手 = 把这张图藏到那条边上（左 / 上各挂一条书签，
		// 见 PinHiddenBar）。藏起来之后窗口就没了，工具条自然也不该再请出来，所以到此为止
		if (hasDragged) {
			if (auto edge = edgeAtCursor()) {
				// 按一次普通拖动的收尾把工具条摆回拖动前的样子（本次按下时收掉的），
				// setHidden 记下的才是用户自己那套开 / 关 —— 否则 hover 回来的是一张
				// 光秃秃的图，工具条要再点一下才出得来
				if (toolsWereVisible) toolMain->show();
				setHidden(true, *edge);
				return;
			}
		}
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
		// 点一下元素 = 选中它，再把这个元素的工具请出来 —— 于是不必先判断"这是哪个组件"、
		// 再回头切一次工具，接着点下一个元素照样切。
		// 拖过的（拖位置 / 拉夹点）不切：那种手势要的是"就地把这一笔调一下"，
		// 切走反而把刚拉开的夹点收起来了。
		// 已经拿着同一个工具也不切：selectTool 会按 curId 重建整个子面板，而刚画完的这一笔
		// 本来就还拿着它的工具 —— 白重建一次会把「编号」那个输入框拨回 1（连续编号时每画
		// 一个都重置，正是 refreshToolSub 特意保存 / 还原它的原因）
		if (!hasDragged && drawing->shapeHover->toolId != toolMain->curId) {
			toolMain->selectTool(drawing->shapeHover->toolId);
		}
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
	if (id == 103) {   // 选文的轻提示（"已复制"）到点了，收掉
		killTimer(103);
		toastTip = nullptr;
		refresh();
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

void WinPin::refreshNow()
{
	if (!hwnd) return;
	refresh();
	UpdateWindow(hwnd);
}

History* WinPin::getHistory() const
{
	return drawing->history.get();
}

void WinPin::onToolStyleChanged(bool styleEnumChanged)
{
	// 多选那一批优先：颜色、填充、滑块 / 滚轮调粗细一次作用到整批。
	// 马赛克与擦除跳过：它们没有"颜色 / 线宽"这一说，而各自的 applyStyle 改的是马赛克块
	// 大小、擦除笔刷宽度那类东西 —— 批量调一次颜色把它们一起改了，看着就是"顺手改坏了"。
	// 图片（applyStyle 早退）与水印（不进多选）天然不参与
	if (!drawing->multiSelected.empty()) {
		for (auto* s : drawing->multiSelected) {
			if (s->toolId == L"mosaic" || s->toolId == L"eraser") continue;
			s->applyStyle();
			if (styleEnumChanged) s->applyToolStyle();
		}
		refresh();
		return;
	}
	// 优先级：正在编辑的文本 > 选中的元素。两者都没有就什么都不改 ——
	// 这条链路以前只认 editingShape，选中态没有单独的载体，选中的矩形族
	// 连 applyStyle 都没实现，颜色永远是构造那一刻的快照
	auto target = editingShape ? editingShape : drawing->selected;
	if (!target) return;
	target->applyStyle();
	// 档位（箭头样式 / 线条类型 / 端点 / 线型）只有用户真去动那个下拉时才套过去。
	// 颜色、填充开关、滚轮调粗细走的是同一个入口，带上档位的话它们会把形状一起换掉
	if (styleEnumChanged) target->applyToolStyle();
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
		// 「全」的意思是"照我工具条这套来"，包括形状那一档 —— 比照选中态多走一步
		// applyToolStyle（见 onToolStyleChanged 里那两条为什么分开）
		if (shape->toolId == toolMain->curId) {
			shape->applyStyle();
			shape->applyToolStyle();
		}
	}
	refresh();
}

// 一键清除图上所有水印（水印面板上的「清除」按钮）。
// 遍历整份 shapes 而不是只看末尾那几个：水印加完之后又画了别的标注，它就不在末尾了；
// 被别的标注压住的水印同样得清掉 —— 这类"看不见的残留"正是这一键要清的东西。
// 走 History::undoShapes 而不是 removeShape：后者是真删，清完 Ctrl+Y 也找不回来
void WinPin::clearWatermark()
{
	auto& history = drawing->history;
	std::vector<ShapeBase*> targets;
	for (auto& shape : history->shapes) {
		// 已经处于撤销态的不必再算一遍：它这会儿本来就没画在图上
		if (shape->isUndo) continue;
		if (dynamic_cast<ShapeWatermark*>(shape.get())) targets.push_back(shape.get());
	}
	if (targets.empty()) return;
	// 正在编辑的文本先收尾：这里是"清空一类元素"，被清掉的要是当前选中的那个，
	// 编辑框还留在屏幕上就会挂在已撤销的元素上（撤销只是不画，对象还在）
	if (editingShape) editingShape->finishEditing();
	history->undoShapes(targets);
}

// ---- 选文：贴图窗口里把识别出来的文字当成可选文本 ----
//
// 整条链路：窗口建好（或换底图 / 剪裁完）→ 后台 Ocr::recognizeWords 认一遍 → 词框换算到
// 标注坐标系存起来 → 「选文」开关打开后拖拽按插入点划出一段词，Ctrl+C 把那段文字送进剪贴板。
// 词框存的是标注坐标（与 shape 同一套），所以剪裁、Ctrl+滚轮缩放都不影响它贴不贴得住字。
void WinPin::setTextSelect(bool on)
{
	if (isClosed || textSelect == on) return;
	textSelect = on;
	if (on) {
		// 细条 / 缩略图这两种收法都没给"选一段字"留位置，先还原（剪裁采样点同一个道理）
		if (isThumb) setThumbMode(false);
		if (isMinimized) setMinimized(false);
		// 两套手势都要吃左键，画笔必须让位：curId 空着 onDown 才会把这一下当"选文字"。
		// 反方向也堵上了 —— 拿起任何标注工具都会把选文关掉（见 ToolMain::selectTool）
		toolMain->cancelSelect();
		if (editingShape) editingShape->finishEditing();
		drawing->shapeHover = nullptr;
		drawing->selected = nullptr;
		hideNumberPreview();
		selAnchor = selCur = 0;
		selDragging = false;
		// 刚建好窗口就点进来时识别还在跑（甚至还没起），催一次；已经在跑就不用管
		if (!ocrRunning && ocrWords.empty()) startOcr();
	}
	else {
		selDragging = false;
	}
	// 开关状态住在这一层（ESC、拿起标注工具、关窗都要复位它），按钮的外观得回报过去
	toolMain->setToggle(L"textSelect", on);
	refresh();
}

void WinPin::startOcr()
{
	auto sz = drawing->screenImg ? drawing->screenImg->GetPixelSize() : D2D1::SizeU(0, 0);
	// 滚动截图拼出来的长图动辄上万像素：识别又慢又吃内存，系统引擎自己也有尺寸上限。
	// 这种图就当"没识别到文字"，选文点开也只有一句提示，总好过卡住
	if (sz.width == 0 || sz.height == 0 || sz.width > 10000 || sz.height > 10000) return;
	std::vector<BYTE> pixels;
	int w{ (int)sz.width }, h{ (int)sz.height };
	if (!readBasePixels(pixels, w, h)) return;
	const auto seq = ++ocrSeq;
	// 起识别这一刻的偏移：词框出来的是"底图像素"，回填时按它换算到标注坐标系。
	// 不能等回填时再去读成员 —— 这中间用户可能正好剪了一刀，那个值已经不是当时那个了
	const auto origin = drawing->imgOrigin;
	auto lang = Setting::get()->getToolStr(L"ocr", L"lang", L"");
	ocrRunning = true;
	// 状态条上那句"正在识别文字…"是由本标志驱动的，得让它立刻显出来
	if (textSelect) refresh();
	auto alive = ocrAlive;
	// 识别要几百毫秒到几秒，压在 UI 线程上整张图会僵住（同 WinOcr / GlobalMouse 的做法）
	std::thread([this, alive, pixels = std::move(pixels), w, h, seq, origin, lang = std::move(lang)]() mutable {
		// 新线程里没有 WinRT 单元，不初始化就用不了 OcrEngine
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
		auto words = Ocr::recognizeWords(w, h, pixels.data(), lang);
		Ling::App::get()->dq.TryEnqueue([this, alive, seq, origin, words = std::move(words)]() mutable {
			// 窗口已经关了、或者又起过一次识别（换底图 / 剪裁），老结果一概不认
			if (!*alive || ocrSeq != seq) return;
			ocrRunning = false;
			applyOcrWords(std::move(words), origin);
		});
	}).detach();
}

void WinPin::clearOcr()
{
	ocrWords.clear();
	ocrLines.clear();
	selAnchor = selCur = 0;
	selDragging = false;
}

void WinPin::applyOcrWords(std::vector<OcrWord> words, POINT origin)
{
	clearOcr();
	if (words.empty()) {
		if (textSelect) refresh();
		return;
	}
	ocrWords = std::move(words);
	// 底图像素 → 标注坐标，往后一律按标注坐标用（与 shape 完全一致）
	for (auto& word : ocrWords) {
		word.x += (float)origin.x;
		word.y += (float)origin.y;
	}
	// 引擎给出来的本来就是"按行、行内从左到右"，这里只把连续的同高度词切成行、不改顺序 ——
	// 顺序一乱，"选中从 A 到 B 这一段"就没有意义了
	float lineCenter = ocrWords[0].y + ocrWords[0].h / 2.f;
	int first{ 0 };
	for (int i = 1; i <= (int)ocrWords.size(); ++i) {
		bool newLine = (i == (int)ocrWords.size());
		if (!newLine) {
			auto center = ocrWords[i].y + ocrWords[i].h / 2.f;
			auto ref = std::max(ocrWords[i].h, ocrWords[first].h);
			// 竖直中心差不到大半个字高就算同一行（同一行里各词的字号可能略有出入）
			if (std::abs(center - lineCenter) > ref * 0.6f) newLine = true;
		}
		if (!newLine) continue;
		ocrLines.push_back(OcrLine{ first, i - first });
		if (i < (int)ocrWords.size()) {
			first = i;
			lineCenter = ocrWords[i].y + ocrWords[i].h / 2.f;
		}
	}
	refresh();
}

bool WinPin::readBasePixels(std::vector<BYTE>& pixels, int& w, int& h)
{
	if (!drawing->screenImg) return false;
	auto sz = drawing->screenImg->GetPixelSize();
	if (sz.width == 0 || sz.height == 0) return false;
	auto ctx = Ling::D2D::get()->deviceContext.Get();
	// GPU 上的位图不能直接 Map，先拷到一块带 CPU_READ 的位图上（同 getImagePixels 的手法）。
	// 像素格式必须照抄底图：截图那条路进来的是 ALPHA_MODE_IGNORE、长图那条是 PREMULTIPLIED，
	// 写死一个就会在 CopyFromBitmap 上吃 E_INVALIDARG
	D2D1_BITMAP_PROPERTIES1 cpuProps{
		.pixelFormat{ drawing->screenImg->GetPixelFormat() },
		.dpiX{ 96.0f }, .dpiY{ 96.0f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW }
	};
	ComPtr<ID2D1Bitmap1> cpuBmp;
	if (FAILED(ctx->CreateBitmap(sz, nullptr, 0, &cpuProps, cpuBmp.GetAddressOf()))) return false;
	if (FAILED(cpuBmp->CopyFromBitmap(nullptr, drawing->screenImg.Get(), nullptr))) return false;
	D2D1_MAPPED_RECT mapped{};
	if (FAILED(cpuBmp->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
	// mapped.pitch 按 GPU 行对齐，可能大于 w*4；识别引擎要的是紧凑步长，逐行紧缩
	const UINT32 rowBytes = sz.width * 4;
	pixels.resize((size_t)rowBytes * sz.height);
	for (UINT32 row = 0; row < sz.height; ++row) {
		CopyMemory(pixels.data() + (size_t)row * rowBytes, mapped.bits + (size_t)row * mapped.pitch, rowBytes);
	}
	cpuBmp->Unmap();
	w = (int)sz.width;
	h = (int)sz.height;
	return true;
}

// 标注坐标 → 选区的"插入点"下标（0..ocrWords.size()）。与文本编辑器同一套：按阅读顺序
// 数下来，落在词与词的缝里、行尾空白上也能定出一个位置，不必非要命中某个词
int WinPin::caretIndexAt(const POINT& imgPos) const
{
	if (ocrLines.empty()) return 0;
	for (auto& line : ocrLines) {
		auto& first = ocrWords[line.first];
		// 行高按这一行第一个词算：同一行基线一致，够用了
		auto top = first.y, bottom = first.y + first.h;
		if (imgPos.y < top) return line.first;      // 落在这一行上面 → 插入点在这一行行首
		if (imgPos.y > bottom) continue;            // 还在更下面的行里，接着往下找
		for (int i = line.first; i < line.first + line.count; ++i) {
			auto& word = ocrWords[i];
			// 以词的中点为界：过了一半就算走到下一个词，与拖选文字的手感一致
			if (imgPos.x < word.x + word.w / 2.f) return i;
		}
		return line.first + line.count;             // 在行尾右边 → 这一行末尾
	}
	return (int)ocrWords.size();
}

int WinPin::wordIndexAt(const POINT& imgPos) const
{
	for (int i = 0; i < (int)ocrWords.size(); ++i) {
		auto& word = ocrWords[i];
		if (imgPos.x >= word.x && imgPos.x <= word.x + word.w
			&& imgPos.y >= word.y && imgPos.y <= word.y + word.h) return i;
	}
	return -1;
}

std::wstring WinPin::selectedText() const
{
	if (!hasSelection() || ocrWords.empty()) return {};
	const int lo = std::min(selAnchor, selCur);
	const int hi = std::min(std::max(selAnchor, selCur), (int)ocrWords.size());
	std::wstring text;
	int lastLine{ -1 };
	for (int i = lo; i < hi; ++i) {
		// 这个下标落在第几行。ocrLines 是按阅读顺序排的，找出 i 落在哪一段里
		int lineNo{ -1 };
		for (int n = 0; n < (int)ocrLines.size(); ++n) {
			auto& line = ocrLines[n];
			if (i >= line.first && i < line.first + line.count) { lineNo = n; break; }
		}
		if (!text.empty() && !ocrWords[i].text.empty()) {
			if (lineNo != lastLine) text += L'\n';
			else {
				// 空格判据统一走 ocrNeedSpace（汉字之间不补、其余看图上有没有空白），
				// 与 Ocr::recognize 拼整行文字时用的是同一条规则
				const auto& prev = ocrWords[i - 1];
				const auto& cur = ocrWords[i];
				const float rel = (cur.x - (prev.x + prev.w)) / std::max(prev.h, cur.h);
				if (!prev.text.empty() && ocrNeedSpace(prev.text.back(), cur.text.front(), rel))
					text += L' ';
			}
		}
		text += ocrWords[i].text;
		lastLine = lineNo;
	}
	return text;
}

void WinPin::copySelectedText()
{
	auto text = selectedText();
	// 空选区（点一下没拖）不去动剪贴板，也不弹提示 —— 那一下本来就没打算复制
	if (text.empty()) return;
	Ling::Util::setTextToClipboard(text);
	// 刻意**不**关窗：这一下只把选中的那段字送进剪贴板，图还得留在屏幕上接着标。
	// "复制整张图并走人"是 Ctrl+C 在非选文态下的那套，两者靠有没有选中文字分开
	showToast(Lang::get(L"tool.textSelectCopied"));
}

void WinPin::showToast(const std::wstring& text)
{
	toastTip = Ling::D2D::get()->makeTextLayout(text, 12.f * dpi);
	// 同一个 id 再调一次 SetTimer 就是重新计时，连续复制两次不会被前一次提前收掉
	setTimer(1400, 103);
	refresh();
}

// 选中的词铺一层半透明蓝底。调用方还在标注坐标系的变换里，所以它跟着缩放、剪裁一起走
void WinPin::paintTextSelect(ID2D1DeviceContext* ctx)
{
	if (!hasSelection() || !selectBrush) return;
	const int lo = std::min(selAnchor, selCur);
	const int hi = std::min(std::max(selAnchor, selCur), (int)ocrWords.size());
	for (int i = lo; i < hi; ++i) {
		auto& word = ocrWords[i];
		if (word.w <= 0 || word.h <= 0) continue;
		ctx->FillRectangle(D2D1::RectF(word.x, word.y, word.x + word.w, word.y + word.h), selectBrush.Get());
	}
}

// 顶部那条状态。"认完了而且认出了东西"就什么都不画；否则要说明是"还没认完"还是"确实没字" ——
// 用户点开选文却选不到任何东西时，这两者的下一步动作完全不同
void WinPin::paintTextTip(ID2D1DeviceContext* ctx)
{
	if (!brushTipBg) return;
	std::wstring want;
	if (ocrRunning) want = Lang::get(L"tool.textSelectLoading");
	else if (ocrWords.empty()) want = Lang::get(L"tool.textSelectNone");
	// 本函数每帧都会跑，状态没变就不重建布局
	if (want != textTipFor) {
		textTipFor = want;
		textTip = want.empty() ? nullptr : Ling::D2D::get()->makeTextLayout(want, 13.f * dpi);
	}
	if (!textTip) return;
	DWRITE_TEXT_METRICS tm{};
	if (FAILED(textTip->GetMetrics(&tm))) return;
	auto pad = 8.f * dpi;
	D2D1_RECT_F bar{ w / 2.f - tm.width / 2.f - pad, pad,
		w / 2.f + tm.width / 2.f + pad, pad + tm.height + pad * 2 };
	// 图窄到装不下时贴左边画，别画到窗口外面去
	if (bar.left < pad) bar.left = pad;
	if (bar.right > w - pad) bar.right = w - pad;
	ctx->FillRectangle(bar, brushTipBg.Get());
	ctx->DrawTextLayout({ bar.left + pad, bar.top + pad }, textTip.Get(), brushTipText.Get(),
		D2D1_DRAW_TEXT_OPTIONS_NONE);
}

// 右下角的轻提示（"已复制"）。摆这个角是因为别的角都占了：右上角是倍数提示、
// 顶上一条是标题、顶部中间是剪裁 / 识别提示
void WinPin::paintToast(ID2D1DeviceContext* ctx)
{
	if (!toastTip || !brushTipBg) return;
	DWRITE_TEXT_METRICS tm{};
	if (FAILED(toastTip->GetMetrics(&tm))) return;
	auto pad = 4.f * dpi;
	auto margin = 5.f * dpi;
	D2D1_RECT_F bar{ w - margin - tm.width - pad * 2, h - margin - tm.height - pad * 2,
		w - margin, h - margin };
	if (bar.left < margin) bar.left = margin;
	ctx->FillRectangle(bar, brushTipBg.Get());
	ctx->DrawTextLayout({ bar.left + pad, bar.top + pad }, toastTip.Get(), brushTipText.Get(),
		D2D1_DRAW_TEXT_OPTIONS_NONE);
}

// 自己认双击。Ling 的窗口类没带 CS_DBLCLKS，收不到 WM_LBUTTONDBLCLK，只能按系统的双击间隔
// 和判定框自己算；比的是**屏幕**坐标 —— 拖动贴图窗口时光标的客户区坐标几乎不动，
// 只有屏幕坐标分得开"拖一下松手再拖一下"和真双击
bool WinPin::takeDoubleClick()
{
	POINT screenPos{};
	GetCursorPos(&screenPos);
	auto now = GetTickCount64();
	bool dbl = (now - lastDownTime <= GetDoubleClickTime())
		&& std::abs(screenPos.x - lastDownPos.x) <= GetSystemMetrics(SM_CXDOUBLECLK)
		&& std::abs(screenPos.y - lastDownPos.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
	lastDownTime = now;
	lastDownPos = screenPos;
	return dbl;
}

void WinPin::onKey(UINT key)
{
	// Ctrl 的"空点一下"：按下时先把"还没被用掉"立起来，之后按下任何一个别的键
	//（Ctrl+C / Ctrl+Z / Ctrl+滚轮…）都说明这一轮 Ctrl 是当修饰键使的，撤掉。
	// 真正收框选的那一下在 onKeyUp —— 为什么不能按下就收，见那里的说明。
	// 这两句必须排在最前面：下面那几处早退（选文态 / 锁定 / 编辑中）都是"按键归别人"，
	// 而"有没有拿 Ctrl 当修饰键"这件事与它们无关
	//
	// ⚠️ **Ctrl 按住不放时系统会一直补 WM_KEYDOWN**（自动重复：先等 500ms 上下，之后每 ~33ms 一条）。
	// 不滤掉这些重复键会出事：按住 Ctrl 拖框选的那几百毫秒里，onDown 刚把标记清掉，
	// 紧接着来的一条重复键又把它立起来 —— 抬手放开 Ctrl 那一刻就被判成"空点一下 Ctrl"，
	// 刚框中的那一批当场被收掉（按住 Ctrl 稍久一点做框选就必然中招，实测把重复键投进去就能复现）。
	// 判据**不能用时间**：自动重复的首次延迟随系统设置可到 1s，跟"两次真按"分不开。
	// 改用"这一轮 Ctrl 已经被当修饰键用掉了没有"（onDown / onWheel 里置位，Ctrl 抬起来才复位）——
	// 重复键于是永远立不起标记，而"真·空点一下 Ctrl"（自始至终没碰鼠标）照旧成立。
	// 极端情况下漏掉一次 Ctrl 抬起（按着 Ctrl 切走了窗口，keyup 落在别人身上）只会让
	// 下一按不生效，再按一下自己就好了 —— 不会一直哑下去
	if (key == VK_CONTROL) {
		if (!ctrlUsedAsModifier) ctrlTapArmed = true;
	}
	else ctrlTapArmed = false;
	// 选文态：只认 Ctrl+C（把选中的那段文字送进剪贴板，**不关窗**）、Ctrl+A（全选）、
	// ESC（先清掉选区，再退整个模式）。这一句必须排在 isLocked 和所有全局快捷键之前 ——
	// 别的键在这里一概不认，否则 Delete 会删掉图上标注、回车会把整张图复制走并关窗
	if (textSelect) {
		bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
		if (ctrl && key == 'C') {
			copySelectedText();
		}
		else if (ctrl && key == 'A') {
			selAnchor = 0;
			selCur = (int)ocrWords.size();
			refresh();
		}
		else if (key == VK_ESCAPE) {
			// 与 ESC 一贯的"退一步"一致：先撤掉这一步正在做的事（选区），再退整个模式
			if (hasSelection()) {
				selAnchor = selCur = 0;
				refresh();
			}
			else {
				setTextSelect(false);
			}
		}
		return;
	}
	if (isLocked) return;
	// 编辑文本时所有按键都归 TextBox：否则 Ctrl+C 复制的是截图、回车会保存并关窗、
	// Delete 删掉的是整个 shape、ESC 直接把窗口关了。ESC 结束编辑由 TextBox 自己处理。
	// 这一句必须排在派发之前 —— 序号拿到 F2 会去开它自己那份描述编辑框，而共用的那个
	// TextBox 上还挂着当前这一个的订阅，两个编辑叠在一起就会把正在写的文字冲掉
	if (editingShape) return;
	// 这里原来有一条"按下 Ctrl 就切到「选择器-选择对象」"。那套模式已经撤掉：对象选择
	// 现在是常驻的（见 selecting），Ctrl 的语义变成了"在选中批次上追加 / 剔除"以及
	// "空白处拖动 = 框选"（见 onDown），不再需要按一下去切模式
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
		// 底图的一级撤销优先：搬画面 / 删除底图内容比标注的手笔更"重"，
		// 刚剪完一刀就按 Ctrl+Z，要退的显然是那一刀
		if (!restoreCanvasUndo()) drawing->history->undo();
	}
	else if (ctrl && key == 'Y') {
		drawing->history->redo();
	}
	else if (ctrl && key == 'C') {
		// 三条路，从"最贴近当前动作"往下排：画布选区 → 选中的标注 → 整张图。
		// 前两条都不关窗，只有最后那条（老语义）才复制整图并关窗
		if (canvasSelecting() && hasSel()) copySelectionToClipboard();
		else if (hasSelectedShapes()) copySelectedShapes(false);
		else copyToClipboard();
	}
	else if (ctrl && key == 'X') {
		// 有选中的标注才是"剪切对象"；一个都没选中时什么都不做（整张图没有"剪切"这回事）
		if (hasSelectedShapes()) copySelectedShapes(true);
	}
	else if (ctrl && key == 'V') {
		pasteShapes();
	}
	else if (ctrl && key == 'S') {
		saveToFile();
	}
	else if (key == VK_RETURN) {
		copyToClipboard();
	}
	else if (key == VK_DELETE) {
		// 「选择画布」下的 Delete 是"删掉选区里那块画面"，不是删标注 —— 排在标注之前
		if (canvasSelecting() && hasSel()) {
			deleteSelection();
		}
		// 框选出来的那一批优先：一次删掉整批。走 undoShapes 而不是逐个 removeShape ——
		// 那是真 erase，框错一次就没得救；undoShapes 只打撤销标记，与「清除全部水印」
		// 同一条路，Ctrl+Y 能整批找回来。整批删除的误伤面比单个大得多，值得留一条退路
		//
		// 传的是副本：undoShapes 会把命中的元素从 multiSelected 里摘掉
		//（见 Canvas::dropFromMultiSelect），把那个 vector 本身递进去就是边遍历边改它
		else if (!drawing->multiSelected.empty()) {
			auto batch = drawing->multiSelected;
			drawing->multiSelected.clear();
			drawing->history->undoShapes(batch);
		}
		else drawing->history->removeActiveShape();
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
	// ESC 退一步：先把当前操作收掉（放掉画笔、收起 ToolSub，回到工具条的初始样子），
	// 已经画在图上的一概不动；再按一次才关窗。
	// 剪裁 / 滚动那几种中途态在 WinCap::onKey 里已经各有各的"退一步"，这里只管贴图窗口这一层
	else if (key == VK_ESCAPE) {
		if (stepBack()) return;
		close();
	}
}

// Ctrl 抬起来：这一轮 Ctrl 如果从头到尾没被用掉（没点鼠标、没滚轮、没按别的键），
// 就把它当成"再点一下 Ctrl = 取消框选"。
//
// 为什么收在**抬起**而不是按下：按住 Ctrl 还要能去点元素做"追加选中"（见 onDown 里那条
// Ctrl+单击）。按下就收的话，用户按住 Ctrl 去点第二个元素时，批里原来那几个已经被清空了，
// 追加就永远只能加到刚点的那一个上。把"这一轮 Ctrl 用过没有"分开记，两种用法就不打架：
//   · 空点一下 Ctrl（抬手时标记还立着）        → 收掉框选那一批
//   · Ctrl 按住 + 点元素 / 拖框选 / 滚轮       → onDown / onWheel 里已把标记撤掉，抬手不收
// 空白处单击本来就能收（onDown 里那句 multiSelected.clear），两条路互不依赖
//
// ⚠️ 抬手这一路还有个坑：Ctrl 按住时的**自动重复键**会一直来，只靠 onDown 清标记挡不住
//（重复键在 onDown 之后又把标记立起来 → 放开 Ctrl 就把刚框中的一批收掉了）。
// 所以 onKey 那边用 ctrlUsedAsModifier 一直压着，这里收尾时连同它一起复位
void WinPin::onKeyRelease(UINT key)
{
	if (key != VK_CONTROL) return;
	const bool tapped = ctrlTapArmed;
	ctrlTapArmed = false;
	ctrlUsedAsModifier = false;      // 这一轮 Ctrl 结束了，下一次按下重新算
	if (!tapped) return;
	// 选文 / 锁定 / 编辑文本时 Ctrl 有别的用途（Ctrl+A 全选、Ctrl+C 复制选中的字），
	// 顺手把框选收掉会让那些操作变得莫名其妙
	if (textSelect || isLocked || editingShape) return;
	if (drawing->multiSelected.empty()) return;
	drawing->multiSelected.clear();
	clearBatchState();
	refresh();
}

// ---- 常驻剪裁 ----
// 底图四周那 8 个采样点。拖它就是改"这张贴图保留原图的哪一块"：往里收缩、整体平移、
// 往外扩大（有原图可扩的话）；已经画好的标注一个都不搬 —— 坐标映射整体挪过去
//（Canvas::imgOrigin），于是拖完还能接着改样式、撤销、导出，裁掉的只是"图"那部分。
// 这一条是作者点名的语义：剪裁不该把标注烘死。
//
// 为什么要单独留一张源图：缩掉的那块不能白丢 —— 往回收的时候得从原图里把它取回来。
// 截图那条路进来的是整屏原图；别的路没有更大的原图，ensureCropSource 拿当前底图顶替自己
bool WinPin::canCrop() const
{
	return !isThumb && !isMinimized && !isHidden && !isLocked && !hasAnim() && drawing->screenImg != nullptr;
}

void WinPin::cropHandleCenters(D2D1_POINT_2F (&centers)[8]) const
{
	// 量的是**窗口**矩形 (0,0)-(w,h)，不是"标注坐标系里那张图"：采样点压在边框上，
	// 所以任何倍数、任何剪裁状态下它都贴着窗口的边
	const float cx{ w / 2.f }, cy{ h / 2.f };
	centers[0] = D2D1::Point2F(0.f, 0.f);   // 左上
	centers[1] = D2D1::Point2F(cx, 0.f);    // 上
	centers[2] = D2D1::Point2F(w, 0.f);     // 右上
	centers[3] = D2D1::Point2F(w, cy);      // 右
	centers[4] = D2D1::Point2F(w, h);       // 右下
	centers[5] = D2D1::Point2F(cx, h);      // 下
	centers[6] = D2D1::Point2F(0.f, h);     // 左下
	centers[7] = D2D1::Point2F(0.f, cy);    // 左
}

void WinPin::paintCropHandles(ID2D1DeviceContext* ctx)
{
	if (!canCrop() || !borderBrush) return;
	D2D1_POINT_2F centers[8];
	cropHandleCenters(centers);
	// 先垫一层白圆再描蓝边，与 shape 那几枚动作按钮同一套画法：底图什么颜色都有可能，
	// 纯蓝实心点压在深色画面上几乎看不见
	const auto r = kCropHandleR * dpi;
	for (auto& c : centers) {
		ctx->FillEllipse(D2D1::Ellipse(c, r, r), brushTipText.Get());
		ctx->DrawEllipse(D2D1::Ellipse(c, r, r), borderBrush.Get(), dpi);
	}
}

// 命中的是第几个采样点，没命中返回 -1。命中框比画出来的圆大一圈（那么小的点差几个像素
// 就够不着），而且取最近的那一个 —— 图小的时候几个采样点会挨在一起，按顺序取第一个
// 会让后一个永远点不着
int WinPin::cropHandleAt(const POINT pos) const
{
	if (!canCrop()) return -1;
	D2D1_POINT_2F centers[8];
	cropHandleCenters(centers);
	const auto r = kCropHandleHitR * dpi;
	int hit{ -1 };
	float best{ 0.f };
	for (int i = 0; i < 8; ++i) {
		const auto dx = centers[i].x - (float)pos.x, dy = centers[i].y - (float)pos.y;
		const auto d2 = dx * dx + dy * dy;
		if (d2 > r * r) continue;
		if (hit < 0 || d2 < best) { hit = i; best = d2; }
	}
	return hit;
}

void WinPin::startCropDrag(const int handle)
{
	ensureCropSource();
	if (!srcImg) return;
	cropHandle = handle;
	// 源图左上角此刻在屏幕的哪儿：由窗口位置和"显示的是哪一块"反算。窗口可以被拖走、
	// 被 Ctrl+滚轮缩放过，这个值存不住，只在一次拖拽期间有效 —— 拖左边 / 上边时窗口
	// 自己就在挪，客户区坐标一路在变，只有这个屏幕坐标是稳的
	const auto vs = viewScale();
	dragSrcPos.x = x - (int)std::lround((float)cropRect.left * vs);
	dragSrcPos.y = y - (int)std::lround((float)cropRect.top * vs);
	// 与拖窗口、画笔那两条路一样：占住 capture，鼠标划出窗口也收得到移动与抬手
	isMouseDown = true;
	SetCapture(hwnd);
}

void WinPin::dragCropTo(const POINT& screenPos)
{
	if (!cropDragging() || !srcImg) return;
	const auto srcSize = srcImg->GetPixelSize();
	const auto vs = viewScale();
	// 光标落在源图的哪个像素。用拖之前定下的源图屏幕位置算**绝对**位置，不做累加 ——
	// 拖左边 / 上边时窗口自己一挪，累加就把同一段位移记两遍，越拖越漂
	const int px = (int)std::lround((screenPos.x - dragSrcPos.x) / vs);
	const int py = (int)std::lround((screenPos.y - dragSrcPos.y) / vs);
	int l{ (int)cropRect.left }, t{ (int)cropRect.top };
	int r{ (int)cropRect.right }, b{ (int)cropRect.bottom };
	// 每个采样点只管它那两条边：角上是横竖各一条，边中点只有一条，对面那条一概不动
	switch (cropHandle) {
	case 0: l = px; t = py; break;
	case 1: t = py; break;
	case 2: r = px; t = py; break;
	case 3: r = px; break;
	case 4: r = px; b = py; break;
	case 5: b = py; break;
	case 6: l = px; b = py; break;
	case 7: l = px; break;
	default: return;
	}
	// 先夹进源图范围，再兜一个最小边长：缩成 0 宽 / 0 高就取不出位图了。
	// 兜的时候动的是正在拖的那条边，对面那条守在原地
	l = std::clamp(l, 0, (int)srcSize.width);
	r = std::clamp(r, 0, (int)srcSize.width);
	t = std::clamp(t, 0, (int)srcSize.height);
	b = std::clamp(b, 0, (int)srcSize.height);
	constexpr int minSide{ 8 };
	if (r - l < minSide) {
		if (cropHandle == 0 || cropHandle == 6 || cropHandle == 7) l = std::max(0, r - minSide);
		else r = std::min((int)srcSize.width, l + minSide);
	}
	if (b - t < minSide) {
		if (cropHandle == 0 || cropHandle == 1 || cropHandle == 2) t = std::max(0, b - minSide);
		else b = std::min((int)srcSize.height, t + minSide);
	}
	if (r - l < 1 || b - t < 1) return;
	if (l == (int)cropRect.left && t == (int)cropRect.top
		&& r == (int)cropRect.right && b == (int)cropRect.bottom) return;
	cropRect = D2D1::RectU((UINT32)l, (UINT32)t, (UINT32)r, (UINT32)b);
	applyCropRect();
}

void WinPin::endCropDrag()
{
	if (!cropDragging()) return;
	cropHandle = -1;
	isMouseDown = false;
	// 与 onUp 里那两条路一致：capture 还挂在手上，得放掉
	ReleaseCapture();
	// 一刀落定，这里才重认一遍文字：拖动中每帧重认一遍等于每帧一次整图回读 + 一个识别线程，
	// 拖起来必然是卡的（见 applyCropRect 里的说明）。认的时机放在这儿，偏移已经由最后那次
	// applyCropRect 对好了，词框照样落在字上
	clearOcr();
	startOcr();
	layoutTools();
	refresh();
}

// 按 cropRect 从源图重切一块当底图，并把窗口摆到"留下来的那块在屏幕上不动"的位置。
// 标注坐标一个都不动：只把 imgOrigin 对到新的左上角，图和标注就重新对齐了
void WinPin::applyCropRect()
{
	if (!srcImg) return;
	const int nw = (int)(cropRect.right - cropRect.left);
	const int nh = (int)(cropRect.bottom - cropRect.top);
	if (nw <= 0 || nh <= 0) return;
	D2D1_BITMAP_PROPERTIES1 props{};
	// 像素格式与 DPI 必须跟源位图一模一样，差一点 CopyFromBitmap 就吃 E_INVALIDARG。
	// 截图那条路进来的是 ALPHA_MODE_IGNORE（抓屏数据没有 alpha），长图那条是 PREMULTIPLIED，
	// 不能写死一个，照抄源位图的
	props.pixelFormat = srcImg->GetPixelFormat();
	srcImg->GetDpi(&props.dpiX, &props.dpiY);
	props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
	ComPtr<ID2D1Bitmap1> bmp;
	if (FAILED(Ling::D2D::get()->deviceContext->CreateBitmap(D2D1::SizeU((UINT32)nw, (UINT32)nh),
		nullptr, 0, &props, bmp.GetAddressOf()))) return;
	// 只在 GPU 内部搬一块，不走 CPU。srcRect 要的是指针，所以把 cropRect 直接传进去
	if (FAILED(bmp->CopyFromBitmap(nullptr, srcImg.Get(), &cropRect))) return;
	drawing->screenImg = bmp;
	// "底图左上角落在标注坐标系的哪儿" = 这一块的左上角 - 标注原点对应的那个源图像素。
	// shape 的坐标一个都不搬，靠这个偏移让图和标注重新对齐
	drawing->imgOrigin = POINT{ (LONG)cropRect.left - cropBase.x, (LONG)cropRect.top - cropBase.y };
	// 文字识别不在这儿重跑：本函数在拖采样点的每个鼠标事件上都会被调用，而重认一遍 = 一次
	// 整幅底图的 GPU→CPU 回读（readBasePixels）外加新开一个识别线程。前者把 UI 线程按住十几
	// 到几十毫秒，后者在拖动期间能堆出上百个线程一起跑推理 —— 拖起来就是"很卡、一跳一跳"。
	// 识别的结果本来就锚在标注坐标系上（与 shape 同一套），剪裁期间一个词框都不会跑偏，
	// 所以整段拖拽认它原来的那批词就够了，等松手再由 endCropDrag 重认一次
	// 窗口跟着走：留下来的那块在屏幕上停在原处，用户看到的是"框住的那块变成了整张图"。
	// 不跟的话整张图会突然缩到原来窗口的左上角去，拖了半天不知道东西跑哪了
	const auto vs = viewScale();
	const auto newX = dragSrcPos.x + (int)std::lround((float)cropRect.left * vs);
	const auto newY = dragSrcPos.y + (int)std::lround((float)cropRect.top * vs);
	const auto newW = std::max(1, (int)std::lround((float)nw * vs));
	const auto newH = std::max(1, (int)std::lround((float)nh * vs));
	x = newX;
	y = newY;
	w = (float)newW;
	h = (float)newH;
	// 位置和尺寸一次推过去：分两次调，中间那一帧的窗口矩形跟底图对不上，看着就是闪一下。
	// SWP_NOREDRAW 配合末尾那次 refresh() —— 这一帧重画由我们自己负责
	SetWindowPos(hwnd, nullptr, newX, newY, newW, newH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
	// 窗口矩形变了，工具条也得跟着重新定位（它摆在窗口外面，尺寸一变就错位）
	layoutTools();
	refresh();
}

// 没有源图时现补一份：拿当前这张底图顶替自己。剪贴板 / 文件 / 长图 / 历史进来的贴图走这条，
// 于是"往外拖"最多拖回它自己的边界 —— 那些图本来就是从外面拿来的，边界外没有东西可补
void WinPin::ensureCropSource()
{
	if (srcImg) return;
	if (!drawing->screenImg) return;
	srcImg = drawing->screenImg;
	const auto sz = srcImg->GetPixelSize();
	// 底图自己就是源图，所以它的像素坐标与标注坐标只差一个 imgOrigin（见 Canvas.h）：
	// 标注坐标 (0,0) 落在源图的 imgOrigin 那个像素上
	cropBase = drawing->imgOrigin;
	cropRect = D2D1::RectU(0, 0, sz.width, sz.height);
}

// ESC 的"退一步"。顺序是"先收手、再放掉东西"：拿着画笔时按 ESC，用户要的是"不画了"，
// 一次就该收起整套工具面板（连续标号那种批量操作尤其如此）；没拿画笔、只是点选着某个
// 元素时才轮到放掉选中。返回是否消费掉了这一次 ESC，false 表示已经退无可退，可以关窗了
bool WinPin::stepBack()
{
	// 「选择画布」的选区排在最前：它是最靠外的一层"当前正在做的事"
	if (canvasSelecting() && hasSel()) {
		// 正搬着画面的时候另说：那一刻框的坐标就是"这块画面现在在哪儿"，直接清成空框的话
		// 紧接着的抬手会拿这个空框去落图（dropSelection），画面就糊到左上角去了。
		// 这里的"退一步"是"不搬了"——把块放回出发的位置，再把选区收掉
		if (selDrag == 2 && selFloat) {
			selRect = D2D1::RectF(selBaseLT.x, selBaseLT.y,
				selBaseLT.x + (float)selBlockW, selBaseLT.y + (float)selBlockH);
			dropSelection();
		}
		// 拉框 / 改大小那些中途态本来就没动过画面，清掉就行。selDrag 一并归零：
		// 抬手那一下不该再按老状态走一遍
		selDrag = 0;
		selHandle = -1;
		selRect = D2D1::RectF(0.f, 0.f, 0.f, 0.f);
		refresh();
		return true;
	}
	// 「拖拽旋转」态排在框选那一批之前：整批还选着，用户要退的是"别转了这个模式"，
	// 而不是把选中一起放掉 —— 退完还能接着拖位置 / 改样式
	if (batchRotateOn) {
		clearBatchState();
		refresh();
		return true;
	}
	// 框选那一批排在最前：它比"收画笔"更近一层 —— 用户刚框出来的是那些元素，
	// 想退掉的第一件事就是"别选它们了"，而不是把整个「选择对象」工具也放掉
	if (drawing && !drawing->multiSelected.empty()) {
		drawing->multiSelected.clear();
		clearBatchState();
		refresh();
		return true;
	}
	if (toolMain && !toolMain->curId.empty()) {
		// 鼠标还停在图上时那个"将要落下的号"也得一起收掉，否则光标不动就白退一步
		hideNumberPreview();
		// cancelSelect 里顺带清了选中态、收了 ToolSub、重排了整组
		toolMain->cancelSelect();
		return true;
	}
	if (drawing && drawing->selected) {
		drawing->selected = nullptr;
		drawing->shapeHover = nullptr;
		refresh();
		return true;
	}
	return false;
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
	// 整张图换掉了，标注坐标系与底图之间那个剪裁偏移也就没意义了
	drawing->imgOrigin = POINT{ 0, 0 };
	// 上一张图的源图与"显示的是哪一块"一并作废：现在这张底图自己就是新的源图，
	// 由 ensureCropSource 在用户真去拖采样点时补上（多建一张大位图是不白花的开销）
	srcImg.Reset();
	cropBase = POINT{ 0, 0 };
	// 换了底图，认出来的那些词一个都对不上了，重认一遍
	clearOcr();
	startOcr();
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

// ---- 对象剪贴板（见 WinPin.h 的说明）----
bool WinPin::hasSelectedShapes() const
{
	return drawing && (!drawing->multiSelected.empty() || drawing->selected);
}

void WinPin::copySelectedShapes(bool cut)
{
	// 框选那一批优先，其次单选 —— 与 Delete 的取舍一致（见 onKey 的 VK_DELETE）
	std::vector<ShapeBase*> src;
	if (!drawing->multiSelected.empty()) src = drawing->multiSelected;
	else if (drawing->selected) src.push_back(drawing->selected);
	if (src.empty()) return;
	shapeClipboard.clear();
	for (auto* s : src) {
		if (auto c = s->clone(0.f, 0.f)) shapeClipboard.push_back(std::move(c));
	}
	clipPasteCount = 0;
	// 剪切：剪贴板拿到之后才动原件。走 undoShapes（只打撤销标记、不真删）——
	// 与框选删除同一条路，误剪一次还能 Ctrl+Y 找回来
	if (cut) {
		if (!drawing->multiSelected.empty()) {
			auto batch = drawing->multiSelected;
			drawing->multiSelected.clear();
			drawing->history->undoShapes(batch);
		}
		else {
			drawing->history->removeActiveShape();
		}
	}
	refresh();
}

void WinPin::pasteShapes()
{
	// 内部剪贴板优先；它是空的才落到系统剪贴板。这个顺序是有意的：
	// 用户刚复制了图上的一个元素，接着按 Ctrl+V，要粘的显然是那个元素，
	// 而不是更早之前从浏览器里复制的一张图
	if (shapeClipboard.empty()) {
		pasteFromSystemClipboard();
		return;
	}
	clipPasteCount++;
	// 落点逐次错开
	const float off{ 10.f * dpi * clipPasteCount };
	ShapeBase* last{ nullptr };
	for (auto& s : shapeClipboard) {
		// 传 drawing.get() 当目标画布：原件可能来自别的贴图窗口（甚至那个窗口已经关了），
		// 换了宿主才能安全地跑善后（马赛克那几支要按宿主回读画面，见 ShapeBase::clone）
		if (auto c = s->clone(off, off, drawing.get())) {
			last = drawing->history->addShape(std::move(c));
		}
	}
	// 粘贴出来的这一批取代原来的选中：addShape 每收一份就把 selected 指过去，
	// 循环结束时正好停在最后一份上。框选那一批同时清掉，否则 Delete 会连旧的整批一起删
	drawing->multiSelected.clear();
	if (last) drawing->selected = last;
	refresh();
}

void WinPin::pasteFromSystemClipboard()
{
	std::vector<BYTE> img;
	int w{ 0 }, h{ 0 };
	std::wstring text;
	const auto kind = Util::readClipboard(img, w, h, text);
	if (kind == Util::ClipContent::None) return;
	// 落点取鼠标。用户多半刚从别的程序里复制完、把光标移回图上再按的 Ctrl+V，
	// 那个位置就是他想放的地方。光标在窗口外时 toImgPos 会给出画布外的坐标，下面各自夹回来
	POINT pos{};
	if (!GetCursorPos(&pos)) return;
	ScreenToClient(hwnd, &pos);
	const auto at = toImgPos(pos);
	const auto imgSize = getImgSize();
	if (kind == Util::ClipContent::Image) {
		auto shape = std::make_unique<ShapeImage>(drawing.get());
		if (!shape->setImage(img, w, h)) return;
		// 图比画布还大就等比缩到画布之内（留一成边）：不缩的话八枚手柄全落在画布外，
		// 画布外的区域不响应鼠标，用户根本够不着它们去改大小
		float sc{ 1.f };
		if (w > (int)imgSize.width || h > (int)imgSize.height) {
			sc = std::min((float)imgSize.width / w, (float)imgSize.height / h) * 0.9f;
		}
		const float dw{ (float)w * sc }, dh{ (float)h * sc };
		// 图片中心对齐鼠标，再整体夹进画布 —— 贴边时不至于只露出一个角
		const auto left = std::clamp((float)at.x - dw / 2.f, 0.f, std::max(0.f, (float)imgSize.width - dw));
		const auto top = std::clamp((float)at.y - dh / 2.f, 0.f, std::max(0.f, (float)imgSize.height - dh));
		shape->placeAt(left, top, dw, dh);
		drawing->history->addShape(std::move(shape));
	}
	else {
		// 复制了一个空字符串（表格里的空单元格之类）时不留一个看不见的空文本框
		if (text.empty()) return;
		auto shape = std::make_unique<ShapeText>(drawing.get());
		shape->setTextAt(text, (float)at.x, (float)at.y);
		// 收下之后直接进编辑态：用户按 Ctrl+V 就是为了接着改这段字，
		// 再让他手工点一下文本组件才进得去编辑，等于白粘一次（作者提的）
		if (auto* added = drawing->history->addShape(std::move(shape))) {
			static_cast<ShapeText*>(added)->startEdit();
		}
	}
	// 与 pasteShapes 同一套收尾：新粘出来的取代原来的选中（addShape 已经指过去了），
	// 框选那一批清掉 —— 否则 Delete 会连旧的整批一起删
	drawing->multiSelected.clear();
	refresh();
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
	// 同 layout()：标注坐标系要比底图多一个 imgOrigin 的平移，导出的图才和屏幕上看到的一致
	const auto& o = drawing->imgOrigin;
	ctx->SetTransform(D2D1::Matrix3x2F::Translation(-(float)o.x, -(float)o.y));
	ctx->BeginDraw();
	ctx->Clear(D2D1::ColorF(0, 0.0f));
	ctx->DrawBitmap(drawing->screenImg.Get(),
		D2D1::RectF((float)o.x, (float)o.y, (float)o.x + (float)imgSize.width, (float)o.y + (float)imgSize.height));
	for (auto& shape : drawing->history->shapes)
	{
		if (!shape->isUndo) {
			shape->paint(ctx);
		}
	}
	hr = ctx->EndDraw();
	// 变换收回单位阵再解绑：这个 deviceContext 是全程共用的，留着上面那个平移会带到别处的绘制上
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
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
	// 「选择画布」：压在采样点上给对应的双向箭头，压在选区里给"可拖动"的四向箭头。
	// 摆在最前 —— 这个子模式下选区的采样点才是主角，别被剪裁采样点那套抢先
	if (canvasSelecting()) {
		POINT pos{};
		GetCursorPos(&pos);
		ScreenToClient(hwnd, &pos);
		auto imgPos = toImgPos(pos);
		switch (selDrag == 3 ? selHandle : selHandleAt(imgPos)) {
		case 0: case 4: SetCursor(LoadCursor(nullptr, IDC_SIZENWSE)); return TRUE;
		case 2: case 6: SetCursor(LoadCursor(nullptr, IDC_SIZENESW)); return TRUE;
		case 1: case 5: SetCursor(LoadCursor(nullptr, IDC_SIZENS)); return TRUE;
		case 3: case 7: SetCursor(LoadCursor(nullptr, IDC_SIZEWE)); return TRUE;
		default: break;
		}
		bool inSel{ hasSel()
			&& (float)imgPos.x > selRect.left && (float)imgPos.x < selRect.right
			&& (float)imgPos.y > selRect.top && (float)imgPos.y < selRect.bottom };
		SetCursor(LoadCursor(nullptr, (selDrag == 2 || inSel) ? IDC_SIZEALL : IDC_CROSS));
		return TRUE;
	}
	// 多选那一批的批量 UI：压在三枚按钮上给手型，处在拖拽旋转态里给十字。
	// 排在剪裁采样点之前 —— 按钮压在采样点内侧，两者一般不会重叠，但按钮是更靠上的那层
	if (!drawing->multiSelected.empty()) {
		POINT pos{};
		GetCursorPos(&pos);
		ScreenToClient(hwnd, &pos);
		if (batchBtnAt(pos) >= 0) {
			SetCursor(LoadCursor(nullptr, IDC_HAND));
			return TRUE;
		}
		if (batchRotateOn) {
			SetCursor(LoadCursor(nullptr, IDC_CROSS));
			return TRUE;
		}
	}
	// 剪裁采样点：光标压在哪个点上就给对应的双向箭头 —— 角上是斜的，边中是直的。
	// 与长截图那边的剪裁一套手势。正在拖的那个点要单算：光标早跑出那个小圆了，
	// 但拖动全程都得保持同一个箭头形状
	{
		POINT pos{};
		GetCursorPos(&pos);
		ScreenToClient(hwnd, &pos);
		switch (cropDragging() ? cropHandle : cropHandleAt(pos))
		{
		case 0: case 4:   // 左上 / 右下
			SetCursor(LoadCursor(nullptr, IDC_SIZENWSE));
			return TRUE;
		case 2: case 6:   // 右上 / 左下
			SetCursor(LoadCursor(nullptr, IDC_SIZENESW));
			return TRUE;
		case 1: case 5:   // 上 / 下
			SetCursor(LoadCursor(nullptr, IDC_SIZENS));
			return TRUE;
		case 3: case 7:   // 右 / 左
			SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
			return TRUE;
		default:
			break;
		}
	}
	// 编辑文本时光标形状交给 TextBox 决定（文本区 I 形、滚动条箭头）。
	// 本函数覆写了基类且不调用它，TextBox 挂在 onCursor 上的那个订阅不会自己被触发，得手动发一次。
	if (editingShape) {
		bool handled{ false };
		onCursor(&handled);
		if (handled) return TRUE;
	}
	// 选文态：整张图都是"文字区"，光标给 I 形。放在 hasDrawTool 之前 —— 那种状态下
	// curId 是空的，不先拦一句就会被下面的"画不了 → 拖窗口"判成四向箭头
	if (textSelect) {
		SetCursor(LoadCursor(nullptr, IDC_IBEAM));
		return TRUE;
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
