#pragma once
#include <include/Ling.h>

class WinPin;

// 藏起来的贴图在屏幕左上角留下的"书签条"。
// 作者要的形态：点"隐藏"之后贴图收进左上角一条 50px 长、2px 宽的竖线，每张一个颜色、
// 自上而下竖着排（藏三张就是三条不同颜色叠成一列）；鼠标移到某一条上，那张贴图回原位。
//
// 为什么是独立窗口：贴图这会儿已经被 hide() 掉了，没有窗口可画；而且这条要一直在，
// 包括所有贴图都藏起来的时候。窗口带 WS_EX_NOACTIVATE，不会把焦点从用户手上的事抢走。
// 尺寸只有几十像素，比 Ling 默认的最小尺寸（800x600）小得多，所以必须覆盖 onMinMaxInfo。
class PinHiddenBar : public Ling::WinBase
{
public:
	// 藏 / 放 / 关窗之后都要走一遍：按当前"藏着的贴图"重建条。一条都没有就把窗口收掉
	static void sync();
	static void dispose();
	// 析构必须公开：本类由文件里的 std::unique_ptr 持有，销毁时是 std::default_delete
	// 在调它，够不着 private。WinDelay 也是这么摆的
	~PinHiddenBar();
private:
	PinHiddenBar();
	void onCreated() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	// 鼠标在条上移动：认出手指底下是第几条，把那张露出来。
	// Ling 把"离开窗口"也报进这条事件（坐标给的是 INT_MAX，见 WinBase::mouseLeave）
	void onMove(POINT pos);
	void onTimerCB(UINT id);
	// 按当前藏着的贴图重建：条数、颜色、窗口宽度
	void rebuild();
	// 光标落在第几条上。条只有 2 逻辑像素宽，按它自己的范围判等于让人去点一根头发丝，
	// 所以直接取最近的一条 —— 竖排之后比的是纵坐标
	int barIndexAt(POINT pos) const;
	// 让第 index 张藏着的贴图露出来。只显窗口，隐藏状态不动 —— 条要一直留着，
	// 不然鼠标一离开条就没了，而"离开就收回去"正是这一套的行为
	void reveal(int index);
	// 把露出来的那张收回去，并停掉复核的定时器
	void conceal();
	// 鼠标是不是还压在"露出来的那张"或它的两条工具条或本窗口上
	bool isOverPeek() const;
private:
	// 50px 长、2px 宽（逻辑像素，交给 Ling 的 setter 时由其内部乘 dpi）
	static constexpr float barW{ 2.f };
	static constexpr float barH{ 50.f };
	// 相邻两条之间的缝。颜色不同才分得清那是两张图，不是一张图的渐变
	static constexpr float gapW{ 2.f };
	// 条四周的内边距。它同时是"这条窗口的命中范围比那 2 像素宽多少" ——
	// 鼠标得能落在窗口里才有 hover 可谈，所以不能贴着条画
	static constexpr float pad{ 4.f };
	// 复核间隔：露出来之后每隔这么久看一眼鼠标还在不在，不在就收回去
	static constexpr UINT tickMs{ 250 };
	static constexpr UINT tickId{ 100 };
	std::vector<Ling::Node*> bars;
	// 当前露出来的那一张。nullptr = 什么都没露
	WinPin* peek{ nullptr };
	// 条落在哪。屏幕左上角，建窗口时就要用，所以缓存下来
	int barX{ 0 }, barY{ 0 };
};
