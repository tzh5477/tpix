#include "pch.h"
#include <algorithm>
#include <thread>
#include "../Lang.h"
#include "../Ocr.h"
#include "../SelectPopup.h"
#include "../Setting.h"
#include "../ShotHistory.h"
#include "../Table.h"
#include "../Util.h"
#include "WinAiTrans.h"
#include "WinOcr.h"
#include "WinPin.h"

namespace {
	std::unique_ptr<WinOcr> winOcr;

	// 拖边框缩放时的最小尺寸（逻辑像素）。Ling 默认的最小跟踪尺寸是 800×600、
	// 比这个窗还大，必须放开；但也不能放到 1×1（原来就是 1）—— 缩成一条缝之后
	// 底下那排按钮会叠在一起，连右上角关窗那枚都点不到
	constexpr int kMinW{ 320 }, kMinH{ 240 };

	// 让模型看图识字用的三段提示词。它们是**给模型看的协议**，不是给用户看的文案，
	// 所以不进语言包 —— 换了界面语言也不该让识别结果的格式跟着变；
	// 表格那条要求输出 TSV，是因为粘贴要的就是它能直接落成一张表（见 parseTsvText）
	constexpr const wchar_t* promptSys{
		L"你是一个精确的 OCR 引擎，严格按用户的格式要求输出图片里的内容。" };
	constexpr const wchar_t* promptText{
		L"请完整识别这张图片里的文字，按原有的阅读顺序与分段输出，保留换行。"
		L"只输出识别到的原文，不要翻译、不要解释、不要加任何说明，也不要用代码块包裹。" };
	constexpr const wchar_t* promptTable{
		L"请识别这张图片里的表格，用制表符分隔的纯文本输出：第一行是表头，之后每行一条记录，"
		L"相邻单元格之间用一个制表符，行与行之间换行；单元格内部不要出现制表符与换行。"
		L"只输出表格内容，不要解释。" };

	// 把模型回的表格文本（行内 tab、行间换行）拆成 TableResult —— 这样同一份结果
	// 既能当纯文本复制，也能当成 HTML 表格粘进 Word / Excel（见 TableResult::toHtml）
	// 一个字都没认出来有两种：图里真没字，或者内置模型压根没准备好。
	// 后者不给提示的话用户只看到一句"没有识别到文字"，还以为这功能坏了没法修
	std::wstring modelErrText()
	{
		switch (Ocr::modelState()) {
		case OcrModelState::download: return Lang::get(L"ocr.modelDownload");
		case OcrModelState::noModel: return Lang::get(L"ocr.modelNoModel");
		case OcrModelState::badModel:
		case OcrModelState::runtime: return Lang::get(L"ocr.modelFail");
		default: return {};
		}
	}

	bool parseTsvText(const std::wstring& text, TableResult& out)
	{
		out.cols = 0;
		out.rows.clear();
		std::vector<std::vector<std::wstring>> rows;
		size_t pos{ 0 };
		while (pos <= text.size()) {
			const auto nl = text.find(L'\n', pos);
			auto line = text.substr(pos, nl == std::wstring::npos ? nl : nl - pos);
			while (!line.empty() && line.back() == L'\r') line.pop_back();
			// 整行都是空白就跳过：模型收尾常多打几个空行，留着会在表尾堆一串空记录
			if (line.find_first_not_of(L" \t") != std::wstring::npos) {
				std::vector<std::wstring> cells;
				size_t c{ 0 };
				while (true) {
					const auto tab = line.find(L'\t', c);
					cells.push_back(line.substr(c, tab == std::wstring::npos ? tab : tab - c));
					if (tab == std::wstring::npos) break;
					c = tab + 1;
				}
				rows.push_back(std::move(cells));
			}
			if (nl == std::wstring::npos) break;
			pos = nl + 1;
		}
		if (rows.empty()) return false;
		for (const auto& r : rows) out.cols = std::max(out.cols, static_cast<int>(r.size()));
		out.rows = std::move(rows);
		return true;
	}
}

