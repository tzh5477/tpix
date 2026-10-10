#include "pch.h"
#include "WinVideoEdit.h"

#include <mfapi.h>

#include <algorithm>
#include <chrono>
#include <format>

#include "../Lang.h"
#include "../Setting.h"
#include "../Util.h"

namespace {
	std::unique_ptr<WinVideoEdit> winVideoEdit;

	// 布局常量（逻辑像素）
	constexpr float kHeaderH{ 40.f };
	constexpr float kBarH{ 52.f };
	constexpr float kReadoutH{ 26.f };
	constexpr float kTimelineH{ 58.f };
	constexpr float kTimelineBlock{ kTimelineH + kReadoutH + kBarH };
	constexpr float kSide{ 16.f };

	std::wstring fmtTime(long long ms, bool withCs)
	{
		if (ms < 0) ms = 0;
		const long long cs = (ms % 1000) / 10;
		const long long sec = (ms / 1000) % 60;
		const long long min = ms / 60000;
		if (withCs) return std::format(L"{:02d}:{:02d}.{:02d}", min, sec, cs);
		return std::format(L"{:02d}:{:02d}", min, sec);
	}

	// 一个纯色按钮。底色给了就必须配 hover 底色（Button 的硬规矩：只设 setBg 的话，
	// 鼠标一上去会换成没建过的空画刷 —— 整块变空白）
	Ling::Button* makeBtn(Ling::Node* parent, const std::wstring& text, unsigned int bg,
		unsigned int hoverBg, unsigned int color, bool iconFont = false)
	{
		auto btn = parent->makeChild<Ling::Button>();
		btn->setText(text);
		btn->setFontSize(iconFont ? 16.f : 13.f);
		btn->setBorderRadius(6.f);
		btn->setBg(bg);
		btn->setHoverBg(hoverBg);
		btn->setColor(color);
		btn->setHoverColor(color);
		if (iconFont) btn->setFontFamily(L"icon");
		return btn;
	}
}

// ---------------------------------------------------------------- 预览画布

void WinVideoEdit::PreviewCanvas::clearFrame()
{
	bmp.Reset();
	fw = fh = 0;
	layout();
}

void WinVideoEdit::PreviewCanvas::setFrame(const std::vector<BYTE>& bgra, int w, int h)
{
	if (w <= 0 || h <= 0 || bgra.size() < (size_t)w * h * 4) return;
	auto d2d = Ling::D2D::get();
	if (!d2d || !d2d->deviceContext) return;
	// 位图只在尺寸变了的时候重建，之后逐帧 CopyFromMemory —— 见头文件里的说明
	if (!bmp || fw != w || fh != h) {
		bmp.Reset();
		D2D1_BITMAP_PROPERTIES1 props{};
		// 解码出来的是 BGRX：没有有意义的 alpha，标成 PREMULTIPLIED 会让暗部发灰
		props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
		props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
		props.dpiX = props.dpiY = 96.f;
		if (FAILED(d2d->deviceContext->CreateBitmap(D2D1::SizeU((UINT32)w, (UINT32)h), nullptr, 0,
			props, bmp.ReleaseAndGetAddressOf())) || !bmp) {
			bmp.Reset();
			return;
		}
		fw = w;
		fh = h;
	}
	D2D1_RECT_U dst{ 0, 0, (UINT32)w, (UINT32)h };
	if (FAILED(bmp->CopyFromMemory(&dst, bgra.data(), (UINT32)w * 4))) return;
	layout();   // 立刻画一次；等窗口重排的话播放时会慢半拍
}

