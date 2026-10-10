#include "pch.h"
#include "../Setting.h"
#include "../Win/WinCap.h"
#include "../Win/WinVideoEdit.h"
#include "../Util.h"
#include "../Lang.h"
#include "../Tip.h"
#include "../Win/WinRecordLimit.h"
#include "ToolVideo.h"

namespace {
	// 秒 → mm:ss；过了 1 小时才带小时位 —— 上限可配到 1440 分钟，
	// 位数不能写死成两位分钟（"录一天"时那是 1440:00）
	std::wstring fmtDur(int sec)
	{
		if (sec < 0) sec = 0;
		const int h = sec / 3600;
		const int m = (sec / 60) % 60;
		const int s = sec % 60;
		if (h > 0) return std::format(L"{}:{:02d}:{:02d}", h, m, s);
		return std::format(L"{:02d}:{:02d}", m, s);
	}
}

ToolVideo* ToolVideo::liveTool{ nullptr };

ToolVideo::ToolVideo(WinCap* win) : Ling::WinBase(), win(win)
{
	// 跟着宿主窗口的缩放走：WinBase 构造里取的是系统 dpi，宿主可能在另一块缩放比例不同的屏上
	dpi = win->dpi;
	// 位置由 CapVideo::makeTool() 在 createNativeWindow 之前设好，这里只算尺寸
	refreshSize();
	// 点按钮会把 ToolVideo 激活，键盘消息进的是它，转发给 WinCap 让 ESC 一致生效
	onKeyDown.add([this](UINT key) { this->win->onKeyDown(key); });
	onTimer.add([this](UINT id) { this->onTimerCB(id); });
	// DPI 变了（工具条被挪到缩放比例不同的显示器上，或者用户改了系统缩放）：
	// Ling 只会把窗口按系统给的建议矩形整体缩放一遍，我们自己定的那套摆放规则不会重跑，
	// 工具条就歪在别处了。位置也不能在 onDpiChanged 里直接改 —— 那个事件在 Ling 应用建议矩形
	// 之前触发，改了马上被覆盖，所以这里只记个标记，等建议矩形应用后紧随而来的 WM_SIZE 再动手
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (!dpiChanged) return;
		dpiChanged = false;
		refreshSize();                    //宿主的摆放规则要用宽高，先按新 dpi 把尺寸定下来
		this->win->layoutTool(this);
	});
}

void ToolVideo::refreshSize()
{
	setSize(isRecording ? recordingWidth() : settingWidth(), btnSize);
}

ToolVideo::~ToolVideo()
{
}

ToolVideo* ToolVideo::living()
{
	return liveTool;
}

void ToolVideo::onCreated()
{
	// 登记"活着的工具条是我"：改上限那个小窗拿不到 ToolVideo*，只能现问这里
	liveTool = this;
	// 工具条一销毁（到点自动停止、或者用户点了丢弃/存盘），那一格倒计时就没了，
	// 改上限的小窗也就没事可做了 —— 跟着收掉，别让它孤儿一样留在桌面上。
	// 注意这条路径可能正是从那个小窗的按钮点击栈上一路过来的（改完上限立刻到点）
	onDestroy.add([this]() {
		if (liveTool == this) liveTool = nullptr;
		WinRecordLimit::closeIfOpen();
	});

	tip = std::make_unique<Tip>(this);
	// 提示气泡是独立顶层窗口，本窗口被摘出屏幕捕获它不跟着走，得单独说一声
	tip->excludeFromCapture();
	body->setBg(0xFFFFFFFF);
	body->setBorder(1.f, 0xA8A8A8ff);
	body->setAlignItems(Ling::Align::Center);
	body->setFlexDirection(Ling::FlexDirection::Row);
	showSetting();
	show();
}

void ToolVideo::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}

float ToolVideo::settingWidth() const
{
	// 左右内边距 + MP4/GIF + 分隔符 + 系统声/麦克风 + 分隔符 + 开始/退出
	return formatW * 2 + spliterW * 2 + btnSize * 4;
}

float ToolVideo::recordingWidth() const
{
	// 左右内边距 + 计时 + 分隔符 + 暂停 + 分隔符 + 丢弃/存文件/存剪切板，
	// MP4 形态再多一枚「编辑」（GIF 不支持裁剪，不出现）
	const float actionBtns = (selectIndex == 0) ? 5.f : 4.f;
	return timerW + spliterW * 2 + btnSize * actionBtns;
}

Ling::Node* ToolVideo::makeSpliter()
{
	auto spliter = body->makeChild<Ling::Node>();
	spliter->setSize(spliterW, 18.f);
	spliter->setBg(0xDDDDDDff);
	return spliter;
}

