#include "pch.h"
#include <algorithm>
#include "../AiTranslate.h"
#include "../Lang.h"
#include "../Setting.h"
#include "../SelectPopup.h"
#include "WinAiTrans.h"

namespace {
	std::unique_ptr<WinAiTrans> winAiTrans;
}

WinAiTrans::WinAiTrans() : Ling::WinBase()
{
	auto self = this;
	onDestroy.add([self]() {
		Ling::App::get()->dq.TryEnqueue([self]() { if (winAiTrans.get() == self) winAiTrans.reset(); });
	});
	setTitle(Lang::get(L"ai.transTitle"));
	setSize(560.f, 520.f);
	setCenter();
	createNativeWindow();
}

WinAiTrans::~WinAiTrans()
{
	*alive = false;
	if (task) task->cancel();
}

void WinAiTrans::init(const std::wstring& preset)
{
	if (winAiTrans) {
		if (winAiTrans->hwnd && IsWindow(winAiTrans->hwnd)) {
			if (!preset.empty()) {
				winAiTrans->input->setText(preset);
				winAiTrans->run();
			}
			winAiTrans->show();
			SetForegroundWindow(winAiTrans->hwnd);
			return;
		}
		winAiTrans.reset();
	}
	winAiTrans.reset(new WinAiTrans());
	winAiTrans->presetText = preset;
	if (winAiTrans->hwnd) SetForegroundWindow(winAiTrans->hwnd);
}

void WinAiTrans::dispose()
{
	winAiTrans.reset();
}

