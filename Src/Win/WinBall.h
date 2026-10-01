#pragma once
#include <include/Ling.h>
#include <shellapi.h>
#include <string>
#include <vector>

class Tip;

// 悬浮球：常驻屏幕边缘的一条细线，鼠标移上去展开成一排操作图标，移开就收回去。
// 贴左 / 右边时图标竖排，贴顶边时横排；拖那条细线可以换边，摆哪几个图标在设置里勾。
//
// 图片文件拖到它上面直接贴图（走 WM_DROPFILES，见 .cpp 里的说明）：资源管理器拖出来的
// 本来就是文件路径，不必为了 OLE 的 IDropTarget 去动 Ling —— Ling 也不转发这条消息，
// 只能自己挂一层窗口过程。
class WinBall : public Ling::WinBase
{
public:
	~WinBall();
	// 配置里开着才建。重复调用只建一次
	static void init();
	static void dispose();
	static bool hasBall();
	// 开关：开着就收掉并记住"关"，关着就建出来。托盘菜单与全局快捷键共用这一份，
	// 免得两处各自写一遍、将来改了一边忘了另一边
	static void toggle();
	// 设置页改完操作项 / 贴边之后调：节点树是照着旧配置建的，不重建就会新旧两套混在一起
	static void reload();
	// 拖到球上的文件，由 .cpp 里接管的那个窗口过程转进来
	void onDropFiles(HDROP drop);
private:
	// 贴着哪条边。折叠态那条细线就贴着它，展开时窗口往屏幕里长
	enum class Edge { Left = 0, Right = 1, Top = 2 };
	WinBall();
	void onCreated() override;
	// 折叠态只有 100×5，比 Ling 默认的 800×600 最小跟踪尺寸小两个数量级。
	// 不放开的话 applyGeometry 里的 SetWindowPos 会被系统按回 800×600：窗口左边缘
	// 还贴在"屏幕右边减 5 像素"处，红线按 justify 排在窗口右端，整条线跑到屏幕外去了
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	void onDown(POINT pos, bool isRight);
	void onMove(POINT pos);
	void onUp();
	// 按当前 edge 与展开状态重算窗口尺寸和位置，并把它摆到位
	void applyGeometry();
	void expand();
	void collapse();
	// 拖动结束：往离得最近的那条边靠，并把落点记进配置
	void snapAndSave();
	// 沿边方向的坐标夹进工作区，细线不该被拖到屏幕外面去
	int clampAlong(const int value) const;
	// 按 edge 与配置重建整棵节点树（换边 / 改勾选之后都得走一遍）
	void rebuildBody();
	void buildBody();
	// 给图标按钮挂名字提示。气泡不能往屏幕外弹，方向得按 edge 定
	void bindTip(Ling::Button* btn, const std::wstring& name);
	void runAction(const std::wstring& id);
	// 图标上点右键：滚动截图切方向、延时截图改秒数
	void showActionMenu(const std::wstring& id, Ling::Button* anchor);
	// 细线上点右键
	void showBallMenu();
	void hideSelf();
	// pos 落在哪个图标上，没有就返回 nullptr
	Ling::Button* itemAt(POINT pos) const;
	// 工作区。不用 RECT 是因为它的成员是 LONG，跟 int 一起丢进 std::clamp
	// 会推不出模板参数
	struct Area { int left{ 0 }, top{ 0 }, right{ 0 }, bottom{ 0 }; };
	// 取细线所在那块屏幕的工作区
	Area workArea() const;
	void storePosition();
private:
	Edge edge{ Edge::Right };
	// 沿边方向的中点（屏幕物理像素）：竖排存 y，横排存 x。
	// 折叠与展开都围着它对齐，所以细线在展开前后不会跳
	int along{ 0 };
	bool expanded{ false };
	bool isMouseDown{ false };
	bool hasDragged{ false };
	bool dpiChanged{ false };
	POINT pressPos{ 0, 0 };
	int dragWinX{ 0 }, dragWinY{ 0 };
	Ling::Node* lineBox{ nullptr };
	Ling::Node* itemBox{ nullptr };
	std::vector<Ling::Button*> itemBtns;
	std::vector<std::wstring> itemIds;
	std::unique_ptr<Tip> tip;
};
