#include "pch.h"
#include "../Win/WinPin.h"
#include "../Lang.h"
#include "../Tip.h"
#include "ToolPinSide.h"

const std::vector<ToolPinSide::BtnDef> ToolPinSide::sideBtns{
	{ L"long",    L"\ue73e", L"cap.long" },
	{ L"video",   L"\ue660", L"cap.video" },
	{ L"ocr",     L"\ue67b", L"cap.ocr" },
	{ L"qrcode",  L"\ue71e", L"cap.qrcode" },
};

ToolPinSide::ToolPinSide(WinPin* win) : Ling::WinBase(), win(win)
{
	// 与 ToolMain 一致：dpi 从宿主取（贴图窗口可能在另一块缩放比例不同的屏上），
	// 位置先按宿主右边缘估一个，真正的落点由 WinPin::layoutTools 统一决定
	dpi = win->dpi;
	x = (int)(win->x + win->w + 5.f * win->dpi);
	y = win->y;
	refreshSize();
	// 点按钮会把本窗口激活，此后键盘消息进的是它 —— 与 ToolMain / ToolSub 一样照转给 WinPin，
	// 这样 ESC、Ctrl 那些在两个窗口上表现一致
	onKeyDown.add([this](UINT key) { this->win->onKeyDown(key); });
	onKeyUp.add([this](UINT key) { this->win->onKeyUp(key); });
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (!dpiChanged) return;
		dpiChanged = false;
		refreshSize();
		this->win->layoutTools();
	});
	createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
}

ToolPinSide::~ToolPinSide()
{
}

// 竖排一条：宽一个格子，高按按钮数
void ToolPinSide::refreshSize()
{
	setSize(btnSize, btnSize * (float)sideBtns.size());
}

void ToolPinSide::onCreated()
{
	tip = std::make_unique<Tip>(this);
	body->setBg(0xFFFFFFFF);
	body->setBorder(1.f, 0xA8A8A8ff);
	body->setFlexDirection(Ling::FlexDirection::Column);
	for (auto& def : sideBtns)
	{
		makeBtn(body.get(), def.id, def.code, def.tip);
	}
	refreshSize();
	show();
}

Ling::Button* ToolPinSide::makeBtn(Ling::Node* parent, const std::wstring& id,
	const std::wstring& code, const std::wstring& tipKey)
{
	auto btn = parent->makeChild<Ling::Button>();
	btn->setId(id);
	btn->setText(code);
	btn->setHeight(btnSize);
	btn->setWidthPercent(100.f);
	btn->setFontFamily(L"icon");
	btn->setFontSize(13.f);
	btn->setHoverBg(0xF2F2F2ff);
	btn->onClick.add([this](Ling::Button* btn) { onClick(btn); });
	if (!tipKey.empty()) {
		tip->bind(btn, Lang::get(tipKey));
	}
	return btn;
}

void ToolPinSide::onClick(Ling::Button* btn)
{
	// 提示先收掉：这几条路要么收走本窗口、要么弹出新窗口，屏幕上不能留着它
	tip->hide();
	auto& id = btn->id;
	if (id == L"long") {
		win->startLongTask();
	}
	else if (id == L"video") {
		win->startVideoTask();
	}
	else if (id == L"ocr") {
		win->ocrToWindow();
	}
	else if (id == L"qrcode") {
		win->decodeQr();
	}
}

void ToolPinSide::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}
