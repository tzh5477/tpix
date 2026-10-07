#include "pch.h"
#include <algorithm>
#include <cmath>
#include "../App.h"
#include "../Lang.h"
#include "../Tool/ToolSub.h"
#include "WinWatermarkPanel.h"

namespace {
	class Panel;
	std::unique_ptr<Panel> panel;
	// 声明在类定义之前：Panel 的 onDestroy 里要按地址比对后把它放掉
	void onPanelDestroy(void* p);

	// "鼠标还在这一片里"的屏幕矩形（物理像素）。移出去就收起。
	// 它是触发按钮与浮层本身的并集，不只是按钮：浮层挂在按钮下方 2 像素处，
	// 只判按钮的话，鼠标从按钮往浮层走的那一下正好落在这 2 像素的缝里，
	// 面板当场收掉 —— 而那正是"点开浮层、把滑块拖过去"最必经的一步
	RECT anchorRect{};

	// 两个矩形的并集
	RECT merge(const RECT& a, const RECT& b)
	{
		RECT r{ a.left, a.top, a.right, a.bottom };
		if (b.left < r.left) r.left = b.left;
		if (b.top < r.top) r.top = b.top;
		if (b.right > r.right) r.right = b.right;
		if (b.bottom > r.bottom) r.bottom = b.bottom;
		return r;
	}

