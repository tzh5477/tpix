#pragma once
#include <include/Ling.h>

class CutMask;
class CapLong;
class CapVideo;
// 截图主窗口。铺满整个虚拟桌面，拖框结束就走人 —— 框完的那块图交给贴图（编辑）窗口，
// 后面的标注、长图、录屏、文字识别、二维码都在那边接着做。
// 留下的只有三种"另起一次流程"的入口：悬浮球的一键图标、命令行 --enter、
// 以及编辑界面右侧那条竖排上的长图 / 录屏（它们用的是同一套）。
class WinCap:public Ling::WinBase
{
public:
	~WinCap();
	// 进截图。配了延时就先走倒计时，数完由 WinDelay 回调下面这个。
	// enter 是"框完选区直接走哪条路"（long / video / ocr / qr / pin），空串表示照常出工具条 ——
	// 悬浮球上那些一键图标点的就是它，命令行 --enter=xxx 走的是同一条路
	static void init(const std::wstring& enter = L"");
	static void initNow(const std::wstring& enter = L"");
	static WinCap* get();
	// 退出流程里调：窗口对象是文件级静态变量，交给静态析构就在 CoUninitialize 之后了
	static void dispose();
	// 退出流程里调：正在录制时先把编码线程停掉，否则线程与设备会卡住
	static void stopIfRecording();
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> getCutImg();
	// 整屏原图。贴图窗口的常驻剪裁要的是它 —— getCutImg() 给的只是选区那一块，
	// 一旦裁掉就补不回来，采样点往外拖时得从这张原图里把框外的画面取回来
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> getScreenImg() { return screenImg; }
	// 工具条统一定位规则：右边与选区右边对齐，下方空间够就摆在选区右下方，
	// 不够就摆右上方，上下都不够就盖在选区右下角内部（留一点边距）。
	// 现在只有长图 / 录屏那两条工具条（ToolLong / ToolVideo）还在用它
	void layoutTool(Ling::WinBase* tool);
	// 整窗让出鼠标：录制中用户要能直接操作被录的应用
	void setMouseTransparent(bool transparent);
	// CapLong 开始滚动之前把选区抠成一个洞，滚轮消息才落得到底下的目标窗口上
	void hollowWin();
	void restoreWin();
	// 下面都是给工具条用的门面 ————————————————
	// toolId 非空时，进贴图窗口的同时预选该标注工具。框选那条路传的就是 rect ——
	// 框完直接是编辑态、矩形已经拿在手里，省掉"先点贴图、再点工具"两步
	void startPin(const std::wstring& toolId = L"");
	void startLong();
	void startVideo();
	void startOcr();
	void startQrcode();
	void saveToFile();
	void copyToClipboard();
	// 一张图真正产出之后（存盘 / 复制）记进截图历史
	void recordHistory(const int w, const int h, BYTE* data);
	// ToolVideo，转给 capVideo
	void startMp4(bool useSpeaker, bool useMic);
	void startGif();
	// 录制中暂停 / 继续，转给 CapVideo（工具条上的暂停按钮用）
	void setRecordPaused(bool on);
	std::wstring stopRecord();
	// ToolLong，转给 capLong
	// ToolLong 的摆放规则在 CapLong 手里，它 DPI 变了要重走一遍，从这里转进去
	void layoutLongTool();
	void longPin();
	// ToolLong 上的新增按钮：手动 / 自动开关、二次剪裁
	void toggleLongMode();
	void longStartCrop();
	// 剪裁框确认（回车）。返回是否已经接手，好让调用方跳过原本的动作
	bool longConfirmCrop();
	// 长截图收工并贴图。ESC 走的就是这条：滚完即贴图，再按一次 ESC 由贴图窗口退出
	void longFinishAndPin();
	bool isLongManual() const;
	// 长截图已经拼出图了没有。ToolLong 上那几个出口按钮靠它挡住"还没开始就点"
	bool longHasImage() const;
	// 用户在另存为对话框里取消时返回 false，此时图还在，不该收工
	bool longSaveToFile();
	void longCopyToClipboard();
public:
	// CapLong / CapVideo 用的就是这一份选区，它们自己不再框选
	std::unique_ptr<CutMask> cutMask;
private:
	WinCap();
	void onCreated() override;
	void layout() override;
	BOOL setCursor() override;
	LRESULT onHitTest(const POINT pos) override;
	void setPixPos(POINT pos);
	void getPixImg(POINT pos);
	void paintPix(ID2D1DeviceContext* ctx);
	void onKey(UINT key);
	// 把当前阶段手上的图存进剪切板，效果与 Ctrl+C 一致。
	// 三个阶段各有各的图（选区像素 / 拼好的长图 / 录到的视频），
	// 还在拖框取色（Select）时手上什么都没有，什么也不做。
	// 注意：Adjust 阶段不从这里走 —— 那个阶段回车改成了"收放工具条"（见 onKey）
	void copyCurrentStage();
	void onDown(POINT pos, bool isRight);
	void onMove(POINT pos);
	void onUp(POINT pos, bool isRight);
	// 拖框 / 调选区期间用：InvalidateRect 之后紧接着 UpdateWindow，把这一帧当场逼出来。
	// WM_PAINT 在 GetMessage 里的优先级排在鼠标输入之后，鼠标一动就又先来一条 WM_MOUSEMOVE，
	// 于是连续拖动时 WM_PAINT 一直排不上号，选区要等鼠标停住才更新 —— 就是那个"迟滞感"。
	// UpdateWindow 绕开排队直接发 WM_PAINT，每个鼠标事件一帧，既不积压也不滞后
	void refreshNow();
	// 拖动期间抓住鼠标：光标掠过挂在选区边上的工具条时，WM_MOUSEMOVE 会进工具条而不是本窗口，
	// 选区就卡在原地不动了，等光标离开工具条才猛地跳过来 —— 顿挫感的一半来自这里
	void captureMouse();
	void releaseMouse();
	void onClosed();
	// 命令行给了 --enter=xxx（long / video / ocr / qr / pin）时，框完选区不照常进编辑界面，
	// 直接走对应的那条路 —— 悬浮球上那几个一键图标走的是同一条路。
	// 返回是否已经接手；值不认识（拼错了）就返回 false，照常进编辑界面
	bool enterByArg();
	// DPI 变了之后重走一遍工具条的摆放规则（哪个阶段就重排哪个工具条）
	void relayoutTool();
	// 进长图 / 录屏阶段的公共动作：收掉底图，并提到最上层
	void enterLiveStage();
	// 选区内的像素。BGRA、top-down、行紧凑，可以直接喂 Util 的存盘与剪切板
	bool getCutPixels(std::vector<BYTE>& pixels, int& cw, int& ch);
	std::tuple<int, int, int, int> getCMYK(const BYTE& r, const BYTE& g, const BYTE& b);
private:
	// 拖框取色 -> 直接进编辑界面（预选矩形工具）；Long / Video 是另外两条独立流程
	//（悬浮球一键图标、命令行 --enter，以及编辑界面右侧竖排上的长图 / 录屏）。
	// Adjust 只是那几条路上的一个中转值：startLong / startVideo 拿它当"选区已经定下来"的凭据，
	// 所以 enterByArg 会先把它置上，紧接着就被换成 Long / Video。它不常驻 ——
	// 用户手上没有"停在 Adjust 阶段"的时候（框完就走，选区要调是在编辑界面上拖裁剪手柄）
	enum class CapStage { Select, Adjust, Long, Video };
	CapStage stage{ CapStage::Select };
	std::unique_ptr<CapLong> capLong;
	std::unique_ptr<CapVideo> capVideo;
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> screenImg,pixImg;
	D2D1_RECT_F pixSrcRect{};
	// 本次进截图要直接走的阶段（悬浮球的一键图标 / 命令行 --enter）。
	// 空串表示照常拖框、然后进编辑界面。框选在 onUp 里才结束，所以得先存下来
	std::wstring enterArg;
	// 铺满窗口的画布，走 swap chain 双缓冲：底图、蒙版、放大镜每帧都重画，
	// 单缓冲会让合成器采到"擦干净还没画完"的中间态
	Ling::Canvas* canvas{ nullptr };
	POINT pixPos;
	bool isPress{ false }, isClosed{ false }, isMouseTransparent{ false };
	// 这一轮拖动是在画手绘选区（而不是拉矩形）。手绘不改拖动中的蒙层语义，
	// 只决定 onMove / onUp 该调 CutMask 的哪一组方法
	bool isPolyDrag{ false };
	// onDpiChanged 与 onSizeChanged 之间的接力标记，见构造函数里的注释
	bool dpiChanged{ false };
	// 进长图 / 录屏后不再画底图：底图是拖框那一刻的静态截图，
	// 留着的话录屏和滚动截图拿到的都是这张死图
	bool hideScreenImg{ false };
	// 截图前用户正在用的那个窗口。开了自动粘贴就在复制之后把焦点还给它。
	// 必须和指针快照一样在建窗之前记：窗口一出来前台就是我们了
	HWND prevForeground{ nullptr };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushText;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> crossBrush;
};