void WinVideoEdit::PreviewCanvas::layout()
{
	Ling::Canvas::layout();
	auto ctx = startPaint();
	if (!ctx) return;
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	// 预览底色给深色：窗口那一圈是浅色，sletterbox 的黑边才有"在看视频"的样子
	ctx->Clear(D2D1::ColorF(0x1E1E1E, 1.f));
	const float cw = w, ch = h;
	if (bmp && cw > 1.f && ch > 1.f) {
		const auto sz = bmp->GetPixelSize();
		// contain：整帧都看得见（裁剪点要按画面找，不能像缩略图那样 cover 裁掉两头）
		const float s = std::min(cw / (float)sz.width, ch / (float)sz.height);
		const float dw = sz.width * s, dh = sz.height * s;
		ctx->SetTransform(D2D1::Matrix3x2F::Translation((cw - dw) / 2.f, (ch - dh) / 2.f)
			* D2D1::Matrix3x2F::Scale(s, s));
		ctx->DrawBitmap(bmp.Get(), D2D1::RectF(0.f, 0.f, (float)sz.width, (float)sz.height), 1.f,
			D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
		ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	}
	finishPaint();
}

// ---------------------------------------------------------------- 时间轴

void WinVideoEdit::TimelineCanvas::layout()
{
	Ling::Canvas::layout();
	auto ctx = startPaint();
	if (!ctx) return;
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	ctx->Clear(D2D1::ColorF(0xFAFAFA, 1.f));
	const float cw = w, ch = h;
	if (cw <= 2.f || ch <= 2.f || !owner) { finishPaint(); return; }

	auto d2d = Ling::D2D::get();
	if (!d2d || !d2d->deviceContext) { finishPaint(); return; }
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> br;
	if (FAILED(d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0xE0E0E0), br.ReleaseAndGetAddressOf()))) {
		finishPaint();
		return;
	}
	const auto g = owner->trackGeom();
	const float y = ch / 2.f;
	const float xa = owner->xOfMs(owner->inMs);
	const float xb = owner->xOfMs(owner->outMs);

	// 轨道
	ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(g.x0, y - 3.f, g.x1, y + 3.f), 3.f, 3.f), br.Get());
	// 选中区间（主题色淡底）—— 从起点画到终点，被裁掉的两头一眼能看出来
	br->SetColor(D2D1::ColorF(Setting::getThemeColor(), 0.24f));
	if (xb > xa) {
		ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(xa, y - 11.f, xb, y + 11.f), 3.f, 3.f), br.Get());
	}
	// 两个手柄
	br->SetColor(D2D1::ColorF(Setting::getThemeColor(), 1.f));
	ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(xa - 2.5f, y - 13.f, xa + 2.5f, y + 13.f), 2.5f, 2.5f), br.Get());
	ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(xb - 2.5f, y - 13.f, xb + 2.5f, y + 13.f), 2.5f, 2.5f), br.Get());
	// 播放头：深灰 + 顶上一个小圆点，压在选中区间上
	br->SetColor(D2D1::ColorF(0x333333, 1.f));
	const float xp = owner->xOfMs(owner->playheadMs);
	ctx->FillRectangle(D2D1::RectF(xp - 1.f, y - 17.f, xp + 1.f, y + 17.f), br.Get());
	ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(xp - 6.f, y - 24.f, xp + 6.f, y - 14.f), 3.f, 3.f), br.Get());

	finishPaint();
}

// ---------------------------------------------------------------- 窗口

WinVideoEdit::WinVideoEdit(const std::wstring& srcPath) : Ling::WinBase(), src(srcPath)
{
	// 关窗回调是从按钮点击栈上来的，那里同步 reset 就是 use-after-free（同 WinHistory）。
	// 在这之前先把两条线程收干净：
	//  ① 转码线程要**先置取消位**再让它析构 —— jthread 的析构只会 request_stop，
	//     而转码循环认的是 jobCancel，不置位就得等整段转完才肯退
	//  ② 转码产物删掉；**原始 temp 留着**（用户可能只是误触关闭）。
	//     ⚠️ 唯一例外：产物已经交给剪切板了就不能删 —— Util::addFileToClipboard 只把
	//     **路径**放进 CF_HDROP，不复制文件，删掉等于剪切板里躺着一条死路径
	//     （2026-10-10 实测：Ctrl+C 导出后 trim.mp4 立刻消失）。剪切板那条路
	//     自己会 closeWindow，所以走到这里时 tmpToClipboard 才是准的。
	onDestroy.add([this]() {
		jobCancel = true;
		if (!tmpToClipboard) DeleteFileW(tmpOut.c_str());
		decThread.request_stop();
		if (jobThread.joinable()) jobThread.join();
		if (decThread.joinable()) decThread.join();
		Ling::App::get()->dq.TryEnqueue([]() { winVideoEdit.reset(); });
	});
	setTitle(Lang::get(L"video.edit.title"));
	setSize(900.f, 620.f);
	setCenter();
	createNativeWindow();
}

WinVideoEdit::~WinVideoEdit() = default;

bool WinVideoEdit::isOpen() { return winVideoEdit != nullptr; }

void WinVideoEdit::init(const std::wstring& srcPath)
{
	if (winVideoEdit) {
		SetForegroundWindow(winVideoEdit->hwnd);
		return;
	}
	winVideoEdit.reset(new WinVideoEdit(srcPath));
}

