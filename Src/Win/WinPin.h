#pragma once
#include <include/Ling.h>
#include <winrt/Windows.Data.Json.h>
#include "../AnimImage.h"
#include "../Canvas.h"
#include "../Ocr.h"

class ToolMain;
class ToolSub;
class ToolPinSide;
class ShapeBase;
class ShapeText;
class ShapeNumber;
class History;
class Canvas;
class WinPin : public Ling::WinBase, public CanvasHost
{
public:
	~WinPin();
	// 藏到屏幕的哪一条边上，对应 PinHiddenBar 的两扇窗。点「隐藏」按钮藏的一律走 Left
	//（作者最早要的形态就是左上角那几条竖线）；拖到屏幕左边 / 顶边释放时按拖到的那条边记下来
	enum class BarEdge { Left, Top };
	// toolId 非空时，贴图窗口一打开就预选该标注工具。截图那条路框完选区就直接这么进来
	//（预选 rect —— 框完就能画），所以用户看到的顺序是"框选 → 已经是编辑态"
	static void init(int x, int y, int w, int h, const std::wstring& toolId = L"");
	// 底图不来自 WinCap 的截屏，而是外部给的一块 BGRA、top-down、行紧凑（步长 = w*4）像素。
	// 滚动截图（CapLong）拼出来的长图走这条路进贴图窗口。
	// toolId 与 init 上那一个是同一个意思：长图截完是直接进编辑界面的（预选 rect），
	// 历史记录 / 剪贴板那几条路留空 —— 那几条要的就是"贴在那儿就行"
	static void initFromData(int x, int y, int w, int h, std::vector<BYTE>& data, const std::wstring& toolId = L"");
	// 动图贴图：底图是 frames[0]，随后由定时器逐帧换。src 是原始动图文件，
	// 退出持久化时把它拷进数据目录，重启后还能接着播
	static void initFromAnim(int x, int y, const std::wstring& src, std::vector<AnimFrame>& frames);
	bool hasAnim() const { return frames.size() > 1; }
	bool isAnimPlaying() const { return animPlaying; }
	void toggleAnim();
	// 把所有贴图并为一组 / 全部解组。成组后拖动与 Ctrl+滚轮缩放会带着整组一起动
	static void toggleGroupAll();
	int getGroupId() const { return groupId; }
	// 缩略图模式：窗口缩成一枚小图摆在原位，点一下或 Ctrl+T 还原（缩的是窗口，不是底图）
	void setThumbMode(bool on);
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
	// 工具栏整组显示 / 隐藏（ToolMain + ToolSub）。空格键走它。
	// 它不只是"开关窗口"，两边各带一步：收起来时连画笔一起放掉（cancelSelect），
	// 请回来时预选矩形工具（selectTool(L"rect")）—— 于是空格就是"编辑"与"拖着图走"
	// 两个状态之间的硬切换，来回都落在同一个起点上（框选完直接进编辑界面那条路也是
	// 预选 rect，见 WinCap::onUp，两个入口手感才对得上）。
	//（右键收起是另一套语义，那边只动窗口，见 onDown 里那段）
	void setToolsVisible(bool on);
	bool isToolsVisible() const;
	// 把底图与所有未撤销的 shape 合成后写入剪切板，成功即关窗
	void copyToClipboard();
	// 同上，但**不动窗口**：ToolMain 上的「复制」用 —— 用户复制一份之后
	// 多半还想接着标注 / 存文件。失败（取不到像素）返回 false
	bool copyImageToClipboard();
	// 弹另存为对话框，把合成结果存成 PNG，成功即关窗；用户取消或失败则保持窗口
	void saveToFile();
	// ---- 右边缘那条竖排（ToolPinSide）上的四个动作 ----
	// 长截图 / 录屏：**另起一次截图流程**去处理，当前这张不参与。
	// "不参与"是字面意思 —— 先把自己收进屏幕边上的书签条（窗口、底图、标注一概不动，
	// 见 setHidden），再起新任务。否则这张图会挡住新任务要截的那片屏幕、还会被录进去
	void startLongTask();
	void startVideoTask();
	// 文本识别：对**当前这张图**跑一次 OCR，结果开在识别窗里（可复制 / 换语言 / 表格模式）。
	// 与「选文」不是一回事：选文是在图上直接选字，这里是把文字整篇抽出来。
	// 用的是底图像素而不是合成结果 —— 标注压在字上只会给识别添乱
	void ocrToWindow();
	// 二维码识别：同样在当前这张图上解，解出来弹窗问要不要复制（与截图界面那一枚一致）
	void decodeQr();
	// 所有 ShapeText 共用的文本输入框，第一次用到时才建。
	// 共用而不是一个 shape 一个：TextBox 构造时会往窗口的十来个事件上挂回调，
	// N 个实例意味着每次鼠标移动都要跑 N 遍，而同一时刻只可能有一个 ShapeText 在编辑。
	Ling::TextBox* getTextBox() override;
	// ShapeText / ShapeNumber 进入 / 退出编辑时登记自己。传 nullptr 表示没有元素在编辑。
	void setEditingShape(ShapeBase* shape) override;
	// ---- CanvasHost：Canvas 与 Shape 层只认这十件事（清单见 Canvas.h），这里把它们接到窗口 ----
	// getTextBox / setEditingShape 就落在这上面两条，签名已经对得上 CanvasHost，不再重复声明
	float dpiValue() const override { return dpi; }
	float scaleValue() const override { return scale; }
	float widthValue() const override { return (float)w; }
	float heightValue() const override { return (float)h; }
	const std::wstring& curToolId() const override;
	ToolMain* getToolMain() override { return toolMain.get(); }
	ToolSub* getToolSub() override { return toolSub.get(); }
	void requestRefresh() override;
	// 标注图层本身（undo / redo、元素枚举）住在 Canvas 里，窗口只转发这一条给
	// ToolMain 的撤销/重做快捷键和 ToolSub 算最大序号用。定义在 cpp 里：
	// 头文件只认得 History 的前置声明，拿不到 drawing->history 的完整类型
	History* getHistory() const;
	// ToolSub 上的序号样式 / 外圈样式变了，让图上所有已画的序号重排几何与文字
	void refreshNumberShapes();
	// ToolSub 上的颜色 / 字号 / 粗体 / 斜体变了，转给正在编辑的文本立即生效。
	// styleEnumChanged = 触发这次改动的是"档位"按钮（箭头样式 / 线条类型 / 端点 / 线型）——
	// 只有这种时候才该把档位也套到选中那一笔上，颜色 / 粗细 / 填充这些不该顺手换掉形状
	void onToolStyleChanged(bool styleEnumChanged = false);
	// 「应用到全部」：把工具条当前样式套到图上同工具的所有标注
	void applyStyleToAllShapes();
	// 一键清除图上所有水印（水印面板上的「清除」）。走 History::undoShapes，
	// 只打撤销标记不真删 —— 清完还能 Ctrl+Y 找回来
	void clearWatermark();
	// ---- 贴图属性（ToolSub 的 pin 面板驱动，各项独立生效，见各自实现里的注释）----
	void setOpacity(float v);
	void setRounded(bool on);
	void setLocked(bool on);
	void setMouseThrough(bool on);
	void setPinTitle(const std::wstring& t);
	const std::wstring& getPinTitle() const { return pinTitle; }
	// pin 面板的三个开关读这里。都是"这张贴图自己的"实例态，不是全局配置 ——
	// 穿透刻意不进持久化：落盘的贴图恢复后若还穿透，既看不见也点不着，像图丢了
	bool getRounded() const { return isRounded; }
	bool getLocked() const { return isLocked; }
	bool getThrough() const { return isThrough; }
	// ---- 选文态（ToolMain 上那个「选文」开关）----
	// 整张图进入"文字选择"：拖拽按词选、Ctrl+C 把选中那段文字送进剪切板（不关窗）、ESC 退出。
	// 底图一进贴图窗口就在后台认一遍文字（Ocr::recognizeWords），所以点开这个开关通常立刻能选
	void setTextSelect(bool on);
	bool getTextSelect() const { return textSelect; }
	// 翻历史截图：step 正负表示往更早 / 更新翻一张（0 = 最新）。换底图会作废旧标注
	void previewHistory(int step);
	// 收成贴边细条 / 展开。悬停细条即展开，Ctrl+M 触发
	void setMinimized(bool on);
public:
	// Ctrl+滚轮的缩放倍数，1 = 原始大小。底图与所有 shape 的坐标一律按底图的原始像素存，
	// 缩放只体现在两处：画的时候给 D2D 上一个缩放变换、收到鼠标坐标时先除回原始像素。
	// 存盘/复制走的是另一条不带变换的离屏绘制，所以导出的图永远是原始大小。
	// ShapeText 编辑中的文字是真控件（TextBox）画的，D2D 的变换管不到，它得自己乘这个倍数
	float scale{ 1.f };
	std::unique_ptr<ToolMain> toolMain;
	std::unique_ptr<ToolSub> toolSub;
	// 编辑区右边缘外那条竖排（长截图 / 录屏 / 文本识别 / 二维码识别）。
	// 与上面两条一样是独立顶层窗口，凡是 show / hide / close / 重排都得跟着处理 ——
	// 那几处的入口都收在 setSideBarVisible 上
	std::unique_ptr<ToolPinSide> sideBar;
	// 本次按下之后光标有没有真的移动过。判"按下马上弹起"只认这个，
	// 不去看各 shape 的几何 —— 那些成员的初值状态不一，不可靠
	bool hasDragged{ false };
	// 「选择画布」模式（工具条最前那枚按钮按下的状态，原「选择器」）。它作用在**底图像素**上
	// （拉选区 / 搬画面），所以在它里面画布上的点击不参与对象选择。
	// 对象选择（点中元素 + Ctrl 加减选 + Ctrl 框选）**不再需要模式**，见 selecting()
	bool canvasMode{ false };
	bool canvasSelecting() const { return canvasMode; }
	// 进出「选择画布」唯一的入口：改状态 + 收掉对侧的选中 / 选区 + 重排工具条
	void setCanvasMode(bool on);
	// 把画布上的对象选中态整批收掉（单选 / 悬停 / 多选那一批 / 批量拖动与旋转态）。
	// 用户主动在工具条上换工具、或退出画笔时才调 —— 那两种情况下"还挑着上一批元素"
	// 没有意义：留着的话换个画笔随手按一下 Delete 会连整批一起删掉
	void clearObjectSelection();
	// 水印工具选中时把水印层铺上（没有才建）。整张图一层，所以不进"点击才落笔"那条路。
	// ToolMain 切到水印工具时调（ToolMain.cpp 的 selectTool）
	void ensureWatermark();
	// ---- 藏进屏幕边上的那条"书签条"（见 PinHiddenBar）----
	// 开关：藏着的时候再按一次就是放回来。藏起来的是"这扇窗"，位置、底图、标注一概不动。
	// 条上的 hover 只是把窗口临时显出来（peek），不改这个状态。
	// edge 只在 on 为真时有用：点「隐藏」按钮和拖到屏幕左边线都走 Left，拖到顶边线走 Top
	void setHidden(bool on, BarEdge edge = BarEdge::Left);
	bool getHidden() const { return isHidden; }
	// 这会儿藏在哪条边上。放回来之后这个值没有意义，下次藏的时候重新给
	BarEdge getBarEdge() const { return barEdge; }
	// hover 时"露一下" / 收回去。只动窗口，isHidden 不动 —— 条本身要一直留着，
	// 不然鼠标一离开条就没了，而"离开就收回去"正是这一套的行为
	void peek(bool on);
	// 隐藏条上那一条的颜色序号：新建贴图时按创建顺序轮转分配，之后不再变 ——
	// 藏了放、放了藏，同一张图前后得是同一个颜色
	int getBarColorIndex() const { return barColorIndex; }
	// 鼠标 / 键盘正落在这张贴图上（拖着窗口、正画一笔、文字编辑器开着）。
	// 隐藏条那边靠它避开"用户正拿着这张图"的时刻，否则拖着拖着图就没了
	bool isBusy() const;
	// 藏在 edge 那条边上的贴图，按创建顺序 —— 那一条边上的书签就按这个顺序排
	static std::vector<WinPin*> getHiddenPins(BarEdge edge);
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
	// Ctrl 的抬键。单独留一个是为了"再点一下 Ctrl 就取消框选"（见 onKey 开头那段）——
	// 键盘消息只送到有焦点的窗口，ToolMain / ToolSub 会把它们的 keydown 转回来，
	// keyup 也照转（那两处各一行）。
	// ⚠️ 名字**不能叫 onKeyUp**：那是 Ling::WinBase 上的事件成员名（winrt::event），
	// 在派生类里声明同名函数会把它整个藏起来，构造函数里那句 onKeyUp.add(...) 当场
	// 报 C2228（".add 的左边必须有类/结构/联合"）
	void onKeyRelease(UINT key);
	// ESC 的"退一步"：先退出当前操作（收起画笔 / 放掉选中），返回是否已经消费掉这一次 ESC。
	// 返回 false 表示已经没什么可退的了，调用方接着才关窗。已画下的标注一概不动
	bool stepBack();
	// 把三条工具条一起提到 topmost 组的最前面。工具条比 WinPin 先建窗口，
	// 而 topmost 组内后建者在上 —— 两者重叠时（全屏贴图的 overlay 模式）工具条会被底图整条盖住
	void raiseTools();
	// 右边缘那条竖排的收 / 放。它与下面两条工具条同生共死（都是独立顶层窗口），
	// 而"收放工具条"在窗口拖拽、右键、空格、缩略图、贴边细条、藏进书签条这些路上有十来处 ——
	// 散着写必漏一处，统一收在这一个口子上
	void setSideBarVisible(bool on);
	// 自己认双击（窗口类没带 CS_DBLCLKS）。比的是屏幕坐标，见实现里的说明。
	// 认一次就把 lastDownTime / lastDownPos 更新掉，所以同一次点击只能问一次
	bool takeDoubleClick();
	void onTimerCB(UINT id);
	void onClosed();
	// 拖动标注 / 框选这些"画面跟着鼠标走"的动作里用：InvalidateRect 之后紧接着 UpdateWindow，
	// 把这一帧当场逼出来。只管 InvalidateRect 的话，WM_PAINT 的优先级排在鼠标消息之后 ——
	// 光标一直在动就一直排不上号，画面要等鼠标停住才更新，就是那股"迟滞 / 顿挫"感。
	// WinCap 调选区当初也是这么治的（见 WinCap::refreshNow）
	void refreshNow();
	// ---- 选文：后台识别 + 选区（详见各自实现）----
	// 起一次后台识别。换底图 / 剪完一刀都会重认，seq 让老结果自己作废
	void startOcr();
	void clearOcr();
	// 识别结果回填（UI 线程）。origin 是起识别那一刻的 Canvas::imgOrigin —— 词框坐标要从
	// 底图像素换算到标注坐标系，剪裁正好在这中间发生时得用当时那个值
	void applyOcrWords(std::vector<OcrWord> words, POINT origin);
	// 把底图像素读一份到内存（BGRA、top-down、行紧凑），喂给识别引擎
	bool readBasePixels(std::vector<BYTE>& pixels, int& w, int& h);
	// 标注坐标 → 选区的"插入点"下标（0..ocrWords.size()）。与文本编辑器同一套：
	// 按阅读顺序数下来，落在词缝里、行尾空白上也能定出一个位置，不必非要命中某个词
	int caretIndexAt(const POINT& imgPos) const;
	int wordIndexAt(const POINT& imgPos) const;
	bool hasSelection() const { return selAnchor != selCur; }
	std::wstring selectedText() const;
	void copySelectedText();
	void showToast(const std::wstring& text);
	// 选中高亮（画在标注之上，用标注坐标系，所以跟着缩放和剪裁走）
	void paintTextSelect(ID2D1DeviceContext* ctx);
	// 顶部那条"正在识别文字…"/"未识别到文字"。按需重建，状态没变就不动
	void paintTextTip(ID2D1DeviceContext* ctx);
	void paintToast(ID2D1DeviceContext* ctx);
	// 当前选中的是不是"能在图上画东西的"标注工具。curId 为空（什么都没选）与 curId 为
	// pin（只开着贴图属性面板）都画不了 —— 这两种状态下左键该拖动贴图本身，
	// 而不是当成画笔落笔，否则选过一次贴图属性后整张图就拖不动了
	bool hasDrawTool() const;
	// ---- 画布上的"选择对象" ----
	// 它**不是一个工具、也没有模式**：只要手里拿着画笔（hasDrawTool），左键就先看光标底下
	// 有没有元素 —— 有就选中它（抬手时再把工具条切到那个元素的工具），没有才按当前画笔
	// 落新的一笔。唯一的例外是「选择画布」（canvasSelecting），那时候左键归底图选区。
	//
	// 所以判据不能看 curId：点中元素后 curId 换成了那个元素的工具（ToolSub 的面板才切得
	// 过去、滑块色板才有正确档位），拿它当判据的话第二次点选就不生效了
	bool selecting() const;
	// 选框的矩形（标注坐标系）。anchor / cur 谁大谁小不确定，统一归一化成矩形
	D2D1_RECT_F marqueeRect() const;
	// 框选抬手：把外接框与选框相交的元素收进 Canvas::multiSelected。
	// 「相交即选中」—— 细长元素（线条 / 箭头）只要被框碰到就算，要求整个框住得瞄得很准
	void collectMarquee();
	// 画框选的两层提示：正在拉的那个选框、以及多选那一批各自的外框。
	// 调用方还在标注坐标系的变换里，所以两者都跟着缩放 / 剪裁走
	void paintSelection(ID2D1DeviceContext* ctx);
	// ---- 多选那一批的批量操作（块选的"加选 / 批量移动 / 批量改样式 / 批量增删转"）----
	// 整组的外接框中心。批量旋转绕它转；一个都没有外接框时退回画布中心
	D2D1_POINT_2F multiSelectCenter() const;
	// 多选时画在画布右上角那三枚按钮的方框（**窗口坐标**，不跟底图缩放走）。
	// i：0 删除、1 复制、2 旋转
	D2D1_RECT_F batchBtnRect(const int i) const;
	// 命中的是第几枚批量按钮，没命中返回 -1（pos 是窗口客户区坐标）
	int batchBtnAt(const POINT pos) const;
	void paintBatchButtons(ID2D1DeviceContext* ctx);
	// 三枚按钮的动作：整批撤销（可 Ctrl+Y 找回）/ 整批复制一份（偏移错开）/ 进入拖拽旋转态
	void batchDeleteShapes();
	void batchCopyShapes();
	void toggleBatchRotate();
	// 把多选的拖动 / 旋转态收掉。退出选择态（换工具、ESC、右键收工具条）时调，
	// 与 multiSelected 一起清 —— 留着的话下一次进选择模式还停在上次的旋转态里
	void clearBatchState();
	// 标号工具的 hover 预览。鼠标还停在图上时，先在光标处画一个"将要落下的编号"，
	// 落笔之前就看得见号是多少（参考 pixpin）。预览实例不进 history、也不占号
	void updateNumberPreview(const POINT& imgPos);
	void hideNumberPreview();
	// ---- 常驻剪裁 ----
	// 底图四周那 8 个采样点（四角 + 四边中点）。拖它就是改"这张贴图保留原图的哪一块"：
	// 往里收缩、整体平移、往外扩大（有原图可扩的话），标注一个都不搬 —— 坐标映射整体挪
	//（Canvas::imgOrigin），所以拖完还能接着改样式、撤销、导出。
	// 顺序：0 左上 / 1 上 / 2 右上 / 3 右 / 4 右下 / 5 下 / 6 左下 / 7 左
	void paintCropHandles(ID2D1DeviceContext* ctx);
	void cropHandleCenters(D2D1_POINT_2F (&centers)[8]) const;
	// 采样点这会儿能不能上场。缩略图 / 细条这两种收法下窗口已经不是图了；藏起来时窗口根本
	// 看不见；锁定的贴图不许改；动图的窗口尺寸钉死在第一帧上，裁了就没法换帧
	bool canCrop() const;
	// 命中的是第几个采样点，没命中返回 -1（命中框比画出来的点大一圈，好点）
	int cropHandleAt(const POINT pos) const;
	// 手上正拉着一个采样点
	bool cropDragging() const { return cropHandle >= 0; }
	void startCropDrag(const int handle);
	// 拖到屏幕坐标 screenPos。用屏幕坐标而不是客户区坐标：拖左边 / 上边时窗口自己就在挪，
	// 客户区坐标跟着变，累加必然漂
	void dragCropTo(const POINT& screenPos);
	void endCropDrag();
	// 按 cropRect 重切底图、挪标注原点、重排窗口的位置与尺寸
	void applyCropRect();
	// 没有源图时现补一份：拿当前这张底图顶替自己。剪贴板 / 文件 / 长图 / 历史进来的贴图走这条，
	// 于是"向外拖"最多拖回它的边界 —— 那些图本来就是从外面拿来的，边界外没有东西可补
	void ensureCropSource();
	BOOL setCursor() override;
	// 离屏合成出最终图像的像素（BGRA、top-down、行步长紧凑为 size.width*4）。
	// 只画底图和未撤销的 shape，不含蓝色边框和夹点。
	// size 是出参，给的是底图的原始尺寸 —— 必须拿它去解释 pixels，不能用窗口的 w/h：
	// Ctrl+滚轮缩放改的只有窗口大小，两者对不上就是按错误的宽高读缓冲区（越界崩溃、图也是花的）
	bool getImagePixels(std::vector<BYTE>& pixels, D2D1_SIZE_U& size);
	bool swapImage(const std::vector<BYTE>& data, const int w, const int h);
	// 动图播放：把第 index 帧的像素拷进底图。帧尺寸与底图尺寸对不上就什么都不做
	// （贴图窗口的尺寸钉死在第一帧上，中途换尺寸的帧画出来是歪的）
	void showFrame(int index);
	void setAnimPlaying(bool on);
	// restoreAll 的收尾：init*Data 刚把新窗口压进 winPins，back() 就是它。
	// 除了属性，还要把组号顶到 nextGroupId 之上 —— 否则新建的组会撞上恢复出来的老组号
	static void finishRestore(winrt::Windows::Data::Json::JsonObject obj);
	std::vector<AnimFrame> frames;
	int frameIndex{ 0 };
	bool animPlaying{ false };
	// 原始动图文件的路径，持久化时按它把文件拷进数据目录
	std::wstring animSrc;
	// 历史翻页当前指到哪一张（0 = 最新一张）。-1 表示还没翻过页
	int previewIndex{ -1 };
	// 收成细条 / 缩略图前的位置与尺寸，还原时恢复。两个模式互斥，共用这一组字段
	bool isMinimized{ false };
	bool isThumb{ false };
	// 上一次 layoutTools 算出来的是不是"工具条落在 WinPin 内部"（overlay）模式。
	// 重叠状态一变就得把工具条重新提到最前面，见 raiseTools
	bool toolsOverlay{ false };
	// 右边缘那条竖排压在窗口上（窗口大到两边都摆不下，被夹回窗口内才会发生）时置位。
	// 与 toolsOverlay 同一套：状态一变才提一次，不然每挪一次窗口就白调一次 SetWindowPos
	bool sideBarOverlapping{ false };
	int savedX{ 0 }, savedY{ 0 }, savedW{ 0 }, savedH{ 0 };
	// 缩略图模式下画底图用的倍数。0 表示不在缩略图模式，此时用 scale
	float thumbScale{ 0.f };
	// 缩略图收起来的那两条工具条，还原时要原样请回来。用户自己右键收的则不该替他打开
	bool toolsHiddenByThumb{ false };
	// 底图画到窗口上用的倍数：缩略图模式下是 thumbScale，否则是 Ctrl+滚轮那个 scale。
	// 底图与 shape 存的都还是原始像素，缩放全靠这一个变换，所以改它一处就够
	float viewScale() const { return isThumb ? thumbScale : scale; }
	// 把倍数夹进这张图能接受的范围（见 applyScale 里那两条上下限的来由）
	float clampScale(float v) const;
	// 组内同步缩放用：只改倍数重排窗口，不带 anchor 逻辑（锚点只对被滚轮指着的那张有意义）
	void syncScale(float newScale);
	static void syncGroupPos(WinPin* src, int dx, int dy);
	// 贴到当前显示器的某条边 / 搬到相邻显示器（dir = -1 左，+1 右），组内成员同步位移
	void alignToEdge(UINT key);
	void moveToMonitor(int dir);
	// 光标这会儿压在屏幕的哪条边线上（都不在就没有值）。拖完窗口看它 —— 拖到边上松手
	// 就是把这张图藏到那条边上。判的是光标而不是窗口：把窗口贴着边摆成 x=0 是很常见的停法，
	// 那不该被当成"藏起来"；而光标推到屏幕最边上（系统不会再让它走出去）是明确的"推到头了"
	std::optional<BarEdge> edgeAtCursor() const;
	// 贴图组号。0 = 不成组
	int groupId{ 0 };
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
	// 标注画布：底图 + shapes + 悬停/正在画的那一个，由窗口组合进来而不是继承（见 Canvas.h）。
	// 与上面那个 Ling 的绘制节点不是一回事，两个别混：一个是"在哪里画"，一个是"画什么"
	std::unique_ptr<Canvas> drawing;
	// 文本输入框与当前正在编辑的 ShapeText。非空表示"编辑中"：此时落在文本框里的
	// 鼠标事件、以及所有键盘事件都归 TextBox，WinPin 自己的那套要让路。
	Ling::TextBox* textBox{ nullptr };
	ShapeBase* editingShape{ nullptr };
	// 标号工具的 hover 预览。它是独立的一份 ShapeNumber，既不进 history 也不占号 ——
	// 放进 history 会被 undo / 导出 / 各种全量遍历当成一个真的标注。
	// 只在鼠标停在图上、且没落在别的元素上时显示（见 updateNumberPreview）
	std::unique_ptr<ShapeNumber> numberPreview;
	bool numberPreviewOn{ false };
	// ---- 常驻剪裁的状态（见 paintCropHandles 那一组）----
	// 采样点画多大（逻辑像素，落笔时乘 dpi）。命中框比它大一圈 —— 那么小的圆，差几个像素
	// 就点不着，可它偏偏压在图的边界上，越靠边越不好瞄
	static constexpr float kCropHandleR{ 4.f };
	static constexpr float kCropHandleHitR{ 9.f };
	// 没剪过的源图。截图那条路进来的是整屏原图（WinCap 抓的那张）；别的路（剪贴板 / 文件 /
	// 长图 / 历史 / 动图）没有更大的原图，ensureCropSource 拿当前这张底图顶替自己
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> srcImg;
	// srcImg 里当前显示的那一块（像素）。drawing->screenImg 永远是它的一块，
	// drawing->imgOrigin 等于"它的左上角 - cropBase" —— 标注坐标因此不随剪裁而变，
	// 拖采样点搬不动任何 shape
	D2D1_RECT_U cropRect{ 0, 0, 0, 0 };
	// 标注坐标 (0,0) 落在源图的哪个像素。建贴图那一刻定下来，之后不再变 ——
	// 它把"标注坐标"与"源图像素"钉在一起（源图里的位置 = 标注坐标 + 本值），
	// 底图与标注因此可以各挪各的。来源见构造函数与 ensureCropSource
	POINT cropBase{ 0, 0 };
	// 正拉着第几个采样点，-1 表示没在拉（见 cropHandleAt）
	int cropHandle{ -1 };
	// 开始拖那一刻源图左上角在屏幕上的位置。窗口可以被拖走 / 被缩放，所以这个值不能长期存着，
	// 只在一次拖拽期间有效 —— 拖左边 / 上边时窗口自己就在挪，靠它算光标落在源图的哪个像素
	POINT dragSrcPos{ 0, 0 };
	// ---- 拖窗口（见 onDown / onUp）----
	// 正在拖的是窗口本身（不是画笔、不是夹点、不是剪裁采样点）。拖窗口只有一个入口：
	// 手里没有画笔（curId 为空，或只开着贴图属性面板 pin）时按在画布上 —— onDown 置它、
	// onUp 清它。想拖窗口又正拿着画笔，先点一下那枚按钮取消选中（开关式）。
	// 用处只有一个：拖窗口期间工具条是收着的，而窗口每挪一步都会走一遍 WM_MOVE ->
	// onMoved -> layoutTools，那两条工具条的 SetWindowPos 一次就是好几毫秒 —— 鼠标消息
	// 根本来不及（实测单条 WM_MOUSEMOVE 要 16ms，其中 8ms 是这次白排的重排）。
	// 抬手时 onUp 会照常重排一次，所以跳过的都不亏
	bool winDragging{ false };
	// 这一下是"Ctrl+单击"在框选那一批上加减选（见 onDown）。onUp 见它为真就到此为止 ——
	// 照常走"选中这一笔"那条路会把整批换成它一个
	bool ctrlToggling{ false };
	// "Ctrl 空点了一下"：按下 Ctrl 后没拿它当修饰键使（没点鼠标、没滚轮、没按别的键），
	// 松开就把框选那一批收掉。按下时置位，一被"用掉"就撤（见 onKey / onDown / onWheel），
	// 抬起时还立着才真收 —— 之所以不在按下那一下就收，见 onKeyUp 里的说明
	bool ctrlTapArmed{ false };
	// 这一轮 Ctrl 已经被当修饰键用掉过了（点过鼠标 / 滚过轮）。语义与 ctrlTapArmed 相近，
	// 但**只在 Ctrl 抬起来时才复位**，中间一直压着 —— 用来挡掉按住 Ctrl 时系统的自动重复键：
	// 那些重复的 WM_KEYDOWN 会紧跟在 onDown 之后到达，不压着就会把 ctrlTapArmed 又立回去，
	// 于是"放开 Ctrl 就把刚框中的一批收掉"（见 onKey 里的说明）
	bool ctrlUsedAsModifier{ false };
	// 正在拉的那个选框。两个点都在标注坐标系里，与 shape 同一套（toImgPos 换算过）。
	// 抬手就清 —— 它只在一次拖拽期间有效
	bool marqueeOn{ false };
	POINT marqueeAnchor{ 0, 0 }, marqueeCur{ 0, 0 };
	// ---- 多选那一批的批量移动 / 批量旋转（见 onDown / onMove / onUp）----
	// 正拖着多选那一批：按在其中一个已选元素上（没按 Ctrl），整批跟着挪
	bool batchMoving{ false };
	// 这一下按的是画布右上角那三枚批量按钮（见 onDown）。它不 SetCapture、也不拖任何东西，
	// 抬手那一下得靠这个标志截住 —— 否则 onUp 会照常走"选中光标底下那个元素"那条路，
	// 刚点完「删除」就会反手选中一个（甚至把整批删掉之后又建立起一个单选）
	bool batchBtnClicked{ false };
	// 上一次拖动的落点（底图像素）。逐次求差就是这一帧要挪的量 ——
	// 记绝对落点而不是"按下时的落点"，光标离窗口或坐标被夹回来时也不会累积误差
	POINT batchMoveLast{ 0, 0 };
	// 「旋转」按钮进入的拖拽旋转态。为真时在画布上按下并拖动 = 整批绕组中心转
	bool batchRotateOn{ false };
	// 旋转态里正按着。与 batchRotateOn 分开：模式可以一直开着，一次拖完还能接着拖
	bool batchRotating{ false };
	// 旋转中心（底图像素）与上一次的鼠标方向角（度）。中心在每次按下时重算一次、
	// 拖动期间固定 —— 跟着走的话转起来会自己漂。逐帧比"新方向角 - 上次方向角"就是这一帧要转的量
	D2D1_POINT_2F batchRotateCenter{ 0.f, 0.f };
	float batchRotatePrevAngle{ 0.f };
	// ---- 「选择画布」（选择器的第二个子模式）----
	// 作用对象是底图 drawing->screenImg 的像素。刻意不走 swapImage：那条路是"整张图换掉了"，
	// 会把标注一并清掉；这里只是把底图的某一块挪个位置，标注不该跟着没
	//
	// 选区（底图像素）。用浮点而不是无符号整型：搬画面时允许拖出画布外，
	// 负坐标存进 UINT32 会绕成天文数字。right <= left 表示还没框出选区
	D2D1_RECT_F selRect{ 0.f, 0.f, 0.f, 0.f };
	bool hasSel() const;
	// 0 没在交互 / 1 正拉新框 / 2 正搬画面 / 3 正改选区大小
	int selDrag{ 0 };
	int selHandle{ -1 };
	// 按下点与按下时选区的左上角（底图像素），搬移 / 改大小都按它们算绝对位置
	D2D1_POINT_2F selDown{ 0.f, 0.f };
	D2D1_POINT_2F selBaseLT{ 0.f, 0.f };
	// 从底图上抠下来的那块画面：CPU 一份（抬手落回去用）、GPU 一份（拖动期间预览）。
	// 抠图发生在第一次真正拖动时（见 canvasSelectMove）—— 只在选区里点一下不该剪一刀
	std::vector<BYTE> selBlockPx;
	int selBlockW{ 0 }, selBlockH{ 0 };
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> selFloat;
	void canvasSelectDown(const POINT& imgPos);
	void canvasSelectMove(const POINT& imgPos);
	void canvasSelectUp();
	void paintCanvasSelection(ID2D1DeviceContext* ctx);
	// 选区上 8 个采样点的中心（0 左上 / 1 上 / 2 右上 / 3 右 / 4 右下 / 5 下 / 6 左下 / 7 左）
	void selHandleCenters(D2D1_POINT_2F (&centers)[8]) const;
	int selHandleAt(const POINT& imgPos) const;
	// 选区上那两枚动作图标（0 复制 / 1 删除），都没命中返回 -1
	int selActionAt(const POINT& imgPos) const;
	D2D1_RECT_F selActionRect(const int i) const;
	// 把选区那块从底图上抠下来（原位填白），放进 selBlockPx / selFloat
	void pickUpSelection();
	// 把抠下来的画面落到底图的当前选区位置（超出画布的部分裁掉，作者定的）
	void dropSelection();
	void deleteSelection();
	void copySelectionToClipboard();
	// 底图整块像素替换。保留标注（不碰 history）—— 与 swapImage 那条"换整张图"的路不同
	bool writeScreenImg(const std::vector<BYTE>& px, const int w, const int h);
	// 底图的一级撤销：搬画面 / 删除都是一次性破坏性操作，留一步可退（见 onKey 的 Ctrl+Z）
	std::vector<BYTE> canvasUndoPx;
	int canvasUndoW{ 0 }, canvasUndoH{ 0 };
	void pushCanvasUndo();
	bool restoreCanvasUndo();
	// ---- 对象剪贴板（Ctrl+C / Ctrl+X / Ctrl+V）----
	// 现在有选中的标注吗（框选那一批优先，其次单选）。Ctrl+C 靠它分流：
	// 有选中的复制标注，一个都没有才是老语义"复制整张图并关窗"
	bool hasSelectedShapes() const;
	// cut = 真时是剪切：先复制进内部剪贴板，再走 undoShapes 把原件撤掉（可 Ctrl+Y 找回）
	void copySelectedShapes(bool cut);
	// 把内部剪贴板里的形状粘到当前画布。每粘一次都重新 clone，所以能连着粘。
	// 内部剪贴板是空的时候落到系统剪贴板（见 pasteFromSystemClipboard）——
	// 作者要的"tpix 的剪贴板与系统剪贴板融合"
	void pasteShapes();
	// 系统剪贴板的兜底：是图就把整张图当成一个图片标注插进当前画布（ShapeImage，
	// 直接粘出来、不用先选任何工具），是文字就在鼠标位置建一个文本标注把内容填进去。
	// 悬浮球 / 托盘那条"从剪贴板贴成一张新窗口"的路（PinSource::fromClipboard）不动 ——
	// 那是再开一张贴图，这里是把内容编进正在编辑的这一张
	void pasteFromSystemClipboard();
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBrush;
	// 「选择画布」的采样点填充与图标白描边用的白色。单独一支：别的白刷各有各的用途，
	// 哪天改了色不该把选区一起带偏
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushSelWhite;
	// 「选择画布」选区那圈虚线用的笔型（作者要的是 fasCapture 那种虚线框，而不是实线）。
	// 与 ShapeText 的那条一样是自定义虚线，建一次够用一辈子
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> selDashStyle;
	// 右上角的倍数提示。非空即显示，缩放停手一会儿由定时器清掉
	Microsoft::WRL::ComPtr<IDWriteTextLayout> scaleTip;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushTipBg, brushTipText;
	// ---- 选文态的成员 ----
	// 识别出来的词，坐标已经换算到标注坐标系（与 shape 同一套）—— 剪裁只改 imgOrigin，
	// 所以剪完这一刀之后词框仍然贴在原来的字上。顺序就是阅读顺序：行自上而下、行内从左到右
	std::vector<OcrWord> ocrWords;
	// 行分组。[first, first+count) 是同一行，也正好是 ocrWords 里连续的一段；
	// 复制时靠它决定词与词之间是补空格还是换行
	struct OcrLine { int first{ 0 }, count{ 0 }; };
	std::vector<OcrLine> ocrLines;
	bool ocrRunning{ false };
	// 每起一次识别就 ++，回填时对不上就丢掉 —— 换底图 / 剪裁会再起一次，老结果不能盖上去
	int ocrSeq{ 0 };
	// 后台线程只拿得到裸 this，而窗口关掉之后这个对象下一轮消息循环就没了。
	// 线程与回填的回调都先看这个标志（与 WinOcr 用 winOcr.get() != this 是同一个用处）
	std::shared_ptr<bool> ocrAlive{ std::make_shared<bool>(true) };
	bool textSelect{ false };
	// 选区用两个"插入点"表示（同文本编辑器）：selAnchor 是按下那一下定的，selCur 跟着鼠标走
	int selAnchor{ 0 }, selCur{ 0 };
	bool selDragging{ false };
	// 选中那一层半透明蓝底
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> selectBrush;
	// 顶部的状态条与右上角的轻提示（"已复制"）。按需重建
	Microsoft::WRL::ComPtr<IDWriteTextLayout> textTip, toastTip;
	std::wstring textTipFor;
	bool isMouseDown{ false }, isClosed{ false };
	// 贴图属性。锁定不是改窗口样式实现的（Ling 自己管拖动），靠 onDown 里早退；
	// 不透明度也不是 WS_EX_LAYERED（窗口带 WS_EX_NOREDIRECTIONBITMAP，与分层窗口冲突），
	// 直接调 composition 树根节点的不透明度
	float opacity{ 1.f };
	bool isRounded{ false };
	bool isLocked{ false };
	// 鼠标穿透。同 round / lock 是实例态，但不落盘（见上面 getter 的注释）
	bool isThrough{ false };
	// 藏进屏幕边上那条里了（窗口不可见，位置 / 底图 / 标注一概保留，见 setHidden）。
	// 刻意不落盘：下次启动照常摆回原位 —— 存了的话用户第二天会以为图丢了
	bool isHidden{ false };
	// 藏在左 / 上哪条边上，见 getBarEdge
	BarEdge barEdge{ BarEdge::Left };
	// 藏起来之前两条工具条是不是开着的。放回来时按原样恢复，用户自己右键收起的不替他打开
	bool toolsWereVisible{ true };
	// 隐藏条上那一条的颜色序号，见 getBarColorIndex
	int barColorIndex{ 0 };
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

