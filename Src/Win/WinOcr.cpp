#include "pch.h"
#include <thread>
#include "../Lang.h"
#include "../Ocr.h"
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

void WinOcr::startRecognize()
{
	auto data = std::move(pixels);
	// 往线程里搬整张图：pixels 是本窗口的成员，窗口关掉它就没了，而识别还在跑
	std::thread([data = std::move(data), w = imgW, h = imgH]() mutable {
		// 新线程里没有 WinRT 单元，不初始化就用不了 OcrEngine
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
		auto text = Ocr::recognize(w, h, data.data());
		Ling::App::get()->dq.TryEnqueue([text = std::move(text)]() {
			// 排在自己前面的可能正是"窗口已关闭"那次 reset，此时不该再去碰界面
			if (winOcr) winOcr->setResult(text);
		});
	}).detach();
}

void WinOcr::setResult(const std::wstring& text)
{
	result = text;
	box->setText(text.empty() ? Lang::get(L"ocr.empty") : text);
}
