#pragma once
#include <include/Ling.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "VideoTrim.h"

// 录后「裁剪 + 导出」编辑窗。
//
// 形态：独立窗口（同 WinHistory / WinAiChat 那套单例），不放进 WinCap 的 stage 体系 ——
// 录完那一刻整个截图流程已经收工了，编辑是另一件事。
//
// 临时文件所有权：**录制的 temp.mp4 从 ToolVideo 交到这里，由本窗口负责到最后** ——
// 丢弃 / 导出成功 / 关闭重开都不该留下碎片。剪切板那条路例外：剪切板持有的是导出产物
// 的路径，所以原始 temp 可以（也应该）删掉。
//
// 线程：两条。
//   ① 预览解码线程：按命令 seek / 顺序解码，把最新一帧丢进单槽信箱，UI 定时器取走画出来。
//      UI 线程永不阻塞（拖动播放头时 seek 要重解一个 GOP，放 UI 线程上就是卡顿）
//   ② 转码线程：跑一次 VideoTrim::trim，进度与结果用原子量回传
class WinVideoEdit : public Ling::WinBase
{
public:
	~WinVideoEdit();
	// srcPath 是录制留下的临时文件。已经开着就直接拉到前台（不重开、不换源）
	static void init(const std::wstring& srcPath);
	// 关窗并把 temp 一起收掉（进程退出时用）
	static void dispose();
	static bool isOpen();

private:
	WinVideoEdit(const std::wstring& srcPath);

	// ---- 两块自绘画布 ----
	// 预览：把当前帧等比（contain）摆在中间。位图只建一次，之后逐帧 CopyFromMemory ——
	// 1080p 每帧重建位图等于每帧一次 8MB 分配 + 拷贝，掉帧很明显
	class PreviewCanvas : public Ling::Canvas
	{
	public:
		explicit PreviewCanvas(Ling::WinBase* win) : Ling::Canvas(win) { enableSwapChain(); }
		void setFrame(const std::vector<BYTE>& bgra, int w, int h);
		void clearFrame();
		// 公开：窗口那边拿到新帧/状态变了要主动重画一次（不用整窗 refresh，省一次全量布局）
		void layout() override;
	private:
		Microsoft::WRL::ComPtr<ID2D1Bitmap1> bmp;
		int fw{ 0 }, fh{ 0 };
	};

	// 时间轴：轨道 + 选中区间 + 两个手柄 + 播放头。几何只有一份 ——
	// 画和命中都走 WinVideoEdit::trackGeom()，免得"看着在这、点着在那"
	class TimelineCanvas : public Ling::Canvas
	{
	public:
		explicit TimelineCanvas(Ling::WinBase* win) : Ling::Canvas(win) { enableSwapChain(); }
		// makeChild<T>() 只会传 win 进来，宿主指针建完再补
		void setOwner(WinVideoEdit* o) { owner = o; }
		void layout() override;
	private:
		WinVideoEdit* owner{ nullptr };
	};

	void onCreated() override;
	void layout() override;
	BOOL setCursor() override;

	void buildUi();
	void buildHeader(Ling::Node* header);
	void buildBottom(Ling::Node* bar);
	void buildFailedUi();

	// 时间轴几何（画布本地坐标）。padX 是两端留白，手柄中心只在这个区间里走
	struct TrackGeom
	{
		float x0{ 0.f }, x1{ 0.f };
	};
	TrackGeom trackGeom() const;
	float xOfMs(long long ms) const;
	long long msOfX(float x) const;

	// 拖动时间轴（localX 是画布本地 x）。按 dragMode 决定动的是起点 / 终点 / 播放头
	void onDragTo(float localX);
	void onKey(UINT key);

	// ---- 播放 ----
	void togglePlay();
	void setPlaying(bool on);
	void seekTo(long long ms);
	void stepFrame(int frames, bool bySecond);
	void updateLabels();
	void onTick();
	// 解码线程：seek / 顺序解码 / 单槽信箱
	void decodeLoop(std::stop_token st);
	void startDecode();

	// ---- 导出 ----
	void exportAs(bool toClipboard);
	void saveRaw();
	void discard();
	void finishExport(int rc, int pct);
	bool exporting() const { return jobState.load() == kJobRunning; }
	void closeWindow();
	void showError(const std::wstring& text);

	static constexpr UINT kTickId{ 7001 };
	static constexpr int kTickMs{ 33 };
	// 最短可留片段：比这还短就不让拖（0.1 秒）
	static constexpr long long kMinSpanMs{ 100 };
	// 手柄的抓取半径（逻辑像素）
	static constexpr float kGrabR{ 9.f };
	// 转码任务状态。转码本身的返回码是负数，所以两个哨兵不能用 -1 / -2
	static constexpr int kJobIdle{ -100 };
	static constexpr int kJobRunning{ -99 };

private:
	std::wstring src;
	VideoTrim::Info info;
	bool loadOk{ false };
	std::wstring loadErr;

	PreviewCanvas* preview{ nullptr };
	TimelineCanvas* timeline{ nullptr };
	Ling::Label* posLabel{ nullptr };      // ▶ 00:12.30 / 03:12.00
	Ling::Label* rangeLabel{ nullptr };    // 起点 / 终点 / 裁剪后
	Ling::Button* playBtn{ nullptr };
	Ling::Node* progressBox{ nullptr };
	Ling::Node* progressFill{ nullptr };
	Ling::Label* statusLabel{ nullptr };
	Ling::Button* exportBtn{ nullptr };
	Ling::Button* copyBtn{ nullptr };
	Ling::Button* rawBtn{ nullptr };
	Ling::Button* discardBtn{ nullptr };

	// 状态（全部只在 UI 线程读写）
	long long inMs{ 0 }, outMs{ 0 }, playheadMs{ 0 };
	bool playing{ false };
	int dragMode{ 0 };      // 0 无 / 1 拖起点 / 2 拖终点 / 3 拖播放头
	bool exportToClip{ false };
	// 产物已经进了剪切板（CF_HDROP 只存**路径**，不复制文件）⇒ 关窗时不能删它
	bool tmpToClipboard{ false };
	std::wstring tmpOut;    // 转码产物路径

	// 解码线程的共享量
	std::mutex mtx;
	std::vector<BYTE> front;      // 待画的一帧（UI 取走时与线程的临时缓冲互换）
	int frontW{ 0 }, frontH{ 0 };
	long long frontPts{ 0 };
	bool freshFrame{ false };
	long long pendingSeek{ -1 };  // >=0 表示有一条定位请求
	bool decPlay{ false };
	bool decEof{ false };
	std::wstring decErr;
	std::jthread decThread;

	// 转码线程的共享量
	std::jthread jobThread;
	std::atomic<bool> jobCancel{ false };
	std::atomic<int> jobProgress{ -1 };   // -1 = 没在跑；0..100
	std::atomic<int> jobState{ kJobIdle };  // 跑起来就是 kJobRunning，收工时换成 VideoTrim 的返回码
	std::wstring jobErr;
};