void WinVideoEdit::dispose()
{
	if (winVideoEdit) winVideoEdit->close();
}

void WinVideoEdit::onCreated()
{
	// 源文件信息先同步读出来（只读文件头，几十毫秒）：时长与尺寸要立刻能显示。
	// 放在这里而不是解码线程里，是为了让"打不开"这种硬失败在窗口第一次画出来之前就定性
	std::wstring err;
	loadOk = VideoTrim::probe(src, info, err) && info.durMs > 0;
	if (!loadOk) {
		loadErr = err.empty() ? Lang::get(L"video.edit.badFile") : err;
		buildFailedUi();
		return;
	}
	inMs = 0;
	outMs = info.durMs;
	playheadMs = 0;

	body->setBg(0xFAFAFAFF);

	// 头带
	auto header = body->makeChild<Ling::Node>();
	header->setPositionType(Ling::Position::Absolute);
	header->setPosition(Ling::Edge::Left, 0);
	header->setPosition(Ling::Edge::Top, 0);
	header->setPosition(Ling::Edge::Right, 0);
	header->setHeight(kHeaderH);
	header->setBg(0xFFFFFFFF);
	header->setAlignItems(Ling::Align::Center);
	header->setPaddingLeft(kSide);
	buildHeader(header);

	// 预览
	preview = body->makeChild<PreviewCanvas>();
	preview->setPositionType(Ling::Position::Absolute);
	preview->setPosition(Ling::Edge::Left, 0);
	preview->setPosition(Ling::Edge::Right, 0);
	preview->setPosition(Ling::Edge::Top, kHeaderH);
	preview->setPosition(Ling::Edge::Bottom, kTimelineBlock);

	// 时间轴
	timeline = body->makeChild<TimelineCanvas>();
	timeline->setOwner(this);
	timeline->setPositionType(Ling::Position::Absolute);
	timeline->setPosition(Ling::Edge::Left, kSide);
	timeline->setPosition(Ling::Edge::Right, kSide);
	timeline->setPosition(Ling::Edge::Bottom, kReadoutH + kBarH);
	timeline->setHeight(kTimelineH);

	// 读数行：播放键 + 位置 + 裁剪区间 + 状态
	auto readout = body->makeChild<Ling::Node>();
	readout->setPositionType(Ling::Position::Absolute);
	readout->setPosition(Ling::Edge::Left, kSide);
	readout->setPosition(Ling::Edge::Right, kSide);
	readout->setPosition(Ling::Edge::Bottom, kBarH);
	readout->setHeight(kReadoutH);
	readout->setFlexDirection(Ling::FlexDirection::Row);
	readout->setAlignItems(Ling::Align::Center);

	playBtn = makeBtn(readout, L"\u25B6", 0, 0xE8E8E8FF, 0x333333FF);
	playBtn->setWidth(30.f);
	playBtn->setHeight(22.f);
	playBtn->setFontSize(12.f);
	playBtn->onClick.add([this](Ling::Button*) { togglePlay(); });

	posLabel = readout->makeChild<Ling::Label>();
	posLabel->setFontSize(13.f);
	posLabel->setColor(0x333333FF);
	posLabel->setMarginLeft(6.f);
	posLabel->setMarginRight(14.f);
	posLabel->setHeightPercent(100.f);
	posLabel->setJustifyContent(Ling::Justify::Center);

	rangeLabel = readout->makeChild<Ling::Label>();
	rangeLabel->setFontSize(13.f);
	rangeLabel->setColor(0x666666FF);
	rangeLabel->setHeightPercent(100.f);
	rangeLabel->setJustifyContent(Ling::Justify::Center);

	auto spacer = readout->makeChild<Ling::Node>();
	spacer->setFlexGrow(1.f);

	statusLabel = readout->makeChild<Ling::Label>();
	statusLabel->setFontSize(13.f);
	statusLabel->setColor(0x999999FF);
	statusLabel->setHeightPercent(100.f);
	statusLabel->setJustifyContent(Ling::Justify::Center);

	// 进度条（默认藏着）
	progressBox = body->makeChild<Ling::Node>();
	progressBox->setPositionType(Ling::Position::Absolute);
	progressBox->setPosition(Ling::Edge::Left, 0);
	progressBox->setPosition(Ling::Edge::Right, 0);
	progressBox->setPosition(Ling::Edge::Bottom, kBarH);
	progressBox->setHeight(4.f);
	progressBox->setBg(0xE8E8E8FF);
	progressFill = progressBox->makeChild<Ling::Node>();
	progressFill->setWidthPercent(0.f);
	progressFill->setHeightPercent(100.f);
	progressFill->setBg((Setting::getThemeColor() << 8) | 0xFF);
	progressBox->hide();

	// 底部按钮条
	auto bar = body->makeChild<Ling::Node>();
	bar->setPositionType(Ling::Position::Absolute);
	bar->setPosition(Ling::Edge::Left, 0);
	bar->setPosition(Ling::Edge::Right, 0);
	buildBottom(bar);

	// ---- 事件 ----
	onMouseDown.add([this](POINT p, bool isRight) {
		if (isRight || !loadOk) return;
		if (timeline && timeline->isPosIn(p)) {
			const float x = (float)(p.x - (LONG)timeline->x);
			const float xa = xOfMs(inMs), xb = xOfMs(outMs);
			// 谁近抓谁：两个手柄离得近时（裁得很短的片段）按距离分，不然永远只能拖到起点
			if (std::abs(x - xa) <= kGrabR && std::abs(x - xa) <= std::abs(x - xb)) dragMode = 1;
			else if (std::abs(x - xb) <= kGrabR) dragMode = 2;
			else dragMode = 3;
			setPlaying(false);
			onDragTo(x);
			return;
		}
		if (preview && preview->isPosIn(p)) togglePlay();
		});
	onMouseMove.add([this](POINT p) {
		if (!dragMode || !timeline) return;
		onDragTo((float)(p.x - (LONG)timeline->x));
		});
	onMouseUp.add([this](POINT, bool) { dragMode = 0; });
	onKeyDown.add([this](UINT key) { onKey(key); });
	onTimer.add([this](UINT id) { if (id == kTickId) onTick(); });

	updateLabels();
	startDecode();
	setTimer(kTickMs, kTickId);
	// createNativeWindow 建的窗口**不带 WS_VISIBLE**（WinBase.cpp:180），每个窗口都得自己 show，
	// 漏了就是"点了没反应、再点一次才有窗口"那种迷惑症状
	show();
}

