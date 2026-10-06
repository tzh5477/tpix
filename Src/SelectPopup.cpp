#include "pch.h"
#include <include/Ling.h>
#include <algorithm>
#include "SelectPopup.h"
#include "App.h"

namespace
{
	// 每项高度、列表最高多少（再高就滚动）、以及不窄于多少（逻辑像素）。
	// 自绘项（线条那几个样例图）另有更窄的一条下限：样例画的就一条线，用不着 120 那么宽，
	// 而那条列表比它上面那块 42 宽的按钮宽出快两倍，摆在一起很突兀
	constexpr float itemH{ 30.f };
	constexpr float listMaxH{ 320.f };
	constexpr float listMinW{ 120.f };
	constexpr float sampleListMinW{ 60.f };

	class Popup;
	// 声明在类定义之前：Popup 自己的 onDestroy 里要按地址比对后把它放掉
	std::unique_ptr<Popup> popup;
	// 列表开着的时候挂一个低级鼠标钩子，用来发现"点到别处去了"。
	// 只有它能在宿主窗口之外也收得到点击 —— 宿主自己的 onMouseDown 只能看见自己这一亩地。
	// WH_MOUSE_LL 不需要 DLL，回调回到装它的那条线程（UI 线程，有消息泵）。
	HHOOK mouseHook{ nullptr };
	// 弹出按钮的屏幕矩形（物理像素）。存矩形而不是存指针：宿主换了工具就重建按钮，
	// 指针会野；矩形的另一个用处是"点在按钮上不算点在外面"，那一下要留给按钮去收起
	RECT anchorRect{};
	// 宿主挪位置时列表要跟着收，否则它就悬在原来的屏幕坐标上了。
	// 不订阅宿主的销毁：所有会让宿主消失的操作（点关闭按钮、切语言后关窗重开）
	// 那一下点击都落在列表之外，钩子已经先把列表收了
	Ling::WinBase* ownerWin{ nullptr };
	winrt::event_token movedTok{};

