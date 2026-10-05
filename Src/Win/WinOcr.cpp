#include "pch.h"
#include <thread>
#include "../Lang.h"
#include "../Ocr.h"
#include "../SelectPopup.h"
#include "../Setting.h"
#include "../ShotHistory.h"
#include "../Table.h"
#include "../Util.h"
#include "WinOcr.h"

namespace {
	std::unique_ptr<WinOcr> winOcr;
}

WinOcr::WinOcr(std::vector<BYTE>&& data, const int w, const int h)
	: Ling::WinBase(), pixels(std::move(data)), imgW(w), imgH(h)
{
	// 关窗按钮的点击栈上不能同步 reset（use-after-free），推迟到下一轮消息循环。
	// 捕获 this 而不是裸用：回调跑的时候这个对象可能已经不是当前那一个了（见 init 的存在性判断）
	auto self = this;
	onDestroy.add([self]() {
		Ling::App::get()->dq.TryEnqueue([self]() { if (winOcr.get() == self) winOcr.reset(); });
	});
	setTitle(Lang::get(L"ocr.title"));
	setSize(560.f, 420.f);
	setCenter();
	createNativeWindow();
}

WinOcr::~WinOcr()
{
}

void WinOcr::init(std::vector<BYTE>&& data, const int w, const int h)
{
	if (winOcr) {
		SetForegroundWindow(winOcr->hwnd);
		return;
	}
	winOcr.reset(new WinOcr(std::move(data), w, h));
}

void WinOcr::dispose()
{
	winOcr.reset();
}

void WinOcr::onCreated()
{
	enableShadow();
	body->setBg(0xFFFFFFFF);
	body->setFlexDirection(Ling::FlexDirection::Column);

	// 标题行：左边标题文字，右边关闭按钮。窗口是 WS_POPUP，系统标题栏不存在 ——
	// 没有这一行的话整个窗只有一块文本框，用户既拖不动它，也没有关它的办法
	auto titleRow = body->makeChild<Ling::Node>();
	titleRow->setHeight(36.f);
	titleRow->setWidthPercent(100.f);
	titleRow->setFlexDirection(Ling::FlexDirection::Row);
	titleRow->setAlignItems(Ling::Align::Center);

	auto title = titleRow->makeChild<Ling::Label>();
	title->setText(Lang::get(L"ocr.title"));
	title->setHeightPercent(100.f);
	title->setPaddingLeft(12.f);
	title->setFontSize(13.f);
	title->setColor(0x333333FF);
	title->setFlexGrow(1.f);
	title->setJustifyContent(Ling::Justify::Start);

	// 绝对定位到右上角：不占标题行的横向空间，也不会把标题文字挤窄
	auto closeBtn = titleRow->makeChild<Ling::Button>();
	closeBtn->setSize(42.f, 32.f);
	closeBtn->setPositionType(Ling::Position::Absolute);
	closeBtn->setPosition(Ling::Edge::Right, 0);
	closeBtn->setPosition(Ling::Edge::Top, 2);
	closeBtn->setHoverColor(0xFFFFFFFF);
	closeBtn->setHoverBg(0xE81123FF);
	closeBtn->setText(L"\ue62d");
	closeBtn->setFontFamily(L"icon");
	closeBtn->onClick.add([](Ling::Button* btn) { btn->win->close(); });

	box = body->makeChild<Ling::TextBox>();
	box->setFlexGrow(1.f);
	box->setWidthPercent(100.f);
	box->setFontSize(14.f);
	box->setPadding(12.f, 12.f, 12.f, 12.f);
	box->setText(Lang::get(L"ocr.recognizing"));

	auto bottom = body->makeChild<Ling::Node>();
	bottom->setHeight(48.f);
	bottom->setWidthPercent(100.f);
	bottom->setFlexDirection(Ling::FlexDirection::Row);
	bottom->setAlignItems(Ling::Align::Center);
	bottom->setPaddingRight(12.f);

	// 语言按钮。系统装了几种就多几个选项，一个都没装时它只显示一句提示、点了没反应
	langs = Ocr::languages();
	langBtn = bottom->makeChild<Ling::Button>();
	langBtn->setHeight(30.f);
	langBtn->setWidth(140.f);
	langBtn->setBorder(1.f, 0xE0E0E0FF);
	langBtn->setHoverBg(0xF2F2F2FF);
	langBtn->onClick.add([this](Ling::Button* b) {
		if (langs.empty()) return;
		// 第 0 项是跟随系统，之后依次是装了的每个语言包
		std::vector<std::wstring> items{ Lang::get(L"ocr.langAuto") };
		for (auto const& lang : langs) items.push_back(lang.name);
		SelectPopup::show(this, b, items, langIndex, [this](int idx) {
			langIndex = idx;
			Setting::get()->setToolStr(L"ocr", L"lang", curLangTag());
			applyLangBtn();
			startRecognize();
		});
	});
	// 记住上次选的；没装的语言包（卸载了）认不出来，退回跟随系统
	auto saved = Setting::get()->getToolStr(L"ocr", L"lang", L"");
	if (!saved.empty()) {
		for (int i = 0; i < static_cast<int>(langs.size()); ++i) {
			if (langs[i].tag == saved) { langIndex = i + 1; break; }
		}
	}
	applyLangBtn();

	// 表格按钮：在"整页文字"和"按格子出表"之间切。切完立刻重认一次
	tableBtn = bottom->makeChild<Ling::Button>();
	tableBtn->setHeight(30.f);
	tableBtn->setWidth(90.f);
	tableBtn->setBorder(1.f, 0xE0E0E0FF);
	tableBtn->setHoverBg(0xF2F2F2FF);
	// 只有"整页文字 / 按格子出表"两种，单击即切换。按钮上的字由 applyTableBtn 跟着状态改
	tableBtn->onClick.add([this](Ling::Button*) {
		tableMode = !tableMode;
		applyTableBtn();
		startRecognize();
	});
	applyTableBtn();

	auto spacer = bottom->makeChild<Ling::Node>();
	spacer->setFlexGrow(1.f);

	auto copyBtn = bottom->makeChild<Ling::Button>();
	copyBtn->setText(Lang::get(L"ocr.copy"));
	copyBtn->setHeight(30.f);
	copyBtn->setWidth(90.f);
	copyBtn->setBorder(1.f, 0xE0E0E0FF);
	copyBtn->setHoverBg(0xF2F2F2FF);
	copyBtn->onClick.add([this](Ling::Button*) {
		if (result.empty()) return;
		if (html.empty()) Ling::Util::setTextToClipboard(result);
		else Util::setHtmlToClipboard(html, result);
		ShotHistory::get()->skipNextClipboard();
	});

	show();
	startRecognize();
}

