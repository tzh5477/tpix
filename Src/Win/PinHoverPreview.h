#pragma once
#include <include/Ling.h>

class WinPin;
class PinHiddenBar;

// 书签条 hover 出来的浮层预览（回答"哪一格是哪张截图"）。
//
// 那条书签每格只有 4 逻辑像素厚，颜色又是按创建顺序从八色里轮转的 —— 藏到三四张就认不出来了。
// 原来唯一的辨认办法是"hover 即整窗 peek"：能看清，代价是那一整张（常常是全屏）贴图跟着闪一下，
// 顺着条扫一遍就是一串整屏闪。于是插一层安静的预览：
//   hover 一格 → 立刻在条旁边弹出这张图等比缩到 ≤240x180 的小预览（不抢焦点、不吃鼠标）。
//   整窗预览不再由 hover 自动升级（避免顺着条扫过去一串整屏闪）；要看整图改用单击「放回桌面」。
// 于是"认图"是安静的，整窗交给显式单击。
//
// 为什么是独立窗口：贴图这会儿是 hide 着的，没有窗口可画；而且要能压在别的 topmost 之上。
// 与 ToolPinSide 同一套窗口参数：顶层 + 不激活 + 不在任务栏留一枚。
class PinHoverPreview : public Ling::WinBase
{
public:
	PinHoverPreview();
	~PinHoverPreview();
	// pos 是浮层左上角的落点（屏幕像素，调用方按条的方向摆好，本类只负责钳进工作区）。
	// dpiVal 取书签条的：两条都贴在同一个角上，显示器不会不一样
	void showPreview(WinPin* pin, POINT pos, float dpiVal);
	void hidePreview();
	// 是谁（哪条边的书签条）唤起本浮层的。左键单击浮层 = 把那张放回桌面，这个回调指回去
	// 让书签条执行恢复；浮层整进程一枚、两条边共用，每次 showPreview 都重写
	PinHiddenBar* owner{ nullptr };
private:
	void onCreated() override;
	// 尺寸是每次换图都可能变的，真正有像素尺寸要等 layout 排过一次；画面在这里补画
	void layout() override;
	// 本窗只有几百像素，Ling 默认的最小跟踪尺寸是 800x600，不放开就建不出来
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	// 把底图等比画进画布。缩的是"看的倍数"，不动 drawing 里任何东西
	void paintPreview();
private:
	// 自绘画布。整扇窗口就它一块，大小随 setSize 铺满
	Ling::Canvas* surface{ nullptr };
	// 当前预览着哪一张。nullptr = 窗口已经收着，别画
	WinPin* cur{ nullptr };
	// 预览的上限（逻辑像素）。取 240x180：够看清一张截图是哪个窗口，
	// 又不到"整屏闪"的地步 —— 与 ToolPinSide 那种小窗同一个量级
	static constexpr float maxW{ 240.f };
	static constexpr float maxH{ 180.f };
};
