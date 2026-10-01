#include "pch.h"
#include <thread>
#include "../Lang.h"
#include "../Ocr.h"
#include "../Setting.h"
#include "../ShotHistory.h"
#include "WinOcr.h"

namespace {
	std::unique_ptr<WinOcr> winOcr;
}

WinOcr::WinOcr(std::vector<BYTE>&& data, const int w, const int h)
	: Ling::WinBase(), pixels(std::move(data)), imgW(w), imgH(h)
{
	// 关窗按钮的点击栈上不能同步 reset（use-after-free），推迟到下一轮消息循环
	onDestroy.add([]() {
		Ling::App::get()->dq.TryEnqueue([]() { winOcr.reset(); });
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
	langBtn->onClick.add([this](Ling::Button*) {
		if (langs.empty()) return;
		langIndex = (langIndex + 1) % static_cast<int>(langs.size() + 1);
		Setting::get()->setToolStr(L"ocr", L"lang", curLangTag());
		applyLangBtn();
		startRecognize();
	});
	// 记住上次选的；没装的语言包（卸载了）认不出来，退回跟随系统
	auto saved = Setting::get()->getToolStr(L"ocr", L"lang", L"");
	if (!saved.empty()) {
		for (int i = 0; i < static_cast<int>(langs.size()); ++i) {
			if (langs[i].tag == saved) { langIndex = i + 1; break; }
		}
	}
	applyLangBtn();

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
		Ling::Util::setTextToClipboard(result);
		ShotHistory::get()->skipNextClipboard();
	});

	show();
	startRecognize();
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
	box->setText(Lang::get(L"ocr.recognizing"));
	// 往线程里拷一份而不是搬走：换语言会再识别一次，pixels 得一直留在成员里。
	// 线程里只拿 this 比地址、不 dereference —— 识别跑到一半窗口可能已经关了
	std::thread([this, data = pixels, w = imgW, h = imgH, lang = std::move(lang), seq]() mutable {
		// 新线程里没有 WinRT 单元，不初始化就用不了 OcrEngine
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
		auto text = Ocr::recognize(w, h, data.data(), lang);
		Ling::App::get()->dq.TryEnqueue(
			[this, seq, text = std::move(text)]() mutable {
				// 排在自己前面的可能正是"窗口已关闭"那次 reset，那之后 winOcr 已经不是本窗口了；
				// 也可能有更新的识别已经出结果，老结果不该盖上去
				if (winOcr.get() == this && taskSeq == seq) setResult(text);
			});
	}).detach();
}

void WinOcr::setResult(const std::wstring& text)
{
	result = text;
	box->setText(text.empty() ? Lang::get(L"ocr.empty") : text);
}