void WinVideoEdit::buildHeader(Ling::Node* header)
{
	auto title = header->makeChild<Ling::Label>();
	title->setText(Lang::get(L"video.edit.title"));
	title->setFontSize(14.f);
	title->setColor(0x333333FF);
	title->setFlexGrow(1.f);
	title->setHeightPercent(100.f);
	title->setJustifyContent(Ling::Justify::Center);

	// 关窗。图标色必须设成深色：顶栏是纯白底，默认那枚白叉在白底上完全看不见
	// （只留一个"悬停才出现"的错觉，2026-10-10 抓图时发现）。悬停转红底，与其它窗口一致
	auto closeBtn = makeBtn(header, L"\ue62d", 0, 0xE81123FF, 0x333333FF, true);
	closeBtn->setSize(34.f, 30.f);
	closeBtn->setMarginLeft(8.f);
	closeBtn->setBorder(1.f, 0xE0E0E0FF);
	closeBtn->setHoverColor(0xFFFFFFFF);
	closeBtn->onClick.add([this](Ling::Button*) { closeWindow(); });
}

void WinVideoEdit::buildBottom(Ling::Node* bar)
{
	bar->setPositionType(Ling::Position::Absolute);
	bar->setPosition(Ling::Edge::Left, 0);
	bar->setPosition(Ling::Edge::Right, 0);
	bar->setPosition(Ling::Edge::Bottom, 0);
	bar->setHeight(kBarH);
	bar->setBg(0xFFFFFFFF);
	bar->setFlexDirection(Ling::FlexDirection::Row);
	bar->setAlignItems(Ling::Align::Center);
	bar->setPaddingLeft(kSide);
	bar->setPaddingRight(kSide);

	exportBtn = makeBtn(bar, Lang::get(L"video.edit.export"), (Setting::getThemeColor() << 8) | 0xFF,
		(Setting::getThemeSoft() << 8) | 0xFF, 0xFFFFFFFF);
	exportBtn->setWidth(96.f);
	exportBtn->setHeight(32.f);
	exportBtn->onClick.add([this](Ling::Button*) { exportAs(false); });

	copyBtn = makeBtn(bar, Lang::get(L"video.edit.copy"), 0xF2F2F2FF, 0xE4E4E4FF, 0x333333FF);
	copyBtn->setWidth(128.f);
	copyBtn->setHeight(32.f);
	copyBtn->setMarginLeft(10.f);
	copyBtn->onClick.add([this](Ling::Button*) { exportAs(true); });

	auto spacer = bar->makeChild<Ling::Node>();
	spacer->setFlexGrow(1.f);

	rawBtn = makeBtn(bar, Lang::get(L"video.edit.raw"), 0, 0xF2F2F2FF, 0x666666FF);
	rawBtn->setWidth(120.f);
	rawBtn->setHeight(32.f);
	rawBtn->onClick.add([this](Ling::Button*) { saveRaw(); });

	discardBtn = makeBtn(bar, Lang::get(L"video.edit.discard"), 0, 0xF2F2F2FF, 0x666666FF);
	discardBtn->setWidth(96.f);
	discardBtn->setHeight(32.f);
	discardBtn->setMarginLeft(10.f);
	discardBtn->onClick.add([this](Ling::Button*) { discard(); });
}

