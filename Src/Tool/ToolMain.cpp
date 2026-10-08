#include "pch.h"
#include "../Win/WinPin.h"
#include "../History.h"
#include "../Lang.h"
#include "../Setting.h"
#include "../Tip.h"
#include "ToolMain.h"
#include "ToolSub.h"

ToolMain::ToolMain(WinPin* win) : Ling::WinBase(), win(win)
{
	// 跟着宿主窗口的缩放走：WinBase 构造里取的是系统 dpi，WinPin 可能在另一块缩放比例不同的屏上
	dpi = win->dpi;
	// 初始位置由 WinPin::layoutTools() 统一决定，这里只算尺寸
	x = win->x;
	y = (int)(win->y + win->h + 5.f * win->dpi);
	refreshSize();
	// 点按钮会把 ToolMain 激活，此后键盘消息进的是它而不是 WinPin。
	// 直接把按键转触给 WinPin 的同名事件，快捷键在两个窗口上表现一致。
	onKeyDown.add([this](UINT key) { this->win->onKeyDown(key); });
	// 抬起也照转一路：WinPin 靠它做"再点一下 Ctrl = 取消框选"（见 WinPin::onKeyRelease，
	// 它挂在 WinPin 的 onKeyUp 事件上）。焦点落在工具条上时那一下才收得到
	onKeyUp.add([this](UINT key) { this->win->onKeyUp(key); });
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
	createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
}

// 宽度 = 各按钮宽度之和（btnIds 里的 "|" 是分隔符，只占 spliterW），高度 = 按钮高
void ToolMain::refreshSize()
{
	float logicW{ 0.f };
	for (auto& id : btnIds) {
		logicW += (id == L"|") ? spliterW : btnSize;
	}
	setSize(logicW, btnSize);
}

ToolMain::~ToolMain()
{
}

void ToolMain::init()
{
}

// 返回 curId 对应按钮的中心相对 ToolMain 左边的偏移（物理像素）。
// btnIds 含分隔符而 btns 不含，所以要单独维护 btns 的下标，不能拿 i 去索引 btns。
float ToolMain::getBtnCenterX()
{
	float result{ 0.f };
	size_t btnIndex{ 0 };
	for (size_t i = 0; i < btnIds.size(); i++)
	{
		if (btnIds[i] == L"|") {
			// 分隔符不在 btns 里，宽度与 onCreated 里 spliter 的 setSize(dpi, ...) 一致
			result += dpi;
			continue;
		}
		if (curId == btnIds[i]) {
			result += btns[btnIndex]->w / 2.f;
			return result;
		}
		result += btns[btnIndex]->w;
		btnIndex++;
	}
	return result;
}

