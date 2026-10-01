#include "pch.h"
#include "../Win/WinCap.h"
#include "../Lang.h"
#include "../Tip.h"
#include "ToolCap.h"

// 两个表与 shapeBtns / actionBtns 的成员意义见头文件。
// 图标码位统一从 ToolMain 那边抄过来 —— 同一个工具在两级工具条上长一个样，
// 用户从截图界面直接点工具时才不会觉得是另一个东西。
const std::vector<ToolCap::BtnDef> ToolCap::shapeBtns{
	{ L"rect",    L"\ue8e8", L"tool.rect" },
	{ L"ellipse", L"\ue6bc", L"tool.ellipse" },
	{ L"arrow",   L"\ue603", L"tool.arrow" },
	{ L"number",  L"\ue776", L"tool.number" },
	{ L"line",    L"\ue601", L"tool.line" },
	{ L"text",    L"\ue6ec", L"tool.text" },
	{ L"mosaic",  L"\ue82e", L"tool.mosaic" },
	{ L"eraser",  L"\ue6be", L"tool.eraser" },
	{ L"watermark", L"\ue607", L"tool.watermark" },
};

const std::vector<ToolCap::BtnDef> ToolCap::actionBtns{
	{ L"mark",      L"\ue97f", L"cap.mark" },
	{ L"save",      L"\ue608", L"tool.save" },
	{ L"clipboard", L"\ue6ad", L"tool.clipboard" },
	{ L"close",     L"\ue62d", L"tool.close" },
};

ToolCap::ToolCap(WinCap* win) : Ling::WinBase(), win(win)
{
	// 跟着宿主窗口的缩放走：WinBase 构造里取的是系统 dpi，宿主可能在另一块缩放比例不同的屏上
	dpi = win->dpi;
	// 位置由 WinCap::layoutTool() 在 createNativeWindow 之前设好，这里只算尺寸
	refreshSize();
	// 点按钮会把 ToolCap 激活，键盘消息进的是它，转发给 WinCap 让 ESC 一致生效
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
		this->win->layoutTool(this);
	});
}

void ToolCap::refreshSize()
{
	// 一行到底：标注工具 + 分隔线 + 动作按钮。没有第二行了，也就不必再取两行里更宽的那一行
	auto count = (float)(shapeBtns.size() + actionBtns.size());
	setSize(btnSize * count + spliterW, btnSize);
}

ToolCap::~ToolCap()
{
}

void ToolCap::onCreated()
{
	tip = std::make_unique<Tip>(this);
	body->setBg(0xFFFFFFFF);
	body->setBorder(1.f, 0xA8A8A8ff);
	body->setFlexDirection(Ling::FlexDirection::Row);
	body->setAlignItems(Ling::Align::Center);

	// 标注工具全部平铺，不再按配置分主次 —— "更多"折叠去掉之后，开关也就没有意义了
	for (auto& def : shapeBtns)
	{
		makeBtn(body, def.id, def.code, def.tip);
	}
	// 分隔线留着：左边是"往图上加东西"，右边是"拿这张图怎么办"
	auto spliter = body->makeChild<Ling::Node>();
	spliter->setSize(spliterW, 18.f);
	spliter->setBg(0xDDDDDDff);
	for (auto& def : actionBtns)
	{
		makeBtn(body, def.id, def.code, def.tip);
	}
	refreshSize();
	show();
}

Ling::Button* ToolCap::makeBtn(Ling::Node* parent, const std::wstring& id,
	const std::wstring& code, const std::wstring& tipKey)
{
	auto btn = parent->makeChild<Ling::Button>();
	btn->setId(id);
	btn->setText(code);
	btn->setWidth(btnSize);
	btn->setHeightPercent(100.f);
	btn->setHoverBg(0xF2F2F2ff);
	btn->setFontFamily(L"icon");
	btn->setFontSize(13.f);
	btn->onClick.add([this](Ling::Button* btn) { onClick(btn); });
	if (!tipKey.empty()) {
		tip->bind(btn, Lang::get(tipKey));
	}
	return btn;
}

void ToolCap::onClick(Ling::Button* btn)
{
	// 提示框跟着按钮所在的窗口走，这里马上要换阶段或者关窗口，先把它收掉
	tip->hide();
	auto& id = btn->id;
	if (id == L"save") {
		win->saveToFile();
	}
	else if (id == L"clipboard") {
		win->copyToClipboard();
	}
	else if (id == L"close") {
		win->close();
	}
	else if (id == L"mark") {
		win->startPin();
	}
	else {
		// 剩下的都是标注工具：进贴图窗口并预选它，等于替用户点了图像标记 + 那个工具
		win->startPin(id);
	}
}

void ToolCap::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}
