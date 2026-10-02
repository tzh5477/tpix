#pragma once
#include <include/Ling.h>

class WinCap;
class ToolLong;
class CutMask;
// 滚动截图。选区已经由 WinCap 定好了，所以它不是窗口，画布、定时器、光标全借宿主的。
class CapLong
{
public:
	CapLong(WinCap* win);
	~CapLong();
	// 宿主窗口要销毁了：停掉定时器、收掉工具条
	void dispose();
	// 以下几个都由 WinCap 在对应的事件里转进来
	void onMove(POINT pos);
	void onUp(POINT pos);
	// 剪裁阶段的按下 / 抬起。滚动阶段不需要：那时光标归被截的窗口
	void onDown(POINT pos, bool isRight);
	void onTimerCB(UINT timerId);
	void setCursor();
	void paint(ID2D1DeviceContext* ctx);
	// 三个都是 ToolLong 的按钮动作，做完由 ToolLong 关掉宿主窗口。
	// saveToFile 返回是否真的存下来了：用户在另存为对话框里按了取消时不该收工
	void copyToClipboard();
	bool saveToFile();
	void pin();
	// Ctrl+S / Ctrl+C 用：还没点"开始"的时候一张图都没有，此时快捷键不该生效
	bool hasImage() const { return !imgData.empty(); }
	// 正在滚动（还没收工）。ESC 与工具条上的按钮都靠它判断该不该先停下来
	bool isRunning() const { return isScrolling && !isFinish; }
	bool isCropping() const { return isCrop; }
	// 手动 / 自动切换：手动不发滚轮，只按固定间隔抓屏比对，滚动条由用户自己拖
	bool isManual() const { return manual; }
	void toggleMode();
	// 收工。reachedEnd = 是滚到底自己停的（会显示"已触底"），false = 用户叫停的。
	// toPin = 收工后把拼好的图钉到桌面上（ESC 走的就是这条）
	void finish(bool toPin);
	// 二次剪裁：滚动时带进来的滚动条断断续续，成图之后可以再框一次把它剪掉
	void startCrop();
	// 回车确认剪裁：把剪裁框映射回成图像素，就地替换 imgData
	void confirmCrop();
	void cancelCrop();
	// ToolLong 的摆放规则：摆在选区右侧（右边放不下就改到左侧），底边与选区底边齐，
	// 最后一律夹进工作区 —— 全屏截图时前两条规则算出来的都在屏幕外。
	// 建窗口时走一遍，工具条的 DPI 变了之后由它回头再走一遍
	void layoutTool();
private:
	void firstStep();
	void makeImgPreview();
	void capStep();
	void makeTool();
	void paintImgPreview(ID2D1DeviceContext* ctx);
	void stopCap(bool reachedEnd);
	void makeStopText(bool reachedEnd);
	// 沿滚动轴找出两帧开始不一样的位置：竖向是行号，横向是列号。全同返回 -1
	int findChangeStart(const std::vector<BYTE>& data);
	// 匹配出这一帧相对上一帧滚了多少像素。0 表示没对上
	int matchShift(const std::vector<BYTE>& data);
	// 把新帧里 changeStart 之后的内容接到结果图上
	void stitch(const std::vector<BYTE>& data, const int shift);
	// 配置的那个方向滚不动：换另一个方向再来一次，一次截图里只换一次
	void flipDir();
	// 剪裁：把成图缩到窗口里铺开，记下映射关系；画与命中都靠它
	void makeCropImg();
	void paintCrop(ID2D1DeviceContext* ctx);
	void makeCropTip();
	// 重新起表等下一次抓屏 / 发下一次滚轮
	void armScroll();
	// 这一帧"没滚得动"：手动模式就此打住（用户自己在控制节奏），自动模式累计到次数就换方向 / 触底
	void countDismiss();
private:
	WinCap* win;
	bool isShowStartBtn{ false }, isScrolling{ false }, isFinish{ false };
	bool firstCheck{ true };
	// true = 横向滚动（拼出来的图往右长），false = 竖向
	bool horizontal{ false };
	bool dirFlipped{ false };
	// true = 手动模式：不发滚轮，只轮询抓屏，滚动条交给用户拖
	bool manual{ false };
	int dismissTime{ 0 };
	// 抓到"帧在变但匹配不出滚动量"的帧时连续等待的次数，防止一直卡住
	int settleRecheckCount{ 0 };
	// 两帧开始不一样的位置，沿滚动轴计：竖向是行号，横向是列号
	int changeStart{ -1 };
	D2D1_RECT_F stopTextRect{};
	// 两处文字的绘制起点。IDWriteTextLayout 默认左上对齐，DrawTextLayout 给的又是
	// layout 框的左上角，所以得先测出文本实际宽高，才能算出居中要的那个起点
	D2D1_POINT_2F stopTextPos{};
	D2D1_SIZE_F startTextSize{};
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layoutTextStart;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layoutTextEnd;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layoutCropTip;
	float startCircleR{ 30.f };
	POINT circleCenter{};
	HWND targetHwnd{ nullptr };
	std::unique_ptr<ToolLong> tool;
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> imgPreview;
	std::vector<BYTE> imgData;
	std::vector<BYTE> img1;
	int imgW{ 0 }, imgH{ 0 };
	// 成图尺寸：竖向时宽 = imgW、高在长；横向时高 = imgH、宽在长
	int resultW{ 0 }, resultH{ 0 };
	POINT capStartPos{};
	// —— 二次剪裁 ——
	bool isCrop{ false };
	bool cropDragging{ false };
	// 这一轮拖动是在调已有的剪裁框（而不是第一次框出它）
	bool cropAdjusting{ false };
	// 剪裁用的独立蒙层，与宿主的 cutMask 分开：它框的是成图，不是屏幕上的选区
	std::unique_ptr<CutMask> cropMask;
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> cropImg;
	// 成图铺在窗口里的落点与缩放。剪裁框（窗口坐标）换算回成图像素全靠它
	D2D1_RECT_F cropDest{};
	float cropScale{ 1.f };
};
