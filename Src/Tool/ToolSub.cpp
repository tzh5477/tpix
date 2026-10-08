#include "pch.h"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include "../Win/WinPin.h"
#include "../Lang.h"
#include "../SelectPopup.h"
#include "../Setting.h"
#include "../History.h"
#include "../Shape/ShapeArrow.h"
#include "../Shape/ShapeLine.h"
#include "../Shape/ShapeNumber.h"
#include "../Shape/ShapeWatermark.h"
#include "../Tip.h"
#include "../Win/WinColorPicker.h"
#include "../Win/WinWatermarkPanel.h"
#include "../Win/WinWatermarkText.h"
#include "ToolSub.h"
#include "ToolMain.h"

using namespace Microsoft::WRL;

namespace {
	// 每个工具的滑块：值在 config.json 里的键名、值域、默认值（都是逻辑像素）。
	// 键名各工具语义不同 —— 矩形/椭圆/箭头/线条是线宽，序号是圆半径，文字是字号。
	// 集中放这一张表，是因为值域有两处要用：beginTool 建滑块，以及在图形上滚滚轮
	// 改尺寸时（setShapeSliderVal）夹值 —— 分开写迟早会对不上。
	struct SliderCfg {
		const wchar_t* key;
		float min, max, def;
	};
	const std::pair<const wchar_t*, SliderCfg> sliderCfgs[]{
		{ L"rect",    { L"width",     1.f, 26.f,  2.f } },
		{ L"ellipse", { L"width",     1.f, 26.f,  2.f } },
		{ L"arrow",   { L"width",     1.f, 16.f,  3.f } },
		{ L"number",  { L"radius",    6.f, 86.f, 16.f } },
		{ L"line",    { L"width",     1.f, 60.f, 12.f } },
		{ L"text",    { L"fontSize", 10.f, 60.f, 20.f } },
		{ L"mosaic",  { L"width",    18.f, 68.f, 28.f } },
		{ L"eraser",  { L"width",    18.f, 68.f, 28.f } },
		{ L"watermark", { L"fontSize", 10.f, 72.f, 24.f } },
	};
	// 找不到就返回 nullptr：调用方传的都是本文件里的字面量或图形自己的工具名，
	// 真没命中说明表漏了一项，此时什么都不做比崩掉或按错值域夹要好
	const SliderCfg* findSliderCfg(const std::wstring& tool)
	{
		for (auto& [id, cfg] : sliderCfgs) {
			if (tool == id) return &cfg;
		}
		return nullptr;
	}
	// 外圈样式按钮上显示的五样东西，顺序与 ShapeNumber::RingStyle 一一对应
	// （这个值要落盘，顺序不能随便动）：无尾圆 / 无尾方 / 一条横线（表示"没有外圈"）/
	// 带箭头的圆 / 带箭头的方。这几个符号在任何语言的字体里都在，不必跟着语言包走；
	// 箭头用 → 而不是 ➤ 那批符号，后者的字形不一定装得到
	const wchar_t* RingSample[]{ L"\u25cf", L"\u25a0", L"\u2014", L"\u25cf\u2192", L"\u25a0\u2192" };
	// 箭头样式的四档：普通（平口尾、平底实心头）/ 尖尾 / 细箭头（开口 V）/ 凹口实心。
	// 按钮与列表里画的是真实的那个箭头（ShapeArrow::paintSample），所以不是图标字体。
	// 这四条字符串只在数档位时用到（SelectPopup 按 items.size() 定行数），
	// 顺手也让配置读坏了时有个人能看懂的文字兜底
	const std::vector<std::wstring>& arrowStyleItems()
	{
		static const std::vector<std::wstring> items{
			L"\u2014\u25b6", L"\u2794", L"\u2014\u2192", L"\u27a4"
		};
		return items;
	}
	// 线条类型只有两项，按钮上写汉字，所以走语言包
	std::vector<std::wstring> lineKindItems()
	{
		return { Lang::get(L"tool.lineKind0"), Lang::get(L"tool.lineKind1") };
	}
	// 端点的十档，顺序 = config.json 里 line/end 的落盘值，就是 ShapeLine.cpp 里那张
	// kEndTable 的顺序。按钮与列表里画的是真实的端点（ShapeLine::paintEndSample），
	// 这十个字符串只在数档位时用到
	const std::vector<std::wstring>& lineEndItems()
	{
		static const std::vector<std::wstring> items{
			L"\u2014\u2014\u2014",       // 无
			L"\u2014\u25b6",             // 末端实心箭头
			L"\u25cf\u2014\u25b6",       // 起点圆点 + 末端实心箭头
			L"\u2014\u2192",             // 末端细箭头
			L"\u25cf\u2014\u2192",       // 起点圆点 + 末端细箭头
			L"\u2014\u25cf",             // 末端圆点
			L"\u25cf\u2014",             // 起点圆点
			L"\u25cf\u2014\u25cf",       // 两端圆点
			L"\u25c0\u2014\u25b6",       // 两端实心箭头
			L"\u2190\u2014\u2192",       // 两端细箭头
		};
		return items;
	}
	// 线条样式六档，顺序 = config.json 里 line/style 的落盘值：
	// 实线 / 虚线 / 波浪线 / 点状线 / 长短虚线 / 删除线。
	// 按钮与列表里画的是真实的线型（ShapeLine::paintStyleSample），这六个字符串只在数档位时用到
	const std::vector<std::wstring>& lineStyleItems()
	{
		static const std::vector<std::wstring> items{
			L"___", L"- -", L"~~~", L"\u00b7\u00b7\u00b7", L"-\u00b7-", L"-\u00b7\u00b7"
		};
		return items;
	}
	// 线条工具条上三个下拉按钮的宽度。线条类型写的是"直角折线"四个汉字，端点 / 线条样式
	// 写的是两个符号，都比一格按钮（btnSize）宽 —— 交给 flex 分会被压到放不下
	constexpr float lineKindW{ 60.f };
	constexpr float lineChoiceW{ 42.f };
	// 下拉里那条样例线的粗细（逻辑像素）。刻意不跟着当前线宽走 —— 线宽 20 的时候
	// 下拉里就只剩一个箭头了。三个下拉共用这一个数，"端点 / 线型 / 箭头样式"三条线一样粗
	constexpr float sampleLineW{ 2.f };
	// 贴图不透明度的四档。index 落盘的是下标，百分比文本由 showPinTools 按这张表生成
	const float pinOpacitySteps[]{ 1.f, 0.75f, 0.5f, 0.25f };
	// 水印的旋转档位（角度只有在平铺下才有意义）
	const float watermarkRotateSteps[]{ 0.f, 30.f, 45.f, 60.f };

	// 色块上的对勾用黑还是白，要看底色定 —— 浅黄块上的白勾、纯蓝块上的黑勾都看不见。
	// 用感知亮度（人眼对绿最敏感、对蓝最迟钝、对红居中）而不是三通道平均：
	// 纯蓝的平均亮度是 85（看着"暗"）、白勾其实很清楚，感知亮度算出来 29，给的正是白勾。
	// 颜色是 RRGGBBAA 排的（与 Ling::Color(uint32_t) 一致，见 0xCF1322FF = 红）
	UINT32 checkInkOn(UINT32 rgba)
	{
		const auto r = (rgba >> 24) & 0xFF, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF;
		return (0.299 * r + 0.587 * g + 0.114 * b) > 150 ? 0x000000FF : 0xFFFFFFFF;
	}

	// 浅色底要补一圈描边：白块摆在 0xFAFAFA 的工具条 / 白底弹窗上，不描边就是一个看不见的洞
	bool isLightColor(UINT32 rgba)
	{
		const auto r = (rgba >> 24) & 0xFF, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF;
		return (0.299 * r + 0.587 * g + 0.114 * b) > 200;
	}

	// 8 位十六进制（RRGGBBAA）→ 颜色。配置被手工改坏时**返回 false 而不是抛出 / 读半截值**：
	// 半截值看着就是"某个颜色不对"，比缺一格难查得多
	bool parseHex8(const std::wstring& s, UINT32& out)
	{
		if (s.size() != 8) return false;
		wchar_t* end{ nullptr };
		const auto v = std::wcstoul(s.c_str(), &end, 16);
		if (end != s.c_str() + 8) return false;
		out = static_cast<UINT32>(v);
		return true;
	}

	// 颜色 → 8 位十六进制。落盘用，所以要能在下次启动时被 parseHex8 读回来
	std::wstring toHex8(UINT32 v)
	{
		wchar_t buf[16]{};
		swprintf_s(buf, L"%08X", v);
		return buf;
	}

	std::wstring joinHexColors(const std::vector<UINT32>& list)
	{
		std::wstring out;
		for (size_t i = 0; i < list.size(); i++) {
			if (i) out += L",";
			out += toHex8(list[i]);
		}
		return out;
	}
}

// 「字体」下拉里固定这十款：系统里装的族往往上百个，全列出来既翻不到底、九成也没人用。
// family 是喂给 DWrite 的英文族名（中英文名 DWrite 都认，但英文名在任何语言的系统上都一样），
// show 是界面上显示的名字。机器上没装的那几款在下面被剔掉 ——
// 留下的都是"选了真能生效"的，不会出现点完没反应。
// 声明在 ToolSub.h 上：水印的内容编辑弹窗要用同一份
const std::vector<ToolSub::FontItem>& ToolSub::commonFonts()
{
	static std::vector<FontItem> list;
	if (!list.empty()) return list;
	static const std::pair<const wchar_t*, const wchar_t*> table[]{
		{ L"Microsoft YaHei", L"微软雅黑" },
		{ L"SimSun",          L"宋体" },
		{ L"SimHei",          L"黑体" },
		{ L"KaiTi",           L"楷体" },
		{ L"FangSong",        L"仿宋" },
		{ L"DengXian",        L"等线" },
		{ L"Arial",           L"Arial" },
		{ L"Times New Roman", L"Times New Roman" },
		{ L"Calibri",         L"Calibri" },
		{ L"Consolas",        L"Consolas" },
	};
	auto factory = Ling::D2D::get()->dwriteFactory;
	ComPtr<IDWriteFontCollection> collection;
	if (!factory || FAILED(factory->GetSystemFontCollection(collection.GetAddressOf(), FALSE))) return list;
	for (auto& [family, show] : table) {
		UINT32 idx{ 0 };
		BOOL exists{ FALSE };
		if (FAILED(collection->FindFamilyName(family, &idx, &exists)) || !exists) continue;
		list.push_back({ family, show });
	}
	return list;
}
float ToolSub::getWatermarkOpacity() const
{
	// 滑块给的是整数百分比，画的时候要 0~1
	return std::clamp(watermarkAlpha, 5, 100) / 100.f;
}

