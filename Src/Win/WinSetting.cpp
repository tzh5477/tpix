#include "pch.h"
#include <filesystem>
#include "../App.h"
#include "../Lang.h"
#include "../SelectPopup.h"
#include "WinSetting.h"
#include "WinSettingCommon.h"
#include "WinSettingShortcut.h"
#include "WinSettingAbout.h"

std::unique_ptr<WinSetting> winSetting;

WinSetting::WinSetting() :Ling::WinBase()
{
	// 关窗按钮是 body 的子节点，而 close() 正是从它的点击回调里一路进来的 ——
	// 在那里同步 winSetting.reset() 就是 use-after-free，所以推迟到下一轮消息循环。
	// 不放掉的话这个对象会一直活着，Ling 那边就永远看不到"一个窗口都不剩"，D2D 设备
	// 也就永远还不回去
	onDestroy.add([]() {
		Ling::App::get()->dq.TryEnqueue([]() { winSetting.reset(); });
	});
	setTitle(Lang::get(L"setting.title"));
	// 通用设置每一行是定高的（连分隔线 40），既不压缩也不滚动，
	// 所以窗口高度得跟着行数走 —— 拦在底部的那几行点不到，等于没做。
	// 加一行就把这个数 +40。但再高也不许超过工作区：960 在 1080p 上带任务栏就出屏了，
	// 上半截连拖都拖不到，超出的部分交给内容区的 ScrollerBox 滚出来
	RECT wa{};
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
	auto workH = (wa.bottom - wa.top) / dpi;
	setSize(680, std::min(960.f, workH - 40.f));
	setCenter();
	createNativeWindow();
}

WinSetting::~WinSetting()
{

}

void WinSetting::init()
{
	// 已经开着就拉到前台，不建第二个。原来是"关掉旧的再建新的"，那样会和上面那个
	// 延迟释放撞车：排在队列里的 reset 跑起来时放掉的是刚建好的这一个
	if (winSetting) {
		SetForegroundWindow(winSetting->hwnd);
		return;
	}
	winSetting.reset(new WinSetting());
}

void WinSetting::dispose()
{
	winSetting.reset();
}

void WinSetting::makeContent(int index)
{
	// 滚动容器连同内容一起销毁重建，滚动位置随之回到顶（切菜单回顶也合理）。
	// 不复用容器逐个换 child：ScrollerBox::setChild 把子节点挂到自己的 content 下，
	// ownership 却记在容器的 children 里，跨层 removeChild 要同时拆 yoga 与 visual 两棵树，
	// 容易留残影；重建容器则两条路都干净
	if (scroller) body->removeChild(scroller);
	scroller = body->makeChild<Ling::ScrollerBox>();
	scroller->setFlexGrow(1.f);
	scroller->setWidthPercent(100.f);
	// 内容保持自然高度（超出容器才滚动），所以不再给它设 flexGrow / 百分比
	if (index == 0) {
		content = scroller->makeChild<WinSettingCommon>();
	}
	else if (index == 1) {
		content = scroller->makeChild<WinSettingShortcut>();
	}
	else {
		content = scroller->makeChild<WinSettingAbout>();
	}
	content->setPaddingTop(40.f);
	content->setPadding(20.f, 40.f, 20.f, 40.f);
	content->setFlexDirection(Ling::FlexDirection::Column);
}

void WinSetting::onCreated()
{
	enableShadow();
	body->setBg(0xFAFAFAFF);
	body->setFlexDirection(Ling::FlexDirection::Row);
	auto menuBox = body->makeChild<Ling::Node>();
	menuBox->setBg(0xEEEEF0FF);
	menuBox->setWidth(160.f);
	menuBox->setHeightPercent(100.f);
	menuBox->setPaddingTop(40.f);
	initMenuItems(menuBox);

	makeContent(0);

	auto closeBtn = body->makeChild<Ling::Button>();
	closeBtn->setSize(42.f, 32.f);
	closeBtn->setPositionType(Ling::Position::Absolute);
	closeBtn->setPosition(Ling::Edge::Right, 0);
	closeBtn->setPosition(Ling::Edge::Top, 0);
	closeBtn->setHoverColor(0xFFFFFFFF);
	closeBtn->setHoverBg(0xE81123FF);
	closeBtn->setText(L"\ue62d");
	closeBtn->setFontFamily(L"icon");
	closeBtn->onClick.add([](Ling::Button* btn) {
		btn->win->close();
		});
	show();
}
void WinSetting::initMenuItems(Ling::Node* menuBox)
{
	for (size_t i = 0; i < 3; i++)
	{
		auto menuItem = menuBox->makeChild<Ling::Button>();
		menuItem->setFontSize(14.f);
		menuItem->setHeight(40.f);
		if (i == 0) {
			menuItem->setColor(0xFFFFFFFF);
			menuItem->setBg(0x597ef7ff);
			menuItem->setHoverColor(0xFFFFFFFF);
			menuItem->setHoverBg(0x597ef7ff);
			menuItem->setText(Lang::get(L"setting.common"));
		}
		else {
			menuItem->setHoverColor(0x000000ff);
			menuItem->setHoverBg(0xE1E1E3ff);
			if (i == 1) {
				menuItem->setText(Lang::get(L"setting.shortcut"));
			}
			else if (i == 2) {
				menuItem->setText(Lang::get(L"setting.about"));
			}
		}
		menuItem->onClick.add([this](auto menuItem) {this->onMenuItemClick(menuItem);});
		menus.push_back(menuItem);
	}
}
void WinSetting::onMenuItemClick(Ling::Button* menuItem)
{
	auto index = Ling::Util::getIndex(menus, menuItem);
	if (index < 0 || index == menuIndex) return;
	// 通用设置里弹出的下拉列表是独立窗口，content 被换掉它不会跟着消失，
	// 所以切菜单之前先收掉
	if (menuIndex == 0) {
		SelectPopup::close();
	}
	auto oldItem = menus[menuIndex];
	oldItem->setColor(0x333333FF);
	oldItem->setBg(0x00000000);
	oldItem->setHoverColor(0x000000ff);
	oldItem->setHoverBg(0xE1E1E3ff);
	menuIndex = index;
	menuItem->setColor(0xFFFFFFFF);
	menuItem->setBg(0x597ef7ff);
	menuItem->setHoverColor(0xFFFFFFFF);
	menuItem->setHoverBg(0x597ef7ff);

	makeContent(menuIndex);
}

LRESULT WinSetting::onHitTest(const POINT pos)
{
	POINT pt = pos;
	ScreenToClient(hwnd, &pt);
	if (!isMaximized) {
		auto result = borderHitTest(pt);
		if (result != HTCLIENT) return result;
	}
	if (pt.x > 0 && pt.y > 0 && pt.x < w - 32 * dpi && pt.y < 40 * dpi) {
		return HTCAPTION;
	}
	if (pt.x > 0 && pt.y > 40*4*dpi && pt.x < 120 * dpi && pt.y < h) {
		return HTCAPTION;
	}
	return HTCLIENT;
}