WinOcr::WinOcr(std::vector<BYTE>&& data, const int w, const int h, WinPin* owner)
	: Ling::WinBase(), pixels(std::move(data)), imgW(w), imgH(h), pinOwner(owner)
{
	// 关窗按钮的点击栈上不能同步 reset（use-after-free），推迟到下一轮消息循环。
	// 捕获 this 而不是裸用：回调跑的时候这个对象可能已经不是当前那一个了（见 init 的存在性判断）
	auto self = this;
	onDestroy.add([self]() {
		Ling::App::get()->dq.TryEnqueue([self]() { if (winOcr.get() == self) winOcr.reset(); });
	});
	setTitle(Lang::get(L"ocr.title"));
	setSize(560.f, 420.f);
	setCenter();
	// 必须置顶：这个窗是"截图之后"弹出来的，此刻编辑页（WinPin）正占着最上层，
	// 不置顶就会被它整块盖住 —— 用户只能看见编辑页，以为识别结果没出来。
	// 与编辑页同一族（TOPMOST + TOOLWINDOW），谁后 show 谁在上面
	createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
}

WinOcr::~WinOcr()
{
	// 关窗时在飞的那次模型识别必须掐掉：postDone 是"取消也照送"的语义，
	// 光靠 alive 标记只能让它别去动节点，请求本身还在跑
	*alive = false;
	if (aiTask) aiTask->cancel();
	// 从贴图打开的这条：关掉识别窗，把藏在侧边栏的那张图放回来（会顺带钳回工作区）
	if (pinOwner) pinOwner->revealAfterOcr();
}

void WinOcr::init(std::vector<BYTE>&& data, const int w, const int h, WinPin* owner)
{
	if (winOcr) {
		// 句柄还在才谈"拉到前台"。对象活着、窗口却已经没了（销毁的收尾没跑到），
		// SetForegroundWindow 会落在失效句柄上，表现就是"点了没反应" —— 那就丢掉重建
		if (winOcr->hwnd && IsWindow(winOcr->hwnd)) {
			// 上一张主子还藏在侧边栏等着放回的话，先还给它，再认新主子 ——
			// 否则连着两张贴图先后开识别窗，前一张就永远留在侧边栏里了
			if (winOcr->pinOwner && winOcr->pinOwner != owner) winOcr->pinOwner->revealAfterOcr();
			winOcr->pinOwner = owner;
			winOcr->show();
			SetForegroundWindow(winOcr->hwnd);
			return;
		}
		winOcr.reset();
	}
	winOcr.reset(new WinOcr(std::move(data), w, h, owner));
	// 刚建出来的这一枚可能落在编辑页后面（两者都是 TOPMOST），明确再拉到最前一次
	if (winOcr->hwnd) {
		winOcr->show();
		SetForegroundWindow(winOcr->hwnd);
	}
}

void WinOcr::dispose()
{
	winOcr.reset();
}