float ToolSub::getWatermarkRotation() const
{
	auto i = std::clamp(watermarkRotate, 0, 3);
	return watermarkRotateSteps[i];
}

float ToolSub::getWatermarkGapRatio() const
{
	// 间距是"文字尺寸的几成"：滑块 0~100 映射到 0~2 倍 —— 100 时两格之间空出两个文字宽，
	// 默认 25（即 0.5 倍）与原「标准」档一致
	return std::clamp(watermarkGapPct, 0, 100) / 100.f * 2.f;
}

// 竖排浮层（WinWatermarkPanel）改值走这三个。落盘的键名与工具条上那三个滑块完全一致 ——
// 同一份样式只能有一个落盘处，两边各写一遍键名的话，改了工具条上的滑块浮层不跟着、
// 反过来也一样，是最难查的那类"改了没反应"
// 大小的键名写死 L"fontSize"：它必须落在 watermark 这一组，不能跟着 curSliderKey 走 ——
// 浮层若在别的工具下被打开，curSliderKey 就成了那个工具的键名，字号会被写进别人的组里

void ToolSub::getWatermarkSizeRange(float& min, float& max) const
{
	// 查配置表里 watermark 那一档，不借 sliderMin / sliderMax ——
	// 那两个是随当前工具切来切去的
	auto cfg = findSliderCfg(L"watermark");
	if (!cfg) {
		// 表里漏了 watermark 这一项才会走到这（本文件内的字面量，正常不会发生）。
		// 给一个保守值域，别让浮层拿到未初始化的 min/max
		min = 10.f; max = 72.f;
		return;
	}
	min = cfg->min;
	max = cfg->max;
}

void ToolSub::setWatermarkAlpha(float v)
{
	watermarkAlpha = (int)std::lround(v);
	Setting::get()->setToolNum(L"watermark", L"alpha", v);
	win->refresh();
}

void ToolSub::setWatermarkSize(float v)
{
	// 只动水印自己这一份，不碰 sliderVal —— 那个是"当前工具"的滑块值，
	// 改水印字号把它一起改了的话，切回原工具会发现它的线宽 / 字号被顶掉了
	watermarkFontSize = v;
	Setting::get()->setToolNum(L"watermark", L"fontSize", v);
	// 水印不是选中态元素，没选中任何东西时也得重画才看得见
	win->refresh();
}

void ToolSub::setWatermarkGap(float v)
{
	watermarkGapPct = (int)std::lround(v);
	Setting::get()->setToolNum(L"watermark", L"gapPct", v);
	win->refresh();
}

ToolSub::ToolSub(WinPin* win) :Ling::WinBase(), win(win)
{
	// 跟着宿主窗口的缩放走：WinBase 构造里取的是系统 dpi，WinPin 可能在另一块缩放比例不同的屏上
	dpi = win->dpi;
	// 色板表先就位。后面任何一个 getSelectedColor* 都直接按下标取 colors[]，
	// 表空着的话第一次取色就是越界读 —— 而那可能发生在做任何 UI 之前（图上的水印每帧取色）
	refreshColors();
	// DPI 变了（工具条被挪到缩放比例不同的显示器上，或者用户改了系统缩放）：
	// Ling 只会把窗口按系统给的建议矩形整体缩放一遍，我们自己定的那套摆放规则不会重跑，
	// 工具条就歪在别处了。位置也不能在 onDpiChanged 里直接改 —— 那个事件在 Ling 应用建议矩形
	// 之前触发，改了马上被覆盖，所以这里只记个标记，等建议矩形应用后紧随而来的 WM_SIZE 再动手
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (!dpiChanged) return;
		dpiChanged = false;
		refreshSize();                    //宿主的摆放规则要用宽高，先按新 dpi 把尺寸定下来
		this->win->layoutTools();
	});
	// 点按钮会把 ToolSub 激活，此后键盘消息进的是它而不是 WinPin ——
	// WS_EX_NOACTIVATE 只挡得住程序化激活，鼠标点上来那一下 DefWindowProc 照样回 MA_ACTIVATE。
	// 把按键转回去，ESC / 空格 / 快捷键在工具条上和贴图上才是一个手感。
	// 唯一的例外是焦点在输入框里（编号）：那时候 ESC 是"结束编辑"、空格就是个空格，
	// 都该归 TextBox，转过去会被 WinPin 当成"退一步 / 切工具条"
	onKeyDown.add([this](UINT key) {
		if (numberBox && numberBox->isFocused()) return;
		this->win->onKeyDown(key);
	});
	// 抬起同理转回去：WinPin 的"再点一下 Ctrl = 取消框选"在 WinPin::onKeyRelease 里
	// （挂在 WinPin 的 onKeyUp 事件上），见那里
	onKeyUp.add([this](UINT key) {
		if (numberBox && numberBox->isFocused()) return;
		this->win->onKeyUp(key);
	});
	// 工具栏不参与激活：编辑文本时点一下颜色/字号，WinPin 不该因此丢掉键盘焦点
	// （丢焦点 = WM_KILLFOCUS = TextBox 失焦 = 编辑被打断）。
	createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
}

ToolSub::~ToolSub()
{
}

void ToolSub::onCreated()
{
	tip = std::make_unique<Tip>(this);
	auto d2d = Ling::D2D::get();
	// 画刷与设备（而非某次 BeginDraw 拿到的 context）绑定，建一次就够，paintBorder 每帧复用
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushBg.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0xA8A8A8), brushBorder.GetAddressOf());
	// 下拉里那几格样例固定的墨色。作者定的规矩：枚举值一律黑，不跟当前选中色走 ——
	// 那几格是"这一档长什么样"的说明图，染上红线 / 绿线之后形状反而不如黑的好认
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x333333), brushSample.GetAddressOf());
	// 画布先建，才排在 contentNode 之前，按钮才不会被背景盖住
	canvas = body->makeChild<Ling::Canvas>();
	canvas->setPositionType(Ling::Position::Absolute);
	canvas->setSizePercent(100.f, 100.f);
	contentNode = body->makeChild<Ling::Node>();
	contentNode->setPositionType(Ling::Position::Absolute);
	// 四边让开描边宽度，上边再额外让开箭头区域（都是逻辑像素，setPosition 内部乘 dpi）
	contentNode->setPosition(Ling::Edge::Left, borderW);
	contentNode->setPosition(Ling::Edge::Top, marginTop + borderW);
	contentNode->setPosition(Ling::Edge::Right, borderW);
	contentNode->setPosition(Ling::Edge::Bottom, borderW);
	contentNode->setFlexDirection(Ling::FlexDirection::Row);
	// 滑块的数值提示：Slider 没有 hover 事件，就在窗口级 mousemove 里自己判位置。
	// 显示出来后横向跟着鼠标走（同 2.4.25）；离开滑块只收自己这一个提示，
	// 不能无条件 hide()：颜色按钮的 onLeave 排在这个回调后面，会把它刚显示出来的提示误关掉。
	onMouseMove.add([this](POINT pos) {
		// 本工具条上可能有多个滑块（水印那不透明度 / 大小 / 间距三个并排），挨个判
		for (size_t i = 0; i < sliders.size(); ++i) {
			auto* s = sliders[i];
			if (s->isPosIn(pos)) {
				// 显示的是鼠标底下那个位置的值，不是当前值 —— 光标只是在滑轨上路过时 value 并没变
				auto val = std::format(L"{}", static_cast<int>(std::round(s->getValueAt(static_cast<float>(pos.x)))));
				// 三个滑块长得一样，光给个数字分不清是哪个 —— 有名字就写成"不透明度 25"
				auto name = i < sliderNames.size() ? sliderNames[i] : std::wstring{};
				tip->showAt(s, static_cast<float>(x + pos.x), y + s->y + Tip::anchorInset * dpi,
					name.empty() ? val : name + L" " + val);
				return;
			}
		}
		for (auto* s : sliders) tip->hide(s);
	});
}

// 各工具在 config.json 里的那一组（组名 = ToolMain 的按钮 id）就是唯一的状态存放处：
// 切到某个工具时从里面读，用户一改就写回去，所以内存里不用再留一份每工具的状态。
void ToolSub::beginTool(const std::wstring& id)
{
	// 按钮和滑块马上要被销毁，onLeave 不会触发，提示得手动收掉。
	// 列表是独立窗口、锚点按钮也在这批要销毁的里面，一并收掉
	tip->hide();
	SelectPopup::close();
	contentNode->removeAllChildren();
	// 滑块与编号输入框一并作废：不是每个面板都建它们，留着就是悬垂指针
	slider = nullptr;
	sliders.clear();
	sliderNames.clear();
	// 同上：下拉按钮上那几格自绘的小图也随 contentNode 一起销毁了
	samples.clear();
	numberBox = nullptr;
	numberBoxSilent = false;
	// 同上：水印的旋转、内容、样式三枚按钮也只在水印面板里存在。
	// 竖排浮层上一档工具就收：它上面那三个滑块调的是水印样式，
	// 切到别的工具之后它还挂着就成了一组调不动作用的滑块
	watermarkRotBtn = nullptr;
	watermarkContentBtn = nullptr;
	styleBtn = nullptr;
	watermarkClearBtn = nullptr;
	// 同上：色板行末尾那块「当前色」也随 contentNode 一起销毁
	colorMoreBtn = nullptr;
	colorMoreLabel = nullptr;
	WinWatermarkPanel::close();
	// 同上：取色器的锚点（色板行末尾那块）马上要被销毁重建，留着它就是悬空的
	WinColorPicker::close();
	curToolId = id;
	auto cfg = findSliderCfg(id);
	if (!cfg) return;
	curSliderKey = cfg->key;
	sliderMin = cfg->min;
	sliderMax = cfg->max;
	auto setting = Setting::get();
	// 夹一遍值域：配置文件可能是上个版本写的（值域变过），也可能被手工改坏，
	// 而这个值会直接当线宽/字号喂给 D2D，超出范围要么看不见要么慢得离谱
	sliderVal = std::clamp(setting->getToolNum(id, cfg->key, cfg->def), cfg->min, cfg->max);
	// colors 要赶在读 colorIndex 之前重建：它决定这个下标还有没有意义
	// （自定义色被删掉 / 换了机器上的配置，下标就可能指到表外）
	refreshColors();
	// colors[selectColorIndex] 那几处都不做边界检查，越界就读到界外了
	auto idx = static_cast<UINT>(setting->getToolNum(id, L"colorIndex", 0.f));
	selectColorIndex = idx < colors.size() ? idx : 0;
}