// 标题行（顶 36 逻辑像素内）当拖拽区。WS_POPUP 没有系统标题栏，不给这一段
// HTCAPTION 的话用户按不住窗口 —— 识别结果可能很长，窗口得能拖到别处看
LRESULT WinOcr::onHitTest(const POINT pos)
{
	POINT pt = pos;
	ScreenToClient(hwnd, &pt);
	// 右边 42 留给关闭按钮：那一块要按成 HTCLIENT，否则点 × 会变成拖窗口，点不掉
	if (pt.y > 0 && pt.y < 36 * dpi && pt.x < w - 42 * dpi) return HTCAPTION;
	return HTCLIENT;
}

void WinOcr::onMinMaxInfo(MINMAXINFO* mmi)
{
	// 同 ToolMain / ToolSub / WinPin：Ling 默认的最小跟踪尺寸是 800×600，
	// 比这个窗大得多，不放开的话 setSize 会被系统按回去
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}

std::wstring WinOcr::curLangTag() const
{
	// 0 号是空标签：让 Ocr 按系统语言档案自己挑
	return langIndex == 0 ? std::wstring{} : langs[langIndex - 1].tag;
}

void WinOcr::applyLangBtn()
{
	if (!langBtn) return;
	if (langs.empty()) {
		langBtn->setText(Lang::get(L"ocr.notInstalled"));
		return;
	}
	langBtn->setText(langIndex == 0 ? Lang::get(L"ocr.langAuto") : langs[langIndex - 1].name);
}

void WinOcr::startRecognize()
{
	const auto seq = ++taskSeq;
	auto lang = curLangTag();
	// 模式在这里定格：线程跑到一半用户可能又点了切换，那边不能再去读成员
	const auto asTable = tableMode;
	box->setText(Lang::get(L"ocr.recognizing"));
	// 往线程里拷一份而不是搬走：换语言会再识别一次，pixels 得一直留在成员里。
	// 线程里只拿 this 比地址、不 dereference —— 识别跑到一半窗口可能已经关了
	std::thread([this, data = pixels, w = imgW, h = imgH, lang = std::move(lang), seq, asTable]() mutable {
		// 新线程里没有 WinRT 单元，不初始化就用不了 OcrEngine
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
		std::wstring text;
		TableResult table;
		std::wstring err;
		bool ok{ false };
		if (asTable) ok = Table::recognize(w, h, data.data(), lang, table, err);
		else {
			text = Ocr::recognize(w, h, data.data(), lang);
			ok = true;   // 文字识别认不出来就是空，不算失败
		}
		Ling::App::get()->dq.TryEnqueue(
			[this, seq, asTable, ok, text = std::move(text), table = std::move(table), err = std::move(err)]() mutable {
				// 排在自己前面的可能正是"窗口已关闭"那次 reset，那之后 winOcr 已经不是本窗口了；
				// 也可能有更新的识别已经出结果，老结果不该盖上去
				if (winOcr.get() != this || taskSeq != seq) return;
				if (!ok) {
					result.clear();
					html.clear();
					box->setText(err.empty() ? Lang::get(L"ocr.empty") : err);
					return;
				}
				if (asTable) setTable(table);
				else setResult(text);
			});
	}).detach();
}

void WinOcr::setResult(const std::wstring& text)
{
	html.clear();
	result = text;
	box->setText(text.empty() ? Lang::get(L"ocr.empty") : text);
}

void WinOcr::setTable(const TableResult& table)
{
	html = table.toHtml();
	result = table.toTsv();
	// 一个字都没填进去时别显示一片空白的制表符，那看着像坏了
	if (result.find_first_not_of(L"\t\r\n") == std::wstring::npos) {
		html.clear();
		result.clear();
	}
	box->setText(result.empty() ? Lang::get(L"ocr.empty") : result);
}

void WinOcr::applyTableBtn()
{
	if (!tableBtn) return;
	tableBtn->setText(tableMode ? Lang::get(L"ocr.text") : Lang::get(L"ocr.table"));
}