Ling::Button* ToolVideo::makeIconBtn(const std::wstring& code)
{
	auto btn = body->makeChild<Ling::Button>();
	btn->setText(code);
	btn->setWidth(btnSize);
	btn->setHeightPercent(100.f);
	btn->setHoverBg(0xF2F2F2ff);
	btn->setFontFamily(L"icon");
	btn->setFontSize(13.f);
	return btn;
}

void ToolVideo::applyToggleStyle(Ling::Button* btn, bool selected)
{
	if (selected) {
		btn->setBg((Setting::getThemeSoft() << 8) | 0xFF);
		btn->setHoverBg((Setting::getThemeSoft() << 8) | 0xFF);
		btn->setColor((Setting::getThemeColor() << 8) | 0xFF);
		btn->setHoverColor((Setting::getThemeColor() << 8) | 0xFF);
	}
	else {
		btn->setBg(0);
		btn->setHoverBg(0xF2F2F2ff);
		btn->setColor(0x333333ff);
		btn->setHoverColor(0x333333ff);
	}
}

void ToolVideo::showSetting()
{
	// 重建前先把旧指针作废：removeAllChildren 会连带销毁所有子节点
	btnMp4 = nullptr;
	btnGif = nullptr;
	btnSpeaker = nullptr;
	btnMic = nullptr;
	btnPause = nullptr;
	btnEdit = nullptr;
	timerBtn = nullptr;
	// 按钮被销毁时 onLeave 不会触发，提示得手动收掉，否则它会一直挂在屏幕上
	tip->hide();
	body->removeAllChildren();
	setSize(settingWidth(), btnSize);

	// MP4 / GIF 二选一。高度比按钮条矮一截 + 圆角，选中时是一颗胶囊
	btnMp4 = body->makeChild<Ling::Button>();
	btnMp4->setText(L"MP4");
	btnMp4->setSize(formatW, btnSize);
	btnMp4->setFontSize(13.f);
	btnMp4->onClick.add([this](Ling::Button*) { onFormatClick(0); });
	tip->bind(btnMp4, Lang::get(L"video.outputMp4"));

	btnGif = body->makeChild<Ling::Button>();
	btnGif->setText(L"GIF");
	btnGif->setSize(formatW, btnSize);
	btnGif->setFontSize(13.f);
	btnGif->onClick.add([this](Ling::Button*) { onFormatClick(1); });
	tip->bind(btnGif, Lang::get(L"video.outputGif"));

	makeSpliter();

	btnSpeaker = makeIconBtn(L"\ue654");
	btnSpeaker->onClick.add([this](Ling::Button* btn) {
		// GIF 不带声音，此时两个音源按钮不可切换
		if (selectIndex != 0) return;
		selectSpeaker = !selectSpeaker;
		applyToggleStyle(btn, selectSpeaker);
	});
	tip->bind(btnSpeaker, Lang::get(L"video.recordSystem"));
	btnMic = makeIconBtn(L"\ue73b");
	btnMic->onClick.add([this](Ling::Button* btn) {
		if (selectIndex != 0) return;
		selectMic = !selectMic;
		applyToggleStyle(btn, selectMic);
	});
	tip->bind(btnMic, Lang::get(L"video.recordMic"));

	makeSpliter();

	auto btnStart = makeIconBtn(L"\ue660");
	btnStart->onClick.add([this](Ling::Button*) { startRecord(); });
	tip->bind(btnStart, Lang::get(L"video.startRecord"));
	auto btnClose = makeIconBtn(L"\ue62d");
	btnClose->onClick.add([this](Ling::Button*) { this->win->close(); });
	tip->bind(btnClose, Lang::get(L"video.exit"));

	applyFormatStyle();
}

