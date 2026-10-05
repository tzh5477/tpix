#include "pch.h"
#include <algorithm>
#include <ctime>
#include "../App.h"
#include "../Lang.h"
#include "../SelectPopup.h"
#include "../Shape/ShapeWatermark.h"
#include "../Tool/ToolSub.h"
#include "WinWatermarkText.h"

namespace {
	class Dlg;
	// 同一时刻只有一个内容弹窗（点开另一个要先收起前一个），做成文件级静态。
	// 声明在 Dlg 之前 —— unique_ptr 的析构要看到完整类型
	std::unique_ptr<Dlg> dlg;
	// Dlg 的 onDestroy 里要按地址比对后把它放掉，比对的是同一个 unique_ptr
	void onDlgDestroy(void* p);

	class Dlg : public Ling::WinBase
	{
	public:
		Dlg(std::wstring text, std::wstring family,
			std::function<void(const std::wstring&, const std::wstring&)> onApply)
			: text{ std::move(text) }, family{ std::move(family) },
			onApply{ std::move(onApply) }
		{
			createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
			onDestroy.add([this]() {
				// 不能在销毁回调里同步 reset 自己
				Ling::App::get()->dq.TryEnqueue([this]() { onDlgDestroy(this); });
				});
		}
	private:
		// 420×236，比 Ling 默认的 800×600 最小跟踪尺寸小得多。不放开的话 setSize 里的
		// SetWindowPos 会被系统按回 800×600，弹窗变成屏幕上好大一块白板
		void onMinMaxInfo(MINMAXINFO* mmi) override
		{
			mmi->ptMinTrackSize.x = 1;
			mmi->ptMinTrackSize.y = 1;
		}
		void onCreated() override
		{
			enableShadow();
			body->setBg(0xFFFFFFFF);
			body->setFlexDirection(Ling::FlexDirection::Column);
			body->setPadding(pad, pad, pad, pad);

			// 标题行：左标题右 ×。窗口是 WS_POPUP 没有系统标题栏，这一行就是标题栏。
			// 必须显式 Row：yoga 默认是 Column，不写的话标题与 × 会上下叠着，
			// × 跑到标题底下居中（label 有 flexGrow，在 Column 下撑的是高度）
			auto titleRow = body->makeChild<Ling::Node>();
			titleRow->setHeight(titleH);
			titleRow->setWidthPercent(100.f);
			titleRow->setFlexDirection(Ling::FlexDirection::Row);
			titleRow->setAlignItems(Ling::Align::Center);
			auto title = titleRow->makeChild<Ling::Label>();
			title->setText(Lang::get(L"wmText.title"));
			title->setFontSize(14.f);
			title->setColor(0x333333FF);
			title->setFlexGrow(1.f);
			auto closeBtn = titleRow->makeChild<Ling::Button>();
			closeBtn->setSize(28.f, 28.f);
			closeBtn->setText(L"\ue62d");
			closeBtn->setFontFamily(L"icon");
			closeBtn->setHoverColor(0xFFFFFFFF);
			closeBtn->setHoverBg(0xE81123FF);
			// × 与「取消」是一回事：不回调 onApply，配置里一个字都不动
			closeBtn->onClick.add([this](Ling::Button*) { close(); });

			// 输入区：多行、滚轮可滚、Enter 是换行。工具条上那个单行框给不了"标题 + 时间"两行，
			// 而那正是水印最常见的写法
			box = body->makeChild<Ling::TextBox>();
			box->setHeight(editH);
			box->setWidthPercent(100.f);
			box->setFontSize(13.f);
			box->setBg(0xF7F7F7FF);
			box->setBorder(1.f, 0xE0E0E0FF);
			box->setBorderRadius(4.f);
			box->setPadding(10.f, 10.f, 10.f, 10.f);
			box->setPlaceholder(Lang::get(L"tool.watermarkTip"));
			box->setText(text);
			// 点进输入框就选中全部：改水印多半是整句重写，从头选比逐字删省事
			box->onFocusChanged.add([this](Ling::TextBox*, bool focused) {
				if (focused) box->selectAll();
				});

			// 底部一行：时间格式、字体、应用、取消。同上，不写 Row 的话这几项会竖排，
			// 一行的高度装不下四行，确定 / 取消被挤出窗外（作者报"确定按钮看不到了"就是这个）
			auto bottom = body->makeChild<Ling::Node>();
			bottom->setHeight(bottomH);
			bottom->setWidthPercent(100.f);
			bottom->setFlexDirection(Ling::FlexDirection::Row);
			bottom->setAlignItems(Ling::Align::Center);

			auto timeLabel = bottom->makeChild<Ling::Label>();
			timeLabel->setText(Lang::get(L"tool.watermarkTime") + L":");
			timeLabel->setFontSize(13.f);
			timeLabel->setColor(0x333333FF);

			timeBtn = bottom->makeChild<Ling::Button>();
			timeBtn->setHeight(btnH);
			timeBtn->setWidth(timeBtnW);
			timeBtn->setMarginLeft(8.f);
			timeBtn->setFontSize(12.f);
			timeBtn->setBorder(1.f, 0xE0E0E0FF);
			timeBtn->setHoverBg(0xF2F2F2FF);
			timeBtn->onClick.add([this](Ling::Button* b) { pickTime(b); });

			fontBtn = bottom->makeChild<Ling::Button>();
			fontBtn->setHeight(btnH);
			fontBtn->setWidth(fontBtnW);
			fontBtn->setMarginLeft(8.f);
			fontBtn->setFontSize(12.f);
			fontBtn->setBorder(1.f, 0xE0E0E0FF);
			fontBtn->setHoverBg(0xF2F2F2FF);
			fontBtn->onClick.add([this](Ling::Button* b) { pickFont(b); });

			// 右对齐那一对：中间垫一个撑满剩余宽度的空节点
			auto spacer = bottom->makeChild<Ling::Node>();
			spacer->setFlexGrow(1.f);
			spacer->setHeight(1.f);

			auto applyBtn = bottom->makeChild<Ling::Button>();
			applyBtn->setHeight(btnH);
			applyBtn->setWidth(applyBtnW);
			applyBtn->setMarginLeft(8.f);
			applyBtn->setText(Lang::get(L"wmText.apply"));
			applyBtn->setColor(0xFFFFFFFF);
			applyBtn->setBg(0x597EF7FF);
			applyBtn->setHoverBg(0x3E7BFAFF);
			applyBtn->onClick.add([this](Ling::Button*) { apply(); });

			auto cancelBtn = bottom->makeChild<Ling::Button>();
			cancelBtn->setHeight(btnH);
			cancelBtn->setWidth(cancelBtnW);
			cancelBtn->setMarginLeft(8.f);
			cancelBtn->setText(Lang::get(L"wmText.cancel"));
			cancelBtn->setHoverBg(0xF2F2F2FF);
			cancelBtn->onClick.add([this](Ling::Button*) { close(); });

			syncFontBtn();
			// 打开就把光标放进输入框：用户十有八九是来改字的
			box->focus();
			// 点到别处就收起。这里靠失焦而不是"点到外面"：本弹窗要收键盘（不然打不了字），
			// 而时间 / 字体那两个下拉是 WS_EX_NOACTIVATE 的独立窗口 —— 它们弹出来时本窗口
			// 不会失焦，正好不会把正在选的那一档中途收掉
			onBlur.add([this]() { close(); });
			// Esc 取消。Enter 走「应用」要绕开输入框：多行框里 Enter 是换行，
			// 直接在窗口层拦会把换行吃掉，所以只在焦点不在输入框时才认
			onKeyDown.add([this](UINT key) {
				if (key == VK_ESCAPE) { close(); return; }
				if (key == VK_RETURN && box && !box->isFocused()) apply();
				});
		}
		// 时间格式下拉。列表里显示"按此刻展开之后的样子"：{yyyy}-{MM}-{dd} 这种模板串
		// 认得的人不多，展开成 2026-10-05 一眼就知道是哪一档
		void pickTime(Ling::Button* anchor)
		{
			auto& fmts = ShapeWatermark::timeFormats();
			auto now = std::time(nullptr);
			std::vector<std::wstring> items;
			items.reserve(fmts.size());
			for (auto& f : fmts) items.push_back(ShapeWatermark::expandTime(f, now));
			// 当前文字里已经带了哪一档模板，就把它勾上：不然不知道选中的是哪一档，
			// 一个带时间的旧水印改起格式来要逐个试
			int cur{ -1 };
			for (size_t i = 0; i < fmts.size(); i++) {
				if (text.find(fmts[i]) != std::wstring::npos) { cur = (int)i; break; }
			}
			SelectPopup::show(this, anchor, items, cur, [this, &fmts](int picked) {
				if (picked < 0 || picked >= (int)fmts.size()) return;
				// 插在末尾。原来那句不为空就先换行 —— 时间单独占一行才是水印的常见写法。
				// 同一个模板连按两次不重复插：先把上一份从现有文字里剔掉再插
				const auto& f = fmts[picked];
				auto at = text.find(f);
				if (at != std::wstring::npos) {
					text.erase(at, f.size());
					// 剔完可能留下一个空行或行尾空白，一并收拾掉，免得空出来第二行
					while (!text.empty() && (text.back() == L'\n' || text.back() == L' ')) {
						text.pop_back();
					}
				}
				if (!text.empty() && text.back() != L'\n') text += L'\n';
				text += f;
				box->setText(text);
				});
		}
		void pickFont(Ling::Button* anchor)
		{
			auto& fonts = ToolSub::commonFonts();
			std::vector<std::wstring> items;
			items.reserve(fonts.size());
			for (auto& f : fonts) items.push_back(f.show);
			SelectPopup::show(this, anchor, items, ToolSub::fontIndexOf(family),
				[this](int picked) {
					if (picked < 0 || picked >= (int)ToolSub::commonFonts().size()) return;
					family = ToolSub::commonFonts()[picked].family;
					syncFontBtn();
				}, {}, fontPopupMinW);
		}
		void syncFontBtn()
		{
			auto show = ToolSub::fontShowName(family);
			// 按钮只有 96 宽，"Times New Roman" 这种要截一下；下拉里是全名
			if (show.size() > 8) show = show.substr(0, 7) + L"\u2026";
			fontBtn->setText(show);
		}
		// 收尾：把输入框里的文字与当前字体交出去，落盘与重画由调用方做
		void apply()
		{
			text = box->getText();
			auto cb = std::move(onApply);
			close();
			if (cb) cb(text, family);
		}
	public:
		// 弹窗各部分的高度（逻辑像素）。跟 pixpin 对齐：一行标题、多行输入框、底部一行按钮
		static constexpr float titleH{ 36.f };
		static constexpr float editH{ 132.f };
		static constexpr float bottomH{ 44.f };
		static constexpr float pad{ 12.f };
		static constexpr float btnH{ 30.f };
		static constexpr float timeBtnW{ 104.f };    // 「时间」下拉
		static constexpr float fontBtnW{ 96.f };
		static constexpr float applyBtnW{ 72.f };
		static constexpr float cancelBtnW{ 72.f };
		static constexpr float fontPopupMinW{ 200.f };
		static constexpr float winW{ 420.f };
		static constexpr float winH{ titleH + editH + bottomH + pad * 3 };
	private:
		Ling::TextBox* box{ nullptr };
		Ling::Button* timeBtn{ nullptr }, * fontBtn{ nullptr };
		std::wstring text;
		std::wstring family;
		std::function<void(const std::wstring&, const std::wstring&)> onApply;
	};

