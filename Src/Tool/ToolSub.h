#pragma once
#include <include/Ling.h>
#include <functional>
#include <string>

class WinPin;
class Tip;
class ShapeNumber;
class ToolSub:public Ling::WinBase
{
public:
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
	// 序号的编号样式（阿拉伯 / 字母小写 / 字母大写 / 罗马 / 中文）与外圈样式（圆 / 方 / 无）。
	// 值与 ShapeNumber 的两个枚举一一对应，转枚举行取 static_cast
	int numberStyle{ 0 }, numberRing{ 0 };
	// 贴图不透明度的当前档位（下标进 .cpp 里的 pinOpacitySteps 表），值本身落盘
	int pinOpacity{ 0 };
	// 水印。文字 / 平铺是状态本体；档位存下标，换算成透明度与角度的表在 .cpp 里
	std::wstring watermarkText{ L"" };
	bool watermarkTile{ false };
	int watermarkOpacity{ 0 }, watermarkRotate{ 0 };
	float getWatermarkOpacity() const;
	float getWatermarkRotation() const;
private:
	void onCreated() override;
	void layout() override;
	void onMinMaxInfo(MINMAXINFO* mmi);
	void paintBorder(ID2D1DeviceContext* ctx);
	void onColorSelect(Ling::Button* btn);
	void initColorBtns();
	void initSlider();
	// 选中/未选中两套配色，与 ToolMain 的选中效果保持一致
	void applyToggleStyle(Ling::Button* btn, bool selected);
	// 建一个可切换的工具按钮：初始态从配置文件读（写回 flag），点击时翻转 flag、刷新配色并落盘。
	// tipKey 是提示文字的语言键（如 tool.rectFill），cfgKey 是这个开关在 config.json 里的键名
	// （fill / semiTransparent / bold / …，同一工具下不能重名）。
	Ling::Button* makeToggleBtn(const std::wstring& text, bool* flag, const std::wstring& tipKey, const std::wstring& cfgKey);
	// 多值循环按钮：每点一次把 index 往前推一格，按钮上的文本换成新一格对应的样子。
	// textOf 按 index 生成文本 —— 序号样式那两个按钮要显示"当前编号在各种样式下长什么样"，
	// 这个文本随 index 变，所以不能像 ToggleBtn 那样一次性把整表传进来
	// refreshNumbers：切完之后要不要把图上已有的序号重排一遍。只有序号的样式按钮需要，
	// 马赛克模式那一个跟序号没关系，不该顺带去遍历一遍 shape
	Ling::Button* makeCycleBtn(const std::wstring& tipKey, const std::wstring& cfgKey,
		int* index, int count, std::function<std::wstring(int)> textOf,
		bool useIconFont = false, bool refreshNumbers = true);
	// pin 面板用的文字开关：跟 makeToggleBtn 一样的两态配色，但按钮上写的是字（圆角 / 锁定 / 穿透）
	// 而不是图标 —— 图标字体里没有锁、穿透这类符号，硬猜码位只会显示成方块。
	// apply 由调用方给，开关翻转后直接调 WinPin 上对应的 setter
	Ling::Button* makeTextToggle(const std::wstring& text, const std::wstring& tipKey,
		const std::wstring& cfgKey, bool def, std::function<void(bool)> apply);
	// 「应用到全部」：把工具条当前样式套到图上同工具的所有标注。
	// 只在改了样式真能看出来的那些工具条上建（水印单实例且每次 paint 现取样式，不建）
	void makeApplyAllBtn();
	// 样式切换按钮上示例用哪个序号：取图上最大的那个编号，没有序号时用 1
	int getNumberSampleVal();
	// 每个 show*Tools 开头都要做的事：收提示、清旧内容、记下当前工具，
	// 再把这个工具存在 config.json 里的滑块值和颜色读回来（读不到就用默认值 / 第一个颜色）。
	// 滑块的键名与值域查 .cpp 里那张表，id 必须是表里有的（就是 ToolMain 的按钮 id）。
	void beginTool(const std::wstring& id);
	// 按内容算出窗口尺寸并应用。btnCount 只数工具按钮，不含颜色按钮。
	// centerOnBtn 为 true 时窗口居中对齐到 ToolMain 上选中的那个按钮，否则与 ToolMain 左对齐。
	// extraW 给文字输入框这类"宽度不是一格按钮"的控件预留
	void initSize(int btnCount, bool withColors, bool centerOnBtn = false, float extraW = 0.f);
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
	// 悬停提示。要 hwnd，所以在 onCreated 里才建得起来
	std::unique_ptr<Tip> tip;
	static constexpr float btnSize{ 32.f };
	static constexpr float sliderSize{ 80.f };     // 滑块宽度，initSlider 和 initSize 都用它，改这里就够
	static constexpr float sliderMargin{ 3.f };    // 滑块左右各留的间距
	static constexpr float marginTop{ 3.f };       // 顶部箭头区域高度
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