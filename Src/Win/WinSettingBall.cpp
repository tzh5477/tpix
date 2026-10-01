#include "pch.h"
#include <algorithm>
#include "../Lang.h"
#include "../Setting.h"
#include "WinBall.h"
#include "WinSettingBall.h"

WinSettingBall::WinSettingBall(Ling::WinBase* parent) : Ling::Node(parent)
{
	picked = ballParseActions(Setting::get()->getToolStr(L"ball", L"actions", ballDefaultActions()));
	initShowCtrl();
	initActionCtrls();
}

WinSettingBall::~WinSettingBall()
{
}

Ling::Node* WinSettingBall::makeRow(const std::wstring& labelKey)
{
	auto box = makeChild<Ling::Node>();
	box->setHeight(39.f);
	box->setFlexDirection(Ling::FlexDirection::Row);
	box->setAlignItems(Ling::Align::Center);

	auto label = box->makeChild<Ling::Label>();
	label->setText(Lang::get(labelKey));
	label->setHeightPercent(100.f);
	label->setJustifyContent(Ling::Justify::Center);
	label->setFlexGrow(1.f);

	auto border = makeChild<Ling::Node>();
	border->setHeight(1.f);
	border->setBg(0xE0E0E0FF);
	return box;
}

void WinSettingBall::initShowCtrl()
{
	auto row = makeRow(L"setting.ball.show");
	auto btn = row->makeChild<Ling::Button>();
	btn->setFontFamily(L"icon");
	btn->setHeightPercent(100.f);
	btn->setFontSize(18.f);
	btn->setWidth(60.f);
	applySwitch(btn, Setting::get()->getToolFlag(L"ball", L"show", false));
	btn->onClick.add([this](Ling::Button* b) {
		auto next = !Setting::get()->getToolFlag(L"ball", L"show", false);
		Setting::get()->setToolFlag(L"ball", L"show", next);
		applySwitch(b, next);
		// 开关一动就建 / 收，不用等重启
		if (next) WinBall::init();
		else WinBall::dispose();
		});
}

void WinSettingBall::initActionCtrls()
{
	auto tip = makeChild<Ling::Label>();
	tip->setText(Lang::get(L"setting.ball.pickTip"));
	tip->setFontSize(12.f);
	tip->setColor(0x888888FF);
	tip->setHeight(34.f);
	tip->setJustifyContent(Ling::Justify::Center);

	for (auto& def : ballActionDefs()) {
		auto row = makeActionRow(def);
		auto btn = row->makeChild<Ling::Button>();
		btn->setFontFamily(L"icon");
		btn->setHeightPercent(100.f);
		btn->setFontSize(18.f);
		btn->setWidth(60.f);
		applySwitch(btn, isPicked(def.id));
		auto id = std::wstring{ def.id };
		btn->onClick.add([this, btn, id](Ling::Button*) {
			togglePick(id);
			applySwitch(btn, isPicked(id));
			});
	}
}

Ling::Node* WinSettingBall::makeActionRow(const BallActionDef& def)
{
	auto box = makeChild<Ling::Node>();
	box->setHeight(39.f);
	box->setFlexDirection(Ling::FlexDirection::Row);
	box->setAlignItems(Ling::Align::Center);

	// 把图标也画出来：名字是译名，图标才是展开条上真正看到的那个，
	// 对着名字勾容易勾错
	auto icon = box->makeChild<Ling::Label>();
	icon->setFontFamily(L"icon");
	icon->setFontSize(16.f);
	icon->setColor(0x555555FF);
	icon->setText(def.icon);
	icon->setWidth(26.f);
	icon->setHeightPercent(100.f);
	icon->setJustifyContent(Ling::Justify::Center);

	auto label = box->makeChild<Ling::Label>();
	label->setText(Lang::get(def.nameKey));
	label->setHeightPercent(100.f);
	label->setJustifyContent(Ling::Justify::Center);
	label->setFlexGrow(1.f);

	auto border = makeChild<Ling::Node>();
	border->setHeight(1.f);
	border->setBg(0xE0E0E0FF);
	return box;
}

void WinSettingBall::applySwitch(Ling::Button* btn, bool on)
{
	// 开 / 关两套配色，与设置页其他开关、ToolSub::applyToggleStyle 保持一致
	btn->setText(on ? L"\ue688" : L"\ue687");
	btn->setColor(on ? 0x597ef7ff : 0x666666FF);
	btn->setHoverColor(on ? 0x597ef7ff : 0x666666FF);
}

bool WinSettingBall::isPicked(const std::wstring& id) const
{
	return std::find(picked.begin(), picked.end(), id) != picked.end();
}

void WinSettingBall::togglePick(const std::wstring& id)
{
	auto it = std::find(picked.begin(), picked.end(), id);
	if (it == picked.end()) picked.push_back(id);
	else picked.erase(it);
	// 按定义表重新排一遍：勾选的先后顺序不该决定图标在展开条上的排列，
	// 而且这样也就顺带把重复项去掉了
	std::vector<std::wstring> ordered;
	for (auto& def : ballActionDefs()) {
		if (isPicked(def.id)) ordered.push_back(def.id);
	}
	picked = std::move(ordered);
	Setting::get()->setToolStr(L"ball", L"actions", ballJoinActions(picked));
	// 悬浮球是照着旧的那一份建的，不重建就还是老几个图标
	WinBall::reload();
}
