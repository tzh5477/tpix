#include "pch.h"
#include <algorithm>
#include <format>
#include <optional>
#include "WinOverlay.h"
#include "../App.h"
#include "../Lang.h"
using namespace Microsoft::WRL;

namespace {
	std::unique_ptr<WinOverlay> winOverlay;
	// 换一种辅助层时要先等旧窗口真销毁完（hwnd 收不到消息了）再开新的，
	// 否则旧的那个延迟 reset 任务会把刚建的新窗口一起放掉
	std::optional<OverlayMode> pendingMode;
	// 光标位置读不到鼠标事件，只能定时去问；25fps 足够跟手又不至于一直重画整屏
	constexpr UINT cursorTimerId{ 100 };
	constexpr UINT cursorTimerMs{ 40 };
}

WinOverlay::WinOverlay(OverlayMode mode) : Ling::WinBase(), mode(mode)
{
	auto [sx, sy, sw, sh] = App::get()->getScreenArea();
	// 直接写物理像素：x/y/w/h 就是物理的，setter 收的是逻辑像素
	this->x = sx; this->y = sy; this->w = (float)sw; this->h = (float)sh;
	onKeyDown.add([this](UINT key) {
		if (key == VK_ESCAPE) close();
	});
	onTimer.add([this](UINT id) {
		if (id != cursorTimerId) return;
		// 穿透层一点就把焦点让给底下的窗口，之后 WM_KEYDOWN 再也到不了这里 ——
		// 而"开着标尺继续点东西"正是它的主用法，所以 Esc 靠轮询兜底（&1 = 上次查询后按过）
		if (mode != OverlayMode::Focus && (GetAsyncKeyState(VK_ESCAPE) & 1)) {
			close();
			return;
		}
		POINT pt{};
		GetCursorPos(&pt);
		// 标尺 / 十字是鼠标穿透的，拿不到鼠标事件；focus 自己收得到，但顺着一起更新也无害
		if (pt.x - x == cursor.x && pt.y - y == cursor.y) return;
		cursor = { pt.x - x, pt.y - y };
		refresh();
	});
	onDestroy.add([]() {
		// 与 WinCap / WinDelay 同一个规矩：不能在销毁回调里同步 reset 自己
		Ling::App::get()->dq.TryEnqueue([]() {
			winOverlay.reset();
			if (pendingMode) {
				auto next = *pendingMode;
				pendingMode.reset();
				winOverlay.reset(new WinOverlay(next));
			}
		});
	});
	if (mode == OverlayMode::Focus) {
		// 亮区默认摆在屏幕正中，取屏幕的六成，太小看不出"聚焦"的意思
		holeW = (int)(sw * 0.6f);
		holeH = (int)(sh * 0.6f);
		holeX = sx + (sw - holeW) / 2;
		holeY = sy + (sh - holeH) / 2;
		onMouseDown.add([this](POINT pos, bool isRight) {
			if (isRight) { close(); return; }
			// 只有按住亮区里才拖：按在压暗的部分直接关掉，跟"点空白处退出"一个意思
			if (pos.x < holeX - x || pos.x > holeX - x + holeW
				|| pos.y < holeY - y || pos.y > holeY - y + holeH) {
				close();
				return;
			}
			isDragging = true;
			dragFrom = pos;
		});
		onMouseMove.add([this](POINT pos) {
			if (!isDragging) return;
			holeX += pos.x - dragFrom.x;
			holeY += pos.y - dragFrom.y;
			dragFrom = pos;
			clampHole();
			refresh();
		});
		onMouseUp.add([this](POINT, bool) { isDragging = false; });
		onMouseWheel.add([this](POINT, float space) {
			// 一格滚轮 60*dpi，取 6% 的变化量；以亮区中心为锚点缩放
			auto k = 1.f + space / 1000.f;
			auto cx = holeX + holeW / 2.f;
			auto cy = holeY + holeH / 2.f;
			holeW = (int)(holeW * k);
			holeH = (int)(holeH * k);
			holeX = (int)(cx - holeW / 2.f);
			holeY = (int)(cy - holeH / 2.f);
			clampHole();
			refresh();
		});
	}
	// 标尺 / 十字要能隔着它继续操作底下的窗口，所以鼠标穿透；focus 要拖亮区，不能穿透
	auto exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
	if (mode != OverlayMode::Focus) {
		exStyle |= WS_EX_LAYERED | WS_EX_TRANSPARENT;
	}
	createNativeWindow(exStyle, WS_POPUP);
}