	// 列表的窗口过程外面套的那一层，只拦 WM_MOUSEACTIVATE 一条。
	// WS_EX_NOACTIVATE 只管得住"程序主动激活"（ShowWindow / 不带 SWP_NOACTIVATE 的
	// SetWindowPos，见 show 里那段说明）；**鼠标点上来**的激活它管不住 ——
	// DefWindowProc 处理 WM_MOUSEACTIVATE 时一律回 MA_ACTIVATE，不认这个扩展样式。
	// 于是点一下列表，激活就从宿主挪到了列表上：宿主（水印内容弹窗这种"失焦即收"
	// 的弹层）立刻收到 WM_KILLFOCUS，在下拉还开着的时候把自己收掉、正在编辑的内容
	// 被还原成打开时的值 —— 用户看到的就是"选个时间 / 换个字体，刚输入的文字没了"。
	// Ling 不转派 WM_MOUSEACTIVATE，所以在 tpix 侧挂一层：答 MA_NOACTIVATE，
	// 鼠标消息照旧送到列表里，激活不动。只有列表会被套上，别的窗口不受影响
	WNDPROC popupPrevProc{ nullptr };
	LRESULT CALLBACK popupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
		return CallWindowProcW(popupPrevProc, hwnd, msg, wp, lp);
	}

	class Popup : public Ling::WinBase
	{
	public:
		Popup(std::vector<std::wstring> items, int cur, std::function<void(int)> onPick,
			std::wstring fontFamily, SelectPopup::SamplePainter paintSample)
			: items{ std::move(items) }, cur{ cur }, onPick{ std::move(onPick) },
			fontFamily{ std::move(fontFamily) }, paintSample{ std::move(paintSample) }
		{
			// 不激活：弹出列表不该把输入焦点从宿主那儿抢走，否则文本框会丢光标、
			// 贴图窗口也可能因为失焦把自己收了。TOPMOST 保证它盖在宿主之上
			createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP);
			// 换掉窗口过程（见 popupProc）：只为了让鼠标点上来这一下不激活本窗口
			popupPrevProc = reinterpret_cast<WNDPROC>(
				SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&popupProc)));
			onDestroy.add([this]() {
				// 不能在销毁回调里同步 reset 自己
				Ling::App::get()->dq.TryEnqueue([this]() {
					if (popup.get() == this) popup.reset();
				});
			});
		}
		// 鼠标的屏幕坐标（物理像素）是否落在本窗口里
		bool isScreenPosIn(POINT pt) const
		{
			return pt.x >= x && pt.x < x + w && pt.y >= y && pt.y < y + h;
		}
	private:
		// 列表按内容算宽高，最小的那一档（开 / 关两项）只有 120×60，
		// 比 Ling 默认的 800×600 最小跟踪尺寸小得多。不放开的话 setSize 里的
		// SetWindowPos 会被系统按回 800×600（WinPin 那条注释里踩的是同一处），
		// 列表就变成屏幕上好大一块白板
		void onMinMaxInfo(MINMAXINFO* mmi) override
		{
			mmi->ptMinTrackSize.x = 1;
			mmi->ptMinTrackSize.y = 1;
		}
		void onCreated() override
		{
			body->setBg(0xFFFFFFFF);
			body->setBorder(1.f, 0x597EF766);
			auto list = body->makeChild<Ling::ScrollerBox>();
			list->setSizePercent(100.f, 100.f);
			for (int i = 0; i < (int)items.size(); ++i)
			{
				auto btn = list->makeChild<Ling::Button>();
				btn->setHeight(itemH);
				btn->setWidthPercent(100.f);
				btn->setHoverBg(0xF2F2F2FF);
				btn->setHoverColor(0x000000FF);
				// 当前那一档标成选中色，与设置页工具勾选的高亮一致
				if (i == cur) {
					btn->setBg(0xE6F4FFFF);
					btn->setColor(0x597EF7FF);
				}
				// 自绘项：文字留空，底下铺一层铺满的画布画小图 —— Button 不能自绘，
				// Canvas 又收不到鼠标，叠起来才两样都有（悬停底色是按钮画的，
				// 画布清成透明，透下去正好）
				if (paintSample) {
					auto canvas = btn->makeChild<Ling::Canvas>();
					canvas->setSizePercent(100.f, 100.f);
					canvas->setFlexShrink(0.f);
					samples.push_back(canvas);
				}
				else {
					btn->setText(items[i]);
					if (!fontFamily.empty()) btn->setFontFamily(fontFamily);
				}
				btn->onClick.add([this, i](Ling::Button*) { picked(i); });
			}
			refresh();
		}
	private:
		// 自绘项的小图。画在这一层而不是某个 Canvas 子类自己的 layout 里：startPaint /
		// finishPaint 要成对用（没 finish 之前同一设备再 BeginDraw 会失败），
		// 在宿主这里挨个画一遍最省事 —— ToolSub 的背景画布也是这么画的
		void layout() override
		{
			Ling::WinBase::layout();
			if (!paintSample) return;
			for (int i = 0; i < (int)samples.size(); ++i) {
				auto ctx = samples[i]->startPaint();
				if (!ctx) continue;
				ctx->Clear(0);
				paintSample(ctx, D2D1::RectF(0.f, 0.f, samples[i]->w, samples[i]->h), i);
				samples[i]->finishPaint();
			}
		}
		void picked(int index)
		{
			// 先收起再回调：回调里多半要重画界面（换语言那处甚至是关窗重开），
			// 列表还挂着的话会跟着一起被卷进去
			auto cb = std::move(onPick);
			SelectPopup::close();
			if (cb) cb(index);
		}
	private:
		std::vector<std::wstring> items;
		int cur{ -1 };
		std::function<void(int)> onPick;
		std::wstring fontFamily;
		// 自绘项的那几格画布，与 items 一一对应（不自绘时是空的）
		std::vector<Ling::Canvas*> samples;
		SelectPopup::SamplePainter paintSample;
	};

	LRESULT CALLBACK hookProc(int code, WPARAM wp, LPARAM lp)
	{
		// 每次都要往下传，否则会掐掉别人的鼠标消息
		if (code >= 0 && popup && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN)) {
			auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
			POINT pt{ info->pt.x, info->pt.y };
			if (!PtInRect(&anchorRect, pt) && !popup->isScreenPosIn(pt)) SelectPopup::close();
		}
		return CallNextHookEx(mouseHook, code, wp, lp);
	}

}