void WinVideoEdit::buildFailedUi()
{
	body->setBg(0xFAFAFAFF);
	auto box = body->makeChild<Ling::Node>();
	box->setSizePercent(100.f, 100.f);
	box->setFlexDirection(Ling::FlexDirection::Column);
	box->setAlignItems(Ling::Align::Center);
	box->setJustifyContent(Ling::Justify::Center);

	auto msg = box->makeChild<Ling::Label>();
	msg->setText(Lang::get(L"video.edit.badFile"));
	msg->setFontSize(14.f);
	msg->setColor(0x333333FF);
	msg->setMarginBottom(8.f);

	auto detail = box->makeChild<Ling::Label>();
	detail->setText(loadErr);
	detail->setFontSize(12.f);
	detail->setColor(0x999999FF);
	detail->setMarginBottom(20.f);

	auto ok = makeBtn(box, Lang::get(L"video.edit.discard"), 0xF2F2F2FF, 0xE4E4E4FF, 0x333333FF);
	ok->setSize(96.f, 32.f);
	ok->onClick.add([this](Ling::Button*) { discard(); });
	// 失败页同样得自己 show —— 见 onCreated 末尾那条注释
	show();
}

void WinVideoEdit::layout()
{
	Ling::WinBase::layout();
}

BOOL WinVideoEdit::setCursor()
{
	return FALSE;
}

// ---- 几何：画与命中共用这一份 ----

WinVideoEdit::TrackGeom WinVideoEdit::trackGeom() const
{
	TrackGeom g;
	if (!timeline) return g;
	// ⚠️ 单位：kSide 是**逻辑**像素，而 Node 的 w 是**物理**像素（Ling 的 setter 已乘过 dpi）。
	// 这里必须自己乘 dpi 才对得上 —— 直接写 kSide 的话 125% 缩放下左右各差 4px，
	// 画的轨道、手柄和 msOfX/xOfMs 的命中会一起偏（2026-10-10 修）
	const float side = kSide * dpi;
	g.x0 = side;
	g.x1 = std::max(side + 1.f, timeline->w - side);
	return g;
}

float WinVideoEdit::xOfMs(long long ms) const
{
	const auto g = trackGeom();
	const long long dur = std::max(1LL, info.durMs);
	const float t = std::clamp((float)((double)ms / (double)dur), 0.f, 1.f);
	return g.x0 + (g.x1 - g.x0) * t;
}

long long WinVideoEdit::msOfX(float x) const
{
	const auto g = trackGeom();
	const float span = std::max(1.f, g.x1 - g.x0);
	const float t = std::clamp((x - g.x0) / span, 0.f, 1.f);
	return (long long)((double)t * (double)std::max(1LL, info.durMs));
}

void WinVideoEdit::onDragTo(float localX)
{
	if (!loadOk) return;
	const long long ms = msOfX(localX);
	if (dragMode == 1) {
		inMs = std::clamp(ms, 0LL, std::max(0LL, outMs - kMinSpanMs));
		playheadMs = inMs;
		seekTo(inMs);
	}
	else if (dragMode == 2) {
		outMs = std::clamp(ms, std::min(info.durMs, inMs + kMinSpanMs), info.durMs);
		playheadMs = outMs;
		seekTo(outMs);
	}
	else {
		playheadMs = std::clamp(ms, inMs, outMs);
		seekTo(playheadMs);
	}
	// 拖动时先本地画一版（不等解码回来），松手后解码帧到达会再刷一次
	updateLabels();
	if (timeline) timeline->layout();
}