void ToolSub::showRectTools()
{
	beginTool(L"rect");
	initSize(1, true);
	makeToggleBtn(L"\ue602", &isRectFill, L"tool.rectFill", L"fill");
	initSlider();
	initColorBtns();
	makeApplyAllBtn();
}

void ToolSub::showEllipseTools()
{
	beginTool(L"ellipse");
	initSize(1, true);
	makeToggleBtn(L"\ue600", &isEllipseFill, L"tool.ellipseFill", L"fill");
	initSlider();
	initColorBtns();
	makeApplyAllBtn();
}

void ToolSub::showArrowTools()
{
	beginTool(L"arrow");
	initSize(2, true);
	makeToggleBtn(L"\ue604", &isArrowFill, L"tool.arrowFill", L"fill");
	// 四种箭头样式：普通（首尾等粗、平口尾）、尖尾、细箭头（开口 V）、凹口实心。
	// 默认普通那一个。按钮与列表里画的是真实的箭头 —— 几何与图上画的那个共用一份
	// （见 ShapeArrow::paintSample），所以不是图标字体。
	// 切完让当前选中的箭头立刻换形状 —— 下一个新建的本来就会用新档位。
	// 传 true：换的是"档位"，与颜色 / 填充 / 粗细那些"外观"分流（见 WinPin::onToolStyleChanged）
	makeSelectBtn(L"tool.arrowStyle", L"style", &arrowStyle, arrowStyleItems(),
		[this]() { win->onToolStyleChanged(true); }, false, false,
		[this](ID2D1DeviceContext* ctx, const D2D1_RECT_F& rect, int index) {
			ShapeArrow::paintSample(ctx, rect, index, sampleLineW * dpi, brushSample.Get());
		});
	initSlider();
	initColorBtns();
	makeApplyAllBtn();
}

int ToolSub::getNumberSampleVal()
{
	int maxVal{ 0 };
	for (auto& shape : win->getHistory()->shapes) {
		auto number = dynamic_cast<ShapeNumber*>(shape.get());
		if (number && !number->isUndo && number->val > maxVal) {
			maxVal = number->val;
		}
	}
	return maxVal > 0 ? maxVal : 1;
}

void ToolSub::showNumberTools()
{
	// 序号的滑块调的是圆半径（ShapeNumber 直接拿 getSliderVal 当 r），不是线宽
	beginTool(L"number");
	// 编号输入框是固定宽度，宽度从 extraW 里预留
	initSize(3, true, false, numberBoxW);
	initNumberBox();
	makeToggleBtn(L"\ue605", &isNumberFill, L"tool.numberFill", L"fill");
	// 两个样式按钮上显示的是"当前编号在这个样式下长什么样"，比写死的图标好认：
	// 图上已经有 3 个序号时，这里就显示 3 / c / C / III / 三
	auto sampleVal = getNumberSampleVal();
	std::vector<std::wstring> styleItems;
	for (int i = 0; i < 5; ++i) {
		styleItems.push_back(ShapeNumber::serializeVal(sampleVal, (ShapeNumber::NumStyle)i));
	}
	makeSelectBtn(L"tool.numberStyle", L"numStyle", &numberStyle, styleItems);
	// 圆 / 方 / 无外圈，用几何符号，不依赖某一种语言
	std::vector<std::wstring> ringItems;
	for (auto s : RingSample) ringItems.push_back(std::wstring{ s });
	makeSelectBtn(L"tool.numberRing", L"ringStyle", &numberRing, ringItems);
	initSlider();
	initColorBtns();
	makeApplyAllBtn();
}

void ToolSub::showLineTools()
{
	beginTool(L"line");
	// 三个下拉（线条类型 / 端点 / 线条样式）+ 半透明开关。线条类型只有两档，按两态约定
	// 单击即切换、不弹列表；另外两个走下拉列表。
	// 三个按钮的宽度差额从 extraW 里补给它们（initSize 只按按钮数算宽度）
	initSize(4, true, false, lineKindW + lineChoiceW * 2 - btnSize * 3);
	// 宽度写死，不让 flex 分：这三个放的是汉字与成对的符号，压到一格按钮宽就糊成一团
	auto fixW = [](Ling::Button* btn, float w) {
		btn->setFlexGrow(0.f);
		btn->setFlexShrink(0.f);
		btn->setWidth(w);
	};
	// 类型排在最前：它管的是"这一笔怎么长出来"，端点和线型都是它下游的样式。
	// 三个都是改完立刻套到图上选中的那一笔上（没选中就只影响之后新画的）。
	// 类型的作用面窄一些 —— 只有"普通线条 → 直角折线"这一半改得动已画的那一笔（见 snapExisting）。
	// 传 true：这三个是"档位"，与颜色 / 线宽 / 填充那些"外观"分流
	//（见 WinPin::onToolStyleChanged —— 滚轮调粗细走的也是那个入口，带上档位就把形状一起换了）
	auto applyNow = [this]() { win->onToolStyleChanged(true); };
	// 「端点」「线条样式」两格里画的是真实的线条 —— 几何与图上那条线共用一份
	// （ShapeLine::paintEndSample / paintStyleSample）。线条类型那一个写的是"直角折线"四个
	// 汉字，没有图形可画，仍是文字按钮。墨色一律用 brushSample 那个固定黑，不跟选中色走
	auto endPaint = [this](ID2D1DeviceContext* ctx, const D2D1_RECT_F& rect, int index) {
		ShapeLine::paintEndSample(ctx, rect, index, sampleLineW * dpi, brushSample.Get());
	};
	auto stylePaint = [this](ID2D1DeviceContext* ctx, const D2D1_RECT_F& rect, int index) {
		ShapeLine::paintStyleSample(ctx, rect, index, sampleLineW * dpi, brushSample.Get());
	};
	fixW(makeSelectBtn(L"tool.lineKind", L"kind", &lineKind, lineKindItems(), applyNow, false, false), lineKindW);
	fixW(makeSelectBtn(L"tool.lineEnd", L"end", &lineEnd, lineEndItems(), applyNow, false, false, endPaint), lineChoiceW);
	fixW(makeSelectBtn(L"tool.lineStyle", L"style", &lineStyle, lineStyleItems(), applyNow, false, false, stylePaint), lineChoiceW);
	makeToggleBtn(L"\ue607", &isLineTransparent, L"tool.semiTransparent", L"semiTransparent");
	initSlider();
	initColorBtns();
	makeApplyAllBtn();
}

void ToolSub::showTextTools()
{
	beginTool(L"text");
	// 字体按钮是固定宽度，宽度从 extraW 里预留（粗体 / 斜体两枚跟着 flex 走）
	initSize(2, true, false, fontBtnW);
	makeFontBtn(L"text", fontFamily, [this]() { win->onToolStyleChanged(); });
	// 粗体默认关（isTextBold 的初值），新建出来的文字就是不粗的
	makeToggleBtn(L"\ue634", &isTextBold, L"tool.bold", L"bold");
	makeToggleBtn(L"\ue682", &isTextItalic, L"tool.italic", L"italic");
	initSlider();
	initColorBtns();
	makeApplyAllBtn();
}

const std::wstring& ToolSub::getFontFamily() const
{
	// 工具条还没建过字体按钮时（比如刚启动就贴图、文本工具还没选过）也要给个明确的字体，
	// 空串会让 DWrite 退回它自己的默认值
	static const std::wstring defaultFamily{ L"Microsoft YaHei" };
	return fontFamily.empty() ? defaultFamily : fontFamily;
}

const std::wstring& ToolSub::getWatermarkFontFamily() const
{
	static const std::wstring defaultFamily{ L"Microsoft YaHei" };
	return watermarkFont.empty() ? defaultFamily : watermarkFont;
}

int ToolSub::fontIndexOf(const std::wstring& family)
{
	auto& list = commonFonts();
	for (size_t i = 0; i < list.size(); i++) {
		if (list[i].family == family) return (int)i;
	}
	return -1;
}

std::wstring ToolSub::fontShowName(const std::wstring& family)
{
	auto idx = fontIndexOf(family);
	// 找不到就在列表里现查一遍显示名；机器上确实没这款字体（配置从别处搬来的）才退回族名本身
	return idx >= 0 ? commonFonts()[idx].show : family;
}

void ToolSub::syncFontBtnText(Ling::Button* btn, const std::wstring& family)
{
	// 按钮只有一格宽，长名字截断 —— 下拉里显示的是全名
	auto show = fontShowName(family);
	if (show.size() > (size_t)fontMaxChars) {
		show = show.substr(0, (size_t)fontMaxChars - 1) + L"\u2026";
	}
	btn->setText(show);
}

