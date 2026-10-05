#pragma once
#include <include/Ling.h>

class WinSetting :public Ling::WinBase
{
public:
	~WinSetting();
	static void init();
	// 退出流程里调：窗口对象是文件级静态变量，交给静态析构就在 CoUninitialize 之后了
	static void dispose();
private:
	WinSetting();
	void initMenuItems(Ling::Node* menuBox);
	void onCreated() override;
	void makeContent(int index);
	void onMenuItemClick(Ling::Button* menu);
	LRESULT onHitTest(const POINT pos) override;
	// 把内容区子树的 Node::y 摆成「yoga 绝对坐标 - 滚动量」，供本轮鼠标事件的
	// 命中测试用。每次鼠标事件派发前调用，幂等。
	void syncScrollHitCoords();
private:
	std::vector<Ling::Button*> menus;
	int menuIndex{ 0 };
	// 内容区外面那层滚动容器。窗口高度被夹进工作区后放不下的行靠它滚出来
	Ling::ScrollerBox* scroller{ nullptr };
	Ling::Node* content{nullptr};
};