void WinOcr::onCreated()
{
	enableShadow();
	body->setBg(0xFFFFFFFF);
	body->setFlexDirection(Ling::FlexDirection::Column);
	// 交叉轴（水平）方向拉满：内容区 box 不写死宽度、靠 stretch 撑满窗口（再减左右 margin），
	// 否则 Yoga 默认的 FlexStart 会让 box 缩成内容宽度，滚动条落到窗口中间
	body->setAlignItems(Ling::Align::Stretch);

	// 标题行：左边标题文字，右边关闭按钮。窗口是 WS_POPUP，系统标题栏不存在 ——
	// 没有这一行的话整个窗只有一块文本框，用户既拖不动它，也没有关它的办法
	auto titleRow = body->makeChild<Ling::Node>();
	titleRow->setHeight(36.f);
	titleRow->setWidthPercent(100.f);
	titleRow->setFlexDirection(Ling::FlexDirection::Row);
	titleRow->setAlignItems(Ling::Align::Center);
	// 标题区自带一层浅灰底，与白底的内容区拉开（下面再补一条分隔线）
	titleRow->setBg(0xF7F7F8FF);

	auto title = titleRow->makeChild<Ling::Label>();
	title->setText(Lang::get(L"ocr.title"));
	title->setPaddingLeft(12.f);
	// 字号与翻译窗的标题一致（14）。不给 Label 定高：定满整行的话 Label 内部
	// 文字是顶对齐的，Align::Center 就失效了 —— 标题会顶到窗口上沿、与右边的
	// × 按钮不在一条水平线上。让它按内容自适应，交给行容器的 alignItems 居中
	title->setFontSize(14.f);
	title->setColor(0x333333FF);
	title->setFlexGrow(1.f);

	// 绝对定位到右上角：不占标题行的横向空间，也不会把标题文字挤窄
	auto closeBtn = titleRow->makeChild<Ling::Button>();
	closeBtn->setSize(42.f, 32.f);
	closeBtn->setPositionType(Ling::Position::Absolute);
	closeBtn->setPosition(Ling::Edge::Right, 0);
	closeBtn->setPosition(Ling::Edge::Top, 2);
	closeBtn->setHoverColor(0xFFFFFFFF);
	closeBtn->setHoverBg(0xE81123FF);
	closeBtn->setText(L"\ue62d");
	closeBtn->setFontFamily(L"icon");
	closeBtn->onClick.add([](Ling::Button* btn) { btn->win->close(); });

	// 标题区与内容区之间的分隔线
	auto titleLine = body->makeChild<Ling::Node>();
	titleLine->setHeight(1.f);
	titleLine->setWidthPercent(100.f);
	titleLine->setBg(0xE0E0E0FF);

	// ⚠️ TextBox 构造函数里写死了 setWidth(240)，显式宽度优先级高于 alignItems 的
	// stretch —— 直接放进 body 拉不开，灰底只有 240 逻辑像素宽（看着就是"背景只有一半"）。
	// 所以套一层没有显式宽度的容器：容器靠 stretch 撑满（body 已设 alignItems:Stretch），
	// 文本框再按容器宽的百分比铺满 —— 百分比落在容器上，叠外边距也不会溢出窗口右边
	auto boxWrap = body->makeChild<Ling::Node>();
	boxWrap->setFlexGrow(1.f);
	boxWrap->setMargin(12.f, 10.f, 12.f, 10.f);

	box = boxWrap->makeChild<Ling::TextBox>();
	box->setWidthPercent(100.f);
	box->setFlexGrow(1.f);
	box->setFontSize(14.f);
	box->setPadding(12.f, 12.f, 12.f, 12.f);
	box->setText(Lang::get(L"ocr.recognizing"));
	// 内容区自成一格：铺一层浅灰底、圆角，与白底的标题区 / 按钮区拉开。
	// 用底色而不用描边做区分，与翻译窗的译文区同一套做法。TextBox 本来就是可编辑
	// 控件（点进去改、Ctrl+C/V/X/A、IME 都在），识别结果直接就能二次加工
	box->setBg(0xF7F7F9FF);
	box->setBorderRadius(6.f);

	// 内容区与按钮区之间的分隔线
	auto bottomLine = body->makeChild<Ling::Node>();
	bottomLine->setHeight(1.f);
	bottomLine->setWidthPercent(100.f);
	bottomLine->setBg(0xE0E0E0FF);

	auto bottom = body->makeChild<Ling::Node>();
	bottom->setHeight(48.f);
	bottom->setWidthPercent(100.f);
	bottom->setFlexDirection(Ling::FlexDirection::Row);
	bottom->setAlignItems(Ling::Align::Center);
	// 按钮区与标题区同用一层浅灰底：上中下三段一眼分明
	bottom->setBg(0xF7F7F8FF);
	bottom->setPaddingLeft(12.f);
	bottom->setPaddingRight(12.f);

	// 语言按钮。系统装了几种就多几个选项，一个都没装时它只显示一句提示、点了没反应。
	// ⚠️ 它管的是系统离线 OCR 用哪个语言包 —— 切到大模型那条路之后它不起作用，直接藏掉
	langs = Ocr::languages();
	langBtn = bottom->makeChild<Ling::Button>();
	langBtn->setHeight(30.f);
	langBtn->setWidth(120.f);
	langBtn->setBorder(1.f, 0xE0E0E0FF);
	langBtn->setHoverBg(0xF2F2F2FF);
	langBtn->onClick.add([this](Ling::Button* b) {
		if (langs.empty()) return;
		// 第 0 项是跟随系统，之后依次是装了的每个语言包
		std::vector<std::wstring> items{ Lang::get(L"ocr.langAuto") };
		for (auto const& lang : langs) items.push_back(lang.name);
		SelectPopup::show(this, b, items, langIndex, [this](int idx) {
			langIndex = idx;
			Setting::get()->setToolStr(L"ocr", L"lang", curLangTag());
			applyLangBtn();
			startRecognize();
		});
	});
	// 记住上次选的；没装的语言包（卸载了）认不出来，退回跟随系统
	auto saved = Setting::get()->getToolStr(L"ocr", L"lang", L"");
	if (!saved.empty()) {
		for (int i = 0; i < static_cast<int>(langs.size()); ++i) {
			if (langs[i].tag == saved) { langIndex = i + 1; break; }
		}
	}
	applyLangBtn();

	// 识别模型。**本地离线模型每一代 + 本场景（文字 / 表格）在设置里绑的那一个大模型**，
	// 显示的都是具体名字（PP-OCRv6-small / PP-OCRv6-tiny / agnes-2.5-pro …）：
	// 用户的实际诉求是"这个模型认得不准，换一个再认一遍"，笼统的
	// "系统识别 / 大模型识别"两档给不出"换哪一个"的答案。
	// 大模型只列绑定的那一个 —— 换接口 / 换模型是设置页的事，这里只切换引擎
	engineBtn = bottom->makeChild<Ling::Button>();
	engineBtn->setHeight(30.f);
	// 按钮上显示当前用的那个模型名，名字可能较长（agnes-image-2.5-flash 有 22 字符），
	// 放宽到 150 —— 再宽就会把右边的"复制"挤到窗口外（见下面那排宽度的配平）
	engineBtn->setWidth(150.f);
	engineBtn->setFontSize(13.f);
	engineBtn->setBorder(1.f, 0xE0E0E0FF);
	engineBtn->setHoverBg(0xF2F2F2FF);
	engineBtn->onClick.add([this](Ling::Button* b) {
		auto choices = Ocr::modelChoices(curScenario());
		std::vector<std::wstring> labels;
		labels.reserve(choices.size());
		for (auto& c : choices) labels.push_back(c.label);
		// 选中的下标按 choices 算（-1 = 都不匹配），下标会原样回给 onPick
		SelectPopup::show(this, b, labels, curChoiceIndex(choices), [this, choices](int idx) {
			if (idx < 0 || idx >= (int)choices.size()) return;
			const auto& c = choices[idx];
			if (c.ai) {
				aiMode = true;
				Setting::get()->setToolStr(L"ocr", L"engine", L"ai");
				// 只改**当前场景**（文字 / 表格）绑的模型：另一档是用户单独配的，别顺手覆盖
				Setting::get()->setScenario(curScenario(), c.providerId, c.model);
			}
			else {
				aiMode = false;
				Setting::get()->setToolStr(L"ocr", L"engine", L"sys");
				Ocr::setModel(c.model);
			}
			applyEngineBtn();
			// 换完立刻用新模型重认一次 —— "换一个模型再试试"就是这个动作
			startRecognize();
		}, {}, 220.f);
	});
	aiMode = Setting::get()->getToolStr(L"ocr", L"engine", L"sys") == L"ai";
	applyEngineBtn();

	// 表格按钮：在"整页文字"和"按格子出表"之间切。切完立刻重认一次
	tableBtn = bottom->makeChild<Ling::Button>();
	tableBtn->setHeight(30.f);
	tableBtn->setWidth(90.f);
	tableBtn->setBorder(1.f, 0xE0E0E0FF);
	tableBtn->setHoverBg(0xF2F2F2FF);
	// 只有"整页文字 / 按格子出表"两种，单击即切换。按钮上的字由 applyTableBtn 跟着状态改
	tableBtn->onClick.add([this](Ling::Button*) {
		tableMode = !tableMode;
		applyTableBtn();
		// 文字与表格是两个场景，各自可能绑不同的大模型 —— 引擎按钮上的名字得跟着换
		//（本地模型没这回事，但统一走一次最省心）
		applyEngineBtn();
		startRecognize();
	});
	applyTableBtn();

	// 翻译：把刚认出来的文字交给翻译窗，顺带立刻翻一次。
	// 这是本应用做"截图翻译"最短的一条路 —— 文字已经在手上了，不必再去模拟 Ctrl+C
	// 抓别人程序的选区（那种做法在浏览器里经常取不到东西）
	auto transBtn = bottom->makeChild<Ling::Button>();
	transBtn->setText(Lang::get(L"ocr.trans"));
	transBtn->setHeight(30.f);
	transBtn->setWidth(70.f);
	transBtn->setBorder(1.f, 0xE0E0E0FF);
	transBtn->setHoverBg(0xF2F2F2FF);
	transBtn->onClick.add([this](Ling::Button*) {
		if (result.empty()) return;
		// 以文本框里当下的内容为准：识别结果支持二次编辑，改完再点翻译，翻的是改后的。
		// 翻译窗置顶弹出后把识别窗自己藏起来 —— 两个置顶窗叠着只会互相挡内容，
		// 关掉翻译窗时（WinAiTrans 的 owner 回调）识别窗会自己回来
		auto text = box->getText();
		if (text.empty()) return;
		WinAiTrans::init(text, this);
		hide();
	});

	auto spacer = bottom->makeChild<Ling::Node>();
	spacer->setFlexGrow(1.f);

	auto copyBtn = bottom->makeChild<Ling::Button>();
	copyBtn->setText(Lang::get(L"ocr.copy"));
	copyBtn->setHeight(30.f);
	copyBtn->setWidth(90.f);
	copyBtn->setBorder(1.f, 0xE0E0E0FF);
	copyBtn->setHoverBg(0xF2F2F2FF);
	copyBtn->onClick.add([this](Ling::Button*) {
		if (result.empty()) return;
		auto text = box->getText();
		if (text.empty()) return;
		// 内容没动过且带着表格 HTML 才走 HTML 那条（粘进 Word / Excel 是真的一张表）；
		// 编辑过就以框里的内容为准出纯文本 —— 那份 HTML 是对着识别原文编的，改完就对不上了
		if (text == result && !html.empty()) Util::setHtmlToClipboard(html, result);
		else Ling::Util::setTextToClipboard(text);
		ShotHistory::get()->skipNextClipboard();
	});

	show();
	startRecognize();
}