Ling::Button* ToolSub::makeFontBtn(const std::wstring& toolId, std::wstring& family,
	std::function<void()> onPick)
{
	// 上次用的字体存在配置里。存族名而不是下标：下标会随机器上装的字体变化而串味
	family = Setting::get()->getToolStr(toolId, L"fontFamily", L"Microsoft YaHei");
	auto btn = contentNode->makeChild<Ling::Button>();
	btn->setHeight(btnSize - 2.5);
	btn->setWidth(fontBtnW);
	btn->setFontSize(12.f);
	btn->setBg(0);
	btn->setHoverBg(0xF2F2F2ff);
	syncFontBtnText(btn, family);
	tip->bind(btn, Lang::get(L"tool.font"));
	btn->onClick.add([this, btn, toolId, &family, onPick](Ling::Button*) {
		// 列表会盖住按钮，悬停提示先收掉（与其他下拉一致）
		tip->hide();
		auto& list = commonFonts();
		std::vector<std::wstring> items;
		items.reserve(list.size());
		for (auto& font : list) items.push_back(font.show);
		SelectPopup::show(this, btn, items, fontIndexOf(family),
			[this, btn, toolId, &family, onPick](int picked) {
				auto& fonts = commonFonts();
				if (picked < 0 || picked >= (int)fonts.size()) return;
				family = fonts[picked].family;
				Setting::get()->setToolStr(toolId, L"fontFamily", family);
				syncFontBtnText(btn, family);
				// 收尾由调用方给：文本要作用到正在编辑 / 选中的文字上，水印只要重画
				if (onPick) onPick();
			}, {}, fontPopupMinW);
	});
	return btn;
}

void ToolSub::initNumberBox()
{
	// 每次进入标号工具都从 1 起。以前这里是"接着上一次的号往下数"，于是新开一轮标号
	// 会莫名其妙从 7 开始；想从别的数起，直接改这个输入框 —— 那一份只活在这一轮里
	numberNext = 1;
	numberManual = false;
	auto box = contentNode->makeChild<Ling::TextBox>();
	box->setHeight(btnSize - 2.5);
	box->setWidth(numberBoxW);
	box->setVerticalCenter(true);
	box->setFontSize(12.f);
	box->setMarginLeft(sliderMargin);
	box->setMarginRight(sliderMargin);
	box->setText(std::to_wstring(numberNext));
	numberBox = box;
	box->onTextChanged.add([this, box](Ling::TextBox*, const std::wstring& val) {
		if (numberBoxSilent) return;
		// 只认 1~9999 的整数：其余输入（空、字母、超长）一律回填上一个有效值
		auto valid = !val.empty() && val.size() <= 4
			&& std::all_of(val.begin(), val.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
		auto parsed = valid ? std::stoi(val) : 0;
		if (parsed < 1) {
			numberBoxSilent = true;
			box->setText(std::to_wstring(numberNext));
			numberBoxSilent = false;
			return;
		}
		// 这是用户手改的：从此锁住，不再跟着图上最大号回写（见 syncNumberVal）
		numberManual = true;
		setNumberVal(parsed);
	});
}

int ToolSub::takeNumberVal()
{
	auto val = numberNext > 0 ? numberNext : 1;
	// 领走一个号之后就不再算"手改值"了：接着按图上最大号 + 1 往下走
	numberManual = false;
	setNumberVal(val + 1);
	return val;
}

void ToolSub::syncNumberVal()
{
	// 手改过就不动它：他填 11 就是要从 11 起跳
	if (numberManual) return;
	int maxVal{ 0 };
	for (auto& shape : win->getHistory()->shapes) {
		auto number = dynamic_cast<ShapeNumber*>(shape.get());
		if (number && !number->isUndo && number->val > maxVal) maxVal = number->val;
	}
	setNumberVal(maxVal + 1);
}

void ToolSub::setNumberVal(int val)
{
	numberNext = std::clamp(val, 1, 9999);
	// 输入框可能没建（当前不是序号工具），此时只更新成员
	if (!numberBox) return;
	// setText 也会触发 onTextChanged，回填时要挡掉，否则会被当成用户改的再解析一遍
	numberBoxSilent = true;
	numberBox->setText(std::to_wstring(numberNext));
	numberBoxSilent = false;
}

void ToolSub::showMosaicTools()
{
	// 这两种模式没有颜色按钮、窗口窄，initSize 要居中对齐到按钮上
	beginTool(L"mosaic");
	initSize(1, false, true);
	// 三个模式共用一个循环按钮：矩形马赛克 / 涂抹马赛克 / 智能擦除。
	// 三个图标码位都是项目里已经在用的（矩形填充、线条、橡皮擦），不存在画成豆腐块的风险
	static const wchar_t* mosaicIcons[]{ L"\ue602", L"\ue601", L"\ue6be" };
	// 按钮上显示的是当前模式，选项同样是这三个图标，所以列表要用图标字体
	std::vector<std::wstring> modeItems;
	for (auto c : mosaicIcons) modeItems.push_back(std::wstring{ c });
	makeSelectBtn(L"tool.mosaicMode", L"mode", &mosaicMode, modeItems, nullptr, true, false);
	initSlider();
	makeApplyAllBtn();
}

void ToolSub::showEraserTools()
{
	beginTool(L"eraser");
	initSize(1, false, true);
	makeToggleBtn(L"\ue602", &isEraserRect, L"tool.rectFill", L"rect");
	initSlider();
	makeApplyAllBtn();
}

void ToolSub::showPinTools()
{
	beginTool(L"pin");
	// 比原来多一枚按钮：成组 / 解组（同一枚，文案随状态切换）；动图另有播放 / 暂停。
	// +1 是把末尾的标题输入框也计入宽度（它同样 flex 抢空间，不算进去就会被挤成一条缝），
	// extraW 再单给它 180 的预算 —— 公式对 pin 面板本来是亏的：它没有滑块，那 86 的滑块宽
	// 相当于白送，正好补上文字按钮比图标格宽出来的部分
	initSize((win->hasAnim() ? 6 : 5) + 1, false, true, 180.f);
	// 四档不透明度。按钮上直接写百分比，比一个滑杆直观、也比滑杆少占一截宽度
	std::vector<std::wstring> opacityItems;
	for (auto v : pinOpacitySteps) {
		opacityItems.push_back(std::format(L"{}%", (int)std::lround(v * 100)));
	}
	// makeSelectBtn 只负责换档与落盘，"档位变了要作用到窗口"这半截由 onPicked 补上
	makeSelectBtn(L"tool.pinOpacity", L"opacity", &pinOpacity, opacityItems, [this]() {
		win->setOpacity(pinOpacitySteps[pinOpacity]);
		}, false, false);
	win->setOpacity(pinOpacitySteps[pinOpacity]);
	// 三个开关读写的是这张贴图自己的状态（getRounded/getLocked/getThrough），
	// 不再走全局配置 —— 详见 makeStateToggle 的注释
	makeStateToggle(Lang::get(L"tool.pinRound"), L"tool.pinRound",
		[this] { return win->getRounded(); },
		[this](bool on) { win->setRounded(on); });
	makeStateToggle(Lang::get(L"tool.pinLock"), L"tool.pinLock",
		[this] { return win->getLocked(); },
		[this](bool on) { win->setLocked(on); });
	makeStateToggle(Lang::get(L"tool.pinThrough"), L"tool.pinThrough",
		[this] { return win->getThrough(); },
		[this](bool on) { win->setMouseThrough(on); });
	// 成组：把当前所有贴图并为一组，之后拖一张 / Ctrl+滚轮缩放一张，整组跟着动；
	// 已经成组时点一下就是解散。按钮上写的是"点了会变成什么"
	auto groupBtn = contentNode->makeChild<Ling::Button>();
	groupBtn->setHeight(btnSize - 2.5);
	groupBtn->setFlexGrow(1.f);
	groupBtn->setFontSize(12.f);
	groupBtn->setBg(0);
	groupBtn->setHoverBg(0xF2F2F2ff);
	auto syncGroupText = [this, groupBtn]() {
		groupBtn->setText(win->getGroupId() != 0 ? Lang::get(L"tool.pinUngroup") : Lang::get(L"tool.pinGroup"));
		};
	syncGroupText();
	tip->bind(groupBtn, Lang::get(L"tool.pinGroupTip"));
	groupBtn->onClick.add([this, syncGroupText](Ling::Button*) {
		WinPin::toggleGroupAll();
		syncGroupText();
		});
	// 动图才有的播放 / 暂停。按钮上写的是"点了会变成什么"，与录音机那类按钮一个习惯
	if (win->hasAnim()) {
		auto playBtn = contentNode->makeChild<Ling::Button>();
		playBtn->setHeight(btnSize - 2.5);
		playBtn->setFlexGrow(1.f);
		playBtn->setFontSize(12.f);
		playBtn->setBg(0);
		playBtn->setHoverBg(0xF2F2F2ff);
		auto syncText = [this, playBtn]() {
			playBtn->setText(win->isAnimPlaying() ? Lang::get(L"tool.pinPause") : Lang::get(L"tool.pinPlay"));
			};
		syncText();
		tip->bind(playBtn, Lang::get(L"tool.pinPlayTip"));
		playBtn->onClick.add([this, syncText](Ling::Button*) {
			// 只有播 / 停两种状态，点一下就是切换；按钮上的字同步成"再点会变成什么"
			win->toggleAnim();
			syncText();
			});
	}
	// 标题：写什么显示什么，清空即隐藏。失焦才生效，边打边刷没必要
	auto titleBox = contentNode->makeChild<Ling::TextBox>();
	titleBox->setHeight(btnSize - 2.5);
	// 多吃两份空间：五个按钮各占一份，标题要能放下"截图标题"四个字加光标
	titleBox->setFlexGrow(2.f);
	titleBox->setMarginLeft(sliderMargin);
	titleBox->setMarginRight(sliderMargin);
	titleBox->setVerticalCenter(true);
	titleBox->setFontSize(12.f);
	titleBox->setPlaceholder(Lang::get(L"tool.pinTitleTip"));
	titleBox->setText(win->getPinTitle());
	titleBox->onTextChanged.add([this](Ling::TextBox*, const std::wstring& val) {
		win->setPinTitle(val);
		});
}

float ToolSub::setShapeSliderVal(const std::wstring& tool, float px)
{
	auto cfg = findSliderCfg(tool);
	if (!cfg) return px;
	auto logical = std::clamp(px / dpi, cfg->min, cfg->max);
	// 正显示着这个工具的工具条（在自己刚画的图形上滚滚轮，基本都是这种情况）：走滑块，
	// 让滑块上的位置也跟着动，它的 onValueChanged 会同步 sliderVal 并落盘。
	// 值没变时 Slider::setValue 会提前返回，不会白写一次盘
	if (curToolId == tool && slider) {
		slider->setValue(logical);
	}
	else {
		// 滚的是别的工具画出来的图形（比如选着椭圆工具，在一个已有的矩形边框上滚）：
		// 只更新那个工具的配置，别去动当前这条工具条的滑块
		Setting::get()->setToolNum(tool, cfg->key, logical);
	}
	return logical * dpi;
}

float ToolSub::getShapeSliderVal(const std::wstring& tool) const
{
	auto cfg = findSliderCfg(tool);
	// 表里没这一项时退回"当前工具"那一份：调用方传的都是本文件里的工具名，
	// 真没命中说明表漏了一项，给个能用值比给 0（字号 0、线宽 0，画不出来）好
	if (!cfg) return getSliderVal();
	const auto setting = Setting::get();
	return std::clamp(setting->getToolNum(tool, cfg->key, cfg->def), cfg->min, cfg->max) * dpi;
}

UINT32 ToolSub::getToolColorValue(const std::wstring& tool) const
{
	// 取法与 getWatermarkColorValue 一模一样，只是组名由调用方给。
	// colors[] 各处都不做边界检查，配置被手工改坏或旧版本写了越界值时这里兜一下
	auto n = static_cast<int>(Setting::get()->getToolNum(tool, L"colorIndex", 0.f));
	if (n < 0 || n >= (int)colors.size()) n = 0;
	return colors[(size_t)n];
}

bool ToolSub::getCurrentFill() const
{
	if (curToolId == L"rect") return isRectFill;
	if (curToolId == L"ellipse") return isEllipseFill;
	if (curToolId == L"arrow") return isArrowFill;
	if (curToolId == L"number") return isNumberFill;
	// 其余工具的面板上没有「填充」这一项（线条那枚是"半透明"，语义不同，不算）
	return false;
}

void ToolSub::layout()
{
	Ling::WinBase::layout();
	if (canvas) {
		auto ctx = canvas->startPaint();
		if (ctx) {
			ctx->Clear(0);
			paintBorder(ctx);
			canvas->finishPaint();
		}
	}
	// 下拉按钮上那一格自绘的小图。与背景画布分开、前后不套着画：startPaint / finishPaint
	// 要成对用（没 finish 之前同一设备再 BeginDraw 会失败）
	for (auto& slot : samples) {
		auto ctx = slot.canvas->startPaint();
		if (!ctx) continue;
		ctx->Clear(0);
		slot.paint(ctx, D2D1::RectF(0.f, 0.f, slot.canvas->w, slot.canvas->h), *slot.index);
		slot.canvas->finishPaint();
	}
}

void ToolSub::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}

