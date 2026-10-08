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

namespace {
	std::unique_ptr<WinOcr> winOcr;

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

WinOcr::WinOcr(std::vector<BYTE>&& data, const int w, const int h)
	: Ling::WinBase(), pixels(std::move(data)), imgW(w), imgH(h)
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
	createNativeWindow();
}

WinOcr::~WinOcr()
{
	// 关窗时在飞的那次模型识别必须掐掉：postDone 是"取消也照送"的语义，
	// 光靠 alive 标记只能让它别去动节点，请求本身还在跑
	*alive = false;
	if (aiTask) aiTask->cancel();
}

void WinOcr::init(std::vector<BYTE>&& data, const int w, const int h)
{
	if (winOcr) {
		SetForegroundWindow(winOcr->hwnd);
		return;
	}
	winOcr.reset(new WinOcr(std::move(data), w, h));
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

	// 标题行：左边标题文字，右边关闭按钮。窗口是 WS_POPUP，系统标题栏不存在 ——
	// 没有这一行的话整个窗只有一块文本框，用户既拖不动它，也没有关它的办法
	auto titleRow = body->makeChild<Ling::Node>();
	titleRow->setHeight(36.f);
	titleRow->setWidthPercent(100.f);
	titleRow->setFlexDirection(Ling::FlexDirection::Row);
	titleRow->setAlignItems(Ling::Align::Center);

	auto title = titleRow->makeChild<Ling::Label>();
	title->setText(Lang::get(L"ocr.title"));
	title->setHeightPercent(100.f);
	title->setPaddingLeft(12.f);
	title->setFontSize(13.f);
	title->setColor(0x333333FF);
	title->setFlexGrow(1.f);
	title->setJustifyContent(Ling::Justify::Start);

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

	box = body->makeChild<Ling::TextBox>();
	box->setFlexGrow(1.f);
	box->setWidthPercent(100.f);
	box->setFontSize(14.f);
	box->setPadding(12.f, 12.f, 12.f, 12.f);
	box->setText(Lang::get(L"ocr.recognizing"));

	auto bottom = body->makeChild<Ling::Node>();
	bottom->setHeight(48.f);
	bottom->setWidthPercent(100.f);
	bottom->setFlexDirection(Ling::FlexDirection::Row);
	bottom->setAlignItems(Ling::Align::Center);
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

	// 识别引擎。系统离线 OCR 与大模型两条路并存：前者不联网不花钱，后者认得下的照片、
	// 艺术字更好，也能直接把表格认出来。切完立刻重认一次（与语言、表格开关一致）
	engineBtn = bottom->makeChild<Ling::Button>();
	engineBtn->setHeight(30.f);
	engineBtn->setWidth(104.f);
	engineBtn->setBorder(1.f, 0xE0E0E0FF);
	engineBtn->setHoverBg(0xF2F2F2FF);
	engineBtn->onClick.add([this](Ling::Button* b) {
		std::vector<std::wstring> items{ Lang::get(L"ocr.engineSys"), Lang::get(L"ocr.engineAi") };
		SelectPopup::show(this, b, items, aiMode ? 1 : 0, [this](int idx) {
			aiMode = idx == 1;
			Setting::get()->setToolStr(L"ocr", L"engine", aiMode ? L"ai" : L"sys");
			applyEngineBtn();
			startRecognize();
		});
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
		WinAiTrans::init(result);
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
		if (html.empty()) Ling::Util::setTextToClipboard(result);
		else Util::setHtmlToClipboard(html, result);
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
	// 右边 42 留给关闭按钮：那一块要按成 HTCLIENT，否则点 × 会变成拖窗口，点不掉
	if (pt.y > 0 && pt.y < 36 * dpi && pt.x < w - 42 * dpi) return HTCAPTION;
	return HTCLIENT;
}

void WinOcr::onMinMaxInfo(MINMAXINFO* mmi)
{
	// 同 ToolMain / ToolSub / WinPin：Ling 默认的最小跟踪尺寸是 800×600，
	// 比这个窗大得多，不放开的话 setSize 会被系统按回去
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
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
			ok = true;   // 文字识别认不出来就是空，不算失败
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

void WinOcr::applyEngineBtn()
{
	if (!engineBtn) return;
	engineBtn->setText(aiMode ? Lang::get(L"ocr.engineAi") : Lang::get(L"ocr.engineSys"));
	// 语言包是系统离线 OCR 才有的概念，大模型那条路用不上 —— 留着会让人以为它在生效
	if (langBtn) {
		if (aiMode) langBtn->hide();
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