// 标题行（顶 36 逻辑像素内）当拖拽区。WS_POPUP 没有系统标题栏，不给这一段
// HTCAPTION 的话用户按不住窗口 —— 识别结果可能很长，窗口得能拖到别处看
LRESULT WinOcr::onHitTest(const POINT pos)
{
	POINT pt = pos;
	ScreenToClient(hwnd, &pt);
	// 四条边 / 四个角交给系统做窗口缩放（拖边框改大小）：识别结果一长就得一直滚，
	// 窗口能拉大才读得顺（作者报的"无法拖动"）。判据与翻译窗同款，且**放在标题栏之前** ——
	// 标题行只有 36 逻辑像素高，四角那 4px 缩放带落在它的范围里，反过来的话
	// 右上角会被判成"拖窗"，边框反而抓不住
	auto border = borderHitTest(pt);
	if (border != HTCLIENT) return border;
	// 右边 42 留给关闭按钮：那一块要按成 HTCLIENT，否则点 × 会变成拖窗口，点不掉
	if (pt.y > 0 && pt.y < 36 * dpi && pt.x < w - 42 * dpi) return HTCAPTION;
	return HTCLIENT;
}

void WinOcr::onMinMaxInfo(MINMAXINFO* mmi)
{
	// 同 ToolMain / ToolSub / WinPin：Ling 默认的最小跟踪尺寸是 800×600，
	// 比这个窗大得多，不放开的话 setSize 会被系统按回去。
	// 放开的下限取 kMinW/kMinH，不是"随便缩"—— 再小按钮行就挤成一团了
	mmi->ptMinTrackSize.x = (LONG)(kMinW * dpi);
	mmi->ptMinTrackSize.y = (LONG)(kMinH * dpi);
}

