#pragma once
#include <include/Ling.h>
#include <functional>
#include <string>
#include <vector>

class WinPin;
class Tip;
class ShapeNumber;
class ToolSub:public Ling::WinBase
{
public:
	// 系统字体表里"常用十款"的一项。family 是喂给 DWrite 的族名，show 是界面上显示的名字。
	// 放在头文件里是因为水印的内容编辑弹窗也要用同一份表 —— 两处各列一份的话，
	// 改了一处忘了另一处，同一个字体在两处的下拉里就长得不一样
	struct FontItem {
		std::wstring family;
		std::wstring show;
	};
	// 常用十款（机器上没装的会自动剔掉）。整个进程一份，第一次调用时才去查系统字体表
	static const std::vector<FontItem>& commonFonts();
	// 某款字体在 commonFonts 里的下标，找不到（换过机器 / 字体被卸了）返回 -1
	static int fontIndexOf(const std::wstring& family);
	// 界面上的显示名：表里查得到就用表里的（中文显示名），查不到退回族名本身
	static std::wstring fontShowName(const std::wstring& family);
	ToolSub(WinPin* win);
	~ToolSub();
	void showRectTools();
	void showEllipseTools();
	void showArrowTools();
	void showNumberTools();
	void showLineTools();
	void showTextTools();
	void showMosaicTools();
	void showEraserTools();
	// 贴图本身的属性：不透明度、圆角、锁定、鼠标穿透、标题
	void showPinTools();
	// 文字水印：文字、字号、颜色、透明度、旋转、平铺
	void showWatermarkTools();

	// 文本当前的字体族名（DWrite 认的名字）。工具条还没建过字体按钮时给默认的微软雅黑 ——
	// ShapeText 在文本工具下取它，空串会让 DWrite 退回默认字体，不如直接给个明确的
	const std::wstring& getFontFamily() const;
	// 水印的字体族名，同上。与文本各存一份：两个工具的用法不同 ——
	// 水印一般是固定一款，不该被文字工具上一次的选择带着跑
	const std::wstring& getWatermarkFontFamily() const;
	// 给新建的序号取一个编号，并把「下一个」自增回填到输入框。
	// 工具条没开着也照记不误 —— 编号的进度不能依赖面板是否可见
	int takeNumberVal();
	// 看一眼下一个编号是多少，不推进计数。标号工具在鼠标 hover 时预览它（见 WinPin::onMove）
	int peekNumberVal() const { return numberNext; }
	// 写「下一个序号」的值（输入框同步）
	void setNumberVal(int val);
	// 图上的编号集合变了（删掉一个序号、用 +/- 顺移过）之后调它：把「下一个编号」对齐到
	// 图上最大号 + 1。最大的那个号被删掉时，框里要退回去而不是接着往下数，
	// 否则下一笔会跳过刚空出来的号。用户手改过的编号不动（见 numberManual）
	void syncNumberVal();