void ToolSub::paintBorder(ID2D1DeviceContext* ctx)
{
	auto d2d = Ling::D2D::get();
	// 几何体每帧都要按当前 w/h/arrowX 重建，用局部 ComPtr，别存成员：
	// 存成员时 GetAddressOf() 不会 Release 旧对象，等于每帧漏一个 ID2D1PathGeometry。
	ComPtr<ID2D1PathGeometry> borderPath;
	d2d->d2dFactory->CreatePathGeometry(borderPath.GetAddressOf());
	ComPtr<ID2D1GeometrySink> sink;
	borderPath->Open(sink.GetAddressOf());
	// 以下都是物理像素：D2D 直接画在 surface 上，不像 Ling 的 setter 会自己乘 dpi。
	// D2D 的描边以路径为中心线，所以路径放在距边 borderWPx/2 处，描边正好填满最外侧 borderWPx 像素。
	// 右/下边界要用 surface 的整数尺寸（窗口和 surface 都是 (int)w/(int)h），
	// 直接用带小数的 w/h 会让描边画到 surface 之外，看起来就是底边缺一条。
	auto borderWPx{ borderW * dpi };
	auto half{ borderWPx / 2.f };
	auto top{ toPx(marginTop) + half };
	auto right{ std::floor(w) - half };
	auto bottom{ std::floor(h) - half };
	sink->BeginFigure({ half,top }, D2D1_FIGURE_BEGIN_FILLED);
	D2D1_POINT_2F points[6] = {
		D2D1_POINT_2F{arrowX - 3.f * dpi,top},
		D2D1_POINT_2F{arrowX,half},
		D2D1_POINT_2F{arrowX + 3.f * dpi,top},
		D2D1_POINT_2F{right,top},
		D2D1_POINT_2F{right,bottom},
		D2D1_POINT_2F{half,bottom}
	};
	sink->AddLines(points, 6);
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	sink->Close();
	ctx->FillGeometry(borderPath.Get(), brushBg.Get());
	ctx->DrawGeometry(borderPath.Get(), brushBorder.Get(), borderWPx);
}

void ToolSub::onColorSelect(Ling::Button* btn)
{
	// 先按下标找出点的是哪一格。必须这样绕一下，不能直接拿 selectColorIndex 去索引
	// colorBtns —— 选中自定义色时那个下标大于预设格子数（预设行里根本没有它的格子），
	// 原来那版一上来就是 colorBtns[selectColorIndex]，那一下是越界读
	size_t idx{ colors.size() };
	for (size_t i = 0; i < colorBtns.size(); i++) {
		if (colorBtns[i] == btn) { idx = i; break; }
	}
	if (idx >= colors.size()) return;
	if (idx == selectColorIndex) return;
	setColorIndex(idx);
}

void ToolSub::initColorBtns()
{
	// contentNode->removeAllChildren() 已经把上一批按钮销毁了，这里必须同步清空，
	// 否则 colorBtns 会越积越长且前面全是野指针，selectColorIndex 也会越界。
	colorBtns.clear();
	// 与**预设**一一对应的语言键后缀。自定义色不在表里，取不到名字就退回色号
	//（见下面 tip 那一段），所以这里只管预设那 9 项
	static const std::vector<std::wstring> colorNames{ L"red",L"yellow",L"green",L"cyan",L"blue",L"purple",L"pink",L"black",L"white" };
	// 只画预设。自定义色不进这一行 —— 它们可能攒到十几二十个，全铺出来会把
	// 工具条撑成一条长长的色带，而工具条上真正要紧的是线宽 / 填充那几个控件
	for (size_t i = 0; i < presetCount_; i++)
	{
		auto btn = contentNode->makeChild<Ling::Button>();
		btn->setHeight(btnSize-2.5);
		btn->setFlexGrow(1.f);
		btn->setAlignItems(Ling::Align::Center);
		btn->setJustifyContent(Ling::Justify::Center);
		btn->setHoverBg(0XF2F2F2ff);
		btn->onClick.add([this](Ling::Button* btn) {this->onColorSelect(btn);});
		if (i < colorNames.size()) tip->bind(btn, Lang::get(std::format(L"color.{}", colorNames[i])));
		colorBtns.push_back(btn);

		auto label = btn->makeChild<Ling::Label>();
		label->setAlignItems(Ling::Align::Center);
		label->setJustifyContent(Ling::Justify::Center);
		label->setSize(13.f, 13.f);
		// 重建后要把对勾画在当前选中的那一项上，而不是固定第一项
		if (i == selectColorIndex) {
			label->setText(L"\ue6ad");
		}
		label->setFontFamily(L"icon");
		label->setFontSize(8.f);
		label->setBg(colors[i]);
		label->setBorderRadius(2.f);
		label->setColor(checkInkOn(colors[i]));
		// 白块（以及以后的浅色自定义色）在浅色工具条上要描一圈边才看得出是个色块
		if (isLightColor(colors[i])) label->setBorder(1.f, 0xA8A8A8FF);
	}

	// 末尾这块：底色就是当前选中色，点它开取色器。宽度比预设格子大 ——
	// 对齐 pixpin 的「最大的颜色块」，宽度本身就是"这里能点开调"的提示
	colorMoreBtn = contentNode->makeChild<Ling::Button>();
	colorMoreBtn->setHeight(btnSize-2.5);
	colorMoreBtn->setWidth(colorMoreW);
	colorMoreBtn->setAlignItems(Ling::Align::Center);
	colorMoreBtn->setJustifyContent(Ling::Justify::Center);
	colorMoreBtn->setHoverBg(0XF2F2F2ff);
	colorMoreBtn->onClick.add([this](Ling::Button*) { this->toggleColorPicker(colorMoreBtn); });
	tip->bind(colorMoreBtn, Lang::get(L"color.more"));

	colorMoreLabel = colorMoreBtn->makeChild<Ling::Label>();
	colorMoreLabel->setAlignItems(Ling::Align::Center);
	colorMoreLabel->setJustifyContent(Ling::Justify::Center);
	// 比预设那 13 格大一圈：它既是入口，也是"当前到底用着哪个颜色"的唯一一处预览 ——
	// 选中自定义色时预设行里没有任何一格能反映出来，只靠它
	colorMoreLabel->setSize(22.f, 18.f);
	colorMoreLabel->setFontFamily(L"icon");
	colorMoreLabel->setFontSize(10.f);
	colorMoreLabel->setBorderRadius(3.f);
	syncColorBtns();
}

void ToolSub::syncColorBtns()
{
	// 预设行：只有下标落在预设范围内时才有一格打勾。选中自定义色时一个都不打，
	// 信息全在末尾那块当前色块上 —— 硬把勾打在某格预设上是错的（那格并不是当前色）
	for (size_t i = 0; i < colorBtns.size(); i++) {
		// children[0] 是 Button 自己那个 Text（见 Ling::Button 构造里的 makeChild<Text>），
		// 我们后加的那个色块标签在 [1]
		if (colorBtns[i]->children.size() < 2) continue;
		auto label = dynamic_cast<Ling::Label*>(colorBtns[i]->children[1].get());
		if (!label) continue;
		label->setText(i == selectColorIndex ? L"\ue6ad" : L"");
	}
	if (!colorMoreLabel) return;
	if (selectColorIndex >= colors.size()) selectColorIndex = 0;
	const auto cur = colors[selectColorIndex];
	colorMoreLabel->setText(L"\ue6ad");
	colorMoreLabel->setBg(cur);
	colorMoreLabel->setColor(checkInkOn(cur));
	if (isLightColor(cur)) colorMoreLabel->setBorder(1.f, 0xA8A8A8FF);
	else colorMoreLabel->setBorder(0.f, 0);
}