std::wstring WinOcr::curLangTag() const
{
	// 0 号是空标签：让 Ocr 按系统语言档案自己挑
	return langIndex == 0 ? std::wstring{} : langs[langIndex - 1].tag;
}

void WinOcr::applyLangBtn()
{
	if (!langBtn) return;
	if (langs.empty()) {
		langBtn->setText(Lang::get(L"ocr.notInstalled"));
		return;
	}
	langBtn->setText(langIndex == 0 ? Lang::get(L"ocr.langAuto") : langs[langIndex - 1].name);
}

void WinOcr::startRecognize()
{
	// 走大模型那条路：系统离线 OCR 那套线程 / 语言包一概用不上
	if (aiMode) {
		startAiRecognize();
		return;
	}
	// 从大模型切回来时，上一次可能还在飞 —— 掐掉它，否则它的收尾会盖住这一次的结果
	if (aiTask) {
		aiTask->cancel();
		aiTask.reset();
	}
	const auto seq = ++taskSeq;
	auto lang = curLangTag();
	// 模式在这里定格：线程跑到一半用户可能又点了切换，那边不能再去读成员
	const auto asTable = tableMode;
	box->setText(Lang::get(L"ocr.recognizing"));
	// 往线程里拷一份而不是搬走：换语言会再识别一次，pixels 得一直留在成员里。
	// 线程里只拿 this 比地址、不 dereference —— 识别跑到一半窗口可能已经关了
	std::thread([this, data = pixels, w = imgW, h = imgH, lang = std::move(lang), seq, asTable]() mutable {
		// 新线程里没有 WinRT 单元，不初始化就用不了 OcrEngine
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
		std::wstring text;
		TableResult table;
		std::wstring err;
		bool ok{ false };
		if (asTable) ok = Table::recognize(w, h, data.data(), lang, table, err);
		else {
			text = Ocr::recognize(w, h, data.data(), lang);
			// 认不出来就是空，不算失败；但模型没准备好要说清是哪一种
			if (text.empty()) err = modelErrText();
			ok = true;
		}
		Ling::App::get()->dq.TryEnqueue(
			[this, seq, asTable, ok, text = std::move(text), table = std::move(table), err = std::move(err)]() mutable {
				// 排在自己前面的可能正是"窗口已关闭"那次 reset，那之后 winOcr 已经不是本窗口了；
				// 也可能有更新的识别已经出结果，老结果不该盖上去
				if (winOcr.get() != this || taskSeq != seq) return;
				if (!ok) {
					result.clear();
					html.clear();
					box->setText(err.empty() ? Lang::get(L"ocr.empty") : err);
					return;
				}
				if (asTable) setTable(table);
				else if (!err.empty()) {
					// 空结果 + 模型没准备好：把原因摆出来，别只留一句"没有识别到文字"
					result.clear();
					html.clear();
					box->setText(err);
				}
				else setResult(text);
			});
	}).detach();
}