// ---- 播放 ----

void WinVideoEdit::setPlaying(bool on)
{
	if (playing == on) return;
	playing = on;
	{
		std::scoped_lock lk(mtx);
		decPlay = on;
	}
	if (playBtn) {
		playBtn->setText(on ? L"\u2016" : L"\u25B6");
		playBtn->setBg(on ? 0xE8E8E8FF : 0);
		playBtn->setHoverBg(0xE8E8E8FF);
	}
}

void WinVideoEdit::togglePlay()
{
	if (!loadOk) return;
	if (playing) { setPlaying(false); return; }
	// 播放头已经在终点（或者跑到选中区间之外）时，从头/起点开始播 —— 否则一点反应都没有
	if (playheadMs >= outMs - 20 || playheadMs < inMs) {
		seekTo(inMs);
		playheadMs = inMs;
		updateLabels();
	}
	setPlaying(true);
}

void WinVideoEdit::seekTo(long long ms)
{
	if (!loadOk) return;
	std::scoped_lock lk(mtx);
	pendingSeek = std::clamp(ms, 0LL, info.durMs);
}

void WinVideoEdit::stepFrame(int frames, bool bySecond)
{
	if (!loadOk) return;
	setPlaying(false);
	long long delta = 0;
	if (bySecond) delta = 1000LL * frames;
	else {
		const double fps = info.fps > 1.0 ? info.fps : 30.0;
		delta = (long long)std::lround(frames * 1000.0 / fps);
	}
	playheadMs = std::clamp(playheadMs + delta, 0LL, info.durMs);
	seekTo(playheadMs);
	updateLabels();
	if (timeline) timeline->layout();
}

void WinVideoEdit::onKey(UINT key)
{
	if (!loadOk) {
		if (key == VK_ESCAPE) discard();
		return;
	}
	const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
	const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	if (ctrl && key == 'S') { exportAs(false); return; }
	if (ctrl && key == 'C') { exportAs(true); return; }
	switch (key) {
	case VK_SPACE: togglePlay(); break;
	case VK_LEFT: stepFrame(-1, shift); break;
	case VK_RIGHT: stepFrame(1, shift); break;
	case VK_OEM_4:   // [
		setPlaying(false);
		inMs = std::clamp(playheadMs, 0LL, std::max(0LL, outMs - kMinSpanMs));
		updateLabels();
		if (timeline) timeline->layout();
		break;
	case VK_OEM_6:   // ]
		setPlaying(false);
		outMs = std::clamp(playheadMs, std::min(info.durMs, inMs + kMinSpanMs), info.durMs);
		updateLabels();
		if (timeline) timeline->layout();
		break;
	case VK_HOME:
		setPlaying(false);
		playheadMs = 0;
		seekTo(0);
		updateLabels();
		if (timeline) timeline->layout();
		break;
	case VK_END:
		setPlaying(false);
		playheadMs = info.durMs;
		seekTo(info.durMs);
		updateLabels();
		if (timeline) timeline->layout();
		break;
	case VK_ESCAPE: closeWindow(); break;
	default: break;
	}
}

void WinVideoEdit::updateLabels()
{
	if (posLabel) {
		posLabel->setText(std::format(L"{} / {}", fmtTime(playheadMs, true), fmtTime(info.durMs, false)));
	}
	if (rangeLabel) {
		rangeLabel->setText(std::format(L"{}  {}  {}", 
			Lang::get(L"video.edit.start") + L" " + fmtTime(inMs, true),
			Lang::get(L"video.edit.end") + L" " + fmtTime(outMs, true),
			Lang::get(L"video.edit.trimmed") + L" " + fmtTime(outMs - inMs, true)));
	}
}

// ---- 解码线程 ----

void WinVideoEdit::startDecode()
{
	decThread = std::jthread([this](std::stop_token st) { decodeLoop(st); });
}

