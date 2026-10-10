#include "pch.h"
#include <algorithm>
#include "../App.h"
#include "../Lang.h"
#include "../Setting.h"
#include "../Tool/ToolVideo.h"
#include "WinRecordLimit.h"

namespace {
	std::unique_ptr<WinRecordLimit> winRecordLimit;
	// 窗口尺寸（逻辑像素）。一行标题 + 一行说明 + 一个输入框 + 一行按钮。
	// 高度不是拍脑袋定的：内容实高 = 16(上边距) + 19(标题 14px 行高) + 4 + 16(说明 12px 行高)
	// + 10 + 30(输入行) + 14 + 30(按钮行) + 16(下边距) = 155，取 164 留 9 像素余量
	// （行高按 1.32 倍字号推，换字体/换语言差一两像素不至于把按钮挤掉）
	constexpr float kWinW{ 300.f }, kWinH{ 164.f };
	// 四周内边距
	constexpr float kPad{ 16.f };
	// 与录屏工具条的间距
	constexpr float kGap{ 8.f };
	// 按钮宽 80：给「Без лимита」/「Tanpa batas」这类外语留够 —— 68 宽时它们会
	// 顶出按钮框压到隔壁（Button 里的 Text 是按内容量的，不会自己折行）
	constexpr float kBtnW{ 80.f }, kBtnH{ 30.f };

	// 主题色提亮一档，当悬停底色用。Button 设了 setBg 就必须给 setHoverBg ——
	// 否则 onMove 进 hover 时会把画刷换成那个从没建过的 hoverBrush（空），整块变空白
	unsigned int liftColor(unsigned int theme)
	{
		auto lift = [](unsigned int v) { return std::min(255u, (v & 0xFF) + 32); };
		return (lift(theme >> 16) << 24) | (lift(theme >> 8) << 16) | (lift(theme) << 8) | 0xFF;
	}
}

WinRecordLimit::WinRecordLimit(HWND toolHwnd, float dpi)
	: Ling::WinBase(), toolHwnd(toolHwnd)
{
	// 跟着工具条那块屏的缩放走。WinBase 构造里取的是系统 dpi，工具条可能被挪到了
	// 缩放比例不同的另一块屏上（与 ToolVideo 构造函数同一处理）
	this->dpi = dpi;
	// setSize 内部乘 dpi，之后 w/h 就是物理像素，placeAboveTool 直接用它们
	setSize(kWinW, kWinH);
	placeAboveTool();
	// C++ 对象不能在这里就放掉：关窗多半是从本窗口自己的按钮点击栈上进来的（use-after-free），
	// 推迟到下一轮消息循环。⚠️ 放之前要确认 static 里拿着的还是"已经关掉的这一扇" ——
	// show() 是"先关旧的再开新的"，不加这个判断就会把刚开起来的那扇顺手收掉
	onDestroy.add([this]() {
		Ling::App::get()->dq.TryEnqueue([this]() {
			if (winRecordLimit.get() == this) winRecordLimit.reset();
		});
	});
	// ⚠️ 这里**不能**带 WS_EX_NOACTIVATE：它要收键盘（回车确定 / ESC 取消），
	// 而 WinDelay 那个倒计时窗正好相反 —— 它是特意不要焦点的，两者诉求相反
	createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
}

void WinRecordLimit::show(HWND toolHwnd, float dpi)
{
	// 已经开着就收掉重开：位置要按工具条重新算（工具条可能被拖过），
	// 焦点也该抢回来，重开一次比"复用并挪位置"少一堆状态
	closeIfOpen();
	// 构造函数是私有的，std::make_unique 不是它的朋友（编译不过 C2248），
	// 这里照 WinTextPin / WinDelay 那套直接 new 再交给 unique_ptr
	winRecordLimit = std::unique_ptr<WinRecordLimit>(new WinRecordLimit(toolHwnd, dpi));
}

void WinRecordLimit::closeIfOpen()
{
	if (!winRecordLimit) return;
	// close() 是 DestroyWindow，同步走 onDestroy；C++ 对象推迟到下一轮消息循环再放，
	// 否则这条路径要是从它自己的按钮点击栈上进来，就是 use-after-free
	winRecordLimit->close();
}

void WinRecordLimit::dispose()
{
	winRecordLimit.reset();
}

