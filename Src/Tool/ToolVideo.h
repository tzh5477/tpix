#pragma once
#include <include/Ling.h>

class WinCap;
class Tip;
// 录屏工具条。两种形态，切换时把 body 的子节点整批重建（同 ToolSub 的做法）：
//   未录制：MP4 / GIF 二选一 + 系统声 / 麦克风开关 + 开始录制 + 退出
//   录制中：计时 + 暂停 / 继续 + 丢弃 / 存文件 / 存剪切板 / 编辑（仅 MP4）
// 「编辑」是"停录并进裁剪窗"：裁剪靠重编码，GIF 那条自己一套 8bit 调色板，
// 不支持裁剪，所以 GIF 记录形态下这一枚不出现
//
// 计时那一格：设了「录制时长上限」就是**倒计时**（剩 mm:ss，最后 30 秒变主题色），
// 不限（上限 = 0）时是已录时长。点它弹小窗改上限，改成什么下一个 tick 就生效。
// 工具条整体不出现在屏幕捕获里（CapVideo::makeTool），所以这些数字不会被录进视频
class ToolVideo : public Ling::WinBase
{
public:
	ToolVideo(WinCap* win);
	~ToolVideo();
	// Ctrl+S / Ctrl+C 从 WinCap 转进来，等价于录制中那两个按钮。
	// 没在录制（还停在设置形态）时没东西可存，返回 false
	bool onSaveKey(bool toClipboard);
	// 时长上限被改了（录制中点计时弹出来的那个小窗改的）→ 立刻重画计时文字。
	// 就算不调，下一个 tick 也会自己刷新 —— 调它只是为了"改完马上看见"
	void onLimitChanged();
	// 当前活着的录屏工具条（同一时刻最多一个），没有则 nullptr。
	// 「改上限」那个小窗靠它回调 —— 它不持有 ToolVideo*，因为工具条随时可能被销毁
	//（典型是倒计时到点自动停止，那正是小窗最可能开着的时刻），存指针就是悬垂
	static ToolVideo* living();
private:
	void onCreated() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	void showSetting();
	void showRecording();
	void onFormatClick(int index);
	void onTimerCB(UINT id);
	void startRecord();
	// 停止录制并另存为文件；计时到上限时也走这里
	void saveFile();
	// 停止录制 → 关掉截图流程 → 把临时文件交给编辑窗裁剪
	void editRecord();
	// 停止录制后收工：toClipboard 为真则把文件放进剪切板，否则直接删掉
	void finishRecord(bool toClipboard);
	void updateTimerText();
	void applyFormatStyle();
	// 选中/未选中两套配色，与 ToolSub、ToolMain 的选中效果保持一致
	void applyToggleStyle(Ling::Button* btn, bool selected);
	// 暂停按钮上写的是"点了会变成什么"，与录音机那类按钮一个习惯
	void applyPauseStyle();
	Ling::Button* makeIconBtn(const std::wstring& code);
	Ling::Node* makeSpliter();
	float settingWidth() const;
	float recordingWidth() const;
	// 按当前 dpi 与当前形态（未录制 / 录制中）把窗口尺寸算出来并应用
	void refreshSize();
private:
	WinCap* win;
	// 见 living()。在 onCreated 里登记、onDestroy 里注销
	static ToolVideo* liveTool;
	// onDpiChanged 与 onSizeChanged 之间的接力标记，见构造函数里的注释
	bool dpiChanged{ false };

	std::unique_ptr<Tip> tip;
	Ling::Button* btnMp4{ nullptr };
	Ling::Button* btnGif{ nullptr };
	Ling::Button* btnSpeaker{ nullptr };
	Ling::Button* btnMic{ nullptr };
	// 计时文字。是 Button 不是 Label：点它要弹"改时长上限"那个小窗，
	// 而 Label 没有 onClick（见 Ling Label.h）
	Ling::Button* timerBtn{ nullptr };
	Ling::Button* btnPause{ nullptr };
	// 录制中那一枚「编辑」（只 MP4 形态有）
	Ling::Button* btnEdit{ nullptr };
	// 暂停按钮当前绑的提示文案，见 applyPauseStyle
	std::wstring pauseTip;
	// 0 = MP4，1 = GIF
	int selectIndex{ 0 };
	int totalSeconds{ 0 };
	bool selectSpeaker{ true }, selectMic{ false }, isRecording{ false }, isPaused{ false };
	// 以下都是逻辑像素，交给 Ling 的 setter 时由其内部乘 dpi
	static constexpr float btnSize{ 32.f };
	static constexpr float formatW{ 42.f };
	static constexpr float timerW{ 112.f };
	static constexpr float spliterW{ 1.f };
	static constexpr UINT tickTimerId{ 100 };
};
