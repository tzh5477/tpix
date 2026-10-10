#include "pch.h"
#include <algorithm>
#include "../Win/WinPin.h"
#include "PinHoverPreview.h"
#include "PinHiddenBar.h"

PinHoverPreview::PinHoverPreview() : Ling::WinBase()
{
	// 先给个上限尺寸：createNativeWindow 拿 0 宽高建窗会连 WM_MOUSEMOVE 都收不到，
	// 真正的尺寸由每次 showPreview 按底图像素算
	setSize(maxW, maxH);
	// 与 ToolPinSide 同一套：TOPMOST（贴图也是顶层，不跟上的话全屏应用一起就被压掉）、
	// NOACTIVATE（这条绝对不能把用户手上的焦点抢走）、TOOLWINDOW（不在任务栏留一枚）
	createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP);
}

PinHoverPreview::~PinHoverPreview()
{
}

void PinHoverPreview::onCreated()
{
	body->setBg(0xFFFFFFFF);
	body->setBorder(1.f, 0xA8A8A8ff);
	surface = body->makeChild<Ling::Canvas>();
	// 走 swap chain 后端，与 WinPin 自己那块画布同一套（都是"窗口建好就可能换尺寸、
	// 画一次要整帧换出"的画法）。默认的 CompositionDrawingSurface 是单缓冲，
	// 尺寸一变内容就被清掉，这里等不起那个坑
	surface->enableSwapChain();
	surface->setSizePercent(100.f, 100.f);
	// 画布比窗口小的唯一可能就是被 flex 压了 —— 这里要的是它就等于窗口，别让它收缩
	surface->setFlexShrink(0.f);
	// 这里不 show：没有 hover 的时候不该在屏幕角上多一枚空窗
	// 单击浮层 = 把那张贴图放回桌面（与书签条上单击同义）。浮层压在书签条旁边、中间隔着 4px 缝，
	// 光标从条挪到浮层必经那条缝、书的离开事件会把它藏掉 —— 所以浮层自己得能接住这次点击，
	// 否则"点浮层恢复"永远点不中（见 PinHiddenBar 的存活期管理）
	onMouseDown.add([this](POINT, bool isRight) {
		if (!cur) return;
		if (!isRight && owner) { owner->restoreFromPreview(cur); return; }
		// 右键只是把浮层收掉，不弹菜单（菜单由书签条自己管）
		if (isRight) hidePreview();
	});
	// 光标离开浮层（挪回条或到桌面）= 收掉。收回本扇窗由书签条的复核定时器统一判，
	// 这里只负责"从浮层本身离开"这一半
	onMouseMove.add([this](POINT pos) {
		if (pos.x == INT_MAX) hidePreview();
	});
}

void PinHoverPreview::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}

void PinHoverPreview::showPreview(WinPin* pin, POINT pos, float dpiVal)
{
	if (!pin) {
		hidePreview();
		return;
	}
	auto bmp = pin->getScreenImg();
	if (!bmp) {
		hidePreview();
		return;
	}
	auto sz = bmp->GetPixelSize();
	if (sz.width == 0 || sz.height == 0) {
		hidePreview();
		return;
	}
	// 要的是像素数：GetSize 返回的是按位图自身 dpi 折算过的 DIP，拿它算倍数会算歪
	float s = std::min(maxW / (float)sz.width, maxH / (float)sz.height);
	// 小图不放太：比上限还矮小就按原始像素显示，放上去只是把像素拉糊
	s = std::min(s, 1.f);
	dpi = dpiVal;
	cur = pin;
	setSize(sz.width * s, sz.height * s);
	// 钳进工作区：落点是从屏幕角上那条书签算过来的，贴着角的几格最容易算到屏外
	RECT wa{};
	SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
	const auto wPx = (int)std::lround(sz.width * s * dpiVal);
	const auto hPx = (int)std::lround(sz.height * s * dpiVal);
	// POINT 的 x/y 是 LONG，与左边的 int 边界凑不到一起会让 std::clamp 推不出模板参数
	const int x = std::clamp((int)pos.x, (int)wa.left, std::max((int)wa.left, (int)wa.right - wPx));
	const int y = std::clamp((int)pos.y, (int)wa.top, std::max((int)wa.top, (int)wa.bottom - hPx));
	setPosition(x, y);
	// 先把窗口建好尺寸、再 show，最后才 layout 补画 —— 画（swap chain 的 BeginDraw + Present）
	// 必须发生在窗口**可见**之后。这与 WinPin / TextBox 那些从不出空白的 swap chain 画布是同一套
	// 时序：藏在隐藏态画完再 show 偶尔会丢那一次 present（DWM 还没消费），浮层就整片白。
	// 换图换尺寸时隐藏态下的 ResizeBuffers 也偶发失败（DWM 仍持着上一次 present 的缓冲），
	// 同样表现为白板 —— 见 Ling::Canvas::ensureSwapChain 的补偿。
	show();
	layout();
}

void PinHoverPreview::layout()
{
	Ling::WinBase::layout();
	paintPreview();
}

void PinHoverPreview::paintPreview()
{
	if (!cur || !surface) return;
	auto bmp = cur->getScreenImg();
	if (!bmp) return;
	auto ctx = surface->startPaint();
	if (!ctx) return;
	const auto sz = bmp->GetSize();
	const float cw = surface->w, ch = surface->h;
	if (cw <= 0.f || ch <= 0.f) {
		surface->finishPaint();
		return;
	}
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	ctx->Clear(D2D1::ColorF(D2D1::ColorF::White));
	// 等比、**居中**：取小的一边当倍数，把整张（含标注）摆到画布正中。底图与标注的合成、
	// imgOrigin 偏移、缩放对齐全交给 WinPin::drawAnnotationComposite —— 与正式窗口同一套画法，
	// 浮层里就不会"有图没标注"或"标注偏一截"
	float s = std::min(cw / sz.width, ch / sz.height);
	s = std::min(s, 1.f);
	const float dw = sz.width * s, dh = sz.height * s;
	const float offX = (cw - dw) / 2.f, offY = (ch - dh) / 2.f;
	cur->drawAnnotationComposite(ctx, s, offX, offY);
	surface->finishPaint();
}

void PinHoverPreview::hidePreview()
{
	cur = nullptr;
	if (hwnd) hide();
}