	class Panel : public Ling::WinBase
	{
	public:
		explicit Panel(ToolSub* sub) : sub{ sub }
		{
			// NOACTIVATE：浮层不该抢焦点。贴图窗口失去焦点会自己收工具条，
			// 拖个滑块就把工具条弄没了的话，这三项就再也调不到了
			createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP);
			onDestroy.add([this]() {
				// 不能在销毁回调里同步 reset 自己
				Ling::App::get()->dq.TryEnqueue([this]() { onPanelDestroy(this); });
				});
		}
		// 鼠标的屏幕坐标（物理像素）是否落在本窗口里，或落在触发它的那个控件上
		bool isScreenPosIn(POINT pt) const
		{
			if (pt.x >= x && pt.x < x + w && pt.y >= y && pt.y < y + h) return true;
			return PtInRect(&anchorRect, pt) != FALSE;
		}
	private:
		// 300×116，比 Ling 默认的 800×600 最小跟踪尺寸小得多。不放开的话 setSize 里的
		// SetWindowPos 会被系统按回 800×600，浮层变成屏幕上好大一块白板
		void onMinMaxInfo(MINMAXINFO* mmi) override
		{
			mmi->ptMinTrackSize.x = 1;
			mmi->ptMinTrackSize.y = 1;
		}
		void onCreated() override
		{
			enableShadow();
			if (!sub) return;
			body->setBg(0xFFFFFFFF);
			body->setBorder(1.f, 0xE0E0E0FF);
			body->setBorderRadius(6.f);
			body->setFlexDirection(Ling::FlexDirection::Column);
			body->setPadding(pad, pad, pad, pad);

			// 三项的值域与初值都问工具条 —— 它才是水印样式的归属（也是落盘的那一方），
			// 浮层只管显示与拖动，不自己存一份，免得两边打架。
			// 捕获的是局部副本而不是成员 sub：lambda 不能按名字捕获成员变量
			auto* target = sub;
			auto setAlpha = [target](float v) { target->setWatermarkAlpha(v); };
			auto setSize = [target](float v) { target->setWatermarkSize(v); };
			auto setGap = [target](float v) { target->setWatermarkGap(v); };
			addRow(Lang::get(L"tool.watermarkOpacity"), 5.f, 100.f, sub->getWatermarkAlpha(), setAlpha);
			// 大小沿用水印在 config.json 里的 fontSize，值域查工具条那张表
			float smin{ 0.f }, smax{ 0.f };
			sub->getWatermarkSizeRange(smin, smax);
			addRow(Lang::get(L"tool.watermarkSize"), smin, smax, sub->getWatermarkSize(), setSize);
			addRow(Lang::get(L"tool.watermarkGap"), 0.f, 100.f, sub->getWatermarkGap(), setGap);
		}
		// 一行 = 名称 + 滑块 + 数值。竖排（每行一项）是这一版的核心：
		// 横排时三个滑块挤在 32 像素高的工具条上，每个不到 60 宽，几乎拖不动
		void addRow(const std::wstring& name, float min, float max, float val,
			std::function<void(float)> onVal)
		{
			auto row = body->makeChild<Ling::Node>();
			row->setHeight(rowH);
			row->setWidthPercent(100.f);
			// 必须显式 Row：yoga 默认是 Column，不写的话名称 / 滑块 / 数值会竖成三行，
			// 行高只有 32 装不下，整块浮层就散了（作者截图里"不透明度"、滑块、"23"各占一行）
			row->setFlexDirection(Ling::FlexDirection::Row);
			row->setAlignItems(Ling::Align::Center);

			auto label = row->makeChild<Ling::Label>();
			label->setText(name);
			label->setFontSize(12.f);
			label->setColor(0x555555FF);
			// 横向对齐归 AlignItems（交叉轴），JustifyContent 管的是纵向（主轴）——
			// Label 是个 Column 容器，原来这里写 JustifyContent 是空操作，只是恰好被
			// "文字拉到 56 宽 + 文字自己左对齐"这个默认行为蒙对了
			label->setAlignItems(Ling::Align::FlexStart);
			label->setWidth(nameW);
			label->setFlexShrink(0.f);

			auto s = row->makeChild<Ling::Slider>();
			// 高 12：Slider 的滑块半径固定 3（直径 6），轨道只有 1 粗，
			// 给 12 高正好是滑块居中、上下各留 3 —— 比贴着 6 高要透气
			s->setHeight(12.f);
			// Slider 的构造函数里写死了 setWidth(200)，那是一个确定宽度 —— 它的两个兄弟
			// 也都有确定宽度（名称 56 / 数值 30），而 yoga 的 flexShrink 默认是 0，一个
			// 像素都不肯让。三者相加 56 + 200 + 30，再加两侧各 10 的间距是 306，比这一行
			// 的内容宽（winW 300 减两侧 pad 20 = 280）多出 26 ⇒ 最右边的数值那一格被顶到
			// 面板右边界之外，只露出贴着边框的一小截（作者报的"数值超出边界"就是这个）。
			// flexGrow 压不过确定宽度，所以宽度只能按行宽自己算出来给它
			s->setFlexGrow(0.f);
			s->setWidth(sliderW);
			s->setMarginLeft(sliderGap);
			s->setMarginRight(sliderGap);
			// setValue 排在 onValueChanged 之前：填初值这一下不会反过来又写一次盘
			s->setRange(min, max);
			s->setValue(val);
			s->setStep(1.f);
			s->setThumbColor(0x595959FF);
			s->setHoverThumbColor(0x595959FF);
			s->setTrackColor(0xD9D9D9FF);
			s->setFillColor(0x1677FFFF);
			// 数值：显示整数，与滑块的 step=1 配套。滑块变了就同步刷新那一格。
			// 居右：位数从 1 位变 3 位时左边缘不动，读数不会左右跳
			auto num = row->makeChild<Ling::Label>();
			num->setFontSize(12.f);
			num->setColor(0x555555FF);
			num->setWidth(numW);
			// 同上：居右要写 AlignItems。JustifyContent 对 Label 是纵向的，写在这里等于
			// 没设，数值会从左边缘起排，三位数时尾巴越过面板右边框
			num->setAlignItems(Ling::Align::FlexEnd);
			num->setFlexShrink(0.f);
			syncNum(num, s->getValue());
			s->onValueChanged.add([this, onVal, s, num](Ling::Slider*, float v) {
				syncNum(num, v);
				if (onVal) onVal(v);
				});
		}
		// 数值标签是固定宽的，居中写：位数从 1 变 2 时文字不会左右跳
		void syncNum(Ling::Label* num, float v)
		{
			num->setText(std::to_wstring((int)std::lround(v)));
		}
	public:
		static constexpr float rowH{ 32.f };
		static constexpr float pad{ 10.f };
		static constexpr float nameW{ 56.f };   // 「不透明度」四个字 + 一点余量
		static constexpr float numW{ 30.f };    // 数值列：最宽是三位数（透明度到 100）
		static constexpr float sliderGap{ 10.f };   // 滑块与左右两格之间的间距
		static constexpr float winW{ 300.f };
		// 滑块的宽度由行宽反推 —— 名称 + 间距 + 滑块 + 间距 + 数值 要正好填满行内容宽
		//（winW 减两侧 pad）。给确定值而不是靠 flexGrow：Slider 构造函数里写死的那个宽度
		// 是确定宽度，flexGrow 压不过它，见 addRow 里的说明
		static constexpr float sliderW{ winW - pad * 2 - nameW - numW - sliderGap * 2 };
		static constexpr float winH{ pad * 2 + rowH * 3 };
	private:
		ToolSub* sub{ nullptr };
	};

	void onPanelDestroy(void* p)
	{
		if (panel.get() == p) panel.reset();
	}

	// 监听鼠标移动：滑块要能一路拖到底，中途经过"浮层与工具条之间那道缝"时
	// 不能把浮层收了。低级钩子是唯一在宿主窗口之外也收得到鼠标移动的地方
	HHOOK hook{ nullptr };
	LRESULT CALLBACK hookProc(int code, WPARAM wp, LPARAM lp)
	{
		// 每次都要往下传，否则会掐掉别人的鼠标消息
		if (code >= 0 && panel && wp == WM_MOUSEMOVE) {
			// 正在拖滑块：Slider 按下时会对本窗口 SetCapture，那期间鼠标在窗口内外
			// 都还收得到移动 —— 这时按"移出去了"收面板，等于拖到一半面板没了
			if (GetCapture() == panel->hwnd) return CallNextHookEx(hook, code, wp, lp);
			auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
			POINT pt{ info->pt.x, info->pt.y };
			if (!panel->isScreenPosIn(pt)) WinWatermarkPanel::close();
		}
		return CallNextHookEx(hook, code, wp, lp);
	}
}