void ToolVideo::showRecording()
{
	btnMp4 = nullptr;
	btnGif = nullptr;
	btnSpeaker = nullptr;
	btnMic = nullptr;
	btnPause = nullptr;
	btnEdit = nullptr;
	timerBtn = nullptr;
	tip->hide();
	body->removeAllChildren();
	setSize(recordingWidth(), btnSize);

	// 计时：有上限时是**倒计时**（剩 mm:ss，最后 30 秒变主题色），不限时是已录时长。
	// 点它弹小窗改上限 —— 应标那种"录到一半发现还得再撑两分钟"的场合用得上，
	// 改完立刻生效（onTimerCB 每个 tick 现读设置）。
	// 它是工具条的成员，而工具条挂了不出现在屏幕捕获里（CapVideo::makeTool），
	// 所以这个倒计时**不会被录进视频**，弹出来的小窗也一样要自己摘出去
	timerBtn = body->makeChild<Ling::Button>();
	timerBtn->setWidth(timerW);
	timerBtn->setHeightPercent(100.f);
	timerBtn->setFontSize(13.f);
	timerBtn->setBg(0);
	timerBtn->setHoverBg(0xF2F2F2ff);   // 设了底色就必须配 hover 底色（Button 的硬规矩）
	timerBtn->setColor(0x333333ff);
	timerBtn->setHoverColor(0x333333ff);
	timerBtn->onClick.add([this](Ling::Button*) { WinRecordLimit::show(hwnd, dpi); });
	tip->bind(timerBtn, Lang::get(L"video.timeLimitTip"));
	updateTimerText();

	makeSpliter();

	// 暂停。图标不用 icon 字体 —— 那套码位里没有确定的"暂停 / 继续"，
	// 而 U+2016 与 U+25B6 在系统字体里必定有字形，于是这两个按钮走默认字体
	btnPause = body->makeChild<Ling::Button>();
	btnPause->setWidth(btnSize);
	btnPause->setHeightPercent(100.f);
	btnPause->setHoverBg(0xF2F2F2ff);
	btnPause->setFontSize(13.f);
	applyPauseStyle();
	btnPause->onClick.add([this](Ling::Button*) {
		isPaused = !isPaused;
		win->setRecordPaused(isPaused);
		applyPauseStyle();
	});

	makeSpliter();

	// 丢弃 / 存文件 / 存剪切板，三条路都会停掉录制并结束整个流程
	auto btnDiscard = makeIconBtn(L"\ue62d");
	btnDiscard->onClick.add([this](Ling::Button*) { finishRecord(false); });
	tip->bind(btnDiscard, Lang::get(L"video.stopExit"));
	auto btnSave = makeIconBtn(L"\ue608");
	btnSave->onClick.add([this](Ling::Button*) { saveFile(); });
	tip->bind(btnSave, Lang::get(L"video.stopFile"));
	auto btnClipboard = makeIconBtn(L"\ue6ad");
	btnClipboard->onClick.add([this](Ling::Button*) { finishRecord(true); });
	tip->bind(btnClipboard, Lang::get(L"video.stopClipboard"));
	// 「编辑」= 停录并带着临时文件进裁剪窗。只有 MP4 有 —— GIF 是 8bit 调色板 +
	// 自己那套编码器，裁剪要重做量化与调色板，本期不做（见 Doc/P1-录后轻量视频编辑设计.md）
	if (selectIndex == 0) {
		btnEdit = makeIconBtn(L"\ue601");   // 铅笔，与 AI 对话窗的「编辑」同一枚
		btnEdit->onClick.add([this](Ling::Button*) { editRecord(); });
		// 键名是 video.editTip 不是 video.edit —— video.edit 让给裁剪窗那一组文案了
		// （Lang::get 按点分层级，video.edit 必须是对象才取得到 video.edit.title）
		tip->bind(btnEdit, Lang::get(L"video.editTip"));
	}
	// 换形态后宽度变了（多一个暂停按钮），重走一遍摆放规则，
	// 否则工具条右边缘会伸出录制区那一截
	win->layoutTool(this);
}

void ToolVideo::applyPauseStyle()
{
	if (!btnPause) return;
	btnPause->setText(isPaused ? L"\u25B6" : L"\u2016");
	// Tip::bind 是往 onEnter / onLeave 上挂回调，重绑一次就多一对，没有解绑接口
	// —— 所以只在文案真的变了时才重绑，否则点二十次暂停就是四十个回调
	auto text = Lang::get(isPaused ? L"video.resume" : L"video.pause");
	if (text != pauseTip) {
		pauseTip = std::move(text);
		tip->bind(btnPause, pauseTip);
	}
	applyToggleStyle(btnPause, isPaused);
}

void ToolVideo::onFormatClick(int index)
{
	if (selectIndex == index) return;
	selectIndex = index;
	// GIF 不录声音，切过去时把两个音源都关掉
	if (selectIndex == 1) {
		selectSpeaker = false;
		selectMic = false;
	}
	applyFormatStyle();
}

void ToolVideo::applyFormatStyle()
{
	if (!btnMp4 || !btnGif) return;
	applyToggleStyle(btnMp4, selectIndex == 0);
	applyToggleStyle(btnGif, selectIndex == 1);
	btnMp4->setColor(selectIndex == 0 ? (Setting::getThemeColor() << 8) | 0xFF : 0x333333ff);
	btnMp4->setHoverColor((Setting::getThemeColor() << 8) | 0xFF);
	btnGif->setColor(selectIndex == 1 ? (Setting::getThemeColor() << 8) | 0xFF : 0x333333ff);
	btnGif->setHoverColor((Setting::getThemeColor() << 8) | 0xFF);
	applyToggleStyle(btnSpeaker, selectSpeaker);
	applyToggleStyle(btnMic, selectMic);
}

