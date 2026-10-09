#pragma once
#include <include/Ling.h>

class WinPin;

// 书签条 hover 出来的浮层预览（回答"哪一格是哪张截图"）。
//
// 那条书签每格只有 4 逻辑像素厚，颜色又是按创建顺序从八色里轮转的 —— 藏到三四张就认不出来了。
// 原来唯一的辨认办法是"hover 即整窗 peek"：能看清，代价是那一整张（常常是全屏）贴图跟着闪一下，
// 顺着条扫一遍就是一串整屏闪。于是中间插一层安静的：
//   hover 一格 → 立刻在线旁边弹出这张图等比缩到 ≤240x180 的小预览（不抢焦点、不吃鼠标）；
//   停满 peek 延迟（见 PinHiddenBar::peekDelayMs）→ 再走原来那套整窗 peek，此时预览收掉。
// 于是"认图"是安静的，"看图"还是一次 hover —— 两条路分工，互不抢。
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