void WinWatermarkPanel::show(ToolSub* sub, Ling::Node* anchor)
{
	if (!sub || !anchor) return;
	auto* owner = static_cast<Ling::WinBase*>(sub);
	// 已经开着就只挪位置：鼠标在工具条上来回扫过时反复重建面板，
	// 正在拖的滑块会被 Rebuild 掉
	if (panel) {
		if (panel->hwnd && IsWindow(panel->hwnd)) {
			// 已经开着：只把停留区并上新的按钮矩形。浮层本身不重建 ——
			// 重建会把正在拖的滑块连同它已改好的值一起扔掉
			RECT btn{ (int)(owner->x + anchor->x), (int)(owner->y + anchor->y),
				(int)(owner->x + anchor->x + anchor->w), (int)(owner->y + anchor->y + anchor->h) };
			anchorRect = merge(anchorRect, btn);
			return;
		}
		panel.reset();
	}
	auto ox = (float)owner->x;
	auto oy = (float)owner->y;
	auto dpi = owner->dpi > 0.f ? owner->dpi : 1.f;
	auto pw = (int)(Panel::winW * dpi);
	auto ph = (int)(Panel::winH * dpi);
	// 摆在工具条正下方居中；下方放不下就翻到上方，都放不下就贴着工作区顶边
	POINT at{ (int)(ox + anchor->x + anchor->w / 2.f), (int)(oy + anchor->y + anchor->h) };
	MONITORINFO mi{ sizeof(mi) };
	GetMonitorInfo(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &mi);
	int px = at.x - pw / 2;
	int py = at.y + 2;
	if (py + ph > mi.rcWork.bottom) py = at.y - ph - 2;
	if (py < mi.rcWork.top) py = mi.rcWork.top;
	if (px + pw > mi.rcWork.right) px = mi.rcWork.right - pw;
	if (px < mi.rcWork.left) px = mi.rcWork.left;

	// 停留区 = 按钮 ∪ 浮层。两者之间的那段走廊因此也在里面，鼠标走过去不会把面板收了
	anchorRect = merge(
		RECT{ (int)(ox + anchor->x), (int)(oy + anchor->y),
			(int)(ox + anchor->x + anchor->w), (int)(oy + anchor->y + anchor->h) },
		RECT{ px, py, px + pw, py + ph });
	panel = std::make_unique<Panel>(sub);
	// 必须走 setter：createNativeWindow 在构造里就已经按当时还是 0 的 w/h 把 hwnd 建好了，
	// 事后改成员不会动窗口，ShowWindow 出来的是个 0×0 的窗，看着就是"点了没反应"
	panel->setSize(Panel::winW, Panel::winH);   // setter 收逻辑像素，内部乘 dpi
	panel->setPosition(px, py);                  // 屏幕物理像素
	panel->show();
	hook = SetWindowsHookEx(WH_MOUSE_LL, hookProc, nullptr, 0);
}

void WinWatermarkPanel::close()
{
	if (hook) {
		UnhookWindowsHookEx(hook);
		hook = nullptr;
	}
	if (!panel) return;
	// 只销毁窗口句柄，C++ 对象推迟到下一轮消息循环 —— 收起多半是从钩子回调里发起的
	panel->close();
}

bool WinWatermarkPanel::isOpen()
{
	return panel != nullptr;
}
