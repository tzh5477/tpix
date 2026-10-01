#include "pch.h"
#include "../Win/WinCap.h"
#include "../Lang.h"
#include "../Tip.h"
#include "ToolCapStage.h"

// 码位与提示 key 都从 ToolCap 那份表里挪过来，一个不改：
// 用户从截图那一刻起看到的图标就没变过，换窗口不该顺带换图标
const std::vector<ToolCapStage::BtnDef> ToolCapStage::stageBtns{
	{ L"long",    L"\ue73e", L"cap.long" },
	{ L"video",   L"\ue660", L"cap.video" },
	{ L"ocr",     L"\ue67b", L"cap.ocr" },
	{ L"qrcode",  L"\ue71e", L"cap.qrcode" },
};

ToolCapStage::ToolCapStage(WinCap* win) : Ling::WinBase(), win(win)
{
	// 与 ToolCap 一致：dpi 从宿主取，先按它把尺寸定下来
	dpi = win->dpi;
	refreshSize();
	// 按钮点击归这边管，但 ESC 之类要与 ToolCap 一样落回 WinCap
	onKeyDown.add([this](UINT key) { this->win->onKeyDown(key); });
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (!dpiChanged) return;
		dpiChanged = false;
		refreshSize();
		this->win->layoutToolSide(this);
	});
}

ToolCapStage::~ToolCapStage()
{
}

// 竖排一条：宽一个格子，高按按钮数
void ToolCapStage::refreshSize()
{
	setSize(btnSize, btnSize * (float)stageBtns.size());
}

void ToolCapStage::onCreated()
{
	tip = std::make_unique<Tip>(this);
	body->setBg(0xFFFFFFFF);
	body->setBorder(1.f, 0xA8A8A8ff);
	body->setFlexDirection(Ling::FlexDirection::Column);
	for (auto& def : stageBtns)
	{
		makeBtn(body, def.id, def.code, def.tip);
	}
	refreshSize();
	show();
}

Ling::Button* ToolCapStage::makeBtn(Ling::Node* parent, const std::wstring& id,
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

void ToolCapStage::onClick(Ling::Button* btn)
{
	// 提示先收掉：这几条路都是马上换阶段或者弹窗，屏幕上不能留着它
	tip->hide();
	auto& id = btn->id;
	if (id == L"long") {
		win->startLong();
	}
	else if (id == L"video") {
		win->startVideo();
	}
	else if (id == L"ocr") {
		win->startOcr();
	}
	else if (id == L"qrcode") {
		win->startQrcode();
	}
}

void ToolCapStage::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}