void WinRecordLimit::placeAboveTool()
{
	auto* mon = MonitorFromWindow(toolHwnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi{ sizeof(MONITORINFO) };
	const bool hasMon = mon && GetMonitorInfo(mon, &mi);
	const int pw = (int)w, ph = (int)h;
	const int gap = (int)(kGap * dpi + 0.5f);

	RECT toolRect{};
	if (!toolHwnd || !GetWindowRect(toolHwnd, &toolRect)) {
		// 拿不到工具条位置（理论上走不到）：退回主屏正中，至少还能用
		MONITORINFO def{ sizeof(MONITORINFO) };
		GetMonitorInfo(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &def);
		x = def.rcWork.left + (def.rcWork.right - def.rcWork.left - pw) / 2;
		y = def.rcWork.top + (def.rcWork.bottom - def.rcWork.top - ph) / 2;
		return;
	}

	// 与工具条左对齐而不是居中：计时那一格本来就在工具条最左边，小窗顶在它上方，
	// 盯完数字抬头就是输入框，视线不用横移
	int px = toolRect.left;
	// 默认贴上方。工具条多半挂在选区下方，于是上方就是选区内部或者桌面的空白处
	int py = toolRect.top - gap - ph;
	if (hasMon && py < mi.rcWork.top) {
		// 上方放不下就翻到下方（全屏录制时工具条本身就压在录制区右下角，上面没地方）
		py = toolRect.bottom + gap;
	}
	if (hasMon) {
		py = std::clamp(py, (int)mi.rcWork.top, (int)mi.rcWork.bottom - ph);
		px = std::clamp(px, (int)mi.rcWork.left, (int)mi.rcWork.right - pw);
	}
	x = px;
	y = py;
}

void WinRecordLimit::onCreated()
{
	disableBorderRadius();
	enableShadow();
	// 这扇窗当然不能被录进视频。工具条自己已经摘出去了（CapVideo::makeTool），
	// 但这是另一个独立顶层窗口，得单独说一声
	App::excludeFromCapture(hwnd);

	body->setBg(0xFFFFFFFF);
	body->setFlexDirection(Ling::FlexDirection::Column);
	body->setPadding(kPad);

	auto title = body->makeChild<Ling::Label>();
	title->setText(Lang::get(L"video.limitTitle"));
	title->setFontSize(14.f);
	title->setColor(0x222222FF);

	auto hint = body->makeChild<Ling::Label>();
	hint->setText(Lang::get(L"video.limitHint"));
	hint->setFontSize(12.f);
	hint->setColor(0x888888FF);
	hint->setMarginTop(4.f);

	// 输入行：数字框 + 「分钟」后缀
	auto row = body->makeChild<Ling::Node>();
	row->setFlexDirection(Ling::FlexDirection::Row);
	row->setAlignItems(Ling::Align::Center);
	row->setMarginTop(10.f);

	edit = row->makeChild<Ling::TextBox>();
	edit->setWidth(112.f);
	edit->setHeight(kBtnH);
	// 单行输入框的上下内边距要让位给 setVerticalCenter：TextBox 构造默认上下各 6，
	// 这框只剩 18 逻辑像素装文本，而一行 14 号字就有 ~19 —— 内容永远"溢出"半行，
	// 滚动条就常驻在框上（没换行也在）。同 WinSettingAi 里那六个框
	edit->setPadding(6.f, 0.f, 6.f, 0.f);
	edit->setBorder(1.f, 0xDDDDDDFF);
	edit->setVerticalCenter(true);
	edit->setFontSize(14.f);
	// 预填当前值而不是用占位符：占位符是灰的，看着像"这里现在是空的、不填就是不限时"，
	// 而这恰好是最需要说清的一处（不填其实是"不改"）。预填 + 全选，想改就直接打
	edit->setText(std::to_wstring(Setting::get()->getVideoTimeLimitMin()));

	auto unit = row->makeChild<Ling::Label>();
	unit->setText(Lang::get(L"setting.videoTimeLimitUnit"));
	unit->setFontSize(13.f);
	unit->setColor(0x666666FF);
	unit->setMarginLeft(8.f);

	// 回车 = 确定，ESC = 取消。必须挂在 TextBox 的 onKeyDown 上并把 *handled 置真 ——
	// TextBox 自己只认 VK_RETURN 一律换行、VK_ESCAPE 一律失焦，窗口级事件抢不过它的默认行为
	edit->onKeyDown.add([this](Ling::TextBox*, UINT key, bool* handled) {
		if (key == VK_RETURN) { *handled = true; apply(true); }
		else if (key == VK_ESCAPE) { *handled = true; apply(false); }
	});

	auto btns = body->makeChild<Ling::Node>();
	btns->setFlexDirection(Ling::FlexDirection::Row);
	btns->setAlignItems(Ling::Align::Center);
	btns->setMarginTop(14.f);

	// 「不限」单独放左边：它和「确定」不是一类动作（一个是"改成不限时"，一个是"用框里那个数"），
	// 挨在一起容易点错
	auto btnUnlimited = btns->makeChild<Ling::Button>();
	btnUnlimited->setText(Lang::get(L"video.limitUnlimited"));
	btnUnlimited->setSize(kBtnW, kBtnH);
	btnUnlimited->setFontSize(13.f);
	btnUnlimited->setBorder(1.f, 0xDDDDDDFF);
	btnUnlimited->setHoverBg(0xF5F5F5FF);
	btnUnlimited->onClick.add([this](Ling::Button*) { applyValue(0); });

	auto spacer = btns->makeChild<Ling::Node>();
	spacer->setFlexGrow(1.f);

	auto btnCancel = btns->makeChild<Ling::Button>();
	btnCancel->setText(Lang::get(L"video.limitCancel"));
	btnCancel->setSize(kBtnW, kBtnH);
	btnCancel->setFontSize(13.f);
	btnCancel->setBorder(1.f, 0xDDDDDDFF);
	btnCancel->setHoverBg(0xF5F5F5FF);
	btnCancel->onClick.add([this](Ling::Button*) { apply(false); });

	const unsigned int theme = Setting::getThemeColor();
	auto btnOk = btns->makeChild<Ling::Button>();
	btnOk->setText(Lang::get(L"video.limitOk"));
	btnOk->setSize(kBtnW, kBtnH);
	btnOk->setFontSize(13.f);
	btnOk->setMarginLeft(8.f);
	// 主按钮用主题色实底 + 白字。设了 bg 就必须配 hoverBg（见 liftColor 的注释）
	btnOk->setBg((theme << 8) | 0xFF);
	btnOk->setHoverBg(liftColor(theme));
	btnOk->setColor(0xFFFFFFFF);
	btnOk->setHoverColor(0xFFFFFFFF);
	btnOk->onClick.add([this](Ling::Button*) { apply(true); });

	// 窗口级兜底：TextBox 失焦时（比如点了标题栏）ESC 也要能取消
	onKeyDown.add([this](UINT key) {
		if (key == VK_ESCAPE) apply(false);
	});

	// ⚠️ 这里必须写全 Ling::WinBase::show()：本类也有个 show（静态、带参数的弹窗入口），
	// 它把基类那个无参 show 的名字挡住了 —— 直接写 show() 会解析到本类的静态版本上
	Ling::WinBase::show();
	edit->focus();
	edit->selectAll();
}

void WinRecordLimit::apply(bool ok)
{
	if (closed) return;
	if (ok) {
		const int val = Setting::parseTimeLimitText(edit ? edit->getText() : std::wstring{});
		// 一个数字都没有（清空了、或者敲了串别的）就当成"没改"，保留原值 ——
		// 想设不限时请点「不限」，那里是明确的
		if (val >= 0) {
			applyValue(val);
			return;
		}
	}
	closeWin();
}

void WinRecordLimit::applyValue(int minutes)
{
	if (closed) return;
	Setting::get()->setVideoTimeLimitMin(minutes);
	// 立刻让工具条重画倒计时。不刷也行（下一个 tick 就自己跟上了），
	// 但点完确定眼神还停在那一格上，隔一秒才变会被当成没生效。
	// ⚠️ 不能存 ToolVideo*：工具条可能已经因为到点自动停止而被销毁了，
	// 现问 living() 拿到的才是活的那个
	if (auto* tool = ToolVideo::living()) tool->onLimitChanged();
	closeWin();
}

void WinRecordLimit::closeWin()
{
	if (closed) return;
	closed = true;
	close();
}