void WinAiTrans::onCreated()
{
	enableShadow();
	body->setBg(0xFFFFFFFF);
	body->setFlexDirection(Ling::FlexDirection::Column);
	body->setPadding(12.f, 12.f, 12.f, 12.f);

	auto titleRow = body->makeChild<Ling::Node>();
	titleRow->setHeight(32.f);
	titleRow->setWidthPercent(100.f);
	titleRow->setFlexDirection(Ling::FlexDirection::Row);
	titleRow->setAlignItems(Ling::Align::Center);

	auto title = titleRow->makeChild<Ling::Label>();
	title->setText(Lang::get(L"ai.transTitle"));
	title->setFontSize(14.f);
	title->setColor(0x333333FF);
	title->setFlexGrow(1.f);

	auto closeBtn = titleRow->makeChild<Ling::Button>();
	closeBtn->setSize(32.f, 28.f);
	closeBtn->setPositionType(Ling::Position::Absolute);
	closeBtn->setPosition(Ling::Edge::Right, 0);
	closeBtn->setPosition(Ling::Edge::Top, 0);
	closeBtn->setHoverColor(0xFFFFFFFF);
	closeBtn->setHoverBg(0xE81123FF);
	closeBtn->setText(L"\ue62d");
	closeBtn->setFontFamily(L"icon");
	closeBtn->onClick.add([](Ling::Button* btn) { btn->win->close(); });

	// 语言方向：源 → 目标，中间那枚是交换。目标语言没有"自动检测"这一项
	auto langRow = body->makeChild<Ling::Node>();
	langRow->setHeight(34.f);
	langRow->setWidthPercent(100.f);
	langRow->setFlexDirection(Ling::FlexDirection::Row);
	langRow->setAlignItems(Ling::Align::Center);
	langRow->setMarginBottom(8.f);

	fromBtn = langRow->makeChild<Ling::Button>();
	fromBtn->setHeight(28.f);
	fromBtn->setWidth(140.f);
	fromBtn->setBorder(1.f, 0xE0E0E0FF);
	fromBtn->setHoverBg(0xFFFFFFFF);
	fromBtn->onClick.add([this](Ling::Button* btn) {
		std::vector<std::wstring> items;
		for (auto& lang : AiTranslate::langs()) items.push_back(lang.name);
		SelectPopup::show(this, btn, items, fromIdx, [this](int idx) {
			fromIdx = idx;
			Setting::get()->setAiStr(L"transFrom", fromCode());
			applyLangBtns();
		});
	});

	auto swapBtn = langRow->makeChild<Ling::Button>();
	swapBtn->setHeight(28.f);
	swapBtn->setWidth(36.f);
	swapBtn->setMarginLeft(8.f);
	swapBtn->setMarginRight(8.f);
	swapBtn->setBorder(1.f, 0xE0E0E0FF);
	swapBtn->setHoverBg(0xFFFFFFFF);
	swapBtn->setText(L"\u2194");
	swapBtn->onClick.add([this](Ling::Button*) {
		// "自动检测"不是一种语言，翻不出"译成自动检测"，所以那一侧不动
		if (fromIdx == 0) return;
		std::swap(fromIdx, toIdx);
		Setting::get()->setAiStr(L"transFrom", fromCode());
		Setting::get()->setAiStr(L"transTo", toCode());
		applyLangBtns();
	});

	toBtn = langRow->makeChild<Ling::Button>();
	toBtn->setHeight(28.f);
	toBtn->setWidth(140.f);
	toBtn->setBorder(1.f, 0xE0E0E0FF);
	toBtn->setHoverBg(0xFFFFFFFF);
	toBtn->onClick.add([this](Ling::Button* btn) {
		std::vector<std::wstring> items;
		// 第 0 项是自动检测，只能当源语言
		auto& langs = AiTranslate::langs();
		for (size_t i = 1; i < langs.size(); ++i) items.push_back(langs[i].name);
		// 下拉的列表从 1 起，选回来的下标要 +1 才是 langs 里的位置
		SelectPopup::show(this, btn, items, toIdx - 1, [this](int idx) {
			toIdx = idx + 1;
			Setting::get()->setAiStr(L"transTo", toCode());
			applyLangBtns();
		});
	});

	input = body->makeChild<Ling::TextBox>();
	input->setHeight(150.f);
	input->setWidthPercent(100.f);
	input->setFontSize(14.f);
	input->setPadding(10.f, 8.f, 10.f, 8.f);
	input->setBorder(1.f, 0xE0E0E0FF);
	input->setPlaceholder(Lang::get(L"ai.transPlaceholder"));

	auto actRow = body->makeChild<Ling::Node>();
	actRow->setHeight(34.f);
	actRow->setWidthPercent(100.f);
	actRow->setFlexDirection(Ling::FlexDirection::Row);
	actRow->setAlignItems(Ling::Align::Center);
	actRow->setMarginTop(8.f);

	runBtn = actRow->makeChild<Ling::Button>();
	runBtn->setHeight(28.f);
	runBtn->setWidth(90.f);
	runBtn->setBorder(1.f, 0xE0E0E0FF);
	runBtn->setHoverBg(0xF2F2F2FF);
	runBtn->setText(Lang::get(L"ai.transBtn"));
	runBtn->onClick.add([this](Ling::Button*) {
		if (busy) {
			if (task) task->cancel();
			return;
		}
		run();
	});

	auto copyBtn = actRow->makeChild<Ling::Button>();
	copyBtn->setHeight(28.f);
	copyBtn->setWidth(90.f);
	copyBtn->setMarginLeft(8.f);
	copyBtn->setBorder(1.f, 0xE0E0E0FF);
	copyBtn->setHoverBg(0xF2F2F2FF);
	copyBtn->setText(Lang::get(L"ai.transCopy"));
	copyBtn->onClick.add([this](Ling::Button*) {
		if (!result.empty()) Ling::Util::setTextToClipboard(result);
	});

	statusLabel = actRow->makeChild<Ling::Label>();
	statusLabel->setMarginLeft(8.f);
	statusLabel->setFlexGrow(1.f);
	statusLabel->setFontSize(12.f);
	statusLabel->setColor(0x888888FF);

	// 结果区。译文可能很长，套一层滚动容器；里面只是个 Label，没有可点控件，
	// 不存在"命中坐标要减滚动量"那回事
	auto scroller = body->makeChild<Ling::ScrollerBox>();
	scroller->setFlexGrow(1.f);
	scroller->setWidthPercent(100.f);
	scroller->setMarginTop(8.f);
	resultLabel = scroller->makeChild<Ling::Label>();
	resultLabel->setWidthPercent(100.f);
	resultLabel->setFontSize(14.f);
	resultLabel->setPadding(10.f, 8.f, 10.f, 8.f);
	resultLabel->setBg(0xF7F7F9FF);
	resultLabel->setBorderRadius(6.f);

	auto setting = Setting::get();
	auto& langs = AiTranslate::langs();
	auto savedFrom = setting->getAiStr(L"transFrom", L"auto");
	auto savedTo = setting->getAiStr(L"transTo", L"zh");
	for (int i = 0; i < static_cast<int>(langs.size()); ++i) {
		if (langs[i].code == savedFrom) fromIdx = i;
		if (langs[i].code == savedTo) toIdx = i;
	}
	// 目标语言被存成了 auto（手改过配置）时兜一下，否则会拿到一个不存在的选项
	if (toIdx == 0 && langs.size() > 1) toIdx = 1;
	applyLangBtns();

	if (!presetText.empty()) {
		input->setText(presetText);
		presetText.clear();
		run();
	}
}

