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
	initLineAlphaCtrl();
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

void WinSettingBall::initLineAlphaCtrl()
{
	// 横线透明度：0 = 整条线看不见（只剩悬浮球图标那一列），255 = 全不透明（原样）。
	// buildBody 里 lineBox->setBg 读的就是这个值
	auto box = makeChild<Ling::Node>();
	box->setHeight(39.f);
	box->setFlexDirection(Ling::FlexDirection::Row);
	box->setAlignItems(Ling::Align::Center);

	auto label = box->makeChild<Ling::Label>();
	label->setText(Lang::get(L"setting.ball.lineAlpha"));
	label->setFontSize(13.f);
	label->setHeightPercent(100.f);
	label->setJustifyContent(Ling::Justify::Center);
	label->setWidth(110.f);
	label->setFlexShrink(0.f);

	auto s = box->makeChild<Ling::Slider>();
	s->setHeight(12.f);
	s->setFlexGrow(1.f);
	s->setMarginLeft(10.f);
	s->setMarginRight(8.f);
	auto alpha = Setting::get()->getToolNum(L"ball", L"lineAlpha", 255.f);
	s->setRange(0.f, 255.f);
	s->setValue((float)std::clamp((int)alpha, 0, 255));
	s->setStep(1.f);
	s->setThumbColor(0x888888FF);
	s->setHoverThumbColor(0x888888FF);
	s->setTrackColor(0xD9D9D9FF);
	s->setFillColor((Setting::getThemeColor() << 8) | 0xFF);

	auto num = box->makeChild<Ling::Label>();
	num->setFontSize(12.f);
	num->setColor(0x555555FF);
	num->setWidth(34.f);
	num->setAlignItems(Ling::Align::FlexEnd);
	num->setFlexShrink(0.f);
	num->setText(std::to_wstring((int)alpha));

	// setValue 排在 onValueChanged 之前：填初值这一下不会反过来又写一次盘
	s->onValueChanged.add([num](Ling::Slider*, float v) {
		int iv = (int)std::lround(v);
		num->setText(std::to_wstring(iv));
		Setting::get()->setToolNum(L"ball", L"lineAlpha", (float)iv);
		// 细线是 buildBody 里现取这个值画的，重建一遍悬浮球节点树就换过来了
		WinBall::reload();
		});

	auto border = makeChild<Ling::Node>();
	border->setHeight(1.f);
	border->setBg(0xE0E0E0FF);
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
	// 开 / 关两套配色，与设置页其他开关、ToolSub::applyToggleStyle 保持一致。
	// 「开」态 = 主题色（原来是写死的 0x597ef7）
	btn->setText(on ? L"\ue688" : L"\ue687");
	const unsigned int accent = (Setting::getThemeColor() << 8) | 0xFF;
	btn->setColor(on ? accent : 0x666666FF);
	btn->setHoverColor(on ? accent : 0x666666FF);
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