void WinVideoEdit::decodeLoop(std::stop_token st)
{
	const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const HRESULT hrMf = MFStartup(MF_VERSION);
	auto cleanup = [&]() {
		if (SUCCEEDED(hrMf)) MFShutdown();
		if (SUCCEEDED(hrCom)) CoUninitialize();
	};

	VideoTrim::Reader rd;
	std::wstring err;
	const bool opened = SUCCEEDED(hrMf) && rd.open(src, err);
	if (!opened) {
		std::scoped_lock lk(mtx);
		decErr = SUCCEEDED(hrMf) ? (err.empty() ? L"打开解码器失败" : err) : L"MediaFoundation 初始化失败";
		decEof = true;
		cleanup();
		return;
	}

	auto post = [&](std::vector<BYTE>& buf, long long pts) {
		std::scoped_lock lk(mtx);
		front.swap(buf);          // 与 UI 那侧互换：不需要拷贝一整帧
		frontW = rd.info().w;
		frontH = rd.info().h;
		frontPts = pts;
		freshFrame = true;
	};

	{
		std::vector<BYTE> buf;
		long long pts = 0;
		if (rd.seek(0, buf, pts)) post(buf, pts);
	}

	// 播放节流用的帧间隔
	const double fps = (rd.info().fps > 1.0) ? rd.info().fps : 30.0;
	const auto frameGap = std::chrono::microseconds((long long)std::lround(1000000.0 / fps));

	while (!st.stop_requested()) {
		long long seek = -1;
		bool wantPlay = false;
		{
			std::scoped_lock lk(mtx);
			if (pendingSeek >= 0) { seek = pendingSeek; pendingSeek = -1; }
			wantPlay = decPlay && !decEof;
		}
		if (seek >= 0) {
			std::vector<BYTE> buf;
			long long pts = 0;
			if (rd.seek(seek, buf, pts)) post(buf, pts);
			continue;
		}
		if (!wantPlay) {
			std::this_thread::sleep_for(std::chrono::milliseconds(8));
			continue;
		}
		const auto t0 = std::chrono::steady_clock::now();
		std::vector<BYTE> buf;
		long long pts = 0;
		if (!rd.next(buf, pts)) {
			std::scoped_lock lk(mtx);
			decPlay = false;    // UI 的定时器看到这个就把播放键弹回去
			decEof = true;
			continue;
		}
		post(buf, pts);
		const auto spent = std::chrono::steady_clock::now() - t0;
		if (spent < frameGap) std::this_thread::sleep_for(frameGap - spent);
	}
	cleanup();
}

// ---- 定时器 ----

void WinVideoEdit::onTick()
{
	std::vector<BYTE> tmp;
	int w = 0, h = 0;
	long long pts = 0;
	bool fresh = false, threadPlay = false;

	{
		std::scoped_lock lk(mtx);
		if (freshFrame) {
			tmp.swap(front);
			w = frontW;
			h = frontH;
			pts = frontPts;
			freshFrame = false;
			fresh = true;
		}
		threadPlay = decPlay;
	}
	if (fresh && preview) {
		preview->setFrame(tmp, w, h);
		if (!playing) {
			playheadMs = std::clamp(pts, 0LL, info.durMs);
			updateLabels();
			if (timeline) timeline->layout();
		}
	}
	// 播放态跟线程对齐：解码到文件尾时线程会把 decPlay 置回 false
	if (playing && !threadPlay) {
		playing = false;
		if (playBtn) {
			playBtn->setText(L"\u25B6");
			playBtn->setBg(0);
			playBtn->setHoverBg(0xE8E8E8FF);
		}
	}
	// 播到终点就停（不然会一直往后空转）
	if (playing && !dragMode) {
		long long cur = 0;
		{
			std::scoped_lock lk(mtx);
			cur = frontPts;
		}
		if (cur >= outMs) setPlaying(false);
	}

	// 导出进度
	if (jobState.load() == kJobRunning) {
		const int pct = std::clamp(jobProgress.load(), 0, 100);
		if (progressFill) progressFill->setWidthPercent((float)pct);
		if (statusLabel) {
			statusLabel->setText(std::format(L"{} {}%", Lang::get(L"video.edit.exporting"), pct));
		}
	}
	else if (jobState.load() != kJobIdle && jobState.load() != kJobRunning) {
		finishExport(jobState.load(), jobProgress.load());
	}
}

// ---- 导出 / 丢弃 ----

