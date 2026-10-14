#include "pch.h"
#include <chrono>
#include <format>
#include "../Lang.h"
#include "../Setting.h"
#include "../Util.h"
#include "../ShotHistory.h"
#include "WinPin.h"
#include "WinHistory.h"

namespace {
	std::unique_ptr<WinHistory> winHistory;
	constexpr float cardW{ 168.f }, cardH{ 160.f }, gap{ 10.f };
	// 预览区高度。缩略图"铺满"这件事只在 cover 的算法里，不依赖它，但改这里要连带看一眼
	constexpr float previewH{ 102.f };

	// 缩略图画布：按 cover 把图铺满整块预览区。
	//
	// 为什么不用 Ling::ImageBox：它内层的 Image 只会等比**收缩**（contain），
	// 一条 5:1 的宽扁截图塞进 156×102 的框里只占三成高度，其余七成是预览区的灰底
	// —— 作者报的"图片预览没有占满预留的矩形框"就是这个。自己画一层，按 cover 放大、
	// 居中，溢出窗口的那部分由画布边界自然裁掉，四边不再留灰边
	class CoverImage : public Ling::Canvas
	{
	public:
		CoverImage(Ling::WinBase* win) :Ling::Canvas(win) {}
		void loadImg(const std::wstring& path)
		{
			std::vector<BYTE> data;
			DWORD w{ 0 }, h{ 0 };
			if (!Util::loadImageBytes(path, data, w, h)) return;
			// decodeWicFrame 给的是**不带预乘**的 BGRA，所以这里按 IGNORE 用 —— 截图没有
			// 有意义的 alpha，标成 PREMULTIPLIED 会让半透明处整片发黑
			D2D1_BITMAP_PROPERTIES1 props{};
			props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
			props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
			props.dpiX = props.dpiY = 96.f;
			Ling::D2D::get()->deviceContext->CreateBitmap(
				D2D1::SizeU(w, h), data.data(), w * 4, props, bitmap.ReleaseAndGetAddressOf());
		}
	protected:
		void layout() override
		{
			Ling::Canvas::layout();
			auto ctx = startPaint();
			if (!ctx) return;
			ctx->Clear(0);
			if (bitmap) {
				auto size = bitmap->GetPixelSize();
				// cover：取"两维各自铺满所需倍率"里更大的那个，于是短的那一维溢出、由边界裁掉
				auto scale = std::max(w / (float)size.width, h / (float)size.height);
				auto dw{ size.width * scale }, dh{ size.height * scale };
				auto dx{ (w - dw) / 2.f }, dy{ (h - dh) / 2.f };
				ctx->DrawBitmap(bitmap.Get(), D2D1::RectF(dx, dy, dx + dw, dy + dh), 1.f,
					D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
			}
			finishPaint();
		}
	private:
		Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
	};
}

WinHistory::WinHistory() : Ling::WinBase()
{
	// 和 WinSetting 一个道理：关窗回调是从按钮点击栈上来的，那里同步 reset 就是 use-after-free
	onDestroy.add([]() {
		Ling::App::get()->dq.TryEnqueue([]() { winHistory.reset(); });
	});
	setTitle(Lang::get(L"history.title"));
	setSize(900.f, 620.f);
	// 窗口可拖边缩放（onHitTest），但再小也至少要容得下两列卡片 + 顶栏
	setMinSize(520.f, 430.f);
	setCenter();
	createNativeWindow();
}

WinHistory::~WinHistory()
{
}

void WinHistory::init()
{
	if (winHistory) {
		SetForegroundWindow(winHistory->hwnd);
		return;
	}
	winHistory.reset(new WinHistory());
}

void WinHistory::dispose()
{
	winHistory.reset();
}

LRESULT WinHistory::onHitTest(const POINT pos)
{
	// onHitTest 收到的是屏幕坐标，先换客户区再交给通用的八向边框判定；
	// 命中边框后由系统接管缩放，窗口自身的鼠标事件不会被打扰
	POINT pt = pos;
	ScreenToClient(hwnd, &pt);
	return borderHitTest(pt);
}

void WinHistory::onCreated()
{
	enableShadow();
	body->setBg(0xFAFAFAFF);
	body->setFlexDirection(Ling::FlexDirection::Column);

	auto top = body->makeChild<Ling::Node>();
	top->setHeight(48.f);
	top->setWidthPercent(100.f);
	top->setFlexDirection(Ling::FlexDirection::Row);
	top->setAlignItems(Ling::Align::Center);
	top->setPaddingLeft(16.f);
	top->setPaddingRight(16.f);
	initTabs(top);

	scroller = body->makeChild<Ling::ScrollerBox>();
	scroller->setFlexGrow(1.f);
	scroller->setWidthPercent(100.f);
	scroller->content->setFlexDirection(Ling::FlexDirection::Row);
	scroller->content->setFlexWrap(Ling::Wrap::Wrap);
	// 每行放不满整数张卡片时，剩余宽度均匀摊进行首、卡片间、行尾 —— 不再全堆在
	// 最右侧。yoga 对 wrap 是逐行应用 justify 的（含最后一行），单卡行的空档也对称。
	// 卡片四边的 margin 保留，作为"行刚好塞满时"卡片之间的最小间距
	scroller->content->setJustifyContent(Ling::Justify::SpaceEvenly);
	scroller->content->setPadding(8.f, 8.f, 8.f, 8.f);
	fillList();
	// Esc 关窗，与其它弹出窗口一致（顶栏那个叉是鼠标路径，这是键盘路径）
	onKeyDown.add([this](UINT key) { if (key == VK_ESCAPE) close(); });
	show();
}

void WinHistory::initTabs(Ling::Node* parent)
{
	auto makeTab = [this](Ling::Node* box, const std::wstring& text, ShotHistory::Source src) {
		auto btn = box->makeChild<Ling::Button>();
		btn->setText(text);
		btn->setHeight(30.f);
		btn->setPaddingLeft(14.f);
		btn->setPaddingRight(14.f);
		btn->setBorder(1.f, 0xE0E0E0FF);
		btn->setHoverBg(0xFFFFFFFF);
		btn->onClick.add([this, src, btn](Ling::Button*) {
			curSource = src;
			fillList();
		});
		return btn;
	};
	// 两个 tab 各建一个，选中态由 fillList 统一刷，避免"点了一个不知道另一个要恢复"
	tabShot = makeTab(parent, Lang::get(L"history.shot"), ShotHistory::Source::Shot);
	tabClip = makeTab(parent, Lang::get(L"history.clipboard"), ShotHistory::Source::Clipboard);

	auto spacer = parent->makeChild<Ling::Node>();
	spacer->setFlexGrow(1.f);

	auto clearBtn = parent->makeChild<Ling::Button>();
	clearBtn->setText(Lang::get(L"history.clear"));
	clearBtn->setHeight(30.f);
	clearBtn->setPaddingLeft(14.f);
	clearBtn->setPaddingRight(14.f);
	clearBtn->setBorder(1.f, 0xE0E0E0FF);
	clearBtn->setHoverBg(0xFFFFFFFF);
	clearBtn->onClick.add([this](Ling::Button*) {
		ShotHistory::get()->clear(curSource);
		fillList();
	});

	// 关窗。窗口是 WS_POPUP、没有标题栏，顶栏不给个叉就没法用鼠标关了
	auto closeBtn = parent->makeChild<Ling::Button>();
	closeBtn->setText(L"\ue62d");
	closeBtn->setFontFamily(L"icon");
	closeBtn->setHeight(30.f);
	closeBtn->setWidth(34.f);
	closeBtn->setMarginLeft(8.f);
	closeBtn->setBorder(1.f, 0xE0E0E0FF);
	// 悬停转红底，与其它窗口的关闭按钮一致
	closeBtn->setHoverBg(0xE81123FF);
	closeBtn->onClick.add([this](Ling::Button*) { close(); });
}

void WinHistory::applyTabStyle()
{
	auto applyTab = [](Ling::Button* btn, bool selected) {
		if (selected) {
			btn->setBg((Setting::getThemeSoft() << 8) | 0xFF);
			btn->setHoverBg((Setting::getThemeSoft() << 8) | 0xFF);
		}
		else {
			btn->setBg(0);
			btn->setHoverBg(0xF2F2F2ff);
		}
	};
	if (tabShot) applyTab(tabShot, curSource == ShotHistory::Source::Shot);
	if (tabClip) applyTab(tabClip, curSource == ShotHistory::Source::Clipboard);
}

void WinHistory::applyTheme()
{
	if (!winHistory) return;
	// 只重刷两个 tab 的底色，不重跑 fillList —— 那条路会把所有卡片连缩略图一起重建，
	// 改个主题色不值得付那个代价
	winHistory->applyTabStyle();
	winHistory->refresh();
}

void WinHistory::fillList()
{
	// ScrollerBox::content 是框架持有的，只能清子节点，不能换掉
	scroller->content->removeAllChildren();
	curItems = ShotHistory::get()->list(curSource);
	if (curItems.empty()) {
		auto empty = scroller->content->makeChild<Ling::Label>();
		empty->setText(Lang::get(L"history.empty"));
		empty->setFontSize(13.f);
		empty->setColor(0x999999FF);
		empty->setMargin(16.f);
	}
	else {
		for (auto& item : curItems)
		{
			makeCard(scroller->content, item);
		}
	}
	applyTabStyle();
	refresh();
}

Ling::Node* WinHistory::makeCard(Ling::Node* parent, const ShotHistory::Item& item)
{
	auto card = parent->makeChild<Ling::Node>();
	card->setSize(cardW, cardH);
	card->setFlexDirection(Ling::FlexDirection::Column);
	card->setBg(0xFFFFFFFF);
	card->setBorder(1.f, 0xE8E8E8FF);
	card->setPadding(6.f, 6.f, 6.f, 6.f);
	// 没有 setGap 可用，靠四边 margin 撑开卡片之间的缝
	card->setMargin(gap / 2.f);

	auto preview = card->makeChild<Ling::Node>();
	preview->setHeight(previewH);
	preview->setWidthPercent(100.f);
	preview->setAlignItems(Ling::Align::Center);
	// 文本条目是多行预览、顶端起排（长了往下溢出），图片条目铺满整块 —— 两种都不要垂直居中
	preview->setJustifyContent(Ling::Justify::Start);
	// 灰底只剩"图没加载出来"时的兜底：正常情况整块被 cover 的缩略图盖满
	preview->setBg(0xF7F7F7FF);
	// 溢出预览区的内容（折行文本的超高部分）从这里裁掉；圆角给 2 是为了换取 clipGeo
	// 的创建，视觉上基本看不出来
	preview->setBorderRadius(2.f);

	Ling::Button* clickable{ nullptr };
	if (item.isText) {
		// 多行预览：按预览区宽度折行铺开，超过预览区高度的行被裁掉。
		// 只截 100 字封顶 —— 内层 Text 的 surface 高度跟随行数走，不设上限的
		// 长文本会把 surface 撑到几千像素高
		auto txt = item.text;
		if (txt.size() > 100) txt = txt.substr(0, 100) + L"...";
		auto label = preview->makeChild<Ling::Label>();
		label->setText(txt);
		label->setFontSize(12.f);
		// 不折行的话整行宽度会顺着 flex 传上去，把卡片横向撑爆 —— 旧版"截 22 字 +
		// 按钮里一行居中"溢出卡片边界的根因就在这
		label->setWrap(true);
		// Label 的尺寸由内层 Text 量出来，折行宽度靠这个上限收进预览区
		label->setMaxWidthPercent(100.f);
		label->setMargin(8.f, 8.f, 8.f, 0.f);
		clickable = preview->makeChild<Ling::Button>();
	}
	else {
		auto thumb = ShotHistory::get()->thumbPath(item);
		// 没生成缩略图（原图本身就窄）就直接拿原图
		auto path = thumb.empty() ? ShotHistory::get()->imagePath(item) : thumb;
		if (!path.empty()) {
			auto box = preview->makeChild<CoverImage>();
			box->setSizePercent(100.f, 100.f);
			box->loadImg(path);
		}
		// 按钮建在图之后，才压在图上接得到点击（Ling 没有 bringToFront）
		clickable = preview->makeChild<Ling::Button>();
	}
	// 必须脱离 flex 流。preview 不再垂直居中了，但 clickable 要盖住整块预览区接点击，
	// 在流内占 100% 高仍会把兄弟节点挤下去
	clickable->setPositionType(Ling::Position::Absolute);
	clickable->setPosition(Ling::Edge::Left, 0.f);
	clickable->setPosition(Ling::Edge::Top, 0.f);
	clickable->setSizePercent(100.f, 100.f);
	clickable->setBg(0);
	clickable->setHoverBg(0);
	clickable->onClick.add([this, id = item.id](Ling::Button*) { this->useItem(id); });

	auto bottom = card->makeChild<Ling::Node>();
	bottom->setFlexGrow(1.f);
	bottom->setWidthPercent(100.f);
	bottom->setFlexDirection(Ling::FlexDirection::Row);
	bottom->setAlignItems(Ling::Align::Center);

	auto time = bottom->makeChild<Ling::Label>();
	time->setText(timeText(item.time));
	time->setFontSize(11.f);
	time->setColor(0x888888FF);
	time->setFlexGrow(1.f);

	auto iconBtn = [](Ling::Node* box, const wchar_t* code) {
		auto btn = box->makeChild<Ling::Button>();
		btn->setText(code);
		btn->setFontFamily(L"icon");
		btn->setFontSize(14.f);
		btn->setSize(26.f, 26.f);
		btn->setBg(0);
		btn->setHoverBg(0xF2F2F2ff);
		return btn;
	};
	// 复制：原来是一枚对勾图标，看着像"选定 / 采用这张"，而它干的是往剪贴板放一份 ——
	// 作者要求直接写成字，省掉这份猜测。宽度按两个字给足，交给 flex 会被时间那一段挤扁
	auto copyBtn = bottom->makeChild<Ling::Button>();
	copyBtn->setText(Lang::get(L"history.copy"));
	copyBtn->setFontSize(12.f);
	copyBtn->setSize(42.f, 26.f);
	copyBtn->setBg(0);
	copyBtn->setHoverBg(0xF2F2F2ff);
	copyBtn->onClick.add([this, id = item.id](Ling::Button*) { this->copyItem(id); });
	auto delBtn = iconBtn(bottom, L"\ue62d");
	delBtn->onClick.add([this, id = item.id](Ling::Button*) { this->removeItem(id); });
	return card;
}

void WinHistory::useItem(const std::wstring& id)
{
	auto item = findItem(id);
	if (!item) return;
	if (item->isText) {
		Ling::Util::setTextToClipboard(item->text);
		ShotHistory::get()->skipNextClipboard();
		return;
	}
	std::vector<BYTE> data;
	int w{ 0 }, h{ 0 };
	if (!ShotHistory::get()->loadImage(*item, data, w, h)) return;
	// 居中落在主显示器上
	auto monitor = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO mi{ sizeof(MONITORINFO) };
	GetMonitorInfo(monitor, &mi);
	auto x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
	auto y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
	WinPin::initFromData(x, y, w, h, data);
}

void WinHistory::copyItem(const std::wstring& id)
{
	auto item = findItem(id);
	if (!item) return;
	if (item->isText) {
		Ling::Util::setTextToClipboard(item->text);
		ShotHistory::get()->skipNextClipboard();
		return;
	}
	std::vector<BYTE> data;
	int w{ 0 }, h{ 0 };
	if (!ShotHistory::get()->loadImage(*item, data, w, h)) return;
	Util::saveToClipboard(w, h, data.data());
	ShotHistory::get()->skipNextClipboard();
}

void WinHistory::removeItem(const std::wstring& id)
{
	ShotHistory::get()->removeById(id);
	fillList();
}

// 卡片上的回调只带 id：Item 是按值存进 curItems 的，刷新列表时整体重建，
// 捕获引用会指到已经被换掉的那份
const ShotHistory::Item* WinHistory::findItem(const std::wstring& id) const
{
	for (auto& item : curItems) {
		if (item.id == id) return &item;
	}
	return nullptr;
}

std::wstring WinHistory::timeText(long long ms)
{
	auto tp = std::chrono::system_clock::time_point{ std::chrono::milliseconds{ ms } };
	auto tt = std::chrono::system_clock::to_time_t(tp);
	std::tm tm{};
	localtime_s(&tm, &tt);
	return std::format(L"{:02d}-{:02d} {:02d}:{:02d}",
		tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}