int ToolSub::presetCount() const
{
	return static_cast<int>(presetCount_);
}

const std::vector<UINT32>& ToolSub::presetColors()
{
	// 静态一份：色板行的格数与它绑定（initSize 按它算宽度），中途变了下标就全乱。
	// 顺序就是色板行上的排列顺序，也是 colorIndex 的落盘值 —— 别随手调：
	// 调了以后所有已存配置里的 colorIndex 都会指到另一个颜色上
	static const std::vector<UINT32> table{
		0XCF1322FF, 0XD48806FF, 0X389E0DFF, 0X13C2C2FF, 0X0958D9FF,
		0X722ED1FF, 0XEB2F96FF, 0X000000FF, 0XFFFFFFFF
	};
	return table;
}

void ToolSub::refreshColors()
{
	const auto& presets = presetColors();
	presetCount_ = presets.size();
	colors.assign(presets.begin(), presets.end());
	// 用户自定义色接在预设后面，落盘在 common.customColors（逗号分隔的 8 位十六进制）。
	// 之所以拼进同一张表而不是另开一张：那样 colorIndex 一套下标语义就同时管住两者，
	// getSelectedColorValue / getToolColorValue / getWatermarkColorValue 一行都不用改
	const auto raw = Setting::get()->getToolStr(L"common", L"customColors", L"");
	size_t pos{ 0 };
	while (pos <= raw.size()) {
		const auto end = raw.find(L',', pos);
		const auto stop = end == std::wstring::npos ? raw.size() : end;
		UINT32 v{ 0 };
		if (stop > pos && parseHex8(raw.substr(pos, stop - pos), v)) colors.push_back(v);
		if (end == std::wstring::npos) break;
		pos = stop + 1;
	}
	// 表长变过（自定义色被顶掉、配置被改坏）之后旧下标可能越界
	if (selectColorIndex >= colors.size()) selectColorIndex = 0;
}

void ToolSub::setColorIndex(size_t idx)
{
	if (idx >= colors.size()) return;
	selectColorIndex = static_cast<UINT>(idx);
	Setting::get()->setToolNum(curToolId, L"colorIndex", static_cast<float>(selectColorIndex));
	// 色板行上的对勾与末尾那块当前色块都要跟着走
	syncColorBtns();
	win->onToolStyleChanged();
}

void ToolSub::applyColor(UINT32 rgba)
{
	// 表里已经有就复用那一格：同一个颜色点两次不该长出两个格子，色板会越用越乱
	for (size_t i = 0; i < colors.size(); i++) {
		if (colors[i] == rgba) { setColorIndex(i); return; }
	}
	// 新颜色插在自定义区的**最前面**（按最近用过排），预设那一段始终在最前。
	// 插在队首而不是队尾：常用的几个会自然浮上来，被 maxCustomColors 顶掉的是最久没用的
	std::vector<UINT32> custom(colors.begin() + presetCount_, colors.end());
	custom.insert(custom.begin(), rgba);
	if (custom.size() > maxCustomColors) custom.resize(maxCustomColors);
	Setting::get()->setToolStr(L"common", L"customColors", joinHexColors(custom));
	refreshColors();
	setColorIndex(presetCount_);   // 刚插进去的那一项
}

void ToolSub::toggleColorPicker(Ling::Node* anchor)
{
	if (WinColorPicker::isOpen()) { WinColorPicker::close(); return; }
	WinColorPicker::show(this, anchor);
}

void ToolSub::applyToggleStyle(Ling::Button* btn, bool selected)
{
	if (selected) {
		// 选中态 hover 色与常态一致，避免鼠标移上去时选中效果被 hover 覆盖掉
		btn->setBg(0xe6f4ffff);
		btn->setHoverBg(0xe6f4ffff);
	}
	else {
		btn->setBg(0);
		btn->setHoverBg(0xF2F2F2ff);
	}
}

// 开 / 关只有两种状态，按钮本身（底色）已经把它表示清楚了，单击直接翻转。
// 不弹下拉：两项的列表要用户"点开、看清、再点一次"，比直接翻转多两步
Ling::Button* ToolSub::makeToggleBtn(const std::wstring& text, bool* flag, const std::wstring& tipKey, const std::wstring& cfgKey)
{
	// 上次退出前的状态在配置文件里，先取回来盖掉内存里那份（成员的初值只是"从没设置过"时的默认）
	*flag = Setting::get()->getToolFlag(curToolId, cfgKey, *flag);
	auto btn = contentNode->makeChild<Ling::Button>();
	btn->setText(text);
	btn->setHeight(btnSize - 2.5);
	btn->setFlexGrow(1.f);
	btn->setFontFamily(L"icon");
	btn->setFontSize(13.f);
	applyToggleStyle(btn, *flag);
	tip->bind(btn, Lang::get(tipKey));
	// flag 指向 ToolSub 的成员，生命周期与 this 相同，btn 也挂在 this 的节点树上，捕获裸指针安全。
	// cfgKey 按值捕获：调用方传进来的是临时量
	btn->onClick.add([this, flag, cfgKey](Ling::Button* b) {
		*flag = !*flag;
		applyToggleStyle(b, *flag);
		Setting::get()->setToolFlag(curToolId, cfgKey, *flag);
		win->onToolStyleChanged();
	});
	return btn;
}

Ling::Button* ToolSub::makeSelectBtn(const std::wstring& tipKey, const std::wstring& cfgKey,
	int* index, const std::vector<std::wstring>& items,
	std::function<void()> onPicked, bool useIconFont, bool refreshNumbers,
	SelectPopup::SamplePainter paintSample)
{
	// 与 makeToggleBtn 同理：上一次的选择在配置文件里，取回来盖掉内存里那份。
	// 夹值域是因为配置文件可能被手工改坏，而这个值要用来取 items，越界就取到表外了
	*index = std::clamp((int)Setting::get()->getToolNum(curToolId, cfgKey, 0.f), 0, (int)items.size() - 1);
	auto btn = contentNode->makeChild<Ling::Button>();
	btn->setHeight(btnSize - 2.5);
	btn->setFlexGrow(1.f);
	btn->setFontSize(13.f);
	if (useIconFont) btn->setFontFamily(L"icon");
	// 自绘的按钮不写字：文字与画布挤在同一格里（按钮内部就一列，居中对齐）。
	// 画布铺满整格，按钮自身的底色与悬停色从它透上来
	if (paintSample) {
		auto sample = btn->makeChild<Ling::Canvas>();
		sample->setSizePercent(100.f, 100.f);
		sample->setFlexShrink(0.f);
		samples.push_back({ sample, index, paintSample });
	}
	else btn->setText(items[*index]);
	// 多档按钮没有"开 / 关"两种态，统一用常态配色
	btn->setBg(0);
	btn->setHoverBg(0xF2F2F2ff);
	tip->bind(btn, Lang::get(tipKey));
	// 换到新档位：自绘的那几个按钮靠重画工具条把那一格的小图换掉（setText 内部就会
	// refresh，不写字的那几个得自己叫一声）
	auto switchTo = [this, btn, items, paintSample](int i) {
		if (paintSample) refresh();
		else btn->setText(items[i]);
	};
	// 只有两项的按钮单击即切换：按钮上写的/画的就是当前那一项，再弹一个只有两项的列表
	// 让用户"点开、看清、再点一次"，比直接切多两步。目前只有线条类型（直角折线 / 普通线条）
	// 是两项的；箭头样式、端点、线条样式这些档位多了，都走下面的列表
	if (items.size() == 2) {
		btn->onClick.add([this, index, cfgKey, switchTo, onPicked, refreshNumbers](Ling::Button*) {
			*index = 1 - *index;
			Setting::get()->setToolNum(curToolId, cfgKey, (float)*index);
			switchTo(*index);
			// 已经画在图上的序号跟着换样子，而不是等下一次新建才生效
			if (refreshNumbers) win->refreshNumberShapes();
			if (onPicked) onPicked();
		});
		return btn;
	}
	// index 与 items 捕获到 lambda 里，按钮重建时会跟着 contentNode 一起销毁，
	// 而 ToolSub 与 WinPin 同生命周期，index 指向的成员不会先没
	btn->onClick.add([this, index, items, cfgKey, btn, switchTo, onPicked, refreshNumbers, useIconFont, paintSample](Ling::Button*) {
		// 列表可能翻到按钮上方，那时它正好压在悬停提示的位置上，先把提示收掉
		tip->hide();
		SelectPopup::show(this, btn, items, *index,
			[this, index, cfgKey, switchTo, onPicked, refreshNumbers](int picked) {
				*index = picked;
				Setting::get()->setToolNum(curToolId, cfgKey, (float)picked);
				switchTo(picked);
				// 已经画在图上的序号跟着换样子，而不是等下一次新建才生效
				if (refreshNumbers) win->refreshNumberShapes();
				if (onPicked) onPicked();
			},
			useIconFont ? std::wstring{ L"icon" } : std::wstring{}, 0.f, paintSample);
	});
	return btn;
}

Ling::Button* ToolSub::makeTextToggle(const std::wstring& text, const std::wstring& tipKey,
	const std::wstring& cfgKey, bool def, std::function<void(bool)> apply)
{
	bool on = Setting::get()->getToolFlag(curToolId, cfgKey, def);
	auto btn = contentNode->makeChild<Ling::Button>();
	btn->setText(text);
	btn->setHeight(btnSize - 2.5);
	btn->setFlexGrow(1.f);
	// 字要小一号：三个开关各两个字，13 号放不下会被 flex 压扁
	btn->setFontSize(12.f);
	btn->setBg(0);
	btn->setHoverBg(0xF2F2F2ff);
	applyToggleStyle(btn, on);
	apply(on);
	tip->bind(btn, Lang::get(tipKey));
	btn->onClick.add([this, cfgKey, apply](Ling::Button* b) {
		bool next = !Setting::get()->getToolFlag(curToolId, cfgKey, false);
		Setting::get()->setToolFlag(curToolId, cfgKey, next);
		applyToggleStyle(b, next);
		apply(next);
	});
	return btn;
}