void SelectPopup::show(Ling::WinBase* owner, Ling::Node* anchor,
	const std::vector<std::wstring>& items, int cur, std::function<void(int)> onPick,
	const std::wstring& fontFamily, float minW, SamplePainter paintSample)
{
	if (items.empty() || !owner || !anchor) return;
	// Node 的 x/y 是窗口内坐标，弹层要的是屏幕坐标 —— 宿主的窗口位置得先加上去。
	// 宿主贴着屏幕边时这一步是必须的（悬浮球就贴在边上）
	const auto ox = (float)owner->x;
	const auto oy = (float)owner->y;
	// 同一个按钮再点一次就是收起。比对矩形而不是指针：宿主重建按钮后指针就野了
	if (popup && anchorRect.left == (int)(ox + anchor->x) && anchorRect.top == (int)(oy + anchor->y)
		&& anchorRect.right == (int)(ox + anchor->x + anchor->w)) {
		close();
		return;
	}
	close();

	auto dpi = owner->dpi > 0.f ? owner->dpi : 1.f;
	// 列表宽度跟着按钮走，窄按钮也留个下限，不然"紧凑"两个字就把列表压成一条缝。
	// minW 是调用方指定的下限（字体名比按钮宽得多）；自绘项走另一条更窄的下限
	const float baseMinW{ paintSample ? sampleListMinW : listMinW };
	auto listW = std::max(std::max(anchor->w / dpi, baseMinW), minW);
	auto listH = std::min(listMaxH, itemH * (float)items.size());
	// 默认往下弹，底下放不下就翻到按钮上方。用按钮所在显示器的工作区判断，
	// 而不是虚拟桌面整体 —— 副屏在左上时后者会把翻转判错
	POINT anchorPt{ (int)(ox + anchor->x + anchor->w / 2.f), (int)(oy + anchor->y + anchor->h / 2.f) };
	MONITORINFO mi{ sizeof(mi) };
	GetMonitorInfo(MonitorFromPoint(anchorPt, MONITOR_DEFAULTTONEAREST), &mi);
	auto top = (int)(oy + anchor->y + anchor->h);
	if (top + (int)(listH * dpi) > mi.rcWork.bottom) top = (int)(oy + anchor->y - listH * dpi);
	auto left = (int)(ox + anchor->x);
	if (left + (int)(listW * dpi) > mi.rcWork.right) left = mi.rcWork.right - (int)(listW * dpi);
	if (left < mi.rcWork.left) left = mi.rcWork.left;

	anchorRect = RECT{ (int)(ox + anchor->x), (int)(oy + anchor->y),
		(int)(ox + anchor->x + anchor->w), (int)(oy + anchor->y + anchor->h) };
	popup = std::make_unique<Popup>(items, cur, std::move(onPick), fontFamily, std::move(paintSample));
	// 这里刻意不走 popup->setSize / setPosition / show()：Ling 的 setSize / setPosition
	// 用的是不带 SWP_NOACTIVATE 的 SetWindowPos，show() 是 ShowWindow(SW_SHOW)，
	// 两个都会把这个带 WS_EX_NOACTIVATE 的窗口真正激活（连尚未显示、只是摆尺寸时都会），
	// 于是当前持有焦点的宿主立刻收到 WM_KILLFOCUS。宿主若是"失焦即关"的弹层
	// （水印内容弹窗就是），就会在下拉还开着的时候被关掉、下一轮消息循环里被释放；
	// 之后点列表里任意一项，回调就落在已释放的宿主上（SelectPopup::close 里的
	// ownerWin->onMoved，以及宿主自己的 onPick 闭包）—— 直接是访问违例。
	// 所以尺寸 / 位置 / 置顶 / 显示合并成一次调用，全程带 SWP_NOACTIVATE，不碰前台。
	// （实测：不带 SWP_NOACTIVATE 的 SetWindowPos 之后前台就是这个弹窗了，
	//  带上的话新窗口可见、前台仍是宿主、宿主也不收 WM_KILLFOCUS）
	// x/y/w/h 仍要写回成员：Ling 的命中判定与布局读的是它们
	popup->x = left;
	popup->y = top;
	popup->w = listW * dpi;   // 逻辑像素 → 物理像素，与 WinBase::setSize / setPosition 口径一致
	popup->h = listH * dpi;
	SetWindowPos(popup->hwnd, HWND_TOPMOST, left, top, (int)popup->w, (int)popup->h,
		SWP_NOACTIVATE | SWP_SHOWWINDOW);
	ownerWin = owner;
	movedTok = owner->onMoved.add([]() { SelectPopup::close(); });
	mouseHook = SetWindowsHookEx(WH_MOUSE_LL, hookProc, nullptr, 0);
}

void SelectPopup::close()
{
	if (mouseHook) {
		UnhookWindowsHookEx(mouseHook);
		mouseHook = nullptr;
	}
	if (ownerWin) {
		ownerWin->onMoved.remove(movedTok);
		ownerWin = nullptr;
	}
	if (!popup) return;
	// 只销毁窗口句柄，C++ 对象推迟到下一轮消息循环 —— 收起多半是从某次点击的栈上发起的
	popup->close();
}

bool SelectPopup::isOpen()
{
	return popup != nullptr;
}