LRESULT WinAiTrans::onHitTest(const POINT pos)
{
	POINT pt = pos;
	ScreenToClient(hwnd, &pt);
	if (!isMaximized) {
		auto result = borderHitTest(pt);
		if (result != HTCLIENT) return result;
	}
	if (pt.y > 0 && pt.y < 34 * dpi && pt.x < w - 32 * dpi) return HTCAPTION;
	return HTCLIENT;
}

void WinAiTrans::applyLangBtns()
{
	auto& langs = AiTranslate::langs();
	if (fromIdx >= 0 && fromIdx < static_cast<int>(langs.size())) fromBtn->setText(langs[fromIdx].name);
	if (toIdx >= 0 && toIdx < static_cast<int>(langs.size())) toBtn->setText(langs[toIdx].name);
}

std::wstring WinAiTrans::fromCode() const
{
	auto& langs = AiTranslate::langs();
	return (fromIdx >= 0 && fromIdx < static_cast<int>(langs.size())) ? langs[fromIdx].code : L"auto";
}

std::wstring WinAiTrans::toCode() const
{
	auto& langs = AiTranslate::langs();
	return (toIdx >= 0 && toIdx < static_cast<int>(langs.size())) ? langs[toIdx].code : L"zh";
}

void WinAiTrans::refreshResult(const std::wstring& text)
{
	resultLabel->setText(text);
}

void WinAiTrans::setBusy(const bool on)
{
	busy = on;
	runBtn->setText(Lang::get(on ? L"ai.stop" : L"ai.transBtn"));
}

void WinAiTrans::run()
{
	auto text = input->getText();
	// 首尾空白不算内容：OCR 出来的文字常常带一堆换行，全送去翻就是白白花钱
	while (!text.empty() && (text.front() == L'\n' || text.front() == L' ' || text.front() == L'\r')) text.erase(0, 1);
	while (!text.empty() && (text.back() == L'\n' || text.back() == L' ' || text.back() == L'\r')) text.pop_back();
	if (text.empty()) {
		statusLabel->setText(Lang::get(L"ai.transEmpty"));
		return;
	}
	if (!AiTranslate::ready()) {
		statusLabel->setText(Lang::get(L"ai.noKey"));
		return;
	}
	statusLabel->setText(Lang::get(L"ai.translating"));
	refreshResult(L"");
	result.clear();
	setBusy(true);

	auto aliveFlag = alive;
	task = AiTranslate::run(text, fromCode(), toCode(),
		[this, aliveFlag](const std::wstring& text2, const std::wstring& detected, const std::wstring& err) {
			if (!*aliveFlag) return;
			task.reset();
			setBusy(false);
			result = text2;
			refreshResult(text2);
			if (err.empty()) {
				statusLabel->setText(detected.empty() ? L""
					: Lang::get(L"ai.transDetected") + AiTranslate::langName(detected));
			}
			else {
				statusLabel->setText(err);
			}
		});
}
