#pragma once
#include <include/Ling.h>

class ToolMain;
class ToolSub;
class ShapeBase;
class ShapeText;
class History;
class WinPin : public Ling::WinBase
{
public:
	~WinPin();
	// toolId 非空时，贴图窗口一打开就预选该标注工具（由 ToolCap 上的标注按钮直达进来）
	static void init(int x, int y, int w, int h, const std::wstring& toolId = L"");
	// 底图不来自 WinCap 的截屏，而是外部给的一块 BGRA、top-down、行紧凑（步长 = w*4）像素。
	// 滚动截图（WinLong）拼出来的长图走这条路进贴图窗口。
	static void initFromData(int x, int y, int w, int h, std::vector<BYTE>& data);
	// 当前屏幕上还有没有贴图窗口。用完即走模式靠它判断"活干完了没"：
	// 截图窗口关掉时贴图窗口可能才刚建起来，那时候不能退进程
	static bool hasWindow();
	// 退出流程里调：窗口对象是文件级静态变量，交给静态析构就在 CoUninitialize 之后了
	static void dispose();
	// 退出前把还开着的贴图合成存盘（数据目录 pin/），元数据写进 config.json 的 pin 组
	static void saveAll();
	// 启动时把上次退出前的贴图摆回原位，属性一并恢复
	static void restoreAll();
	void layoutTools();
	// 把底图与所有未撤销的 shape 合成后写入剪切板，成功即关窗
	void copyToClipboard();
	// 弹另存为对话框，把合成结果存成 PNG，成功即关窗；用户取消或失败则保持窗口
	void saveToFile();
	// 所有 ShapeText 共用的文本输入框，第一次用到时才建。
	// 共用而不是一个 shape 一个：TextBox 构造时会往窗口的十来个事件上挂回调，
	// N 个实例意味着每次鼠标移动都要跑 N 遍，而同一时刻只可能有一个 ShapeText 在编辑。
	Ling::TextBox* getTextBox();
	// ShapeText / ShapeNumber 进入 / 退出编辑时登记自己。传 nullptr 表示没有元素在编辑。
	void setEditingShape(ShapeBase* shape);
	// ToolSub 上的序号样式 / 外圈样式变了，让图上所有已画的序号重排几何与文字
	void refreshNumberShapes();
	// ToolSub 上的颜色 / 字号 / 粗体 / 斜体变了，转给正在编辑的文本立即生效
	void onToolStyleChanged();
	// ---- 贴图属性（ToolSub 的 pin 面板驱动，各项独立生效，见各自实现里的注释）----
	void setOpacity(float v);
	void setRounded(bool on);
	void setLocked(bool on);
	void setMouseThrough(bool on);
	void setPinTitle(const std::wstring& t);
public:
	// Ctrl+滚轮的缩放倍数，1 = 原始大小。底图与所有 shape 的坐标一律按底图的原始像素存，
	// 缩放只体现在两处：画的时候给 D2D 上一个缩放变换、收到鼠标坐标时先除回原始像素。
	// 存盘/复制走的是另一条不带变换的离屏绘制，所以导出的图永远是原始大小。
	// ShapeText 编辑中的文字是真控件（TextBox）画的，D2D 的变换管不到，它得自己乘这个倍数
	float scale{ 1.f };
	std::unique_ptr<ToolMain> toolMain;
	std::unique_ptr<ToolSub> toolSub;
	ShapeBase* shapeHover{ nullptr };
	// 本次按下新建出来的 shape（不是拖已有元素）。抬手时只对它做"有没有画出东西"的判定
	ShapeBase* newShape{ nullptr };
	// 本次按下之后光标有没有真的移动过。判"按下马上弹起"只认这个，
	// 不去看各 shape 的几何 —— 那些成员的初值状态不一，不可靠
	bool hasDragged{ false };
	std::unique_ptr<History> history;
	// 贴图窗口的底图。ShapeMosaic 要读它算马赛克块，ShapeEraser 拿它当"擦回原样"的画刷
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> screenImg;
private:
	WinPin(int x, int y, int w, int h, const std::vector<BYTE>* data = nullptr,
		const std::wstring& initToolId = L"");
	void onCreated() override;
	void layout() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	void onDown(POINT pos, BOOL isRight);
	void onMove(POINT pos);
	void onUp(POINT pos, BOOL isRight);
	void onKey(UINT key);
	void onTimerCB(UINT id);
	void onClosed();
	BOOL setCursor() override;
	// 离屏合成出最终图像的像素（BGRA、top-down、行步长紧凑为 size.width*4）。
	// 只画底图和未撤销的 shape，不含蓝色边框和夹点。
	// size 是出参，给的是底图的原始尺寸 —— 必须拿它去解释 pixels，不能用窗口的 w/h：
	// Ctrl+滚轮缩放改的只有窗口大小，两者对不上就是按错误的宽高读缓冲区（越界崩溃、图也是花的）
	bool getImagePixels(std::vector<BYTE>& pixels, D2D1_SIZE_U& size);
	// 另存为对话框会抢走前台并把 WinPin 激活，取消保存后用它把窗口层级和前台窗口恢复原样
	void restoreWindowState(HWND foregroundBeforeDialog);
	// 把窗口尺寸掰成"底图像素 × scale"。系统在 DPI 变化时会按新旧缩放比擅自缩放窗口
	// （贴图窗口的尺寸其实是钉死在底图上的，见构造函数里的注释），缩放倍数变了也用它
	void applyWinSize();
	// 底图的像素尺寸，也就是导出图的尺寸
	D2D1_SIZE_U getImgSize() const;
	// 窗口客户区坐标（物理像素）→ 底图坐标。shape 存的、认的都是底图像素
	POINT toImgPos(const POINT& pos) const;
	// 缩放到新倍数。anchor 是窗口客户区里要保持不动的那一点（一般就是光标位置），
	// 缩放后窗口跟着改大小，并反向挪一下窗口位置，让 anchor 底下的那块图还停在原处
	void applyScale(float newScale, POINT anchor);
	void paintScaleTip(ID2D1DeviceContext* ctx);
	// pinTitle 非空时画在窗口顶部的一条标题。属窗口装饰，不进导出图
	void paintTitle(ID2D1DeviceContext* ctx);
private:
	// 整个窗口内容都画在这块画布上，走 swap chain 后端：贴图窗口拖动 shape 时每帧重绘，
	// 单缓冲的合成表面会被采样到"擦干净→逐个重画"的中间态，表现为 shape 和边框整帧闪掉。
	Ling::Canvas* canvas{ nullptr };
	// 文本输入框与当前正在编辑的 ShapeText。非空表示"编辑中"：此时落在文本框里的
	// 鼠标事件、以及所有键盘事件都归 TextBox，WinPin 自己的那套要让路。
	Ling::TextBox* textBox{ nullptr };
	ShapeBase* editingShape{ nullptr };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBrush;
	// 右上角的倍数提示。非空即显示，缩放停手一会儿由定时器清掉
	Microsoft::WRL::ComPtr<IDWriteTextLayout> scaleTip;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushTipBg, brushTipText;
	bool isMouseDown{ false }, isClosed{ false };
	// 贴图属性。锁定不是改窗口样式实现的（Ling 自己管拖动），靠 onDown 里早退；
	// 不透明度也不是 WS_EX_LAYERED（窗口带 WS_EX_NOREDIRECTIONBITMAP，与分层窗口冲突），
	// 直接调 composition 树根节点的不透明度
	float opacity{ 1.f };
	bool isRounded{ false };
	bool isLocked{ false };
	std::wstring pinTitle;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> titleLayout;
	// onDpiChanged 与 onSizeChanged 之间的接力标记，见构造函数里的注释
	bool dpiChanged{ false };
	POINT pressPos{ 0,0 };
	// 自己认双击用的上一次按下时间与位置。同 WinCap：Ling 的窗口类没带 CS_DBLCLKS，
	// 收不到 WM_LBUTTONDBLCLK，只能按系统的双击间隔和双击判定框自己算。
	// 位置存的是屏幕坐标 —— 拖窗口时光标的客户区坐标不动，只有屏幕坐标能区分拖动和双击
	ULONGLONG lastDownTime{ 0 };
	POINT lastDownPos{ 0,0 };
	// 上一次按下是不是新建了一个留得住的元素（现在只有序号：按一下就成形，
	// 别的都在抬手时按"没画出东西"清掉了）。双击的前半段放下的东西不该被复制进剪切板
	bool prevPressCreatedShape{ false };
};