WinOverlay::~WinOverlay()
{
}

void WinOverlay::toggle(OverlayMode mode)
{
	if (winOverlay) {
		// 只能走 close()：WinBase 的析构不销毁 hwnd，直接 reset 会留下一个还在收定时器
		// 消息的野窗口，下次消息进来就是 use-after-free
		if (winOverlay->mode == mode) {
			winOverlay->close();
			return;
		}
		pendingMode = mode;
		winOverlay->close();
		return;
	}
	winOverlay.reset(new WinOverlay(mode));
}

void WinOverlay::dispose()
{
	if (winOverlay) winOverlay->close();
}

bool WinOverlay::isOpen(OverlayMode mode)
{
	return winOverlay && winOverlay->mode == mode;
}

void WinOverlay::onCreated()
{
	setTitle(Lang::get(L"overlay.title"));
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushText.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.72f), brushBg.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.5f, 1.f), brushLine.GetAddressOf());
	canvas = body->makeChild<Ling::Canvas>();
	canvas->enableSwapChain();
	canvas->setSizePercent(100.f, 100.f);
	// 与 WinBall / WinDelay 同一套：辅助层绝不能被自己的截图拍进去
	App::excludeFromCapture(hwnd);
	if (mode != OverlayMode::Focus) {
		// 加了 WS_EX_LAYERED 就得给一次 alpha，否则整扇窗可能不显示（WinCap 那边同一套）
		SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
	}
	POINT pt{};
	GetCursorPos(&pt);
	cursor = { pt.x - x, pt.y - y };
	setTimer(cursorTimerMs, cursorTimerId);
	show();
	// show 只是 ShowWindow，不抢焦点的话 Esc 会被底下的窗口吃掉
	SetFocus(hwnd);
}

void WinOverlay::layout()
{
	Ling::WinBase::layout();
	if (!canvas) return;
	auto ctx = canvas->startPaint();
	if (!ctx) return;
	ctx->Clear(0);   // 透明：没画到的地方照常看见底下的桌面
	if (mode == OverlayMode::Ruler) paintRuler(ctx);
	else if (mode == OverlayMode::Crosshair) paintCrosshair(ctx);
	else paintFocus(ctx);
	canvas->finishPaint();
}

void WinOverlay::paintRuler(ID2D1DeviceContext* ctx)
{
	auto band{ 26.f * dpi };
	ctx->FillRectangle(D2D1::RectF(0.f, 0.f, w, band), brushBg.Get());
	ctx->FillRectangle(D2D1::RectF(0.f, 0.f, band, h), brushBg.Get());
	// 每 10px 一小格、50px 一中格、100px 一大格。屏幕像素足够密，再细就糊成一片了
	for (int i = 0; i <= (int)w; i += 10) {
		auto len = (i % 100 == 0) ? band * 0.6f : ((i % 50 == 0) ? band * 0.42f : band * 0.24f);
		ctx->DrawLine(D2D1::Point2F((float)i + 0.5f, band - len), D2D1::Point2F((float)i + 0.5f, band),
			brushText.Get(), 1.f);
		if (i % 100 == 0 && i > 0) {
			drawLabel(ctx, std::to_wstring(i), (float)i + 2.f, 1.f, 10.f * dpi);
		}
	}
	for (int i = 0; i <= (int)h; i += 10) {
		auto len = (i % 100 == 0) ? band * 0.6f : ((i % 50 == 0) ? band * 0.42f : band * 0.24f);
		ctx->DrawLine(D2D1::Point2F(band - len, (float)i + 0.5f), D2D1::Point2F(band, (float)i + 0.5f),
			brushText.Get(), 1.f);
		if (i % 100 == 0 && i > 0) {
			drawLabel(ctx, std::to_wstring(i), 1.f, (float)i + 2.f, 10.f * dpi);
		}
	}
	// 光标落在哪一格：两条标尺上各点一笔
	ctx->DrawLine(D2D1::Point2F((float)cursor.x + 0.5f, 0.f), D2D1::Point2F((float)cursor.x + 0.5f, band),
		brushLine.Get(), 1.f);
	ctx->DrawLine(D2D1::Point2F(0.f, (float)cursor.y + 0.5f), D2D1::Point2F(band, (float)cursor.y + 0.5f),
		brushLine.Get(), 1.f);
}

