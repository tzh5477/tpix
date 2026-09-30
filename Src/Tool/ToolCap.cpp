#include "pch.h"
#include "../Win/WinCap.h"
#include "../Setting.h"
#include "../Lang.h"
#include "../Tip.h"
#include "ToolCap.h"

// 三个表与 stageBtns / shapeBtns / actionBtns 的成员意义见头文件。
// 图标码位统一从 ToolMain 那边抄过来 —— 同一个工具在两级工具条上长一个样，
// 用户从截图界面直接点工具时才不会觉得是另一个东西。
const std::vector<ToolCap::BtnDef> ToolCap::stageBtns{
	{ L"long",    L"\ue73e", L"cap.long" },
	{ L"video",   L"\ue660", L"cap.video" },
	{ L"ocr",     L"\ue67b", L"cap.ocr" },
	{ L"qrcode",  L"\ue71e", L"cap.qrcode" },
};

const std::vector<ToolCap::BtnDef> ToolCap::shapeBtns{
	{ L"rect",    L"\ue8e8", L"tool.rect" },
	{ L"ellipse", L"\ue6bc", L"tool.ellipse" },
	{ L"arrow",   L"\ue603", L"tool.arrow" },
	{ L"number",  L"\ue776", L"tool.number" },
	{ L"line",    L"\ue601", L"tool.line" },
	{ L"text",    L"\ue6ec", L"tool.text" },
	{ L"mosaic",  L"\ue82e", L"tool.mosaic" },
	{ L"eraser",  L"\ue6be", L"tool.eraser" },
};

const std::vector<std::wstring> ToolCap::defaultShapeIds{
	L"rect", L"arrow", L"text", L"mosaic", L"number"
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
	// 按配置实际会摆出来的按钮算：主行是抬头的功能 + 启用的标注工具 + 分隔线 + 更多，
	// 第二行是那些动作按钮 + 被配置关掉的标注工具。窗口宽度取两行里更宽的那一行，
	// 否则展开时第二行会被压扁
	int mainCount = 0, extraCount = 0;
	computeRowCounts(mainCount, extraCount);
	auto mainW = mainCount * btnSize + spliterW + btnSize;
	auto extraW = extraCount * btnSize;
	setSize(std::max(mainW, extraW), expanded ? btnSize * 2.f : btnSize);
}

// 同一个统计串联了构造期（还没建节点，只要尺寸）和窗口尺寸刷新两处，
// 避免"按节点数算"和"按配置算"两边各写一份 —— 配置一侧改了另一处会漏
void ToolCap::computeRowCounts(int& mainCount, int& extraCount)
{
	extraCount = 0;
	auto setting = Setting::get();
	mainCount = (int)stageBtns.size();
	for (auto& def : shapeBtns)
	{
		bool onMain{ false };
		for (auto& id : defaultShapeIds) {
			if (id == def.id) { onMain = true; break; }
		}
		if (setting->getToolFlag(L"toolCap", def.id, onMain)) mainCount++;
		else extraCount++;
	}
	extraCount += (int)actionBtns.size();
}

ToolCap::~ToolCap()
{
}

void ToolCap::onCreated()
{
	tip = std::make_unique<Tip>(this);
	body->setBg(0xFFFFFFFF);
	body->setBorder(1.f, 0xA8A8A8ff);
	body->setFlexDirection(Ling::FlexDirection::Column);
	rowMain = body->makeChild<Ling::Node>();
	rowExtra = body->makeChild<Ling::Node>();
	for (auto row : { rowMain, rowExtra })
	{
		row->setAlignItems(Ling::Align::Center);
		row->setFlexDirection(Ling::FlexDirection::Row);
		row->setHeight(btnSize);
	}
	rowExtra->hide();

	auto setting = Setting::get();
	for (auto& def : stageBtns)
	{
		makeBtn(rowMain, def.id, def.code, def.tip);
	}
	// 标注工具：配置打开的主行进主行，其余连同 actionBtns 一起进第二行。
	// 配置键走 Setting::getToolFlag 的 toolCap 组，缺失时按 toolCap.defaultShapeIds 兜，
	// 所以老配置文件升级上来默认就是主行五项而不是八项塞满
	for (auto& def : shapeBtns)
	{
		bool onMain = false;
		for (auto& id : defaultShapeIds) {
			if (id == def.id) { onMain = true; break; }
		}
		if (setting->getToolFlag(L"toolCap", def.id, onMain)) {
			makeBtn(rowMain, def.id, def.code, def.tip);
		}
		else {
			makeBtn(rowExtra, def.id, def.code, def.tip);
		}
	}
	auto spliter = rowMain->makeChild<Ling::Node>();
	spliter->setSize(spliterW, 18.f);
	spliter->setBg(0xDDDDDDff);
	// "更多"没有对应的图标码位，用系统字体画一个 ≡。为此 makeBtn 得允许跳过图标字体，
	// 猜一个码位画不出来就是豆腐块，反倒不如一个确定存在的字符
	btnMore = makeBtn(rowMain, L"more", L"\u2261", L"cap.more");
	for (auto& def : actionBtns)
	{
		makeBtn(rowExtra, def.id, def.code, def.tip);
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
	if (id != L"more") {
		btn->setFontFamily(L"icon");
		btn->setFontSize(13.f);
	}
	else {
		// ≡ 在系统字体里，且笔画简单，放大到 16 才跟旁边的图标视觉分量相当
		btn->setFontSize(16.f);
	}
	btn->onClick.add([this](Ling::Button* btn) { onClick(btn); });
	if (!tipKey.empty()) {
		tip->bind(btn, Lang::get(tipKey));
	}
	return btn;
}

void ToolCap::setExpanded(const bool expanded)
{
	if (this->expanded == expanded) return;
	this->expanded = expanded;
	// hide/show 自带 refresh，但这里紧接着要改窗口尺寸，两次刷新合成一次前 getSize 都得是准的
	if (expanded) rowExtra->show(); else rowExtra->hide();
	refreshSize();
	// 窗口长高了，宿主那套"下方够不够、够就翻上方"的规则得重跑一遍
	win->layoutTool(this);
}

void ToolCap::onClick(Ling::Button* btn)
{
	// 提示框跟着按钮所在的窗口走，这里马上要换阶段或者关窗口，先把它收掉
	tip->hide();
	auto& id = btn->id;
	if (id == L"more") {
		setExpanded(!expanded);
		return;
	}
	else if (id == L"long") {
		win->startLong();
	}
	else if (id == L"video") {
		win->startVideo();
	}
	else if (id == L"ocr") {
		win->startOcr();
	}
	else if (id == L"qrcode") {
		win->startQrcode();
	}
	else if (id == L"save") {
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
		setExpanded(false);
		win->startPin(id);
	}
}

void ToolCap::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}