void ToolMain::onCreated()
{
	tip = std::make_unique<Tip>(this);
	auto d2d = Ling::D2D::get();
	// 「选择对象」那支鼠标指针的墨色。同 ToolSub 的 brushBg：画刷与设备绑定，建一次复用
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushCursor.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x404040), brushCursorEdge.GetAddressOf());
	// 自绘图标用的是 0x333333，与 Button 的默认字色一致
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x333333), brushIconInk.GetAddressOf());
	body->setBg(0xFFFFFFFF);
	body->setBorder(1.f, 0xA8A8A8ff);
	body->setAlignItems(Ling::Align::Center);
	body->setFlexDirection(Ling::FlexDirection::Row);
	for (size_t i = 0; i < btnIds.size(); i++)
	{
		auto& id = btnIds[i];
		if (id == L"|") {
			auto spliter = body->makeChild<Ling::Node>();
			spliter->setSize(dpi, 18.f);
			spliter->setBg(0xDDDDDDff);
		}
		else {
			auto btn = body->makeChild<Ling::Button>();
			btn->setId(id);
			btn->setHeightPercent(100.f);
			btn->setFlexGrow(1.f);
			btn->setHoverBg(0xF2F2F2ff);
			// 「选择对象」与「选文」这两枚不写字：垫一张铺满的画布，自己在上面画
			//（图标字体里没有指针形状、也没有 I 形文本光标，41 个码位全都有主；
			// 见 layout / paintSelectIcon / paintTextSelectIcon）。其余各枚照旧走 icon 字体
			if (id == L"selector" || id == L"textSelect") {
				// 宽度写死，不交给 flex 分：它不写字、里面那张画布又是按百分比铺的，
				// 于是它的"基准尺寸（auto = 内容尺寸）"算出来是 0，而 flexGrow 是在各自
				// 基准上平分剩余空间 —— 结果这一格比其它格窄一大截（实测 26px vs 41px），
				// 就是作者说的"背景宽度与其它小图标不同"。其余各枚的基准都是 glyph 宽度、
				// 彼此相等，照旧由 flex 平分
				btn->setFlexGrow(0.f);
				btn->setWidth(btnSize);
				auto icon = btn->makeChild<Ling::Canvas>();
				icon->setSizePercent(100.f, 100.f);
				icon->setFlexShrink(0.f);
				if (id == L"selector") selectIcon = icon;
				else textSelectIcon = icon;
			}
			else {
				btn->setText(btnCodes[i]);
				btn->setFontFamily(L"icon");
				// \ue907（「隐藏贴图」）那枚的字形天生又扁又小、墨迹还长在字身框的上半截：
				// 13 号下只有 11x6 逻辑像素，比同排普遍小四成、位置还高 4 个像素
				//（作者报的"大小与其它小图标不同"）。字形改不了 —— 按光学尺寸单独放大字号，
				// 再用上内边距把偏上去的那一截补回中线（Button 的文字在内容框里居中，
				// 上内边距让内容框整块下移，补的是它的一半）
				if (id == L"pinHide") {
					btn->setFontSize(20.f);
					btn->setPaddingTop(9.f);
				}
				else btn->setFontSize(13.f);
			}
			btn->onClick.add([this](Ling::Button* btn) {onClick(btn);});
			tip->bind(btn, Lang::get(std::format(L"tool.{}", id)));
			if (id == L"geom") geomBtn = btn;
			btns.push_back(btn);
		}
	}
	// 图标按上次退出前的类别画（类别存在 config.json 里）。建按钮时用的是矩形那一档的初值
	syncGeomIcon();
	show();
}

// 「几何图形」那枚按钮上的图标跟着类别走（理由见头文件里的说明）。
// 类别读 config.json 的 geom/kind 而不是去问 ToolSub：本窗口的 onCreated 排在 ToolSub 建好之前
//（WinPin 的构造函数里它排在后面），那一刻还没有 ToolSub 可问；而 ToolSub 换类别时是
// 先把新值落盘、再来调这里，两边读的是同一份数，不会差一步
void ToolMain::syncGeomIcon()
{
	if (!geomBtn) return;
	auto kind = (int)Setting::get()->getToolNum(L"geom", L"kind", 0.f);
	// \ue8e8 方框（矩形那一档）、\ue6bc 圆（圆形那一档）—— 就是原来主工具条上那两枚的码位
	geomBtn->setText(kind == 1 ? L"\ue6bc" : L"\ue8e8");
}

void ToolMain::layout()
{
	Ling::WinBase::layout();
	// 与 ToolSub 的样例格子同一套：startPaint / finishPaint 要成对用，
	// 且这一层底子是透明的 —— 按钮的常态 / 悬停底色从它下面透上来
	auto paint = [](Ling::Canvas* canvas, auto&& fn) {
		if (!canvas) return;
		auto ctx = canvas->startPaint();
		if (!ctx) return;
		ctx->Clear(0);
		fn(ctx, canvas->w, canvas->h);
		canvas->finishPaint();
	};
	paint(selectIcon, [this](ID2D1DeviceContext* ctx, float w, float h) {
		paintSelectIcon(ctx, w, h);
	});
	paint(textSelectIcon, [this](ID2D1DeviceContext* ctx, float w, float h) {
		paintTextSelectIcon(ctx, w, h);
	});
}