void WinVideoEdit::exportAs(bool toClipboard)
{
	if (!loadOk || exporting()) return;
	setPlaying(false);
	exportToClip = toClipboard;
	// 产物先落在临时目录，等用户选完保存框再拷过去（剪切板那条路直接留在那儿）。
	// 目录跟录制临时文件同一份（设置页的「录制临时目录」）—— 裁剪产物和源片是一个量级，
	// 就该和源片待在同一块盘上；默认仍是数据目录，所以没配过的话行为与以前完全一致。
	// getRecordTempDir() 给的是 std::filesystem::path，追加文件名要走 append（不是 operator+）
	tmpOut = Setting::get()->getRecordTempDir().append(L"trim.mp4").wstring();
	DeleteFileW(tmpOut.c_str());

	jobCancel = false;
	jobProgress = 0;
	{
		std::scoped_lock lk(mtx);
		jobErr.clear();
	}
	jobState = kJobRunning;

	VideoTrim::TrimParams p;
	p.src = src;
	p.dst = tmpOut;
	p.inMs = inMs;
	p.outMs = outMs;
	p.brKbps = 4000;
	p.quality = 50;
	p.fps = (int)std::lround(info.fps > 1.0 ? info.fps : 30.0);
	p.onProgress = [this](float f) { jobProgress = (int)std::lround(f * 100.f); };
	p.cancelled = [this]() { return jobCancel.load(); };

	jobThread = std::jthread([this, p](std::stop_token) mutable {
		std::wstring err;
		const long rc = VideoTrim::trim(p, err);
		{
			std::scoped_lock lk(mtx);
			jobErr = err;
		}
		jobProgress = 100;
		jobState = (int)rc;   // 0 = 成功，其余负数是失败码
	});

	if (progressBox) progressBox->show();
	if (exportBtn) exportBtn->setText(Lang::get(L"video.edit.exporting"));
	if (statusLabel) statusLabel->setText(Lang::get(L"video.edit.exporting"));
}

void WinVideoEdit::finishExport(int rc, int)
{
	if (rc == kJobRunning || rc == kJobIdle) return;
	if (jobThread.joinable()) jobThread.join();   // 线程已经跑完，这里只是收尸
	jobState = kJobIdle;
	if (progressBox) progressBox->hide();
	if (exportBtn) exportBtn->setText(Lang::get(L"video.edit.export"));
	if (statusLabel) statusLabel->setText(L"");

	if (rc != 0) {
		std::wstring err;
		{
			std::scoped_lock lk(mtx);
			err = jobErr;
		}
		DeleteFileW(tmpOut.c_str());
		showError(err.empty() ? Lang::get(L"video.edit.exportFailed") : err);
		return;
	}
	if (exportToClip) {
		Util::addFileToClipboard(tmpOut);
		// 剪切板握着的是产物路径（不是副本），所以这两条都不能省也不能反：
		//  ① 标记住"产物归剪切板了"，onDestroy 才肯放过它
		//  ② 原始 temp 没用了，删掉
		tmpToClipboard = true;
		DeleteFileW(src.c_str());
		if (progressFill) progressFill->setWidthPercent(0.f);
		closeWindow();
		return;
	}
	auto dst = Util::getSaveFilePath(hwnd, L"mp4");
	if (dst.empty()) {
		// 用户取消了保存框：产物删掉，**原始 temp 留着**（还能再进编辑窗重来一次）
		DeleteFileW(tmpOut.c_str());
		if (progressFill) progressFill->setWidthPercent(0.f);
		return;
	}
	CopyFileW(tmpOut.c_str(), dst.c_str(), FALSE);
	DeleteFileW(tmpOut.c_str());
	DeleteFileW(src.c_str());
	if (progressFill) progressFill->setWidthPercent(0.f);
	closeWindow();
}

void WinVideoEdit::saveRaw()
{
	if (!loadOk || exporting()) return;
	auto dst = Util::getSaveFilePath(hwnd, L"mp4");
	if (dst.empty()) return;
	CopyFileW(src.c_str(), dst.c_str(), FALSE);
	DeleteFileW(src.c_str());
	closeWindow();
}

void WinVideoEdit::discard()
{
	if (exporting()) {
		jobCancel = true;
		if (jobThread.joinable()) jobThread.join();
		jobState = kJobIdle;
	}
	DeleteFileW(tmpOut.c_str());
	DeleteFileW(src.c_str());
	closeWindow();
}

void WinVideoEdit::closeWindow()
{
	// 关窗保留 temp：用户可能只是误触，下次从录制历史里还能找回来（会被下次录制覆盖）
	close();
}

void WinVideoEdit::showError(const std::wstring& text)
{
	MessageBoxW(hwnd, text.data(), Lang::get(L"about.sysTip").data(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}
