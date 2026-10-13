#include "pch.h"
#include <algorithm>
#include "../AiTranslate.h"
#include "../Lang.h"
#include "../Ocr.h"
#include "../Setting.h"
#include "../SelectPopup.h"
#include "WinAiTrans.h"

namespace {
	std::unique_ptr<WinAiTrans> winAiTrans;

	// 判断原文是不是中文。不能只扫"有没有汉字"——日文里本来就夹着汉字，那样会把日文
	// 当成中文；有假名或谚文就一律判成非中文，剩下有汉字的才算中文
	bool looksChinese(const std::wstring& text)
	{
		bool hasHan{ false };
		for (const wchar_t c : text) {
			if ((c >= 0x3040 && c <= 0x30FF) ||    // 平假名 / 片假名
				(c >= 0xAC00 && c <= 0xD7AF) ||    // 谚文音节
				(c >= 0x1100 && c <= 0x11FF))      // 谚文字母
				return false;
			if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF)) hasHan = true;
		}
		return hasHan;
	}
	// 把这枚窗口抬到同族 TOPMOST 窗口的最上面。直接再戴一次 TOPMOST 是不动的
	//（已经在最上层带里），必须先摘掉 NOTOPMOST 再戴回 TOPMOST 才抬得动 ——
	// 否则它盖不住同样置顶的文字识别窗，"点翻译"看起来像没反应
	void raiseToFront(HWND hwnd)
	{
		if (!hwnd || !IsWindow(hwnd)) return;
		SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		BringWindowToTop(hwnd);
	}
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
	// 必须置顶：它常从"同样置顶的 OCR 结果窗 / 编辑页"里被唤起，普通窗口永远
	// 压不过 TOPMOST 那一带 —— 翻译窗会被整个压在底下（截图实测）。与它们同族，
	// 谁后抬谁在上面
	createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
}

WinAiTrans::~WinAiTrans()
{
	*alive = false;
	if (task) task->cancel();
	// 唤起方（识别窗）为让这一扇独占屏幕把自己藏了，现在还给它 —— 摘了再戴 TOPMOST
	// 才抬得动（同族置顶窗排先后只有这一条路），正好也把它顶到最前
	if (ownerWin && ownerWin->hwnd && IsWindow(ownerWin->hwnd)) {
		ownerWin->show();
		raiseToFront(ownerWin->hwnd);
	}
}

void WinAiTrans::init(const std::wstring& preset, Ling::WinBase* owner)
{
	if (winAiTrans) {
		if (winAiTrans->hwnd && IsWindow(winAiTrans->hwnd)) {
			// 上一个唤起方还藏着的话先还给它（连着从两个窗口点翻译，前一个不能永远消失）
			if (winAiTrans->ownerWin && winAiTrans->ownerWin != owner
				&& winAiTrans->ownerWin->hwnd && IsWindow(winAiTrans->ownerWin->hwnd)) {
				winAiTrans->ownerWin->show();
			}
			winAiTrans->ownerWin = owner;
			winAiTrans->applyPreset(preset);
			winAiTrans->show();
			raiseToFront(winAiTrans->hwnd);
			SetForegroundWindow(winAiTrans->hwnd);
			return;
		}
		winAiTrans.reset();
	}
	winAiTrans.reset(new WinAiTrans());
	winAiTrans->ownerWin = owner;
	// 填预设必须排在构造之后：控件是在 onCreated 里建出来的，而在构造函数里就
	// createNativeWindow 把 onCreated 跑完了 —— 构造前存一份成员变量的老写法，
	// 读到它的时候永远是空的，"带原文进来"从来就没生效过
	winAiTrans->applyPreset(preset);
	if (winAiTrans->hwnd) {
		// createNativeWindow 建出来的窗口**不带 WS_VISIBLE**，不 show 就一直是隐藏的。
		// 这条新实例的路以前漏了 show()：第一次调 init 时窗口只被建出来、没露过面
		// （而且 winAiTrans 已经存在，下一次进来走的是上面那条分支，恰好又会 show ——
		// 所以症状是"第一次点没反应，第二次才有窗口"，特别像没反应）
		winAiTrans->show();
		raiseToFront(winAiTrans->hwnd);
		SetForegroundWindow(winAiTrans->hwnd);
	}
}