void ToolMain::paintSelectIcon(ID2D1DeviceContext* ctx, const float w, const float h)
{
	// 鼠标指针的轮廓：尖角在左上、尾巴在右下，就是系统那支标准箭头。
	// 用归一化坐标写，下面按这张画布的大小等比缩放（横竖各算一次取小的，指针不变形）
	static constexpr float pts[][2] = {
		{ 0.000f, 0.000f }, { 0.000f, 0.756f }, { 0.176f, 0.568f },
		{ 0.294f, 0.849f }, { 0.435f, 0.792f }, { 0.318f, 0.510f },
		{ 0.529f, 0.510f },
	};
	// 上面那串点自己的外接尺寸（0.529 × 0.849）
	constexpr float bw{ 0.529f }, bh{ 0.849f };
	// 指针的墨迹高度：图标字体那一排（13 号）的墨迹落在 11~13 个逻辑像素之间，取中间那一档。
	// 原来是把画布撑满、只留 pad 的边 —— 格子越宽指针画得越大，比别的图标高出一大截，
	// 正是作者说的"大小与其它小图标不同"。这里按固定尺寸画，横向居中
	const float s{ 10.f * dpi / bh };
	const float ox{ (w - bw * s) / 2.f }, oy{ (h - bh * s) / 2.f };

	Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
	if (FAILED(Ling::D2D::get()->d2dFactory->CreatePathGeometry(geo.GetAddressOf()))) return;
	Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(geo->Open(sink.GetAddressOf()))) return;
	sink->BeginFigure(D2D1::Point2F(ox + pts[0][0] * s, oy + pts[0][1] * s), D2D1_FIGURE_BEGIN_FILLED);
	for (size_t i = 1; i < _countof(pts); i++) {
		sink->AddLine(D2D1::Point2F(ox + pts[i][0] * s, oy + pts[i][1] * s));
	}
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	sink->Close();
	ctx->FillGeometry(geo.Get(), brushCursor.Get());
	ctx->DrawGeometry(geo.Get(), brushCursorEdge.Get(), 1.2f * dpi);
}

// I 形文本光标（「选文」那枚）。和上面那支鼠标指针同一套：归一化坐标写死，按画布大小等比缩放。
// 三笔描出来 —— 上托横 / 竖杠 / 下托横，用描边而不是填充（笔画本身就细，填充要闭合多边形反而绕）
//
// 尺寸按固定逻辑像素给（12 高），不随格子宽窄变：图标字体那一排（13 号）的墨迹高度落在
// 11~13 个逻辑像素之间，取这一档才和左右邻居一般大（同 paintSelectIcon 里那段说明）
void ToolMain::paintTextSelectIcon(ID2D1DeviceContext* ctx, const float w, const float h)
{
	if (!brushIconInk) return;
	const float s{ 12.f * dpi };                 // 光标墨迹高度
	const float boxW{ s * 0.5f };                // 一支光标大约是 1:2 的瘦长条
	const float ox{ (w - boxW) / 2.f }, oy{ (h - s) / 2.f };
	const float x0{ ox }, x1{ ox + boxW }, xm{ ox + boxW * 0.5f };
	const float y0{ oy }, y1{ oy + s };

	Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
	if (FAILED(Ling::D2D::get()->d2dFactory->CreatePathGeometry(geo.GetAddressOf()))) return;
	Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(geo->Open(sink.GetAddressOf()))) return;
	// 上托横
	sink->BeginFigure(D2D1::Point2F(x0, y0), D2D1_FIGURE_BEGIN_HOLLOW);
	sink->AddLine(D2D1::Point2F(x1, y0));
	sink->EndFigure(D2D1_FIGURE_END_OPEN);
	// 竖杠（略短一点，两头留出半条线宽，免得和下托横叠成一个方块）
	sink->BeginFigure(D2D1::Point2F(xm, y0), D2D1_FIGURE_BEGIN_HOLLOW);
	sink->AddLine(D2D1::Point2F(xm, y1));
	sink->EndFigure(D2D1_FIGURE_END_OPEN);
	// 下托横
	sink->BeginFigure(D2D1::Point2F(x0, y1), D2D1_FIGURE_BEGIN_HOLLOW);
	sink->AddLine(D2D1::Point2F(x1, y1));
	sink->EndFigure(D2D1_FIGURE_END_OPEN);
	sink->Close();
	ctx->DrawGeometry(geo.Get(), brushIconInk.Get(), 1.4f * dpi);
}

