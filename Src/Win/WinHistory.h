#pragma once
#include <include/Ling.h>
#include "../ShotHistory.h"

// 历史窗口：截图历史与剪贴板历史共用一套界面，顶部两个 tab 切换。
// 点条目 = 用它（图片贴图 / 文本进剪贴板），条目上的按钮做复制与删除。
class WinHistory : public Ling::WinBase
{
public:
	WinHistory();
	~WinHistory();
	static void init();
	static void dispose();
	// 主题色改了：两个 tab 的选中底色跟着换（卡片本身的配色不跟主题色走）
	static void applyTheme();
private:
	void onCreated() override;
	// 窗口是 WS_POPUP 没有标题栏，拖边缩放全靠 hit test 返回 HT_* 交给系统
	// （同 WinSetting::onHitTest 的做法）。顶栏全是控件，不给 HTCAPTION 拖拽区
	LRESULT onHitTest(const POINT pos) override;
	void initTabs(Ling::Node* parent);
	void fillList();
	// 两个 tab 的选中态配色。fillList 与 applyTheme 共用这一份 —— 各写一遍迟早对不上
	void applyTabStyle();
	// 一张卡片。图片条目画缩略图，文本条目画截断后的文字
	Ling::Node* makeCard(Ling::Node* parent, const ShotHistory::Item& item);
	// 三个操作都按 id 取条目：卡片回调只捕获 id，Item 本身在刷新列表时会被换掉
	void useItem(const std::wstring& id);
	void copyItem(const std::wstring& id);
	void removeItem(const std::wstring& id);
	const ShotHistory::Item* findItem(const std::wstring& id) const;
	// 毫秒时间戳 -> "MM-DD HH:MM"
	static std::wstring timeText(long long ms);
private:
	ShotHistory::Source curSource{ ShotHistory::Source::Shot };
	Ling::ScrollerBox* scroller{ nullptr };
	Ling::Button* tabShot{ nullptr };
	Ling::Button* tabClip{ nullptr };
	// 每条历史对应卡片上那两个按钮的点击回调要按 id 找回条目，
	// 而 Item 是按值存进卡片的，刷新列表时会被整体重建，所以这里留一份当前的条目表
	std::vector<ShotHistory::Item> curItems;
};