void WinOcr::startAiRecognize()
{
	const auto seq = ++taskSeq;
	// 模式在这里定格：请求飞这几秒里用户可能又切了表格 / 切回系统 OCR
	const auto asTable = tableMode;
	if (aiTask) {
		aiTask->cancel();
		aiTask.reset();
	}
	// 文字与表格是两个场景 —— 需求允许它们各绑不同的接口与模型
	//（翻译可以用 A 家的某某模型，表格识别用 B 家的另一个）
	const auto scenario = asTable ? std::wstring{ AiScenario::table }
		: std::wstring{ AiScenario::recognize };
	auto cred = Setting::get()->credFor(scenario);
	if (!cred.ok) {
		result.clear();
		html.clear();
		box->setText(Lang::get(L"ai.noKey"));
		return;
	}
	box->setText(Lang::get(L"ocr.recognizing"));

	std::vector<AiService::Msg> msgs;
	AiService::Msg sys;
	sys.role = AiService::Role::System;
	sys.content = promptSys;
	AiService::Msg user;
	user.role = AiService::Role::User;
	user.content = asTable ? promptTable : promptText;
	// 图直接走对话窗贴图那条已经做好的通道：AiService::Msg 的 image 会被编 PNG、
	// 转 base64，落成 image_url 的 data URL —— 这里一个字节都不用新写
	user.image = pixels;
	user.imgW = imgW;
	user.imgH = imgH;
	msgs.push_back(std::move(sys));
	msgs.push_back(std::move(user));

	// 流式一段段来，收尾时才一次性给出去 —— 这里要的只是"最后那一整段"
	auto acc = std::make_shared<std::wstring>();
	auto aliveFlag = alive;
	aiTask = AiService::chat(cred, msgs,
		[acc](const std::wstring& delta) { *acc += delta; },
		[this, aliveFlag, seq, asTable, acc](const std::wstring& err) {
			// 窗口已经被销毁（回调排在析构之后），或者已经有更新的识别出了结果 ——
			// 老结果都不能再盖上去（同上面离线那条线程）
			if (!*aliveFlag || winOcr.get() != this || taskSeq != seq) return;
			if (!err.empty() && acc->empty()) {
				result.clear();
				html.clear();
				box->setText(err);
				return;
			}
			// 模型给的东西首尾常带空白（粘了段说明、多打了几个换行），剥干净再用
			while (!acc->empty() && (acc->front() == L'\n' || acc->front() == L' ')) acc->erase(0, 1);
			while (!acc->empty() && (acc->back() == L'\n' || acc->back() == L' ')) acc->pop_back();
			if (asTable) {
				TableResult table;
				// 编排得出表格就走 setTable（顺带把那份 HTML 带上，粘进 Excel 是真的一张表）；
				// 模型没按 TSV 出来就把原文摆回去 —— 那也比一句"没找到表格"有用
				if (parseTsvText(*acc, table)) {
					setTable(table);
					return;
				}
			}
			setResult(*acc);
		});
}