	void onDlgDestroy(void* p)
	{
		if (dlg.get() == p) dlg.reset();
	}
}

void WinWatermarkText::show(Ling::WinBase* owner, Ling::Node* anchor,
	const std::wstring& curText, const std::wstring& curFamily,
	std::function<void(const std::wstring&, const std::wstring&)> onApply)
{
	if (!owner || !anchor) return;
	// 已经开着就只拉到前面：连点两下内容按钮不该弹两个窗，也不该把已改的半截丢掉
	if (dlg) {
		if (dlg->hwnd && IsWindow(dlg->hwnd)) {
			dlg->show();
			SetForegroundWindow(dlg->hwnd);
			return;
		}
		dlg.reset();
	}
	// Node 的 x/y 是窗口内坐标，弹窗要的是屏幕坐标 —— 宿主的窗口位置得先加上去
	auto ox = (float)owner->x;
	auto oy = (float)owner->y;
	auto dpi = owner->dpi > 0.f ? owner->dpi : 1.f;
	auto pw = (int)(Dlg::winW * dpi);
	auto ph = (int)(Dlg::winH * dpi);
	// 默认摆在按钮下方；下方放不下就翻到上方，都放不下就贴着工作区顶边
	POINT anchorPt{ (int)(ox + anchor->x + anchor->w / 2.f), (int)(oy + anchor->y + anchor->h) };
	MONITORINFO mi{ sizeof(mi) };
	GetMonitorInfo(MonitorFromPoint(anchorPt, MONITOR_DEFAULTTONEAREST), &mi);
	int px = anchorPt.x - pw / 2;
	int py = anchorPt.y + 8;
	if (py + ph > mi.rcWork.bottom) py = anchorPt.y - 8 - ph;
	if (py < mi.rcWork.top) py = mi.rcWork.top;
	if (px + pw > mi.rcWork.right) px = mi.rcWork.right - pw;
	if (px < mi.rcWork.left) px = mi.rcWork.left;

	dlg = std::make_unique<Dlg>(curText, curFamily, std::move(onApply));
	// 必须走 setter：createNativeWindow 在构造里就已经按当时还是 0 的 w/h 把 hwnd 建好了，
	// 事后改成员不会动窗口，ShowWindow 出来的是个 0×0 的窗，看着就是"点了没反应"
	dlg->setSize(Dlg::winW, Dlg::winH);   // setter 收逻辑像素，内部乘 dpi
	dlg->setPosition(px, py);             // 屏幕物理像素
	dlg->show();
	// 要键盘才能打字，而 WS_POPUP 不会自己成为前台：显式拉一次，
	// 否则输入框拿不到焦点、光标不闪、按键全落在原来那个窗口上
	SetForegroundWindow(dlg->hwnd);
}

void WinWatermarkText::close()
{
	if (!dlg) return;
	// 只销毁窗口句柄，C++ 对象推迟到下一轮消息循环 —— 收起多半是从某次点击的栈上发起的
	dlg->close();
}