void WinAiTrans::applyPreset(const std::wstring& text)
{
	if (text.empty()) return;
	input->setText(text);
	run();
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

	// 状态字（"检测到：英语" / 错误提示）。占着 flexGrow，把行尾那枚按钮推到最右
	statusLabel = actRow->makeChild<Ling::Label>();
	statusLabel->setMarginLeft(8.f);
	statusLabel->setFlexGrow(1.f);
	statusLabel->setFontSize(12.f);
	statusLabel->setColor(0x888888FF);

	// 行尾那枚「翻译模型」：既是"这一单走的是哪条路"的指示，也是**切换入口**。
	// 两件事必须一起成立，否则就是"点了没反应"：
	//   ① 恒在。早先走火山引擎时把它藏了、换一行静态文字，而那行是 Label、点不动 ——
	//      用户点上去当然没有任何响应（作者报的就是这个）；
	//   ② 下拉里得有当前那条路。火山引擎没有"模型"概念，但它是一条独立的路，必须作为
	//      第 0 项列出来，否则正走火山时点开是一片空，更像坏了。
	// 候选只有两个：火山引擎 + **设置里「翻译使用」绑的那一个模型**（Ocr::aiModelChoices
	// 已按场景过滤，见 Ocr.cpp）。不再把"所有已配接口下的所有模型"铺出来 —— 那是设置页的
	// 活，这里只做"在设置绑的模型和火山之间换一下"，列表与人手一多就会误改设置页的绑定
	srcBtn = actRow->makeChild<Ling::Button>();
	srcBtn->setHeight(28.f);
	srcBtn->setWidth(170.f);
	srcBtn->setMarginLeft(8.f);
	srcBtn->setFontSize(13.f);
	srcBtn->setBorder(1.f, 0xE0E0E0FF);
	srcBtn->setHoverBg(0xF2F2F2FF);
	srcBtn->onClick.add([this](Ling::Button* b) {
		auto choices = Ocr::aiModelChoices(std::wstring{ AiScenario::translate });
		std::vector<std::wstring> labels;
		labels.reserve(choices.size() + 1);
		labels.push_back(std::wstring{ L"API " } + Lang::get(L"ai.providerVolc"));
		for (auto& c : choices) labels.push_back(c.label);
		// 高亮下标与 labels 同一套（火山引擎占 0，模型往后挪一位）
		SelectPopup::show(this, b, labels, curSrcIndex(choices), [this, choices](int idx) {
			if (idx < 0) return;
			auto setting = Setting::get();
			if (idx == 0) {
				setting->setAiStr(L"transProvider", L"volc");
			}
			else {
				if (idx - 1 >= (int)choices.size()) return;
				const auto& c = choices[idx - 1];
				setting->setAiStr(L"transProvider", L"model");
				// 只改「翻译」这一档绑的（接口 + 模型）：对话 / 识别 / 表格是各自单独绑的，别顺手覆盖
				setting->setScenario(std::wstring{ AiScenario::translate }, c.providerId, c.model);
			}
			applySrcBtn();
			// 换完立刻用新路把当前原文重翻一次 —— "换一个模型试试"就是这个动作
			if (!input->getText().empty()) run();
		}, {}, 220.f);
	});
	applySrcBtn();

	// 结果区。用 TextBox 而不是 Label：Label 既不会折行、也没法选中一段来复制，
	// 而译文常常很长。滚动由 TextBox 自己带，不必再套 ScrollerBox
	resultBox = body->makeChild<Ling::TextBox>();
	resultBox->setFlexGrow(1.f);
	resultBox->setWidthPercent(100.f);
	resultBox->setMarginTop(8.f);
	resultBox->setFontSize(14.f);
	resultBox->setPadding(10.f, 8.f, 10.f, 8.f);
	resultBox->setBg(0xF7F7F9FF);
	resultBox->setBorderRadius(6.f);

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

void WinAiTrans::setToLang(const std::wstring& code)
{
	auto& langs = AiTranslate::langs();
	for (int i = 0; i < static_cast<int>(langs.size()); ++i) {
		if (langs[i].code == code) { toIdx = i; break; }
	}
	applyLangBtns();
}

void WinAiTrans::refreshResult(const std::wstring& text)
{
	resultBox->setText(text);
}

void WinAiTrans::applySrcBtn()
{
	if (!srcBtn) return;
	auto setting = Setting::get();
	// 火山引擎那条是固定的一路机器翻译、没有模型概念，按钮上直接点明走的是哪一家
	if (setting->getAiStr(L"transProvider", L"volc") != L"model") {
		srcBtn->setText(std::wstring{ L"API " } + Lang::get(L"ai.providerVolc"));
		return;
	}
	// 大模型那条：显示「翻译」场景当前绑的模型名（与识别窗的「识别模型」同源）。
	// 一个接口都没配时给个占位把按钮画出来 —— 反正点开还有火山引擎那一项可选
	auto m = setting->getScenarioModel(std::wstring{ AiScenario::translate });
	srcBtn->setText(m.empty() ? Lang::get(L"ai.transModel") : m);
}

int WinAiTrans::curSrcIndex(const std::vector<OcrModelChoice>& choices) const
{
	if (Setting::get()->getAiStr(L"transProvider", L"volc") != L"model") return 0;
	const auto scenario = std::wstring{ AiScenario::translate };
	const auto pid = Setting::get()->getScenarioProvider(scenario);
	const auto model = Setting::get()->getScenarioModel(scenario);
	// 下标整体 +1：第 0 项被火山引擎占了
	for (int i = 0; i < (int)choices.size(); ++i) {
		if (choices[i].ai && choices[i].providerId == pid && choices[i].model == model) return i + 1;
	}
	// 绑的那个接口已经被删了之类的：光按模型名再找一遍，总比不高亮强
	for (int i = 0; i < (int)choices.size(); ++i) {
		if (choices[i].ai && choices[i].model == model) return i + 1;
	}
	// 一个模型都没配时，下拉里只有火山引擎那一项
	return choices.empty() ? 0 : 1;
}

void WinAiTrans::setBusy(const bool on)
{
	busy = on;
	runBtn->setText(Lang::get(on ? L"ai.stop" : L"ai.transBtn"));
}

void WinAiTrans::run()
{
	// 设置可能在窗口开着的时候被改过，每次发车前把行尾那枚（路 + 模型名）刷一遍
	applySrcBtn();
	auto text = input->getText();
	// 首尾空白不算内容：OCR 出来的文字常常带一堆换行，全送去翻就是白白花钱
	while (!text.empty() && (text.front() == L'\n' || text.front() == L' ' || text.front() == L'\r')) text.erase(0, 1);
	while (!text.empty() && (text.back() == L'\n' || text.back() == L' ' || text.back() == L'\r')) text.pop_back();
	if (text.empty()) {
		statusLabel->setText(Lang::get(L"ai.transEmpty"));
		return;
	}
	// 源语言是"自动检测"时，目标语言由原文定：中文→英语，非中文→中文。
	// 只在"目标正好是原文语言"时才换一下（那样译出来等于原文，没有意义），
	// 用户自己选的目标（比如英→日）不动它
	if (fromCode() == L"auto") {
		const std::wstring src{ looksChinese(text) ? L"zh" : L"en" };
		if (toCode() == src) setToLang(src == L"zh" ? L"en" : L"zh");
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