std::wstring WinOcr::curScenario() const
{
	// 表格开关是唯一的分档依据：开就认表格、关就认整页文字。两者在设置里是两套
	// 独立的"接口 + 模型"绑定（AiScenario::table / recognize）
	return tableMode ? std::wstring{ AiScenario::table } : std::wstring{ AiScenario::recognize };
}

int WinOcr::curChoiceIndex(const std::vector<OcrModelChoice>& choices) const
{
	if (aiMode) {
		const auto scenario = curScenario();
		const auto pid = Setting::get()->getScenarioProvider(scenario);
		const auto model = Setting::get()->getScenarioModel(scenario);
		for (int i = 0; i < (int)choices.size(); ++i) {
			if (choices[i].ai && choices[i].providerId == pid && choices[i].model == model) return i;
		}
		// 绑的那个接口已经被删了之类的：光按模型名再找一遍，总比不高亮强
		for (int i = 0; i < (int)choices.size(); ++i) {
			if (choices[i].ai && choices[i].model == model) return i;
		}
		return -1;
	}
	const auto cur = Ocr::curModel();
	for (int i = 0; i < (int)choices.size(); ++i) {
		if (!choices[i].ai && choices[i].model == cur) return i;
	}
	return -1;
}

void WinOcr::applyEngineBtn()
{
	if (!engineBtn) return;
	// 按钮上直接显示"当前用的是哪个模型"，而不是笼统的"系统识别 / 大模型识别"：
	// 本地离线那几代显示具体代次（PP-OCRv6-small / PP-OCRv6-tiny …）；
	// 大模型显示**当前场景**（文字 / 表格）绑的那个模型名。没配置大模型时回落到通用文案
	if (aiMode) {
		auto m = Setting::get()->getScenarioModel(curScenario());
		engineBtn->setText(m.empty() ? Lang::get(L"ocr.engineAi") : m);
	}
	else {
		engineBtn->setText(Ocr::modelLabel(Ocr::curModel()));
	}
	// 语言包是系统离线 OCR 才有的概念：大模型用不上，内置模型（PP-OCRv6）是
	// 多语种的也不用挑 —— 留着会让人以为它在生效
	if (langBtn) {
		if (aiMode || Ocr::engine() != OcrEngine::System) langBtn->hide();
		else langBtn->show();
	}
}

void WinOcr::setResult(const std::wstring& text)
{
	html.clear();
	result = text;
	box->setText(text.empty() ? Lang::get(L"ocr.empty") : text);
}

void WinOcr::setTable(const TableResult& table)
{
	html = table.toHtml();
	result = table.toTsv();
	// 一个字都没填进去时别显示一片空白的制表符，那看着像坏了
	if (result.find_first_not_of(L"\t\r\n") == std::wstring::npos) {
		html.clear();
		result.clear();
	}
	box->setText(result.empty() ? Lang::get(L"ocr.empty") : result);
}

void WinOcr::applyTableBtn()
{
	if (!tableBtn) return;
	tableBtn->setText(tableMode ? Lang::get(L"ocr.text") : Lang::get(L"ocr.table"));
}