Ling::Button* ToolSub::makeStateToggle(const std::wstring& text, const std::wstring& tipKey,
	std::function<bool()> read, std::function<void(bool)> apply)
{
	auto btn = contentNode->makeChild<Ling::Button>();
	btn->setText(text);
	btn->setHeight(btnSize - 2.5);
	btn->setFlexGrow(1.f);
	btn->setFontSize(12.f);
	btn->setBg(0);
	btn->setHoverBg(0xF2F2F2ff);
	// 只按当前实例状态上色，构建期不调 apply：这张图本来是什么样就是什么样，
	// 不像 makeTextToggle 那样拿全局配置把状态盖一遍
	applyToggleStyle(btn, read());
	tip->bind(btn, Lang::get(tipKey));
	btn->onClick.add([this, read, apply](Ling::Button* b) {
		bool next = !read();
		applyToggleStyle(b, next);
		apply(next);
	});
	return btn;
}

// 「应用到全部」：把工具条当前样式套到图上同工具的所有标注。
// 用文字不用图标字体 —— 图标码位里没有合适的"应用"符号，硬猜只会显示成方块
void ToolSub::makeApplyAllBtn()
{
	auto btn = contentNode->makeChild<Ling::Button>();
	btn->setText(L"全");
	btn->setHeight(btnSize - 2.5);
	btn->setFlexGrow(1.f);
	btn->setFontSize(12.f);
	// 它是动作按钮不是开关，取循环按钮那一档的常态配色
	btn->setBg(0);
	btn->setHoverBg(0xF2F2F2ff);
	tip->bind(btn, Lang::get(L"tool.applyAll"));
	btn->onClick.add([this](Ling::Button*) { win->applyStyleToAllShapes(); });
}

void ToolSub::showWatermarkTools()
{
	beginTool(L"watermark");
	// 五个按钮（内容 / 位置 / 旋转 / 样式 / 清除）+ 色板。extraW 里要算上固定宽度的内容按钮
	// 与它左右各一次的间距 —— initSize 只按按钮数算间距，固定宽度的控件得自己加。
	// 滑块数给 0：那三个滑块搬去竖排浮层了，这里一个都不建
	initSize(5, true, true, contentBtnW + sliderMargin * 2, 0);
	auto setting = Setting::get();
	// 文字从配置读回：水印十有八九每张截图都写同一句，不该每次都重打
	watermarkText = setting->getToolStr(L"watermark", L"text", L"");
	// 位置也读回并夹一遍：配置可能是旧版写的（那时还没有这个键），也可能被手工改坏
	watermarkPos = std::clamp((int)setting->getToolNum(L"watermark", L"pos", 0.f), 0, 7);
	// 角度、不透明度、间距同样从配置读回并夹紧 —— 这几个值会直接喂给 D2D
	//（透明度的 alpha、平铺步长），越界要么看不见要么慢得离谱。
	// 不透明度与间距另有用户可见的入口（竖排浮层上的滑块），改的是同一份
	watermarkRotate = std::clamp((int)setting->getToolNum(L"watermark", L"rotate", 0.f), 0, 3);
	watermarkAlpha = std::clamp((int)setting->getToolNum(L"watermark", L"alpha", 25.f), 5, 100);
	// 间距的键名换成 gapPct：老配置里的 gap 是"三档下标"（0~2），沿用同名会被读成 0~2%
	// ——那样平铺密得糊成一片。换个键名，老配置自然退回默认的 25%
	watermarkGapPct = std::clamp((int)setting->getToolNum(L"watermark", L"gapPct", 25.f), 0, 100);
	// 字号自己读回并夹紧。原来它是借 sliderVal（"当前工具"的滑块值）的，切到文本工具
	// 就被换成文本字号，而水印每帧都读它 —— 于是"改文本字号，水印跟着变大"。
	// 默认值与值域都取自配置表里 watermark 那一档，不再在别处另写一份
	if (auto cfg = findSliderCfg(L"watermark")) {
		watermarkFontSize = std::clamp(setting->getToolNum(L"watermark", cfg->key, cfg->def), cfg->min, cfg->max);
	}
	// 「内容」按钮：点开是水印内容的编辑弹窗（多行文字 + 时间格式 + 字体，参考 pixpin）。
	// 原来是工具条上的一个单行输入框 + 字体下拉 + 时间按钮三件套 —— 三个控件抢一条
	// 32 像素高的窄条，文字框只剩 150 宽，写两行就装不下了，而"标题 + 时间"正是
	// 水印最常见的写法。收进弹窗之后工具条上只剩它一个入口
	watermarkContentBtn = contentNode->makeChild<Ling::Button>();
	watermarkContentBtn->setHeight(btnSize - 2.5);
	watermarkContentBtn->setWidth(contentBtnW);
	watermarkContentBtn->setFontSize(12.f);
	watermarkContentBtn->setBg(0);
	watermarkContentBtn->setHoverBg(0xF2F2F2ff);
	syncWatermarkContentBtn();
	tip->bind(watermarkContentBtn, Lang::get(L"tool.watermarkContent"));
	watermarkContentBtn->onClick.add([this, btn = watermarkContentBtn](Ling::Button*) {
		// 弹窗可能翻到按钮上方，那时它正好压在悬停提示的位置上，先把提示收掉
		tip->hide();
		// 字体不在工具条上了，攒的是上一次在弹窗里选的那份 —— 每次开弹窗前读回来，
		// 免得换个入口打开就退回默认字体
		watermarkFont = Setting::get()->getToolStr(L"watermark", L"fontFamily", L"Microsoft YaHei");
		WinWatermarkText::show(this, btn, watermarkText, watermarkFont,
			// 实时预览：打字 / 换字体时就改内存里这份并重画，不碰 config.json ——
			// 落盘是「应用」的事。弹窗在取消 / 失焦时会拿原值再回调一次这里，
			// 于是预览被还原，图上不会留下没落盘的半截改动
			[this](const std::wstring& text, const std::wstring& family) {
				watermarkText = text;
				watermarkFont = family;
				syncWatermarkContentBtn();
				win->refresh();
				},
			[this](const std::wstring& text, const std::wstring& family) {
				watermarkText = text;
				// 字体族名与工具条上那份存同一个键：哪边改的都要认
				watermarkFont = family;
				Setting::get()->setToolStr(L"watermark", L"text", text);
				Setting::get()->setToolStr(L"watermark", L"fontFamily", family);
				syncWatermarkContentBtn();
				win->refresh();
				});
		});
	// 位置：平铺 / 右下角 / 左下角 / 右上角 / 左上角 / 顶部居中 / 底部居中 / 居中。
	// 原来这里只有一个"平铺"开关 —— 关了就只能以鼠标落点为中心摆一块，四角 / 上下中这些常用落点
	// 一个都没有。改成表里的八档，一次点中
	std::vector<std::wstring> posItems;
	for (int i = 0; i < 8; ++i) posItems.push_back(Lang::get(std::format(L"tool.watermarkPos{}", i)));
	makeSelectBtn(L"tool.watermarkPos", L"pos", &watermarkPos, posItems,
		[this]() { syncWatermarkRotateBtn(); win->refresh(); }, false, false);
	// 旋转按钮自己建（不用 makeSelectBtn）：非平铺时要置灰且点了不弹列表
	{
		auto btn = contentNode->makeChild<Ling::Button>();
		watermarkRotBtn = btn;
		btn->setHeight(btnSize - 2.5);
		btn->setFlexGrow(1.f);
		btn->setFontSize(13.f);
		btn->setBg(0);
		btn->setHoverBg(0xF2F2F2ff);
		tip->bind(btn, Lang::get(L"tool.watermarkRotate"));
		btn->onClick.add([this, btn](Ling::Button*) {
			// 平铺以外的位置一律水平摆（见 ShapeWatermark::paint），角度这一项用不上
			if (watermarkPos != 0) return;
			// 列表可能翻到按钮上方，那时它正好压在悬停提示的位置上，先把提示收掉
			tip->hide();
			std::vector<std::wstring> items;
			for (auto v : watermarkRotateSteps) items.push_back(std::format(L"{}°", (int)v));
			SelectPopup::show(this, btn, items, std::clamp(watermarkRotate, 0, 3),
				[this](int picked) {
					watermarkRotate = picked;
					Setting::get()->setToolNum(curToolId, L"rotate", (float)picked);
					syncWatermarkRotateBtn();
					win->refresh();
				});
			});
	}
	syncWatermarkRotateBtn();
	// 不透明度 / 大小 / 间距搬到了竖排浮层（WinWatermarkPanel），hover 这一枚时弹在工具条下面。
	// 这里只留"悬停即弹"这一个动作，滑块本身一个都不建 ——
	// 工具条上原本是三个并排的横滑块，每个不到 60 宽，拖起来几乎挪不动
	styleBtn = contentNode->makeChild<Ling::Button>();
	styleBtn->setHeight(btnSize - 2.5);
	styleBtn->setWidth(btnSize);
	styleBtn->setFontSize(12.f);
	styleBtn->setText(Lang::get(L"tool.watermarkStyleShort"));
	styleBtn->setBg(0);
	styleBtn->setHoverBg(0xF2F2F2ff);
	tip->bind(styleBtn, Lang::get(L"tool.watermarkStyle"));
	styleBtn->onEnter.add([this, b = styleBtn](Ling::Button*) {
		// 浮层不是点击弹出的，是悬停就弹 —— 三个滑块要能直接拖，
		// 先点一下才出现的话每次微调都要点两下
		WinWatermarkPanel::show(this, b);
		});
	styleBtn->onClick.add([this, b = styleBtn](Ling::Button*) {
		// 点一下也弹：有人是"看到了但没敢往按钮上悬"地找那一项，
		// 与其让人以为按钮坏了，不如两条路都能弹
		WinWatermarkPanel::show(this, b);
		});
	// 「清除」：一键撤掉图上所有水印。挂在最后：前面四枚管"长什么样"，它管"不要了"。
	// 字号比「样式」小一号 —— 这一格是 flexGrow 分下来的固定余量，
	// 而 en/id/ru 的词（Clear / Hapus / Убрать）比「样式」两个字宽，13 号会顶破格子。
	// 走 History::undoShapes，只打撤销标记不真删，清完还能 Ctrl+Y 找回来
	{
		auto btn = contentNode->makeChild<Ling::Button>();
		watermarkClearBtn = btn;
		btn->setHeight(btnSize - 2.5);
		btn->setFlexGrow(1.f);
		btn->setFontSize(12.f);
		btn->setText(Lang::get(L"tool.watermarkClearShort"));
		btn->setBg(0);
		btn->setHoverBg(0xF2F2F2ff);
		tip->bind(btn, Lang::get(L"tool.watermarkClear"));
		btn->onClick.add([this](Ling::Button*) {
			tip->hide();
			win->clearWatermark();
		});
	}
	initColorBtns();
}

