#include "pch.h"
#include "Tray.h"
#include "App.h"
#include "Lang.h"
#include "Win/WinCap.h"
#include "Win/WinSetting.h"
#include "Win/WinHistory.h"
#include "Win/WinBall.h"
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
}

Tray::Tray()
{
	auto lingApp = Ling::App::get();
	lingApp->initTray(100, L"Screen Capture");
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
		if (WinBall::hasBall()) {
			Setting::get()->setToolFlag(L"ball", L"show", false);
			WinBall::dispose();
		}
		else {
			Setting::get()->setToolFlag(L"ball", L"show", true);
			WinBall::init();
		}
	}
	else if (menuId == settingMsg)
	{
		WinSetting::init();
	}
	else if (menuId == exitMsg)
	{
		Ling::App::get()->quit(0);
	}
}