void WinOverlay::paintCrosshair(ID2D1DeviceContext* ctx)
{
	auto cx{ (float)cursor.x + 0.5f };
	auto cy{ (float)cursor.y + 0.5f };
	ctx->DrawLine(D2D1::Point2F(cx, 0.f), D2D1::Point2F(cx, h), brushLine.Get(), 1.f);
	ctx->DrawLine(D2D1::Point2F(0.f, cy), D2D1::Point2F(w, cy), brushLine.Get(), 1.f);
	drawLabel(ctx, std::format(L"{}, {}", cursor.x, cursor.y), cx + 8.f, cy + 8.f, 12.f * dpi);
}

void WinOverlay::paintFocus(ID2D1DeviceContext* ctx)
{
	D2D1_RECT_F hole = D2D1::RectF((float)(holeX - x), (float)(holeY - y),
		(float)(holeX - x + holeW), (float)(holeY - y + holeH));
	// 压暗的是亮区之外的四块：D2D 没有"挖洞"的画法，用四块矩形拼出来
	ctx->FillRectangle(D2D1::RectF(0.f, 0.f, w, hole.top), brushBg.Get());
	ctx->FillRectangle(D2D1::RectF(0.f, hole.bottom, w, h), brushBg.Get());
	ctx->FillRectangle(D2D1::RectF(0.f, hole.top, hole.left, hole.bottom), brushBg.Get());
	ctx->FillRectangle(D2D1::RectF(hole.right, hole.top, w, hole.bottom), brushBg.Get());
	ctx->DrawRectangle(hole, brushLine.Get(), 1.f);
	// 亮区贴着屏幕上沿时标签会画到窗口外，压回 0
	drawLabel(ctx, std::format(L"{} × {}", holeW, holeH), hole.left,
		std::max(hole.top - 22.f * dpi, 0.f), 12.f * dpi);
}

D2D1_SIZE_F WinOverlay::drawLabel(ID2D1DeviceContext* ctx, const std::wstring& text, float x, float y, float fontSize)
{
	// 坐标 / 尺寸这类每帧都变的文字也在缓存里留一份，条目会无上限地长；
	// 攒到一定数量整表清掉，标尺刻度那几十条下一帧重建一次，代价可以忽略
	if (labelCache.size() > 128) labelCache.clear();
	auto& [cachedSize, layout] = labelCache[text];
	if (!layout || cachedSize != fontSize) {
		cachedSize = fontSize;
		layout = Ling::D2D::get()->makeTextLayout(text, fontSize);
	}
	if (!layout) return {};
	DWRITE_TEXT_METRICS tm{};
	layout->GetMetrics(&tm);
	auto pad{ 3.f * dpi };
	ctx->FillRectangle(D2D1::RectF(x, y, x + tm.width + pad * 2, y + tm.height + pad * 2), brushBg.Get());
	ctx->DrawTextLayout(D2D1::Point2F(x + pad, y + pad), layout.Get(), brushText.Get(),
		D2D1_DRAW_TEXT_OPTIONS_NONE);
	return { tm.width + pad * 2, tm.height + pad * 2 };
}

void WinOverlay::clampHole()
{
	auto minSize = (int)(60.f * dpi);
	if (holeW < minSize) holeW = minSize;
	if (holeH < minSize) holeH = minSize;
	if (holeW > (int)w) holeW = (int)w;
	if (holeH > (int)h) holeH = (int)h;
	holeX = std::clamp(holeX, x, x + (int)w - holeW);
	holeY = std::clamp(holeY, y, y + (int)h - holeH);
}