// 「内容」按钮上显示当前水印文字的第一行，截太长加省略号；没写文字时给个提示
void ToolSub::syncWatermarkContentBtn()
{
	if (!watermarkContentBtn) return;
	auto show = watermarkText;
	// 换行截断：多行内容在这个一格宽的按钮上显示不出第二行
	auto nl = show.find(L'\n');
	if (nl != std::wstring::npos) show = show.substr(0, nl);
	if (show.empty()) {
		watermarkContentBtn->setText(Lang::get(L"tool.watermarkTip"));
		watermarkContentBtn->setColor(0xAAAAAAFF);
		return;
	}
	watermarkContentBtn->setColor(0x000000FF);
	if (show.size() > (size_t)fontMaxChars) {
		show = show.substr(0, (size_t)fontMaxChars - 1) + L"\u2026";
	}
	watermarkContentBtn->setText(show);
}

void ToolSub::syncWatermarkRotateBtn()
{
	if (!watermarkRotBtn) return;
	// 非平铺显示 0°：这就是图上实际的摆法，免得人以为"调了角度没生效"
	auto on = watermarkPos == 0;
	auto deg = on ? (int)watermarkRotateSteps[std::clamp(watermarkRotate, 0, 3)] : 0;
	watermarkRotBtn->setText(std::format(L"{}°", deg));
	watermarkRotBtn->setColor(on ? 0x000000FF : 0xAAAAAAFF);
	watermarkRotBtn->setHoverBg(on ? 0xF2F2F2ff : 0);
}

Ling::Slider* ToolSub::makeSlider(float min, float max, float val, std::function<void(float)> onChange,
	const std::wstring& name)
{
	auto s = contentNode->makeChild<Ling::Slider>();
	// 尺寸从字段来，别写字面量：initSize 按同样的字段算窗口宽度，
	// 两边各写一份的话改了一处就会宽度不匹配（flex 会把误差压在按钮和滑块上）。
	s->setWidth(sliderSize);
	s->setMarginLeft(sliderMargin);
	s->setMarginRight(sliderMargin);
	s->setHeightPercent(100.f);
	// setValue 排在 onValueChanged 之前：加载配置这一下不会反过来又写一次盘
	s->setRange(min, max);
	s->setValue(val);
	s->setStep(1.f);
	s->onValueChanged.add([onChange](Ling::Slider*, float v) {
		if (onChange) onChange(v);
		});
	s->setThumbColor(0x888888FF);
	s->setHoverThumbColor(0x888888FF);
	s->setTrackColor(0x888888FF);
	s->setFillColor(0x888888FF);
	sliders.push_back(s);
	sliderNames.push_back(name);
	return s;
}

void ToolSub::initSlider()
{
	// 值域与当前值都从字段来：切换工具时滑块会被销毁重建，靠 beginTool 把配置里那份带过来
	slider = makeSlider(sliderMin, sliderMax, sliderVal, [this](float val) {
		sliderVal = val;
		Setting::get()->setToolNum(curToolId, curSliderKey, val);
		// 正在编辑的文本要立刻跟着变字号，不然得点完再看效果
		win->onToolStyleChanged();
		});
}

float ToolSub::toPx(float logical) const
{
	return std::floor(logical * dpi);
}

float ToolSub::getDesiredHeight()
{
	// 返回物理像素：调用方（WinPin::layoutTools）拿它和屏幕坐标、ToolMain->h 一起算，那些都是物理值。
	// 视觉高度（btnSize）与 ToolMain 的 h 保持一致 —— ToolMain 也是 h = btnSize，
	// 边框画在这个高度之内、不额外占空间；上面再加箭头区域。
	// 两段各自吸附到整数像素后再相加，避免和界内取整的差异。
	return toPx(btnSize) + toPx(marginTop);
}

// 宽度 = 工具按钮 + 颜色按钮 + 滑块（含左右 margin）。
// 之前这里漏算了滑块的真实宽度，宽工具栏靠 10 个 flexGrow 按钮把误差摊薄了看不出来，
// 而 mosaic/eraser 只有 1 个按钮，误差全压在这个按钮和滑块上，看起来就像被压缩了。
void ToolSub::initSize(int btnCount, bool withColors, bool centerOnBtn, float extraW, int sliderCount)
{
	hasTools = true;
	this->centerOnBtn = centerOnBtn;
	sizeBtnCount = btnCount;
	sizeWithColors = withColors;
	sizeExtraW = extraW;
	// 0 是合法值：水印那三个滑块搬去了竖排浮层，工具条上一个都不留。
	// 早先这里写的是 max(1, ...)，水印传 0 也会被按回 1，窗口凭空宽出一格滑块
	sizeSliderCount = std::max(0, sliderCount);
	// 色板行 = presetCount_ 格预设 + 末尾那块「当前色」。后者比别的格子宽，单独累加。
	// 注意不能用 colors.size()：那是预设 + 自定义，自定义色不铺到行上（见 initColorBtns）
	auto count = btnCount + (withColors ? static_cast<int>(presetCount_) : 0);
	// 宽度只按内容算，边框画在内容之内（与 ToolMain 一致，那边宽度也只累加按钮）。
	auto pxW = toPx(btnSize) * count + (withColors ? toPx(colorMoreW) : 0.f)
		+ toPx(sliderSize) * sizeSliderCount
		+ toPx(sliderMargin) * 2 * sizeSliderCount + toPx(extraW);
	// setSize 收逻辑像素、内部再乘 dpi，所以这里把算好的物理宽高除回去
	setSize(pxW / dpi, getDesiredHeight() / dpi);
}

void ToolSub::refreshSize()
{
	if (!hasTools) return;   //没内容时窗口是藏着的，等下次 show*Tools 自然会按新 dpi 算
	initSize(sizeBtnCount, sizeWithColors, centerOnBtn, sizeExtraW, sizeSliderCount);
}

D2D1_COLOR_F ToolSub::getSelectedColor() const
{
	return Ling::Color(colors[selectColorIndex]).getD2DColor();
}

UINT32 ToolSub::getSelectedColorValue() const
{
	return colors[selectColorIndex];
}

UINT32 ToolSub::getWatermarkColorValue() const
{
	// 水印的选中色只有一处落盘：config.json 里 watermark 组的 colorIndex（onColorSelect 写的）。
	// 不去读 selectColorIndex —— 那是"当前工具"的那份共享下标，切工具时就被换成别人的了，
	// 而水印每帧都来取色（见 ShapeWatermark::prepare），于是"改矩形填充色水印跟着变色"。
	// colors[] 的各处取用都不做边界检查，配置被手工改坏或旧版本写了越界值时这里兜一下
	auto n = static_cast<int>(Setting::get()->getToolNum(L"watermark", L"colorIndex", 0.f));
	if (n < 0 || n >= (int)colors.size()) n = 0;
	return colors[(size_t)n];
}

D2D1_COLOR_F ToolSub::getWatermarkColor() const
{
	return Ling::Color(getWatermarkColorValue()).getD2DColor();
}

float ToolSub::getSliderVal() const
{
	return sliderVal*dpi;
}

bool ToolSub::hasContent()
{
	return hasTools;
}

void ToolSub::hideTools()
{
	hasTools = false;
	tip->hide();
	SelectPopup::close();
	// 取色器是挂在工具条上的浮层：工具条都收了，它还留在屏幕上就是块孤儿
	// （它画的颜色是由色板行那块当前色块标识的，那一块已经没了）
	WinColorPicker::close();
	if (!isVisible) return;
	hide();
	isVisible = false;
}

// ToolSub 永远紧贴 ToolMain 下方（三种模式都是），所以只跟着 ToolMain 走。
// x 默认与 ToolMain 左对齐；窄工具栏（mosaic/eraser）改为居中对齐到 ToolMain 上选中的那个按钮。
// 被屏幕边界裁剪后，同步修正箭头位置让它继续指向选中的按钮。
void ToolSub::updatePosition(const RECT& workArea)
{
	if (!hasTools) return;
	// btnCenterX 是选中按钮中心相对 ToolMain 左边的偏移
	auto btnCenterX = win->toolMain->getBtnCenterX();
	auto mainX = static_cast<float>(win->toolMain->x);
	auto px = centerOnBtn ? mainX + btnCenterX - w / 2.f : mainX;
	auto py = static_cast<float>(win->toolMain->y) + win->toolMain->h + mainGap;
	auto upperX = workArea.right - static_cast<int>(w);
	if (upperX < workArea.left) upperX = workArea.left;
	auto finalX = static_cast<int>(px);
	if (finalX < workArea.left) finalX = workArea.left;
	if (finalX > upperX) finalX = upperX;
	// 箭头始终指向按钮中心的屏幕位置，换算成窗口内坐标
	auto newArrowX = mainX + btnCenterX - finalX;
	// 整组一起平移时 arrowX 一动不动：窗口挪了，箭头在窗口里的位置没挪，那一格内容与上一帧
	// 逐像素相同。而本函数在拖贴图 / 拖剪裁采样点的每个鼠标事件上都要跑一遍，白刷一次就是让
	// 这块单缓冲画布（Clear → 重画边框与箭头）每个鼠标事件闪一帧 —— 拖动时的"一闪一闪"
	// 有一半是这么来的。只有箭头真的换了位置才重画
	const bool arrowMoved = newArrowX != arrowX;
	arrowX = newArrowX;
	setPosition(finalX, static_cast<int>(py));
	if (isVisible) {
		if (arrowMoved) refresh();
	}
	else {
		show();
		isVisible = true;
	}
}

