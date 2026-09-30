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
}

WinHistory::WinHistory() : Ling::WinBase()
{
	// 和 WinSetting 一个道理：关窗回调是从按钮点击栈上来的，那里同步 reset 就是 use-after-free
	onDestroy.add([]() {
		Ling::App::get()->dq.TryEnqueue([]() { winHistory.reset(); });
	});
	setTitle(Lang::get(L"history.title"));
	setSize(900.f, 620.f);
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
	scroller->content->setPadding(8.f, 8.f, 8.f, 8.f);
	fillList();
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
	auto applyTab = [](Ling::Button* btn, bool selected) {
		if (selected) {
			btn->setBg(0xe6f4ffff);
			btn->setHoverBg(0xe6f4ffff);
		}
		else {
			btn->setBg(0);
			btn->setHoverBg(0xF2F2F2ff);
		}
	};
	applyTab(tabShot, curSource == ShotHistory::Source::Shot);
	applyTab(tabClip, curSource == ShotHistory::Source::Clipboard);
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
	preview->setHeight(102.f);
	preview->setWidthPercent(100.f);
	preview->setAlignItems(Ling::Align::Center);
	preview->setJustifyContent(Ling::Justify::Center);
	preview->setBg(0xF7F7F7FF);

	Ling::Button* clickable{ nullptr };
	if (item.isText) {
		clickable = preview->makeChild<Ling::Button>();
		// 只显示头一行，超出的用省略号收掉
		auto txt = item.text;
		auto nl = txt.find_first_of(L"\r\n");
		auto head = nl == std::wstring::npos ? txt : txt.substr(0, nl);
		if (head.size() > 22) head = head.substr(0, 22) + L"...";
		clickable->setText(head);
		clickable->setFontSize(12.f);
	}
	else {
		auto thumb = ShotHistory::get()->thumbPath(item);
		// 没生成缩略图（原图本身就窄）就直接拿原图，ImageBox 会自己等比收缩
		auto path = thumb.empty() ? ShotHistory::get()->imagePath(item) : thumb;
		if (!path.empty()) {
			auto box = preview->makeChild<Ling::ImageBox>();
			box->setSizePercent(100.f, 100.f);
			box->loadImg(path);
		}
		// 按钮建在图之后，才压在图上接得到点击（Ling 没有 bringToFront）
		clickable = preview->makeChild<Ling::Button>();
	}
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
	auto copyBtn = iconBtn(bottom, L"\ue6ad");
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
