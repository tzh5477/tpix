#pragma once
#include <include/Ling.h>

class WinPin;

// 藏起来的贴图在屏幕边上留下的"书签条"。
// 作者要的形态：贴图收进一条 30px 长、4px 宽的细线，每张一个颜色；鼠标移到某一条上，
// 那张贴图回原位。条挂在两条边上 ——
//   左边：竖线自上而下排成一列（藏三张就是三条不同颜色叠成一竖列）；
//   顶边：同样的细线横过来，自左向右排成一行。
// 两扇窗各管一条边：贴图是拖到哪条边线上松手就归哪条边（点「隐藏」按钮藏的一律归左边），
// 所以两边的条数、颜色、hover 都各算各的。
//
// 为什么是独立窗口：贴图这会儿已经被 hide() 掉了，没有窗口可画；而且这条要一直在，
// 包括所有贴图都藏起来的时候。窗口带 WS_EX_NOACTIVATE，不会把焦点从用户手上的事抢走。
// 尺寸只有几十像素，比 Ling 默认的最小尺寸（800x600）小得多，所以必须覆盖 onMinMaxInfo。
class PinHiddenBar : public Ling::WinBase
{
public:
	// 藏 / 放 / 关窗之后都要走一遍：按当前"藏在这条边上的贴图"重建条。一条都没有就把窗口收掉
	static void sync();
	static void dispose();
	// 析构必须公开：本类由文件里的 std::unique_ptr 持有，销毁时是 std::default_delete
	// 在调它，够不着 private。WinDelay 也是这么摆的
	~PinHiddenBar();
private:
	// 本扇窗管哪条边。建窗时定下，之后不变 —— 横竖两套摆法差得比较多，按它分支
	PinHiddenBar(WinPin::BarEdge edge);
	void onCreated() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	// 鼠标在条上移动：认出手指底下是第几条，把那张露出来。
	// Ling 把"离开窗口"也报进这条事件（坐标给的是 INT_MAX，见 WinBase::mouseLeave）
	void onMove(POINT pos);
	void onTimerCB(UINT id);
	// 按当前藏在这条边上的贴图重建：条数、颜色、窗口尺寸
	void rebuild();
	// 顶边那条是横着排的（条本身横过来，自左向右）
	bool isHorizontal() const { return edge == WinPin::BarEdge::Top; }
	// 光标落在第几条上。条只有 4 逻辑像素厚，按它自己的范围判等于让人去点一根头发丝，
	// 所以直接取最近的一条 —— 竖排比纵坐标、横排比横坐标
	int barIndexAt(POINT pos) const;
	// 让第 index 张藏着的贴图露出来。只显窗口，隐藏状态不动 —— 条要一直留着，
	// 不然鼠标一离开条就没了，而"离开就收回去"正是这一套的行为
	void reveal(int index);
	// 把露出来的那张收回去，并停掉复核的定时器
	void conceal();
	// 第 index 条线旁边的摆位（物理像素，不钳位 —— 钳位需要贴图宽高，只有 reveal 拿得到）
	POINT calcPeekPos(int index) const;
	// 鼠标是不是还压在"露出来的那张"或它的两条工具条或本窗口上
	bool isOverPeek() const;
private:
	// 30px 长、4px 厚（逻辑像素，交给 Ling 的 setter 时由其内部乘 dpi）。
	// 竖排时长边是高、横排时长边是宽，所以按"长 / 厚"命名，不叫宽高
	static constexpr float barLong{ 30.f };
	static constexpr float barThick{ 4.f };
	// 相邻两条之间的缝。颜色不同才分得清那是两张图，不是一张图的渐变
	static constexpr float gapW{ 2.f };
	// 条四周的内边距。它同时是"这条窗口的命中范围比那 4 像素宽多少" ——
	// 鼠标得能落在窗口里才有 hover 可谈，所以不能贴着条画
	static constexpr float pad{ 4.f };
	// 回显贴图与那条线之间的缝（逻辑像素）
	static constexpr float peekGap{ 4.f };
	// 复核间隔：露出来之后每隔这么久看一眼鼠标还在不在；离开判定见 onTimerCB（连续离开满 800ms 才收）
	static constexpr UINT tickMs{ 250 };
	static constexpr UINT tickId{ 100 };
	// 离开宽限：第一次发现鼠标离开后，连续这么久仍不在场才收回去（spec 第 4 节）
	static constexpr UINT peekGraceMs{ 800 };
	// 「用户把这张图从条上拖出来了」的判定阈值（物理像素）：回显的摆位是 reveal 时记下的
	// peekX/peekY，位置离开它超过这个量就算拖出来了。留几像素是免得窗口被系统挪一像素
	// 就误判成拖动
	static constexpr int dragOutSlop{ 2 };
	// 离开时刻（GetTickCount）。0 = 在场。每跳轮询里记 / 清
	DWORD leaveAt{ 0 };
	std::vector<Ling::Node*> bars;
	// 当前露出来的那一张。nullptr = 什么都没露
	WinPin* peek{ nullptr };
	// peek 期间的原位记忆（物理像素）。reveal 记下、conceal 用它把没被拖动的图挪回去；
	// 被拖走的图就地藏，窗口自己的 x/y 自然成为新原位 —— 「显示」按钮 show() 当前 x/y 即可
	int prevX{ 0 }, prevY{ 0 };
	// 本次 peek 的摆位（物理像素）。收回时拿它跟当前位置比，判定"用户拖过没有"。
	// 必须在 reveal 时存下来：conceal 时条序号可能已因增删重建而对不上
	int peekX{ 0 }, peekY{ 0 };
	// 条落在哪。建窗口时就要用，所以缓存下来（见构造函数里那两条边的落点）
	int barX{ 0 }, barY{ 0 };
	WinPin::BarEdge edge;
};