void ToolMain::applyNormalStyle(Ling::Button* btn)
{
	btn->setBg(0);
	btn->setHoverBg(0xF2F2F2ff);
}

// 选中底色（「选文」那个两态开关、以及「选择对象」那个常驻模式都用它）。
// 与 selectTool 里给工具按钮上的那层是同一种青色，但两者互斥地发生：
// 拿起任何一个标注工具都会先把选文关掉（见 selectTool 开头）
void ToolMain::setToggle(const std::wstring& id, bool on)
{
	for (auto b : btns)
	{
		if (b->id != id) continue;
		if (on) {
			b->setBg(0xe6f4ffff);
			b->setHoverBg(0xe6f4ffff);
		}
		else {
			applyNormalStyle(b);
		}
		return;
	}
}

// 取消选中：与 onClick 选中某个按钮是对称操作，只是没有新的选中项。
// ToolSub 由 curId 是否为空驱动，所以清空 curId 后 layoutTools() 会自动把它收起来。
void ToolMain::cancelSelect()
{
	// 「选择画布」跟着一起退出：右键收工具条、ESC 退一步、锁定贴图都走到这儿，
	// 那几种情况下"还在框底图"没有意义（选区与按钮底色一并收掉）。
	// 这一步排在下面那个"curId 本来就空"的早退之前 —— 否则那种情况下它就漏了
	win->setCanvasMode(false);
	// 画布上挑着的那一批同理，一并收掉：没有了画笔，"还选着哪些元素"没有意义
	win->clearObjectSelection();
	if (curId.empty()) return;
	for (auto b : btns)
	{
		if (b->id == curId) {
			applyNormalStyle(b);
		}
	}
	curId.clear();
	win->toolSub->hideTools();
	// curId 空了 ToolMain 要下移收回 ToolSub 让出的空间，交给 WinPin 重排整组
	win->layoutTools();
}

void ToolMain::refreshToolSub()
{
	if (curId.empty()) return;
	// 重建会把「编号」输入框一起重来（showNumberTools -> initNumberBox 把下一个编号拨回 1），
	// 而"把工具条请回来"不该动用户填过的起始号，所以进度先存下来、重建完写回去
	const int next = win->toolSub->peekNumberVal();
	// 复用 selectTool 那套派发：它按 curId 重建 ToolSub 的内容并重排整组。
	// 对同一个 id 再走一遍是幂等的（配色复位后又被设回选中色）
	selectTool(curId);
	win->toolSub->setNumberVal(next);
}

