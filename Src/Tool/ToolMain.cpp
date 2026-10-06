#include "pch.h"
#include "../Win/WinPin.h"
#include "../History.h"
#include "../Lang.h"
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
			// 「选择对象」那一枚不写字：垫一张铺满的画布，自己在上面画一个鼠标指针
			//（图标字体里没有指针形状，41 个码位全都有主；见 layout / paintSelectIcon）。
			// 其余各枚照旧走 icon 字体
			if (id == L"select") {
				auto icon = btn->makeChild<Ling::Canvas>();
				icon->setSizePercent(100.f, 100.f);
				icon->setFlexShrink(0.f);
				selectIcon = icon;
			}
			else {
				btn->setText(btnCodes[i]);
				btn->setFontFamily(L"icon");
				btn->setFontSize(13.f);
			}
			btn->onClick.add([this](Ling::Button* btn) {onClick(btn);});
			tip->bind(btn, Lang::get(std::format(L"tool.{}", id)));
			btns.push_back(btn);
		}
	}
	show();
}

void ToolMain::layout()
{
	Ling::WinBase::layout();
	if (!selectIcon) return;
	// 与 ToolSub 的样例格子同一套：startPaint / finishPaint 要成对用，
	// 且这一层底子是透明的 —— 按钮的常态 / 悬停底色从它下面透上来
	auto ctx = selectIcon->startPaint();
	if (!ctx) return;
	ctx->Clear(0);
	paintSelectIcon(ctx, selectIcon->w, selectIcon->h);
	selectIcon->finishPaint();
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
	// 上面那串点自己的外接尺寸（0.529 × 0.849）；pad 是描边要占的边，
	// 留窄了指针会贴着按钮边缘，描边被切掉半个像素
	constexpr float bw{ 0.529f }, bh{ 0.849f };
	const float pad{ 5.f * dpi };
	const float sx{ (w - pad * 2.f) / bw }, sy{ (h - pad * 2.f) / bh };
	const float s{ sx < sy ? sx : sy };
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
	if (curId.empty()) return;
	for (auto b : btns)
	{
		if (b->id == curId) {
			applyNormalStyle(b);
		}
	}
	curId.clear();
	// 选择模式跟着一起退出：右键收工具条、ESC 退一步、锁定贴图都走到这儿，
	// 那几种情况下"还在挑元素"没有意义（「选」那枚的底色也一并复位了）
	win->selectMode = false;
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
	// 「选择对象」是个模式，只在用户按工具条这一处开 / 关：按它进模式，按别的工具出模式。
	// 程序内部按元素换工具不走这里（见 WinPin::onUp），所以"点中元素后面板跟着换"不会
	// 把模式关掉。出模式时顺手把「选」那枚的底色复位
	if (win->selectMode && btn->id != L"select") setToggle(L"select", false);
	win->selectMode = (btn->id == L"select");
	// 再次点击已选中的按钮 = 取消选中（开关式）。cancelSelect 里已经做了配色复位、
	// 隐藏 ToolSub 和重排，这里直接返回，不要再往下走选中流程。
	if (btn->id == curId) {
		cancelSelect();
		return;
	}
	selectTool(btn->id);
}

void ToolMain::selectTool(const std::wstring& id)
{
	// 拿起任何一种标注工具都先退出选文态：两套手势都要吃左键，同时开着必然有一套点不动
	//（选文那一套排在前面，剪裁框就再也拉不起来了）。已经是关的时候就什么都不做
	win->setTextSelect(false);
	for (auto b : btns)
	{
		// 选择模式下「选」那枚一直亮着：它表示"鼠标现在是在挑元素"，与"面板此刻显示
		// 哪个工具的属性"是两件事。点中元素后 curId 换成了那个元素的工具，但模式没退
		//（见 WinPin::selectMode），把它一起按常态复位就看着像模式掉了
		if (b->id == curId && !(win->selectMode && b->id == L"select"))
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
	if (curId == L"rect") {
		win->toolSub->showRectTools();
	}
	else if (curId == L"ellipse") {
		win->toolSub->showEllipseTools();
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
		// 「选择对象」落在这儿：它没有"样式"可调 —— 点中的是哪个元素，样式就切到那个元素的
		// 工具上去（见 WinPin::onUp）。框选出来的那一批也不接受批量改样式，
		// 它们只用于整批高亮 + Delete 一次删掉
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