void ToolVideo::startRecord()
{
	isRecording = true;
	isPaused = false;
	totalSeconds = 0;
	showRecording();
	setTimer(1000, tickTimerId);
	if (selectIndex == 0) {
		win->startMp4(selectSpeaker, selectMic);
	}
	else {
		win->startGif();
	}
}

void ToolVideo::updateTimerText()
{
	if (!timerBtn) return;
	// 上限每次现读，不缓存：设置页改了、录制中点计时改了，下一个 tick 就跟着变。
	// 0 = 不限 —— 不限时显示**已录**时长（就是个秒表）；有上限时显示**剩余**，
	// "还能录多久"一眼可见，不用自己拿上限去减
	const int limitMin = Setting::get()->getVideoTimeLimitMin();
	if (limitMin <= 0) {
		timerBtn->setText(fmtDur(totalSeconds));
		timerBtn->setColor(0x333333ff);
		timerBtn->setHoverColor(0x333333ff);
		return;
	}
	const int remain = std::max(0, limitMin * 60 - totalSeconds);
	timerBtn->setText(Lang::get(L"video.remain") + L" " + fmtDur(remain));
	// 最后 30 秒换成主题色。用颜色而不是闪烁：录屏工具条是个置顶小窗，
	// 用户的眼睛多半在别处，"变色"比"闪烁"更容易被余光抓到，也更好截图取证
	const unsigned int c = (remain <= 30) ? ((Setting::getThemeColor() << 8) | 0xFF) : 0x333333ff;
	timerBtn->setColor(c);
	timerBtn->setHoverColor(c);
}

void ToolVideo::onLimitChanged()
{
	updateTimerText();
}

void ToolVideo::onTimerCB(UINT id)
{
	if (id != tickTimerId) return;
	// 上限现读不缓存：录制当中改上限要立刻算数（见 updateTimerText）
	const int limitSec = Setting::get()->getVideoTimeLimitMin() * 60;
	// 暂停期间计时不走：录出来的视频里这一段本来就被跳过了，显示上得对得上。
	// 但仍要刷一次文字 —— 暂停着改了上限，倒计时得马上变
	if (isPaused) {
		updateTimerText();
		return;
	}
	totalSeconds += 1;
	updateTimerText();
	// 到上限就自动存盘收工（弹保存框）。不限时（limitSec == 0）永远走不到这里
	if (limitSec > 0 && totalSeconds >= limitSec) {
		saveFile();
	}
}

void ToolVideo::saveFile()
{
	hide();
	killTimer(tickTimerId);
	auto srcPath = win->stopRecord();
	// 空路径 = 一帧都没录到（刚开录就停了），没什么可存的，别弹保存框去打扰用户
	if (srcPath.empty()) {
		win->close();
		return;
	}
	auto tarPath = Util::getSaveFilePath(nullptr, selectIndex == 1 ? L"gif" : L"mp4");
	if (!tarPath.empty()) {
		CopyFile(srcPath.data(), tarPath.data(), false);
	}
	DeleteFile(srcPath.data());
	win->close();
}

bool ToolVideo::onSaveKey(bool toClipboard)
{
	if (!isRecording) return false;
	// 停录、存盘、关掉整个流程，这两条都在下面两个函数里一条龙做完
	if (toClipboard) finishRecord(true);
	else saveFile();
	return true;
}

void ToolVideo::editRecord()
{
	hide();
	killTimer(tickTimerId);
	auto srcPath = win->stopRecord();
	// 一帧都没录到（刚开录就点了编辑）：没东西可裁，整个流程收工。
	// stopRecord 那边已经把空壳文件删了
	if (srcPath.empty()) {
		win->close();
		return;
	}
	// 先把截图流程关掉再开编辑窗：录制遮罩/工具条都该收工了，编辑窗是另一件事。
	// WinCap 的 C++ 对象要到下一轮消息循环才释放（见 WinCap::onClosed），
	// 所以这里紧接着用 srcPath 是安全的
	win->close();
	WinVideoEdit::init(srcPath);
}

void ToolVideo::finishRecord(bool toClipboard)
{
	hide();
	killTimer(tickTimerId);
	auto srcPath = win->stopRecord();
	if (toClipboard) {
		// 文件留在临时目录里，剪切板持有的是它的路径，不能删。
		// 空路径 = 一帧都没录到，没东西可放进剪切板
		if (!srcPath.empty())
			Util::addFileToClipboard(srcPath);
	}
	else {
		DeleteFile(srcPath.data());
	}
	win->close();
}