void ToolMain::onClick(Ling::Button* btn)
{
	// 悬停提示是系统 tooltip 控件，不会跟着工具条一起 hide —— 按钮把窗口收起来之后
	// 它会孤零零浮在原地，直到下一轮鼠标移动才消失。和 ToolCap 那边一样，动手之前先收掉
	tip->hide();
	// 关闭整个贴图窗口。WinPin 的 onDestroy 里会连带关掉 ToolMain / ToolSub，
	// 但 C++ 对象的释放被推迟到下一轮消息循环，所以这里 return 之后栈上访问 this 仍是安全的。
	if (btn->id == L"close") {
		win->close();
		return;
	}
	// 下面这几个都是"执行一次动作"而不是"切换绘图工具"，做完就返回，不动 curId 和选中态。
	// undo/redo 由 History 内部负责 refresh；save/clipboard 成功后会关窗，同样不能往下走。
	else if (btn->id == L"undo") {
		win->getHistory()->undo();
		return;
	}
	else if (btn->id == L"redo") {
		win->getHistory()->redo();
		return;
	}
	else if (btn->id == L"save") {
		win->saveToFile();
		return;
	}
	// 复制当前编辑状态的图到剪贴板，但**不关窗** —— 与「剪切板」那枚的唯一区别
	else if (btn->id == L"copy") {
		win->copyImageToClipboard();
		return;
	}
	else if (btn->id == L"clipboard") {
		win->copyToClipboard();
		return;
	}
	// 藏进屏幕左上角那条（再按一次就是放回来）。两态开关，同开 / 关那些：点一下翻一下，
	// 不弹列表。藏的期间贴图窗口与这两条工具条一起收起来，鼠标移到左上角那条上它就露回来
	else if (btn->id == L"pinHide") {
		win->setHidden(!win->getHidden());
		return;
	}
	// 「选文」同样是两态开关：点一下整张图进入文字选择态（拖拽按词选、Ctrl+C 复制、ESC 退出），
	// 再点一下出来。也同 pinHide，做完就返回，不进 curId —— 它不是"画什么"的工具
	else if (btn->id == L"textSelect") {
		win->setTextSelect(!win->getTextSelect());
		return;
	}
	// 再次点击已选中的按钮 = 取消选中（开关式）。cancelSelect 里已经做了配色复位、
	// 隐藏 ToolSub 和重排，这里直接返回，不要再往下走选中流程。
	// 这一条必须排在 setCanvasMode 之前：curId == selector（选择画布）时它正好就是
	// "退出"那一下，而 setCanvasMode(false) 自己也会去 cancelSelect —— 顺序颠倒的话
	// 模式刚关掉，又会被下面那行 selectTool 选回来
	if (btn->id == curId) {
		cancelSelect();
		return;
	}
	// 最前那枚是「选择画布」模式（原来叫「选择器」，底下还挂着「选择对象」子模式，
	// 那层已经撤掉了）：按它进模式，按别的工具出模式。程序内部按元素换工具不走这里
	//（见 WinPin::onUp），所以"点中元素后面板跟着换"不会把模式关掉。
	// 进出统一走 setCanvasMode —— 它自带按钮底色复位、选区收尾与重画
	win->setCanvasMode(btn->id == L"selector");
	// 用户主动换了工具：画布上还挑着的那一批（单选 / 多选）到此为止 —— 那些是"上一个
	// 工具手里挑出来的"，留着的话换个画笔随手按一下 Delete 就会连整批一起删。
	// 程序内部按元素切工具**不**走这儿（那条路正要把新点的那个选中，不能清）
	win->clearObjectSelection();
	selectTool(btn->id);
}

void ToolMain::selectTool(const std::wstring& id)
{
	// 拿起任何一种标注工具都先退出选文态：两套手势都要吃左键，同时开着必然有一套点不动
	//（选文那一套排在前面，剪裁框就再也拉不起来了）。已经是关的时候就什么都不做
	win->setTextSelect(false);
	for (auto b : btns)
	{
		if (b->id == curId)
		{
			applyNormalStyle(b);
		}
		if (b->id == id)
		{
			b->setBg(0xe6f4ffff);
			b->setHoverBg(0xe6f4ffff);
		}
	}
	curId = id;
	// 「几何图形」：矩形与圆形在这里是同一枚工具，画哪一类由子面板上那两枚类别小图标定。
	// 两类共用一套样式（颜色 / 线宽 / 填充）—— 并成一个工具之后，切类别还各带一份颜色
	// 的话，色板会跟着类别跳，看着就像"切个形状把颜色也弄丢了"
	if (curId == L"geom") {
		win->toolSub->showGeomTools();
	}
	else if (curId == L"arrow") {
		win->toolSub->showArrowTools();
	}
	else if (curId == L"number") {
		win->toolSub->showNumberTools();
	}
	else if (curId == L"line") {
		win->toolSub->showLineTools();
	}
	else if (curId == L"text") {
		win->toolSub->showTextTools();
	}
	else if (curId == L"mosaic") {
		win->toolSub->showMosaicTools();
	}
	else if (curId == L"eraser") {
		win->toolSub->showEraserTools();
	}
	else if (curId == L"pin") {
		win->toolSub->showPinTools();
	}
	else if (curId == L"watermark") {
		win->toolSub->showWatermarkTools();
		// 选了水印工具就把水印铺上，不再要求用户去点一下截图区域
		win->ensureWatermark();
	}
	else {
		// 兜底：curId 是空的（没选工具）、「选择画布」，或别的没面板的工具，把子工具条收起来。
		// 「选择画布」没有样式可调 —— 撤掉「选择对象」之后它连子面板也没有了
		win->toolSub->hideTools();
	}
	// curId 变化后 ToolMain 可能要上移给 ToolSub 腾位置，交给 WinPin 重新排布整组
	win->layoutTools();
}

void ToolMain::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}