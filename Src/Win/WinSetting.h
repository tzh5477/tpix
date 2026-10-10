#pragma once
#include <include/Ling.h>

class WinSettingCommon;

class WinSetting :public Ling::WinBase
{
public:
	~WinSetting();
	static void init();
	// 退出流程里调：窗口对象是文件级静态变量，交给静态析构就在 CoUninitialize 之后了
	static void dispose();
	// 主题色改了：设置页自己的强调色也要跟着换（左侧菜单选中项、各开关的「开」态、
	// 主题色块选中那圈描边）。没开着就什么都不做
	static void applyTheme();
private:
	WinSetting();
	void initMenuItems(Ling::Node* menuBox);
	void onCreated() override;
	void makeContent(int index);
	void onMenuItemClick(Ling::Button* menuItem);
	// 侧栏菜单项的两态配色。initMenuItems / onMenuItemClick / applyTheme 三处共用这一份 ——
	// 各写一遍的话，改主题色必然漏掉一处
	void applyMenuStyle(int index);
	void applyThemeNow();
	LRESULT onHitTest(const POINT pos) override;
	// 把内容区子树的 Node::y 摆成「yoga 绝对坐标 - 滚动量」，供本轮鼠标事件的
	// 命中测试用。每次鼠标事件派发前调用，幂等。
	void syncScrollHitCoords();
private:
	std::vector<Ling::Button*> menus;
	int menuIndex{ 0 };
	// 内容区那一列的容器：头带 + 滚动容器都在它里面。makeContent 换页时挂到它下面
	Ling::Node* contentCol{ nullptr };
	// 内容区外面那层滚动容器。窗口高度被夹进工作区后放不下的行靠它滚出来
	Ling::ScrollerBox* scroller{ nullptr };
	Ling::Node* content{ nullptr };
	// "通用设置"那页。applyTheme 要够得到它（开关与主题色块都在那页上）。
	// 换到别的页时置空 —— content 会被整块重建，留着就是悬垂指针
	WinSettingCommon* commonPage{ nullptr };
};

