#include "pch.h"
#include "Tray.h"
#include "App.h"
#include "Lang.h"
#include "Win/WinCap.h"
#include "Win/WinSetting.h"
#include "Win/WinHistory.h"
#include "Win/WinBall.h"
#include "Win/WinOverlay.h"
#include "PinSource.h"
#include "Setting.h"

namespace {
	static std::unique_ptr<Tray> trayIns;
	static constexpr UINT settingMsg = 163;
	static constexpr UINT exitMsg = 164;
	static constexpr UINT historyMsg = 165;
	static constexpr UINT pinClipMsg = 166;
	static constexpr UINT pinFileMsg = 167;
	static constexpr UINT ballMsg = 168;
	static constexpr UINT rulerMsg = 169;
	static constexpr UINT crosshairMsg = 170;
	static constexpr UINT focusMsg = 171;
}

Tray::Tray()
{
	auto lingApp = Ling::App::get();
	// 托盘悬停时显示的名字 = 产品名（改名之后这里漏了，一直是旧的 Screen Capture）
	lingApp->initTray(100, L"tpix");
	Setting::get()->initShortcutKeys();
	// 左键单击 / 双击 都进入截图
	lingApp->onTrayMouseEvent.add([this](bool isDown, bool isRight) {
		if (isDown && !isRight) {
			WinCap::init();
		}
		else if (isDown && isRight) {
			this->onTrayRightClick();
		}
	});
}

Tray::~Tray()
{
}

void Tray::init()
{
	auto ptr = new Tray();
	trayIns.reset(ptr);
}

Tray* Tray::get()
{
	return trayIns.get();
}

void Tray::onTrayRightClick()
{
	auto menu = CreatePopupMenu();
	AppendMenu(menu, MF_STRING, historyMsg, Lang::get(L"tray.history").data());
	AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenu(menu, MF_STRING, pinClipMsg, Lang::get(L"tray.pinClip").data());
	AppendMenu(menu, MF_STRING, pinFileMsg, Lang::get(L"tray.pinFile").data());
	// 悬浮球开关用勾选态表示"当前开着"，不另写"显示 / 隐藏"两套文案
	AppendMenu(menu, MF_STRING | (WinBall::hasBall() ? MF_CHECKED : MF_UNCHECKED),
		ballMsg, Lang::get(L"tray.ball").data());
	// 三个屏幕辅助层都是开关：开着的那一项打勾，再点一次就是关
	AppendMenu(menu, MF_STRING | (WinOverlay::isOpen(OverlayMode::Ruler) ? MF_CHECKED : MF_UNCHECKED),
		rulerMsg, Lang::get(L"tray.ruler").data());
	AppendMenu(menu, MF_STRING | (WinOverlay::isOpen(OverlayMode::Crosshair) ? MF_CHECKED : MF_UNCHECKED),
		crosshairMsg, Lang::get(L"tray.crosshair").data());
	AppendMenu(menu, MF_STRING | (WinOverlay::isOpen(OverlayMode::Focus) ? MF_CHECKED : MF_UNCHECKED),
		focusMsg, Lang::get(L"tray.focus").data());
	AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenu(menu, MF_STRING, settingMsg, Lang::get(L"tray.setting").data());
	AppendMenu(menu, MF_STRING, exitMsg, Lang::get(L"tray.exit").data());
	auto menuId = Ling::App::get()->popupMenu(menu);
	if (menuId == historyMsg)
	{
		WinHistory::init();
	}
	else if (menuId == pinClipMsg)
	{
		PinSource::fromClipboard();
	}
	else if (menuId == pinFileMsg)
	{
		// 托盘没窗口句柄，拿桌面当属主；对话框会自己弹到屏幕中间
		PinSource::fromFile(GetDesktopWindow());
	}
	else if (menuId == ballMsg)
	{
		WinBall::toggle();
	}
	else if (menuId == rulerMsg)
	{
		WinOverlay::toggle(OverlayMode::Ruler);
	}
	else if (menuId == crosshairMsg)
	{
		WinOverlay::toggle(OverlayMode::Crosshair);
	}
	else if (menuId == focusMsg)
	{
		WinOverlay::toggle(OverlayMode::Focus);
	}
	else if (menuId == settingMsg)
	{
		// 菜单刚收，这会儿还压在托盘的消息派发里。隔一拍再建窗口 ——
		// 与 WinSettingCommon 里"切语言 / 导入配置后重开设置"是同一个做法
		Ling::App::get()->dq.TryEnqueue([]() { WinSetting::init(); });
	}
	else if (menuId == exitMsg)
	{
		Ling::App::get()->quit(0);
	}
}