	void hideTools();
	bool hasContent();
	float getDesiredHeight();
	void updatePosition(const RECT& workArea);
	// 当前选中的颜色，直接可喂给 CreateSolidColorBrush。颜色是每工具各存一份的
	// （见 beginTool），切工具时颜色按钮会重建并按配置文件里那一份重新选中。
	// mosaic/eraser 没有颜色按钮，它们的图形也用不到颜色。
	D2D1_COLOR_F getSelectedColor() const;
	// 同一个颜色的 RGBA 原值。Ling::Color 没有从 D2D1_COLOR_F 构造的口子，
	// TextBox::setColor 这类要 Ling::Color 的地方得用它。
	UINT32 getSelectedColorValue() const;
	// 水印用的颜色。刻意不借上面那两个 —— selectColorIndex 是"当前工具"共享的那一份
	// 选中色，beginTool 每切一次工具就把它换成那个工具的。水印绘制每帧都取色，
	// 于是"切到矩形、把填充色改成绿色"会把水印也染成绿色。
	// 与字号 / 字体 / 不透明度同一条理由：水印的样式只认 watermark 这一组。
	// 取的是同一份落盘值（onColorSelect 往 watermark/colorIndex 写的那一个）
	D2D1_COLOR_F getWatermarkColor() const;
	UINT32 getWatermarkColorValue() const;
	// 滑块当前值（逻辑像素语义，用作线宽/字号等；D2D 里当物理像素用的话记得乘 dpi）。
	// 值存在 sliderVal 里而不是问 Slider 节点要 —— 节点每次切换工具都被销毁重建。
	float getSliderVal() const;
	// 在图形上滚滚轮改了尺寸之后回填过来（序号的圆半径、矩形/椭圆的线宽），入参是物理像素。
	// 返回夹到该工具滑块值域内的物理像素值 —— 调用方拿它当最终尺寸，图形就不会滚出滑块的范围。
	// 正显示着这个工具的工具条时滑块跟着动，顺带落盘；滚的是别的工具画的图形时只更新配置。
	float setShapeSliderVal(const std::wstring& tool, float px);
	// ToolMain 与 ToolSub 之间的间距，WinPin::layoutTools() 计算整组高度时要用
	static constexpr float mainGap{ 2.f };
public:
	bool isRectFill{ false }, isEllipseFill{ false }, isArrowFill{ true }, isNumberFill{ true }, isLineTransparent{ false }, isTextBold{ false }, isTextItalic{ false }, isEraserRect{ false };
	// 马赛克模式 0 = 矩形马赛克，1 = 涂抹马赛克，2 = 智能擦除。
	// 三者互斥，所以用一个整数而不是三个布尔 —— 布尔组合里会出现"既涂抹又擦除"这种不存在的状态
	int mosaicMode{ 0 };
	// 序号的编号样式（阿拉伯 / 字母小写 / 字母大写 / 罗马 / 中文）与外圈样式
	// （无尾圆 / 无尾方 / 圆+箭头 / 方+箭头 / 无）。值与 ShapeNumber 的两个枚举一一对应，
	// 转枚举行取 static_cast
	int numberStyle{ 0 }, numberRing{ 0 };
	// 箭头样式：0 = 普通（首尾等粗、平口尾），1 = 尖尾渐变。同 ShapeArrow 的枚举
	int arrowStyle{ 0 };
	// 线条类型：0 = 直角折线（默认，拖拽时按鼠标轨迹吸附成横平竖直），1 = 普通线条（自由画）。
	// 同 ShapeLine::Kind 的枚举
	int lineKind{ 0 };
	// 线条两端的形状。0~9 对应 ShapeLine 里那张端点表（无 / 末端实心箭头 / … / 两端细箭头）
	int lineEnd{ 0 };
	// 线条样式：0 实线 / 1 虚线 / 2 波浪线 / 3 点状线 / 4 长短虚线 / 5 删除线。同 ShapeLine::Style
	int lineStyle{ 0 };
	// 贴图不透明度的当前档位（下标进 .cpp 里的 pinOpacitySteps 表），值本身落盘
	int pinOpacity{ 0 };
	// 水印。文字 / 位置是状态本体；透明度、角度、间距的档位存下标，换算表在 .cpp 里
	std::wstring watermarkText{ L"" };
	// 水印位置：0 平铺 / 1 右下角 / 2 左下角 / 3 右上角 / 4 左上角 / 5 顶部居中 / 6 底部居中 / 7 居中。
	// 顺序与 ShapeWatermark::WmPos 一一对应，也是「位置」下拉里的顺序（要落盘，别随手调）
	int watermarkPos{ 0 };
	// watermarkAlpha 是水印不透明度（5~100 %），watermarkGapPct 是平铺间距（0~100 %，换算系数的表在 .cpp）
	int watermarkRotate{ 0 };
	int watermarkAlpha{ 25 }, watermarkGapPct{ 25 };
	float getWatermarkOpacity() const;
	float getWatermarkRotation() const;
	float getWatermarkGapRatio() const;
	int getWatermarkPos() const { return watermarkPos; }
	// ---- 水印竖排浮层（WinWatermarkPanel）的读写口 ----
	// 浮层只有"取初值"和"写回"两件事，都从这里走。刻意不在浮层里直接摸上面那几个字段：
	// 值域、落盘的键名、以及"改完要让图上已有的水印重画"这三件事分散在两处的话，
	// 改一处忘一处就是滑块动了图不变、或者改了没落盘
	float getWatermarkAlpha() const { return (float)watermarkAlpha; }
	// 水印字号（逻辑像素）。用独立的一份，**不能借 sliderVal** ——
	// sliderVal 是"当前工具"的滑块值，切到文本工具就被换成文本字号了，
	// 而水印绘制每帧都读它，于是"改文本字号，水印跟着变大"
	float getWatermarkSize() const { return watermarkFontSize; }
	// 给绘制用：D2D 要物理像素（与原来 getSliderVal 的语义一致，内部乘 dpi）
	float getWatermarkFontSizePx() const { return watermarkFontSize * dpi; }
	// 大小的值域查配置表里 watermark 那一档（不借 sliderMin/Max，那是随工具切的）
	void getWatermarkSizeRange(float& min, float& max) const;
	float getWatermarkGap() const { return (float)watermarkGapPct; }
	void setWatermarkAlpha(float v);
	void setWatermarkSize(float v);
	void setWatermarkGap(float v);
private:
	void onCreated() override;
	void layout() override;
	void onMinMaxInfo(MINMAXINFO* mmi);
	void paintBorder(ID2D1DeviceContext* ctx);
	void onColorSelect(Ling::Button* btn);
	void initColorBtns();
	void initSlider();
	// 建一个横向滑块。值域 / 当前值都由调用方给（水印的不透明度、间距、大小都要用，
	// 而每工具一份的那套字段只有"大小"这一项），建好之后登记进 sliders 好让悬停提示找到它。
	// name 是悬停提示里写在数值前面的那一截（"不透明度 25"），
	// 只在同排摆着多个滑块时才需要 —— 一个滑块的工具光看数字就知道是什么
	Ling::Slider* makeSlider(float min, float max, float val, std::function<void(float)> onChange,
		const std::wstring& name = {});
	// 水印的「旋转」按钮：非平铺时置灰并让它点了也不动 —— 角度只对平铺有意义。
	// 位置一变（下拉里选的）就要重画一次按钮上的字与配色，所以单独抽出来
	void syncWatermarkRotateBtn();
	// 水印的「内容」按钮上显示什么：当前文字的第一行，截太长加省略号
	void syncWatermarkContentBtn();
	// 选中/未选中两套配色，与 ToolMain 的选中效果保持一致
	void applyToggleStyle(Ling::Button* btn, bool selected);
	// 建一个可切换的工具按钮：初始态从配置文件读（写回 flag），点击时翻转 flag、刷新配色并落盘。
	// tipKey 是提示文字的语言键（如 tool.rectFill），cfgKey 是这个开关在 config.json 里的键名
	// （fill / semiTransparent / bold / …，同一工具下不能重名）。
	Ling::Button* makeToggleBtn(const std::wstring& text, bool* flag, const std::wstring& tipKey, const std::wstring& cfgKey);
	// 多值下拉按钮：按钮上显示当前这一档，点一下弹出全部档位，一次点中。
	// 原来这里是个循环按钮（每点一次往前推一格），档位一多就得点好几下才转到想要的那个。
	// items 就是每一档在按钮上长什么样 —— 序号样式那两个按钮要显示
	// "当前编号在各种样式下长什么样"，这个文本随档位变，所以由调用方整份传进来。
	// onPicked 是选中之后的收尾（贴图不透明度要作用到窗口、水印要重画），没有就传空。
	// refreshNumbers：切完之后要不要把图上已有的序号重排一遍。只有序号的样式按钮需要，
	// 马赛克模式那一个跟序号没关系，不该顺带去遍历一遍 shape
	Ling::Button* makeSelectBtn(const std::wstring& tipKey, const std::wstring& cfgKey,
		int* index, const std::vector<std::wstring>& items,
		std::function<void()> onPicked = nullptr,
		bool useIconFont = false, bool refreshNumbers = true);
	// pin 面板用的文字开关：跟 makeToggleBtn 一样的两态配色，但按钮上写的是字（圆角 / 锁定 / 穿透）
	// 而不是图标 —— 图标字体里没有锁、穿透这类符号，硬猜码位只会显示成方块。
	// apply 由调用方给，开关翻转后直接调 WinPin 上对应的 setter
	Ling::Button* makeTextToggle(const std::wstring& text, const std::wstring& tipKey,
		const std::wstring& cfgKey, bool def, std::function<void(bool)> apply);
	// 两态开关，但状态住在外部对象里（读 read、翻转后调 apply），不进 config.json。
	// pin 面板的圆角 / 锁定 / 穿透用它：这些是"当前这张贴图"的实例属性，谁开的谁自己记着 ——
	// 走全局配置的话，构建面板时会把别的贴图存下的开关盖到当前这张上，
	// 而且取消面板也没有复位点，锁定 / 穿透一旦被盖上整张图就拖不动也画不了
	Ling::Button* makeStateToggle(const std::wstring& text, const std::wstring& tipKey,
		std::function<bool()> read, std::function<void(bool)> apply);
	// 「应用到全部」：把工具条当前样式套到图上同工具的所有标注。
	// 只在改了样式真能看出来的那些工具条上建（水印单实例且每次 paint 现取样式，不建）
	void makeApplyAllBtn();
	// 样式切换按钮上示例用哪个序号：取图上最大的那个编号，没有序号时用 1
	int getNumberSampleVal();
	// 「编号」输入框。它是固定宽度，宽度另算进 initSize 的 extraW
	void initNumberBox();
	// 字体按钮：按钮上显示当前字体名（长了截断），点开是常用十款里装了的那些。
	// toolId 决定这份选择存在 config.json 的哪一组（文本 / 水印各一份），
	// family 是随这份按钮一起走的那份族名，onPick 是选完之后要做的收尾
	Ling::Button* makeFontBtn(const std::wstring& toolId, std::wstring& family,
		std::function<void()> onPick);
	// 把字体名写到按钮上。字体名长短不一，长了就截断加省略号
	void syncFontBtnText(Ling::Button* btn, const std::wstring& family);
	// 每个 show*Tools 开头都要做的事：收提示、清旧内容、记下当前工具，
	// 再把这个工具存在 config.json 里的滑块值和颜色读回来（读不到就用默认值 / 第一个颜色）。
	// 滑块的键名与值域查 .cpp 里那张表，id 必须是表里有的（就是 ToolMain 的按钮 id）。
	void beginTool(const std::wstring& id);
	// 按内容算出窗口尺寸并应用。btnCount 只数工具按钮，不含颜色按钮。
	// centerOnBtn 为 true 时窗口居中对齐到 ToolMain 上选中的那个按钮，否则与 ToolMain 左对齐。
	// extraW 给文字输入框这类"宽度不是一格按钮"的控件预留。
	// sliderCount 是要摆几个滑块，0 表示一个都不摆（水印那三个滑块搬去了竖排浮层，
	// 工具条上一个不留）；别的工具都是一个
	void initSize(int btnCount, bool withColors, bool centerOnBtn = false, float extraW = 0.f,
		int sliderCount = 1);
	// 内容不变、只是 dpi 变了：按上次 initSize 的入参重算一遍尺寸
	void refreshSize();
	// 逻辑像素 → 物理像素
	float toPx(float logical) const;
private:
	Ling::Node* contentNode;
	std::vector<Ling::Button*> colorBtns;
	// 当前的滑块。切换工具时会被销毁重建，重建后由 initSlider 重新赋值。
	// 存下来是为了在窗口的 onMouseMove 里判断鼠标是否在它上面，好显示数值提示。
	Ling::Slider* slider{ nullptr };
	// 本工具条上摆着的所有滑块（水印有三个）。悬停提示要挨个判，切工具时随内容一起作废。
	// sliderNames 与它一一对应，是提示里写在数值前面的那一截（单滑块的工具留空串）
	std::vector<Ling::Slider*> sliders;
	std::vector<std::wstring> sliderNames;
	// 水印面板上的三枚按钮。与 slider 同理：切工具时随 contentNode 一起销毁，
	// beginTool 里必须置空，否则各自的回调（syncWatermarkRotateBtn / syncWatermarkContentBtn、
	// 以及浮层的 show）会往一个已删掉的按钮上写
	Ling::Button* watermarkRotBtn{ nullptr };
	// 「内容」：点开是水印内容编辑弹窗（文字 + 时间 + 字体）
	Ling::Button* watermarkContentBtn{ nullptr };
	// 「样式」：悬停或点击弹开竖排浮层（不透明度 / 大小 / 间距）
	Ling::Button* styleBtn{ nullptr };
	// 「清除」：一键撤掉图上所有水印。加了水印之后又画了别的标注，
	// 想反悔时挨个去撤销够不着，这一枚就是干这个的
	Ling::Button* watermarkClearBtn{ nullptr };
	// 悬停提示。要 hwnd，所以在 onCreated 里才建得起来
	std::unique_ptr<Tip> tip;
	static constexpr float btnSize{ 32.f };
	static constexpr float sliderSize{ 80.f };     // 滑块宽度，initSlider 和 initSize 都用它，改这里就够
	static constexpr float sliderMargin{ 3.f };    // 滑块左右各留的间距
	static constexpr float marginTop{ 3.f };       // 顶部箭头区域高度
	// 字体按钮与编号输入框都是固定宽度（内容长短不一，交给 flex 会被别的按钮挤扁），
	// 宽度要从 initSize 的 extraW 里预留出来。字体下拉的最小宽度单独给，
	// 按按钮宽度开会把"Microsoft YaHei UI Light"这类长名字截掉
	static constexpr float fontBtnW{ 88.f };
	// 水印「内容」按钮的固定宽度。同 fontBtnW 的道理：内容长短不一，交给 flex 会被挤扁。
	// 比 fontBtnW 宽一点 —— 水印文字第一行常有 5~6 个汉字
	static constexpr float contentBtnW{ 96.f };
	static constexpr float fontPopupMinW{ 220.f };
	static constexpr float fontMaxChars{ 6.f };    // 按钮上最多显示几个字符，超了截断
	static constexpr float numberBoxW{ 46.f };
	// 边框描边宽度。与 ToolMain 的 setBorder(1.f) 保持一致
	static constexpr float borderW{ 1.f };
	// 箭头尖端相对窗口左边的偏移，由 updatePosition 按屏幕坐标算出，是物理像素
	float arrowX{0.f};
	WinPin* win;
	// 背景/边框画刷缓存：layout() 每次刷新都会调 paintBorder，别在里面重复建
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg, brushBorder;
	// 铺满窗口的画布，画的是背景与带箭头的边框，按钮都在 contentNode 上，盖在它上面
	Ling::Canvas* canvas{ nullptr };
	bool isVisible{ false };
	bool hasTools{ false };
	bool centerOnBtn{ false };
	// onDpiChanged 与 onSizeChanged 之间的接力标记，见构造函数里的注释
	bool dpiChanged{ false };
	// 上一次 initSize 的入参，DPI 变了要照原样再算一遍尺寸
	int sizeBtnCount{ 0 };
	bool sizeWithColors{ false };
	float sizeExtraW{ 0.f };
	int sizeSliderCount{ 1 };
	// 下一个序号的编号。每次开始标号都回到 1，之后跟着图上最大号 + 1 走（syncNumberVal），
	// 用户也可以在输入框里改成别的数（那一份只活在这一轮标号里）
	int numberNext{ 1 };
	// 这一轮的编号被用户手改过。改过之后就锁住不再跟着图上最大号走 ——
	// 他填 11 就是要从 11 起跳，中间删掉一个号不该把这个意图冲掉
	bool numberManual{ false };
	// 「编号」输入框。切工具时随 contentNode 一起销毁，beginTool 里必须置空，
	// 否则 setNumberVal 会往一个已经删掉的控件上写
	Ling::TextBox* numberBox{ nullptr };
	// setText 自己也会触发 onTextChanged，回填输入框时要挡掉那一次
	bool numberBoxSilent{ false };
	// 文本字体族名（DWrite 认的名字）。落盘存的是族名而不是下标 ——
	// 下标会随着机器上装的字体变化而串味
	std::wstring fontFamily;
	// 水印字体族名，存 config.json 里 watermark 那一组
	std::wstring watermarkFont;
	// 水印字号（逻辑像素），落盘在 watermark 组的 fontSize 键。
	// 单独一份的理由见上面 getWatermarkSize 的注释：它与其它工具共用的 sliderVal 不是一回事
	float watermarkFontSize{ 24.f };
	UINT selectColorIndex{ 0 };
	// 滑块值。每次切换工具都由 beginTool 从 config.json 里换成那个工具自己的那份。
	float sliderVal{ 2.f };
	float sliderMin{ 1.f }, sliderMax{ 20.f };
	// 当前工具在 config.json 里的组名（即 ToolMain 的按钮 id）与滑块值的键名，
	// 落盘时要用。没选工具时是空的，此时不该有任何写入。
	std::wstring curToolId, curSliderKey;
	// 序号半径的取值范围（逻辑像素）。下限跟着 ShapeNumber 拖拽/滚轮的下限走，
	// 上限给滑块留个头 —— 再大就超出滑块能表达的范围了，拖拽仍可继续放大，只是不再回写
	static constexpr float numberMin{ 6.f }, numberMax{ 86.f };
	std::vector<UINT32> colors = { 0XCF1322FF, 0XD48806FF, 0X389E0DFF, 0X13C2C2FF, 0X0958D9FF, 0X722ED1FF, 0XEB2F96FF, 0X000000FF, 0XFFFFFFFF };
};