#include "pch.h"
#include <shobjidl.h>
#include <algorithm>
#include <cstddef>
#include <ctime>
#include "../AiHistory.h"
#include "../AiService.h"
#include "../Lang.h"
#include "../Markdown.h"
#include "../SelectPopup.h"
#include "../Setting.h"
#include "../Util.h"
#include "WinAiChat.h"

namespace {
	std::unique_ptr<WinAiChat> winAiChat;
	// 模型下拉的最小宽度（逻辑像素）。"接口名 - 模型名"比那枚按钮宽得多，
	// 不给下限的话列表就按按钮宽度定，名字全被截断
	constexpr float modelPopupMinW{ 240.f };
	// 侧栏最多列多少条。超出就只显示最近的这些 —— 更老的那批靠"保留天数 / 条数"自己清掉
	constexpr int maxSessions{ 12 };
	// "这一条还没进 msgs"：流式输出中的占位气泡，以及没配好时的提示
	constexpr size_t noIndex{ static_cast<size_t>(-1) };
	// 输入区高低的上下限（逻辑像素）
	constexpr float minInputH{ 110.f }, maxInputH{ 420.f };
	// 侧栏那一行里名字最多显示几个字。不截的话文字会溢出（Label 既不折行也不会省略号）。
	// 日期挪到名字下面之后这一行整宽都归名字，140 逻辑像素 ÷ 12 号字 ≈ 11 个汉字，
	// 取 10 留一点余量（还要减 6 的左内边距）
	constexpr size_t nameMax{ 10 };
	// 用户问题那一条的宽度上限（父容器宽度的百分比）。气泡随内容自适应，只封顶 ——
	// 短问题气泡是短的，长问题才铺到这个宽度。回答条不吃这一项：它顶左、铺满整宽
	constexpr float bubbleMaxPct{ 78.f };
	// 消息区四周的留白（逻辑像素）。右侧还要再让开滚动条那一条竖带（见 applyMsgPadding）——
	// 只让开滚动条、本身留白太小的话，正文看着就像被右边那条线切掉了一块
	constexpr float msgPad{ 16.f };
	// 一个月按 30 天算，只用来说明"多久算最近"，不必精确到日历月
	constexpr long long recentMs{ 30LL * 24 * 60 * 60 * 1000 };
	// 附件缩略图落盘用的固定文件名（每次覆盖）。放数据目录里，不进截图目录
	constexpr const wchar_t* attachTmpName{ L"attach-preview.png" };
	// 回显用的缩略图文件名前缀，后面接这条消息的时间戳（毫秒，唯一）
	constexpr const wchar_t* msgThumbPrefix{ L"msg-" };
	// 流式输出重画 markdown 的最小间隔（毫秒）。见 lastStreamPaint
	constexpr long long streamPaintMs{ 80 };
	// 回显缩略图的上限框（逻辑像素，等比缩，不放大小图）
	constexpr float thumbMaxW{ 160.f }, thumbMaxH{ 120.f };

	// 图标字体（Resource.rc 里的 iconfont.ttf）用到的码点。字形对照见 Doc/ 与
	// D:\tmp\tpix-evidence\glyph_map.png —— 换图标先照那张表挑，别凭印象写
	constexpr const wchar_t* giCopy{ L"\ue902" };     // 双矩形
	constexpr const wchar_t* giEdit{ L"\ue601" };     // 笔
	constexpr const wchar_t* giDelete{ L"\ue62d" };   // ✕
	constexpr const wchar_t* giAttach{ L"\ue901" };   // 剪贴板
	constexpr const wchar_t* giSend{ L"\ue90a" };     // 箭头
	constexpr const wchar_t* giStop{ L"\ue62d" };     // ✕（回答中 = 掐掉）

	// 毫秒时间戳 -> 本地时间文本。传 DWrite 那套 strftime 格式串。
	// 消息落款要精确到秒，会话列表只要年月日，分组表头只要年-月
	std::wstring fmtTime(const long long ms, const wchar_t* format)
	{
		if (ms <= 0) return L"";
		const std::time_t t = static_cast<std::time_t>(ms / 1000);
		std::tm tm{};
		if (localtime_s(&tm, &t) != 0) return L"";
		wchar_t buf[64]{};
		if (wcsftime(buf, 64, format, &tm) == 0) return L"";
		return buf;
	}

	// 会话的排序与显示用哪个时间戳。早前的历史文件里没有 time 字段（那时还没记），
	// 读出来是 0 —— 退回用 id，它就是建这个会话那一刻的时间戳
	long long sessionTime(const AiHistory::Session& s)
	{
		return s.time > 0 ? s.time : s.id;
	}

	std::wstring clipName(const std::wstring& name)
	{
		if (name.size() <= nameMax) return name;
		return name.substr(0, nameMax) + L"…";
	}

	// 消息区里的可点控件都挂在 ScrollerBox 里，而 Button::isPosIn 比的是 Node::y
	// （yoga 算出来的绝对坐标，不含滚动量）。每次鼠标事件派发前把这一子树按
	// 「绝对坐标 - 滚动量」重摆一遍，按钮的命中位置才对得上。每次都从 yoga 重算，
	// 所以幂等（layout 之后被重置回未补偿值也无所谓，下个鼠标事件会再摆正）。
	// absTop 是 root 自己的绝对坐标，摆好之后我们不再动它。
	void shiftForScroll(Ling::Node* root, const float absTop, const float scrollY)
	{
		for (auto& child : root->children) {
			const float childAbsTop = absTop + YGNodeLayoutGetTop(child->node);
			child->y = childAbsTop - scrollY;
			shiftForScroll(child.get(), childAbsTop, scrollY);
		}
	}

	// 按子节点顺序（= 视觉顺序）把一棵子树里的文字块收进 out，空文本跳过。
	// 前序走对得上：引用是 Row[竖条, 文字]、有序列表是 Row[序号, 文字]，文字都在右边。
	// 只走**一棵气泡**而不是整个消息区 —— 落款那排（时间戳 + 图标）挂在气泡外面，
	// 这样天然选不上
	void collectLabels(Ling::Node* n, std::vector<Ling::Label*>& out)
	{
		for (auto& child : n->children) {
			if (auto* lab = dynamic_cast<Ling::Label*>(child.get())) {
				if (!lab->getText().empty()) out.push_back(lab);
			}
			collectLabels(child.get(), out);
		}
	}

	// node 是不是 ancestor 自己或它的后代
	bool underNode(const Ling::Node* node, const Ling::Node* ancestor)
	{
		for (auto* p = node; p; p = p->parent) {
			if (p == ancestor) return true;
		}
		return false;
	}
}

WinAiChat::WinAiChat() : Ling::WinBase()
{
	auto self = this;
	onDestroy.add([self]() {
		Ling::App::get()->dq.TryEnqueue([self]() { if (winAiChat.get() == self) winAiChat.reset(); });
	});
	setTitle(Lang::get(L"ai.title"));
	setSize(760.f, 560.f);
	setCenter();
	createNativeWindow();
}

WinAiChat::~WinAiChat()
{
	*alive = false;
	if (task) task->cancel();
	clearMsgThumbs();
}

void WinAiChat::init(const std::wstring& preset)
{
	if (winAiChat) {
		if (winAiChat->hwnd && IsWindow(winAiChat->hwnd)) {
			winAiChat->applyPreset(preset);
			winAiChat->show();
			SetForegroundWindow(winAiChat->hwnd);
			return;
		}
		winAiChat.reset();
	}
	winAiChat.reset(new WinAiChat());
	// 同 WinAiTrans：控件在 onCreated 里建出来，而 onCreated 在构造函数里就跑完了，
	// 所以填预设只能放在构造之后
	winAiChat->applyPreset(preset);
	if (winAiChat->hwnd) {
		winAiChat->show();
		SetForegroundWindow(winAiChat->hwnd);
	}
}

void WinAiChat::applyPreset(const std::wstring& text)
{
	if (text.empty()) return;
	input->setText(text);
	// 热键带内容进来多半是"接着再补一句要求再发"，把光标放进输入框省一次点击
	input->focus();
}

void WinAiChat::dispose()
{
	winAiChat.reset();
}

void WinAiChat::onCreated()
{
	// 命中坐标补偿必须排在消息区那些 Button **之前**订阅：Ling 的 winrt::event 按
	// 订阅顺序回调，而 Button 是在构造里就订阅 onMouseDown 的（同 WinSetting 的做法）。
	// 窗口与事件源同生共死，token 不用留。
	onMouseDown.add([](POINT, bool) { if (winAiChat) winAiChat->syncScrollHitCoords(); });
	onMouseMove.add([](POINT) { if (winAiChat) winAiChat->syncScrollHitCoords(); });
	// 改名中点别处 = 结束这次改名。这一条也必须排在按钮前面：否则"改着 A 又去点
	// 另一个 ✎"会先把目标换到 B，再拿 A 的文字去改 B
	onMouseDown.add([this](POINT pos, bool isRight) {
		if (isRight || renamingId == 0 || !renameBox) return;
		if (renameBox->isPosIn(pos)) return;   // 点在自己身上，接着改
		commitRename(renamingId, renameBox->getText());
	});
	// 分割线：按住拖动改输入区高度。松手可能落在窗口外，所以按下时抓一次 capture
	onMouseDown.add([this](POINT pos, bool isRight) {
		if (isRight || !onDivider(pos)) return;
		SetCapture(hwnd);
		dividerDragging = true;
		dragStartY = static_cast<float>(pos.y);
		dragStartH = inputAreaH;
	});
	onMouseUp.add([this](POINT, bool) {
		if (!dividerDragging) return;
		ReleaseCapture();
		dividerDragging = false;
	});
	onMouseMove.add([this](POINT pos) {
		if (dividerDragging) {
			// 往上拖 = 输入区变高。鼠标量是物理像素，先折回逻辑值再算
			setInputAreaH(dragStartH + (dragStartY - static_cast<float>(pos.y)) / dpi);
			return;
		}
		if (!divider) return;
		const bool on = onDivider(pos);
		if (on == dividerHover) return;
		dividerHover = on;
		divider->setBg(on ? 0xC8C8D0FF : 0xE8E8ECFF);
	});

	enableShadow();
	body->setBg(0xFFFFFFFF);
	body->setFlexDirection(Ling::FlexDirection::Row);

	auto side = body->makeChild<Ling::Node>();
	side->setWidth(200.f);
	side->setHeightPercent(100.f);
	side->setBg(0xF7F7F9FF);
	side->setFlexDirection(Ling::FlexDirection::Column);
	side->setPadding(8.f, 8.f, 8.f, 8.f);
	buildSidebar(side);

	auto right = body->makeChild<Ling::Node>();
	// 宽度给一个"确定但极小"的值，再让 flexGrow 把它撑开。**不能让 yoga 按内容算基准**：
	// 消息区里是聊天记录，文本不折行时那一整行的宽度会顺着 flex 基准一路传上来，把这一列
	// 撑到几千像素宽 —— 贴在右端的关闭按钮、发送按钮就全被顶到窗口外面去了
	//（"回答完了按钮就消失"就是这么来的）
	right->setWidth(1.f);
	right->setFlexGrow(1.f);
	right->setHeightPercent(100.f);
	right->setFlexDirection(Ling::FlexDirection::Column);

	auto titleRow = right->makeChild<Ling::Node>();
	titleRow->setHeight(36.f);
	titleRow->setWidthPercent(100.f);
	titleRow->setFlexDirection(Ling::FlexDirection::Row);
	titleRow->setAlignItems(Ling::Align::Center);
	auto title = titleRow->makeChild<Ling::Label>();
	title->setText(Lang::get(L"ai.title"));
	title->setFontSize(13.f);
	title->setColor(0x333333FF);
	title->setPaddingLeft(12.f);
	title->setFlexGrow(1.f);
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

	// 标题行下面这条分隔线：没有它的时候，消息区的内容会一路滚到标题底下，
	// 上下两块挤在一起分不出"哪是标题、哪是正文"
	auto titleSep = right->makeChild<Ling::Node>();
	titleSep->setWidthPercent(100.f);
	titleSep->setHeight(1.f);
	titleSep->setBg(0xE8E8ECFF);

	msgScroller = right->makeChild<Ling::ScrollerBox>();
	msgScroller->setFlexGrow(1.f);
	msgScroller->setWidthPercent(100.f);
	msgBox = msgScroller->makeChild<Ling::Node>();
	msgBox->setWidthPercent(100.f);
	msgBox->setFlexDirection(Ling::FlexDirection::Column);
	msgBox->setPadding(msgPad, msgPad, msgPad, msgPad);
	// 右侧还要多让出滚动条那一条竖带：ScrollerBox 的滑块浮在最右边、不占布局宽度，
	// 不让的话正文右边界离滑块只剩几个像素，看着就是"内容顶到边上了"。宽度要等布局
	// 之后才知道（滚动条露不露取决于内容高不高），所以真正的设置放在 applyMsgPadding
	applyMsgPadding();

	divider = right->makeChild<Ling::Node>();
	divider->setWidthPercent(100.f);
	divider->setHeight(3.f);
	divider->setBg(0xE8E8ECFF);

	bottom = right->makeChild<Ling::Node>();
	bottom->setWidthPercent(100.f);
	bottom->setHeight(inputAreaH);
	bottom->setFlexDirection(Ling::FlexDirection::Column);
	bottom->setPadding(10.f, 6.f, 10.f, 10.f);

	// 输入框：边框画在这一层，里面的文本、缩略图、图标按钮是它的子节点 ——
	// 视觉上是"一个整体"（发送与附件那两枚小图标因此落在输入框**内部**）
	inputBox = bottom->makeChild<Ling::Node>();
	inputBox->setWidthPercent(100.f);
	inputBox->setFlexGrow(1.f);
	inputBox->setFlexShrink(1.f);
	inputBox->setFlexDirection(Ling::FlexDirection::Column);
	inputBox->setBorder(1.f, 0xE0E0E0FF);
	inputBox->setBorderRadius(8.f);
	inputBox->setBg(0xFFFFFFFF);
	inputBox->setPadding(6.f, 6.f, 6.f, 6.f);

	// 附件预览条。没有附件时整条 hide()（DisplayNone，不占位），
	// 有附件时在输入框内部显示缩略图 + 尺寸
	attachStrip = inputBox->makeChild<Ling::Node>();
	attachStrip->setWidthPercent(100.f);
	attachStrip->setHeight(50.f);
	attachStrip->setMarginBottom(6.f);
	attachStrip->setFlexDirection(Ling::FlexDirection::Row);
	attachStrip->setAlignItems(Ling::Align::Center);

	attachThumb = attachStrip->makeChild<Ling::ImageBox>();
	attachThumb->setSize(46.f, 46.f);
	attachThumb->setBorderRadius(4.f);
	attachThumb->setBorder(1.f, 0xDDDDDDFF);

	attachInfo = attachStrip->makeChild<Ling::Label>();
	attachInfo->setFontSize(11.f);
	attachInfo->setColor(0x666666FF);
	attachInfo->setMarginLeft(8.f);

	auto rmAttach = attachStrip->makeChild<Ling::Button>();
	rmAttach->setSize(18.f, 18.f);
	rmAttach->setMarginLeft(8.f);
	rmAttach->setFontFamily(L"icon");
	rmAttach->setFontSize(11.f);
	rmAttach->setColor(0x999999FF);
	rmAttach->setHoverColor(0xE81123FF);
	rmAttach->setHoverBg(0xFFECECFF);
	rmAttach->setBorderRadius(4.f);
	rmAttach->setText(giDelete);
	rmAttach->onEnter.add([this, rmAttach](Ling::Button*) { showTip(rmAttach, Lang::get(L"ai.delete")); });
	rmAttach->onLeave.add([this](Ling::Button*) { hideTip(); });
	rmAttach->onClick.add([this](Ling::Button*) { clearAttach(); });

	input = inputBox->makeChild<Ling::TextBox>();
	input->setWidthPercent(100.f);
	input->setFlexGrow(1.f);
	input->setFlexShrink(1.f);
	input->setFontSize(14.f);
	// 边框归 inputBox，控件自己不再画一圈
	input->setPadding(2.f, 2.f, 2.f, 2.f);
	input->setPlaceholder(Lang::get(L"ai.placeholder"));
	input->onKeyDown.add([this](Ling::TextBox*, const UINT key, bool* handled) {
		const bool isCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
		const bool isShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
		if (key == VK_RETURN) {
			// 回车发送；Shift+Enter 不拦，交给控件自己换行（写长问题时要用）
			if (isShift) return;
			*handled = true;
			if (busy) {
				// 正在回答时回车等于按"停止"，跟按钮一致
				if (task) task->cancel();
				return;
			}
			send();
			return;
		}
		if (isCtrl && key == 'V') {
			// 剪贴板里是图就直接挂到这一句上，别再要求用户先存成文件
			if (takeClipboardImage()) *handled = true;
		}
	});

	// 输入框内部右下角那一行：附件 + 发送
	auto tools = inputBox->makeChild<Ling::Node>();
	tools->setWidthPercent(100.f);
	tools->setHeight(26.f);
	tools->setFlexDirection(Ling::FlexDirection::Row);
	tools->setAlignItems(Ling::Align::Center);

	attachBtn = tools->makeChild<Ling::Button>();
	attachBtn->setSize(24.f, 24.f);
	attachBtn->setFontFamily(L"icon");
	attachBtn->setFontSize(15.f);
	attachBtn->setColor(0x777777FF);
	attachBtn->setHoverColor(0x1A73E8FF);
	attachBtn->setHoverBg(0xEEF3FDFF);
	attachBtn->setBorderRadius(12.f);
	attachBtn->setText(giAttach);
	attachBtn->onEnter.add([this](Ling::Button* btn) { showTip(btn, Lang::get(L"ai.attachImg")); });
	attachBtn->onLeave.add([this](Ling::Button*) { hideTip(); });
	attachBtn->onClick.add([this](Ling::Button*) { attach(); });

	// 「接口 - 模型」切换：排在附件按钮右边，回答之前点它换一套。
	// 列表是所有接口 × 各自模型的摊平表 —— 需求里"翻译用 A 接口 A 模型、表格识别用
	// B 接口 X 模型"说的是这一类组合，这里让用户临场挑一个，而不是只能用在设置页配的那套
	modelBtn = tools->makeChild<Ling::Button>();
	modelBtn->setHeight(24.f);
	modelBtn->setWidth(168.f);
	modelBtn->setMarginLeft(4.f);
	modelBtn->setFontSize(11.f);
	modelBtn->setColor(0x666666FF);
	modelBtn->setHoverColor(0x1A73E8FF);
	modelBtn->setHoverBg(0xEEF3FDFF);
	modelBtn->setBorderRadius(12.f);
	refreshModelBtn();
	modelBtn->onEnter.add([this](Ling::Button* btn) {
		showTip(btn, modelLabel);
	});
	modelBtn->onLeave.add([this](Ling::Button*) { hideTip(); });
	modelBtn->onClick.add([this](Ling::Button* btn) {
		// 每一项 = （接口 id, 模型名），标签是"接口名 - 模型名"。
		// 换成整的组合而不是先选接口再选模型 —— 用户要的是"换个模型试试"，两步太重
		struct Choice { std::wstring providerId; std::wstring model; };
		std::vector<Choice> choices;
		std::vector<std::wstring> items;
		const auto bound = Setting::get()->credFor(std::wstring{ AiScenario::chat });
		int cur = -1;
		for (const auto& p : Setting::get()->getAiProviders()) {
			auto candidates = p.models.empty() ? std::vector<std::wstring>{} : p.models;
			if (candidates.empty() && !p.model.empty()) candidates.push_back(p.model);
			if (candidates.empty()) continue;   // 这一家连一个模型都没有，跳过它
			const auto name = Setting::providerName(p);
			for (const auto& model : candidates) {
				if (p.id == bound.providerId && model == bound.model) {
					cur = static_cast<int>(items.size());
				}
				items.push_back(name + L" - " + model);
				choices.push_back(Choice{ p.id, model });
			}
		}
		if (items.empty()) {
			MessageBox(hwnd, Lang::get(L"ai.fetchFirst").data(),
				Lang::get(L"about.sysTip").data(), MB_OK | MB_ICONINFORMATION);
			return;
		}
		SelectPopup::show(this, btn, items, cur, [this, choices](int idx) {
			Setting::get()->setScenario(std::wstring{ AiScenario::chat },
				choices[idx].providerId, choices[idx].model);
			refreshModelBtn();
		}, {}, modelPopupMinW);
	});

	auto spacer = tools->makeChild<Ling::Node>();
	spacer->setFlexGrow(1.f);

	sendBtn = tools->makeChild<Ling::Button>();
	sendBtn->setSize(26.f, 26.f);
	sendBtn->setFontFamily(L"icon");
	sendBtn->setFontSize(15.f);
	sendBtn->setColor(0xFFFFFFFF);
	sendBtn->setHoverColor(0xFFFFFFFF);
	sendBtn->setBg(0x4D6BFEFF);
	sendBtn->setHoverBg(0x3B5BDBFF);
	sendBtn->setBorderRadius(13.f);
	sendBtn->setText(giSend);
	// 提示文案跟着当前状态走：回答中这一下是"停止"而不是"发送"
	sendBtn->onEnter.add([this](Ling::Button* btn) {
		showTip(btn, Lang::get(busy ? L"ai.stop" : L"ai.send"));
	});
	sendBtn->onLeave.add([this](Ling::Button*) { hideTip(); });
	sendBtn->onClick.add([this](Ling::Button*) {
		if (busy) {
			if (task) task->cancel();
			return;
		}
		send();
	});

	// 悬停提示浮层。挂在 body 上、**最后**创建 —— Composition 的子节点按加入顺序叠，
	// 放最后才盖得住消息区与输入框；挂在 body 上也不会被消息区那个 ScrollerBox 裁掉
	tipLabel = body->makeChild<Ling::Label>();
	tipLabel->setPositionType(Ling::Position::Absolute);
	tipLabel->setFontSize(11.f);
	tipLabel->setColor(0xFFFFFFFF);
	tipLabel->setBg(0x3C4043FF);
	tipLabel->setPadding(6.f, 3.f, 6.f, 3.f);
	tipLabel->setBorderRadius(4.f);
	tipLabel->hide();

	// ---- 对话区框选 ----
	// 按下落在消息区里就是一次选区的起手。单击（没拖）也要走这一遍：把手头这次选区收
	// 干净，等于"点一下取消选择"
	onMouseDown.add([this](POINT pos, bool isRight) {
		if (isRight || !msgScroller || !msgScroller->isPosIn(pos)) return;
		size_t bi = 0;
		unsigned int cp = 0;
		// 起手要求"真压在文字上"：消息区里点图标、点空白都不该起一次选区
		// （顺带就是"点一下取消选择"）
		if (!selHit(pos, bi, cp, false)) {
			clearSelVisual();
			clickCount = 0;
			return;
		}
		// 连击计数：双击选词、三击选整段。判据与 TextBox 一致 —— 与上一次按下的间隔
		// 在系统双击时间内、且位置几乎没动。
		// ⚠️ 别把变量叫 near / far：Windows SDK 的老头文件把它们定义成空宏，这里会
		// 直接变成语法错误（C2760 "unexpected )"）
		const ULONGLONG now = GetTickCount64();
		const int dx = pos.x - lastClickPt.x, dy = pos.y - lastClickPt.y;
		const bool sameSpot = dx >= -4 && dx <= 4 && dy >= -4 && dy <= 4;
		clickCount = (now - lastClickTick <= GetDoubleClickTime() && sameSpot) ? clickCount + 1 : 1;
		if (clickCount > 3) clickCount = 1;   // 第四下重新当单击算
		lastClickTick = now;
		lastClickPt = pos;

		selAnchorBlock = bi;
		if (clickCount == 2) {
			// 选词。多选出来的这一段**不进入拖动状态**：连击之后手还没抬，跟着动一下
			// 就把刚选好的词抹了
			selDragging = false;
			unsigned int s = cp, e = cp;
			selBlocks[bi]->wordRange(cp, s, e);
			selAnchorPos = s;
			selCurBlock = bi;
			selCurPos = e;
			applySelVisual();
			return;
		}
		if (clickCount >= 3) {
			// 选整段。一个块就是一段 —— markdown 的每个块各是一个 Label
			selDragging = false;
			selAnchorPos = 0;
			selCurBlock = bi;
			selCurPos = selBlocks[bi]->len();
			applySelVisual();
			return;
		}
		selDragging = true;
		selCurBlock = bi;
		selAnchorPos = selCurPos = cp;
		// 抓一次 capture：松手可能落在窗口外，靠它才收得到 onMouseUp（同分割线）
		SetCapture(hwnd);
		applySelVisual();
	});
	onMouseMove.add([this](POINT pos) {
		if (!selDragging) return;
		size_t bi = selCurBlock;
		unsigned int cp = selCurPos;
		if (!selHit(pos, bi, cp)) return;
		if (bi == selCurBlock && cp == selCurPos) return;   // 热路径：位置没变就别重刷高亮
		selCurBlock = bi;
		selCurPos = cp;
		applySelVisual();
	});
	onMouseUp.add([this](POINT, bool) {
		if (!selDragging) return;
		ReleaseCapture();
		selDragging = false;   // 选区留着，等用户 Ctrl+C
	});
	// Ctrl+C（以及老习惯的 Ctrl+Insert）复制选中的对话文字。焦点在输入框里时让给它 ——
	// 那说明用户正在写这一句，他要复制的是输入框里那一段。反过来说，在消息区按下鼠标
	// 本身就会让输入框失焦（TextBox::onDown 点在控件外就 blur()），而且它的订阅排在
	// 我们前面，所以"框选消息区"之后这里一定放行
	onKeyDown.add([this](UINT key) {
		if (selText.empty()) return;
		if ((GetKeyState(VK_CONTROL) & 0x8000) == 0) return;
		if (key != 'C' && key != VK_INSERT) return;
		if (input && input->isFocused()) return;
		Ling::Util::setTextToClipboard(selText);
	});

	openSession(0);
}

LRESULT WinAiChat::onHitTest(const POINT pos)
{
	POINT pt = pos;
	ScreenToClient(hwnd, &pt);
	if (!isMaximized) {
		auto result = borderHitTest(pt);
		if (result != HTCLIENT) return result;
	}
	// 标题行当拖拽区。窗口是 WS_POPUP，没有系统标题栏，不给它一条拖拽带就拖不动
	if (pt.y > 0 && pt.y < 36 * dpi && pt.x < w - 42 * dpi) return HTCAPTION;
	return HTCLIENT;
}

BOOL WinAiChat::setCursor()
{
	if (divider) {
		POINT pt{};
		if (GetCursorPos(&pt)) {
			ScreenToClient(hwnd, &pt);
			if (dividerDragging || onDivider(pt)) {
				SetCursor(LoadCursor(nullptr, IDC_SIZENS));
				return TRUE;
			}
		}
	}
	return Ling::WinBase::setCursor();
}

void WinAiChat::showTip(Ling::Node* anchor, const std::wstring& text)
{
	if (!tipLabel || !anchor) return;
	tipLabel->setText(text);
	// anchor 的 x/y/w 是物理像素，而且是相对 body 的绝对坐标（Node::layout 逐层累加过，
	// 消息区里那些还额外减掉了滚动量）。setPosition 收逻辑值，这里折回去。
	//
	// 摆法固定为"右端对齐 + 贴在按钮上方"，两个轴都刻意避开 Left/Top：
	//  - 横向用 Right：yoga 给绝对定位节点算可用宽度是按 left 扣的，贴右边缘的按钮
	//   （消息区那排图标就贴着右边）用 Left 定位只剩几十像素可用，提示文字会被裁掉
	//  - 纵向用 Bottom：把提示的底边贴在按钮上方，不需要先知道它自己多高
	const float ax = anchor->x / dpi;
	const float ay = anchor->y / dpi;
	const float aw = anchor->w / dpi;
	tipLabel->setPosition(Ling::Edge::Right, std::max(0.f, w / dpi - (ax + aw)));
	tipLabel->setPosition(Ling::Edge::Bottom, std::max(0.f, h / dpi - ay + 4.f));
	tipLabel->show();
}

void WinAiChat::hideTip()
{
	if (tipLabel) tipLabel->hide();
}

void WinAiChat::resetSelState()
{
	selBlocks.clear();
	selDragging = false;
	selAnchorBlock = selCurBlock = 0;
	selAnchorPos = selCurPos = 0;
	selText.clear();
}

bool WinAiChat::selHit(const POINT pos, size_t& bi, unsigned int& cp, const bool allowFallback)
{
	// 命中全按 Node::x/y 比，而 y 只在鼠标事件里才折算过 —— 自己先折一次，别指望
	// "上一个鼠标事件刚好来过"（重排之后那些 y 又变回自然坐标了）
	syncScrollHitCoords();
	if (selBlocks.empty()) return false;
	const float px = static_cast<float>(pos.x);
	const float py = static_cast<float>(pos.y);
	// 纵向给一点余量：行与行之间有空隙，卡着"必须落在字上"会让按下十次里有几次落空，
	// 手感上就是"框选不稳定"
	const float pad = 4.f * dpi;

	// 先找压住的那一块。Label::textRect 与 Button::isPosIn 是同一套坐标口径
	//（Node::x/y 已被 syncScrollHitCoords 减过滚动量），所以这里直接比
	for (size_t i = 0; i < selBlocks.size(); ++i) {
		RECT r{};
		if (!selBlocks[i]->textRect(r)) continue;
		if (py < static_cast<float>(r.top) - pad || py > static_cast<float>(r.bottom) + pad) continue;
		// 横向放 24 像素余量 —— 点在行尾右边一点点也算这一行
		if (px < static_cast<float>(r.left) - 24.f || px > static_cast<float>(r.right) + 24.f) continue;
		bi = i;
		cp = selBlocks[i]->hitTestPos(px, py);
		return true;
	}

	// 一块都没压住（拖出文字区，或落在两块之间的空隙里）：按纵向取最近的一块，夹到它的
	// 首 / 尾 —— 这样从最后一行继续往下拖，选区还会接着往下长
	if (!allowFallback) return false;
	size_t best = 0;
	float bestD = -1.f;
	bool below = false;
	for (size_t i = 0; i < selBlocks.size(); ++i) {
		RECT r{};
		if (!selBlocks[i]->textRect(r)) continue;
		float d = 0.f;
		bool b = false;
		if (py < static_cast<float>(r.top)) d = static_cast<float>(r.top) - py;
		else if (py > static_cast<float>(r.bottom)) { d = py - static_cast<float>(r.bottom); b = true; }
		if (bestD < 0.f || d < bestD) { bestD = d; best = i; below = b; }
	}
	bi = best;
	cp = below ? selBlocks[best]->len() : 0u;
	return true;
}

void WinAiChat::applySelVisual()
{
	selText.clear();
	if (selBlocks.empty()) return;

	// 归一化：anchor 是按下那一下，cur 是现在。跨块时按 (块下标, 块内位置) 的字典序比
	size_t b0 = selAnchorBlock, b1 = selCurBlock;
	unsigned int p0 = selAnchorPos, p1 = selCurPos;
	if (b0 > b1 || (b0 == b1 && p0 > p1)) {
		std::swap(b0, b1);
		std::swap(p0, p1);
	}

	for (size_t i = 0; i < selBlocks.size(); ++i) {
		auto* lab = selBlocks[i];
		const unsigned int len = lab->len();
		// 首块从 p0 起、末块到 p1 止，中间的整块全要
		const unsigned int s = (i <= b0) ? std::min(p0, len) : 0u;
		const unsigned int e = (i >= b1) ? std::min(p1, len) : len;
		// b1 有可能已经越过末尾（正拖着的当口消息区重画、末尾几块被摘掉了），
		// 那种情况下区间会自己塌成空的，正好什么都不选
		if (i < b0 || i > b1 || e <= s) {
			lab->clearSel();
			continue;
		}
		lab->setSelRange(s, e);
		// 一整段选中文字攒进 selText：块与块之间补一个换行（Ctrl+C 直接抄它）
		if (!selText.empty()) selText += L'\n';
		selText += lab->selText();
	}
	refresh();
}

void WinAiChat::clearSelVisual()
{
	selText.clear();
	selAnchorBlock = selCurBlock = 0;
	selAnchorPos = selCurPos = 0;
	for (auto* lab : selBlocks) lab->clearSel();
	refresh();
}

void WinAiChat::syncScrollHitCoords()
{
	if (!msgScroller || !msgBox) return;
	// shiftForScroll 每次都按 yoga 布局重算绝对值（base 用的是 msgBox 自己的自然 y），
	// 所以是幂等的、也不会随调用次数漂。
	//
	// ⚠️ 这里**不能**拿"滚动量没变"当缓存判据。Node::layout 每次都把 y 写回自然坐标，
	// 而重排（改输入区高度、改窗口大小、重排会话列表…）随时可能发生 —— 一旦重排过，
	// 上一次的折算就被抹掉了，缓存却还以为折过，于是命中判定整体偏一个滚动量。
	// 子树就那么大，每次重算的开销可以忽略
	shiftForScroll(msgBox, msgBox->y, msgScroller->getScrollY());
}

void WinAiChat::applyMsgPadding()
{
	if (!msgScroller || !msgBox) return;
	// 滚动条那一条竖带浮在最右侧、不占布局宽度，所以内容右边界要自己让开它
	//（getScrollBarWidth 返回物理像素，没露滚动条时为 0）
	const float pad = msgPad + msgScroller->getScrollBarWidth() / dpi;
	if (pad == msgRightPad) return;
	msgRightPad = pad;
	msgBox->setPaddingRight(pad);
}

bool WinAiChat::onDivider(const POINT pos) const
{
	if (!divider) return false;
	const float px = static_cast<float>(pos.x), py = static_cast<float>(pos.y);
	if (px < divider->x || px >= divider->x + divider->w) return false;
	// 线只有 3 逻辑像素高，上下各给一点余量 —— 不给的话"悬停变色 / 按住拖动"都要
	// 像素级对准才碰得上
	const float pad = 4.f * dpi;
	return py >= divider->y - pad && py < divider->y + divider->h + pad;
}

void WinAiChat::setInputAreaH(const float logical)
{
	// 上限跟窗口高走：输入区再高也得给消息区留出一块
	const float maxH = std::min(maxInputH, h / dpi - 160.f);
	const float val = std::clamp(logical, minInputH, std::max(minInputH, maxH));
	if (val == inputAreaH) return;
	inputAreaH = val;
	if (bottom) bottom->setHeight(val);
	refresh();
}

void WinAiChat::buildSidebar(Ling::Node* side)
{
	auto newBtn = side->makeChild<Ling::Button>();
	newBtn->setText(Lang::get(L"ai.newChat"));
	newBtn->setHeight(32.f);
	newBtn->setWidthPercent(100.f);
	newBtn->setBorder(1.f, 0xE0E0E0FF);
	newBtn->setBorderRadius(6.f);
	newBtn->setHoverBg(0xFFFFFFFF);
	newBtn->onClick.add([this](Ling::Button*) { openSession(0); });

	sessionBox = side->makeChild<Ling::Node>();
	sessionBox->setWidthPercent(100.f);
	sessionBox->setMarginTop(8.f);
	sessionBox->setFlexDirection(Ling::FlexDirection::Column);
	refreshSessions();
}

void WinAiChat::refreshSessions()
{
	hideTip();
	renameBox = nullptr;
	while (!sessionBox->children.empty()) {
		sessionBox->removeChild(sessionBox->children.front().get());
	}
	auto sessions = AiHistory::get() ? AiHistory::get()->list() : std::vector<AiHistory::Session>{};
	const int n = std::min(static_cast<int>(sessions.size()), maxSessions);
	const long long cur = AiHistory::now();
	// 分组：30 天内一档，更早的按"年-月"各一档。list() 已是时间新的在前，所以顺着走一遍
	// 就够了 —— 同一档一定连在一起，档与档之间也天然是从新到旧
	std::wstring lastGroup;
	for (int i = 0; i < n; ++i) {
		auto& s = sessions[i];
		const auto id = s.id;
		const long long t = sessionTime(s);
		const std::wstring group = (cur - t) < recentMs
			? Lang::get(L"ai.groupRecent")
			: fmtTime(t, L"%Y-%m");
		if (group != lastGroup) {
			lastGroup = group;
			auto head = sessionBox->makeChild<Ling::Label>();
			head->setText(group);
			head->setFontSize(11.f);
			head->setColor(0x9AA0A6FF);
			head->setMarginLeft(6.f);
			head->setMarginTop(i == 0 ? 0.f : 8.f);
			head->setMarginBottom(2.f);
		}

		auto row = sessionBox->makeChild<Ling::Node>();
		row->setWidthPercent(100.f);
		row->setHeight(40.f);
		row->setMarginBottom(2.f);
		row->setFlexDirection(Ling::FlexDirection::Row);
		row->setAlignItems(Ling::Align::Center);
		row->setBorderRadius(4.f);
		// 当前会话整行给个底色。标题本身不再画矩形边框 —— 照 DeepSeek 那样只靠底色区分
		if (id == curId) row->setBg(0xE8F0FEFF);

		// 名字与日期上下两行：日期挪到名字下面一行、字号更小，整块占满剩余宽度，
		// 改名/删除两枚图标贴右端
		auto info = row->makeChild<Ling::Node>();
		info->setFlexGrow(1.f);
		info->setHeightPercent(100.f);
		info->setFlexDirection(Ling::FlexDirection::Column);
		info->setJustifyContent(Ling::Justify::Center);

		// 正在改名的那一条：名字那一格换成输入框。回车提交；点到别处由输入框自己
		// 失焦，也在那里提交
		if (renamingId != 0 && id == renamingId) {
			auto box = info->makeChild<Ling::TextBox>();
			box->setWidthPercent(100.f);
			box->setHeight(22.f);
			box->setFontSize(12.f);
			box->setPadding(4.f, 2.f, 4.f, 2.f);
			box->setBorder(1.f, 0x597EF7FF);
			box->setText(s.title.empty() ? Lang::get(L"ai.newChat") : s.title);
			renameBox = box;
			auto aliveFlag = alive;
			box->onKeyDown.add([this, aliveFlag, box](Ling::TextBox*, const UINT key, bool* handled) {
				if (key != VK_RETURN || !*aliveFlag) return;
				*handled = true;   // 别往名字里插一个换行：这一下是"确认"
				commitRename(renamingId, box->getText());
			});
			// 同一窗口里两个 TextBox 各自记各自的焦点，不主动让旧的失焦就会有两个
			// 控件同时"有焦点"，一按回车两边都响应
			if (input) input->blur();
			box->focus();
			continue;
		}

		auto nameBtn = info->makeChild<Ling::Button>();
		nameBtn->setWidthPercent(100.f);
		nameBtn->setHeight(20.f);
		nameBtn->setFontSize(12.f);
		// 名字靠左。Button 默认是把文字摆在水平居中（它构造里设的 justifyContent 走的是
		// 主轴=纵向，管的是上下），横向那一维要用交叉轴的 alignItems
		nameBtn->setAlignItems(Ling::Align::FlexStart);
		nameBtn->setPaddingLeft(6.f);
		nameBtn->setColor(id == curId ? 0x1A73E8FF : 0x333333FF);
		nameBtn->setHoverBg(0xEDF2FDFF);
		// 一条都没说过话的会话没有标题（create 出来的空会话不写盘，所以这里只会是
		// 已经说过话但标题被清掉的情况），给个占位，别让按钮空着
		nameBtn->setText(clipName(s.title.empty() ? Lang::get(L"ai.newChat") : s.title));
		nameBtn->onClick.add([this, id](Ling::Button*) { openSession(id); });

		auto date = info->makeChild<Ling::Label>();
		date->setFontSize(9.f);
		date->setColor(0x9AA0A6FF);
		date->setPaddingLeft(6.f);
		date->setMarginTop(1.f);
		date->setText(fmtTime(t, L"%Y-%m-%d"));

		auto editBtn = row->makeChild<Ling::Button>();
		editBtn->setSize(20.f, 22.f);
		editBtn->setMarginLeft(2.f);
		editBtn->setFontFamily(L"icon");
		editBtn->setFontSize(12.f);
		editBtn->setColor(0x999999FF);
		editBtn->setHoverColor(0x1A73E8FF);
		editBtn->setHoverBg(0xEEF3FDFF);
		editBtn->setBorderRadius(4.f);
		editBtn->setText(giEdit);
		editBtn->onEnter.add([this, editBtn](Ling::Button*) { showTip(editBtn, Lang::get(L"ai.edit")); });
		editBtn->onLeave.add([this](Ling::Button*) { hideTip(); });
		editBtn->onClick.add([this, id](Ling::Button*) { beginRename(id); });

		auto delBtn = row->makeChild<Ling::Button>();
		delBtn->setSize(20.f, 22.f);
		delBtn->setMarginLeft(2.f);
		delBtn->setFontFamily(L"icon");
		delBtn->setFontSize(12.f);
		delBtn->setColor(0x999999FF);
		delBtn->setHoverColor(0xE81123FF);
		delBtn->setHoverBg(0xFFECECFF);
		delBtn->setBorderRadius(4.f);
		delBtn->setText(giDelete);
		delBtn->onEnter.add([this, delBtn](Ling::Button*) { showTip(delBtn, Lang::get(L"ai.delete")); });
		delBtn->onLeave.add([this](Ling::Button*) { hideTip(); });
		delBtn->onClick.add([this, id](Ling::Button*) { removeSession(id); });
	}
}

void WinAiChat::beginRename(const long long id)
{
	renamingId = id;
	refreshSessions();
}

void WinAiChat::commitRename(const long long id, const std::wstring& raw)
{
	// 已经切走了（改了别的行 / 换了会话）：这一下作废。宁可丢掉这次改名，
	// 也不能拿旧文字去改另一个会话
	if (id == 0 || renamingId != id) return;
	renamingId = 0;
	renameBox = nullptr;
	// 回车确认那条路上可能已经混进来一个换行，首尾空白一并剥掉
	std::wstring title{ raw };
	while (!title.empty() && (title.front() == L' ' || title.front() == L'\n' || title.front() == L'\r')) title.erase(0, 1);
	while (!title.empty() && (title.back() == L' ' || title.back() == L'\n' || title.back() == L'\r')) title.pop_back();
	if (AiHistory::get()) AiHistory::get()->rename(id, title);
	refreshSessions();
}

void WinAiChat::removeSession(const long long id)
{
	if (renamingId == id) renamingId = 0;
	if (AiHistory::get()) AiHistory::get()->remove(id);
	if (curId == id) openSession(0);   // 删的正是当前这个：切回"新对话"
	else refreshSessions();
}

void WinAiChat::openSession(const long long id)
{
	abortTask();
	// 回显用的临时缩略图跟着上一个会话一起清掉：图不进历史，留着也没人认领
	clearMsgThumbs();
	curId = id;
	msgs.clear();
	tailText.clear();
	renamingId = 0;
	renameBox = nullptr;
	// 换会话时把待发附件一并丢掉（临时文件也删）：它属于上一个上下文，留着会误发
	clearAttach();
	if (id != 0) {
		if (auto session = AiHistory::get() ? AiHistory::get()->find(id) : nullptr) msgs = session->msgs;
	}
	if (input) input->setText(L"");
	renderMsgs();
	refreshSessions();
}

Ling::Node* WinAiChat::addItem(const AiService::Role role, const std::wstring& text,
	const size_t index, const long long time)
{
	const bool isUser = role == AiService::Role::User;

	auto item = msgBox->makeChild<Ling::Node>();
	item->setWidthPercent(100.f);
	item->setFlexDirection(Ling::FlexDirection::Column);
	item->setMarginBottom(12.f);

	// 气泡外面再套一层 Row 才能"顶右 / 顶左"。直接在 Column 里用 Align::End 是不行的：
	// 交叉轴对齐时 yoga 量文本不给可用宽度（MeasureModeUndefined），折行会失效，
	// 那一整行又会把布局撑爆 —— 这正是上一轮"回答完按钮消失"的根因，别再踩
	auto line = item->makeChild<Ling::Node>();
	line->setWidthPercent(100.f);
	line->setFlexDirection(Ling::FlexDirection::Row);
	line->setJustifyContent(isUser ? Ling::Justify::End : Ling::Justify::Start);

	// 气泡本身是一个 Column 容器，里面才是文字块：回答条要装下 markdown 分出来的好几个
	// 块（标题 / 代码块 / 列表…），用户条还要在问题下面摆一张回显的图，单靠一个 Label
	// 装不了。用户条仍然"随内容撑开、只封顶"；回答条不吃上限，直接铺满
	auto bubble = line->makeChild<Ling::Node>();
	bubble->setFlexShrink(1.f);
	bubble->setFlexDirection(Ling::FlexDirection::Column);
	if (isUser) {
		bubble->setMaxWidthPercent(bubbleMaxPct);
		bubble->setPadding(10.f, 8.f, 10.f, 8.f);
		bubble->setBorderRadius(8.f);
		bubble->setBg(0xE6F4FFFF);
	}
	else {
		// 回答条没有气泡：顶左、纯文字、右边撑满整个对话区宽度（不留灰底与内边距，
		// 否则那一整块的左边界会比用户条缩进一截，看着对不齐）
		bubble->setWidthPercent(100.f);
	}

	fillBubble(bubble, isUser, text);

	// 用户条：把这一轮发出去的图回显在问题下面。图不进历史，所以只有当前会话里
	// 那份临时缩略图还在的时候才画得出来（重开旧会话时本来也没有这张图）
	if (isUser && index != noIndex && index < msgs.size() && !msgs[index].image.empty()) {
		const auto path = thumbPath(msgs[index].time);
		std::error_code ec;
		if (!path.empty() && std::filesystem::exists(path, ec)) {
			const float iw = static_cast<float>(std::max(1, msgs[index].imgW));
			const float ih = static_cast<float>(std::max(1, msgs[index].imgH));
			const float s = std::min({ thumbMaxW / iw, thumbMaxH / ih, 1.f });
			auto thumb = bubble->makeChild<Ling::ImageBox>();
			// 容器定尺寸、内部的 Image 量出来的是同一个比例，两边一致才不会拉变形
			thumb->setSize(std::max(1.f, iw * s), std::max(1.f, ih * s));
			thumb->setMarginTop(6.f);
			thumb->setBorderRadius(4.f);
			thumb->loadImg(path);
		}
	}

	// 流式占位（还有"没配好"的提示）：还没有落款时间，也没有成品的按钮
	if (index == noIndex) return bubble;

	auto actions = item->makeChild<Ling::Node>();
	actions->setWidthPercent(100.f);
	actions->setHeight(20.f);
	actions->setMarginTop(2.f);
	actions->setFlexDirection(Ling::FlexDirection::Row);
	actions->setAlignItems(Ling::Align::Center);
	// 图标一律贴右，跟气泡同侧（上一轮定的"内容尾部居右"）
	actions->setJustifyContent(Ling::Justify::End);

	// 操作按钮只画图标。图标看不出是哪个动作，靠悬停提示把名字报出来
	auto makeIcon = [&actions, this](const std::wstring& glyph, const std::wstring& name, const float marginLeft) {
		auto btn = actions->makeChild<Ling::Button>();
		btn->setSize(20.f, 20.f);
		btn->setMarginLeft(marginLeft);
		btn->setFontFamily(L"icon");
		btn->setFontSize(13.f);
		btn->setColor(0x9AA0A6FF);
		btn->setHoverColor(0x333333FF);
		btn->setHoverBg(0xEDEDEDFF);
		btn->setBorderRadius(4.f);
		btn->setText(glyph);
		btn->onEnter.add([this, btn, name](Ling::Button*) { showTip(btn, name); });
		btn->onLeave.add([this](Ling::Button*) { hideTip(); });
		return btn;
	};

	if (isUser) {
		// 问题这一条：复制 / 编辑 / 删除（删除删的是整轮，见 removeRound）
		makeIcon(giCopy, Lang::get(L"ai.copy"), 0.f)->onClick.add([this, index](Ling::Button*) { copyMsg(index); });
		makeIcon(giEdit, Lang::get(L"ai.edit"), 4.f)->onClick.add([this, index](Ling::Button*) { loadIntoInput(index); });
		makeIcon(giDelete, Lang::get(L"ai.delete"), 4.f)->onClick.add([this, index](Ling::Button*) { removeRound(index); });
	}
	else {
		// 回答这一条：落款（精确到秒）在图标左边
		auto stamp = actions->makeChild<Ling::Label>();
		stamp->setFontSize(11.f);
		stamp->setColor(0xAAAAAAFF);
		stamp->setMarginRight(4.f);
		stamp->setText(fmtTime(time, L"%Y-%m-%d %H:%M:%S"));
		makeIcon(giCopy, Lang::get(L"ai.copy"), 0.f)->onClick.add([this, index](Ling::Button*) { copyMsg(index); });
	}
	return bubble;
}

void WinAiChat::fillBubble(Ling::Node* bubble, const bool isUser, const std::wstring& text)
{
	if (!bubble) return;
	// 这一条气泡里原有的文字块马上要被销毁，先把指向它们的记录摘掉 —— **只动指针，不碰节点**。
	// 流式输出每 80 ms 就重画一次，走的就是这条路
	selBlocks.erase(std::remove_if(selBlocks.begin(), selBlocks.end(),
		[bubble](Ling::Label* lab) { return underNode(lab, bubble); }), selBlocks.end());

	// 用户问题原样显示：那是用户自己写的原文，把 `**` 之类吃掉反而看不懂他问的是什么
	const auto plain = [bubble, &text]() {
		auto lab = bubble->makeChild<Ling::Label>();
		lab->setWrap(true);
		lab->setMaxWidthPercent(100.f);
		lab->setFontSize(14.f);
		lab->setColor(0x333333FF);
		lab->setLineSpacing(Markdown::lineSpacing);
		lab->setText(text);
	};

	if (isUser) {
		bubble->removeAllChildren();
		plain();
	}
	else {
		// 回答条按 markdown 分块渲染。切不出块（空回答、或者流式刚开始只有几个字）时
		// 退回纯文本，别让气泡空着
		const auto blocks = Markdown::parse(text);
		// ⚠️ 流式输出期间**不能整条重建**：每 80 ms 把气泡里的控件全销毁再新建，
		// 每一遍都要新开一批 composition 绘制表面，正文就会一直闪（"一会消失一会闪现"）。
		// 所以先让前面的块就地改字，只有结构对不上的那几个才重建 ——
		// 从第一个对不上的块开始整段重来，顺序因此不会乱
		size_t keep = 0;
		while (keep < blocks.size() && keep < bubble->children.size()
			&& Markdown::refresh(blocks[keep], bubble->children[keep].get())) {
			++keep;
		}
		while (bubble->children.size() > keep) {
			bubble->removeChild(bubble->children[keep].get());
		}
		if (blocks.empty()) plain();
		else for (size_t i = keep; i < blocks.size(); ++i) Markdown::render(blocks[i], bubble, i == 0);
	}

	// 填完再按视觉顺序收进可选范围。框选只认这些块 —— 用户条与回答条一视同仁
	collectLabels(bubble, selBlocks);
}

void WinAiChat::renderMsgs()
{
	// 重画会把挂着提示的那枚按钮销毁掉，onLeave 不会再来 —— 这里主动收一下
	hideTip();
	// 消息区整棵子树马上要被销毁，选框里记着的那些指针跟着作废（所以只清状态，不回头碰节点）
	resetSelState();
	streamingBubble = nullptr;
	lastStreamPaint = 0;
	msgBox->removeAllChildren();
	for (size_t i = 0; i < msgs.size(); ++i) {
		addItem(msgs[i].role, msgs[i].content, i, msgs[i].time);
	}
	if (!tailText.empty()) addItem(AiService::Role::Assistant, tailText, noIndex, 0);
	if (busy) streamingBubble = addItem(AiService::Role::Assistant, streaming.empty() ? L"..." : streaming, noIndex, 0);
	// 先自己算一次布局再滚：getMaxScrollY() 读的是 content->h，而刚加进去的节点还没过
	// yoga（makeChild 不触发布局，refresh 只是 InvalidateRect），拿到的是上一轮的旧高度 ——
	// 于是"回到底部"永远差着这一次新增的部分，最新那条会被裁在可视区外
	Ling::WinBase::layout();
	// 布局过一次之后才知道滚动条露不露，右内边距要跟着调（折行宽度变了，高度也变，
	// 所以得再算一次布局才拿得到正确的 maxScroll）
	applyMsgPadding();
	Ling::WinBase::layout();
	msgScroller->scrollTo(msgScroller->getMaxScrollY());
}

void WinAiChat::copyMsg(const size_t index)
{
	if (index >= msgs.size()) return;
	if (!msgs[index].content.empty()) Ling::Util::setTextToClipboard(msgs[index].content);
}

void WinAiChat::loadIntoInput(const size_t index)
{
	if (index >= msgs.size()) return;
	// 把这条问题放回输入框让用户改，改完自己再发（不替用户重发：那会多花一次钱，
	// 而且多半还要顺着改几个字）
	input->setText(msgs[index].content);
	input->focus();
}

void WinAiChat::removeRound(const size_t index)
{
	if (index >= msgs.size()) return;
	abortTask();
	msgs.erase(msgs.begin() + static_cast<std::ptrdiff_t>(index));
	// 连带紧接着的那条回答一起删 —— 要的是"删掉这一轮对话"，只删问题会剩一条
	// 回答孤零零挂在上面。没答成（请求失败）时后面本来就没有回答，只删问题
	if (index < msgs.size() && msgs[index].role == AiService::Role::Assistant) {
		msgs.erase(msgs.begin() + static_cast<std::ptrdiff_t>(index));
	}
	if (AiHistory::get()) AiHistory::get()->setMsgs(curId, msgs);
	renderMsgs();
	refreshSessions();
}

void WinAiChat::attach()
{
	// 点这个图标 = 从磁盘挑一张图。Ctrl+V 那条"从剪贴板粘图"走 takeClipboardImage，
	// 两条路各自直给，不再让用户先想"我这张图现在在哪"
	const std::wstring typeName = Lang::get(L"ai.imgFilter");
	const COMDLG_FILTERSPEC filter[]{
		{ typeName.c_str(), L"*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.gif" },
	};
	const auto path = openFileDialog(filter);
	if (path.empty()) return;   // 用户取消
	if (takeImageFile(path)) return;
	// 认不出来的文件给一句提示，别静默什么都不发生
	if (attachBtn) showTip(attachBtn, Lang::get(L"ai.imgFailed"));
}

bool WinAiChat::takeClipboardImage()
{
	std::vector<BYTE> img;
	int w{ 0 }, h{ 0 };
	std::wstring text;
	if (Util::readClipboard(img, w, h, text) != Util::ClipContent::Image) return false;
	pendingImg = std::move(img);
	pendingW = w;
	pendingH = h;
	if (!saveAttachPreview()) return false;
	applyAttachPreview();
	return true;
}

bool WinAiChat::takeImageFile(const std::wstring& path)
{
	std::vector<BYTE> img;
	DWORD w{ 0 }, h{ 0 };
	if (!Util::loadImageBytes(path, img, w, h) || img.empty()) return false;
	pendingImg = std::move(img);
	pendingW = static_cast<int>(w);
	pendingH = static_cast<int>(h);
	if (!saveAttachPreview()) return false;
	applyAttachPreview();
	return true;
}

bool WinAiChat::saveAttachPreview()
{
	// 缩略图控件只认文件路径（Image::loadImg / ImageBox::loadImg），所以要落一次盘。
	// 固定文件名、每次覆盖，放数据目录里 —— 不让这些中间产物出现在截图目录中
	const auto dir = Setting::get()->getDataPath() / L"ai";
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	const auto full = (dir / attachTmpName).wstring();
	if (!Util::saveToFile(full, pendingW, pendingH, pendingImg.data())) return false;
	attachTmp = full;
	return true;
}

void WinAiChat::applyAttachPreview()
{
	if (pendingImg.empty()) {
		attachVisible = false;
		attachStrip->hide();
		return;
	}
	attachInfo->setText(Lang::get(L"ai.imgAttached")
		+ std::to_wstring(pendingW) + L" × " + std::to_wstring(pendingH));
	if (!attachTmp.empty()) attachThumb->loadImg(attachTmp);
	if (!attachVisible) {
		attachVisible = true;
		attachStrip->show();
	}
	// 缩略图的尺寸是它自己量出来的，换图之后要重算一次布局
	refresh();
}

void WinAiChat::clearAttach()
{
	pendingImg.clear();
	pendingW = pendingH = 0;
	if (!attachTmp.empty()) {
		std::error_code ec;
		std::filesystem::remove(attachTmp, ec);
		attachTmp.clear();
	}
	applyAttachPreview();
}

std::wstring WinAiChat::thumbPath(const long long time) const
{
	if (time <= 0 || !Setting::get()) return L"";
	const auto dir = Setting::get()->getDataPath() / L"ai";
	return (dir / (std::wstring{ msgThumbPrefix } + std::to_wstring(time) + L".png")).wstring();
}

void WinAiChat::clearMsgThumbs()
{
	std::error_code ec;
	for (const auto& p : msgThumbs) std::filesystem::remove(p, ec);
	msgThumbs.clear();
}

void WinAiChat::setBusy(const bool on)
{
	busy = on;
	if (!sendBtn) return;
	// 一个图标表达不了"发送 / 停止"两态，换图标 + 换配色一起说（文案在悬停提示里）
	sendBtn->setText(on ? giStop : giSend);
	sendBtn->setBg(on ? 0x9AA0A6FF : 0x4D6BFEFF);
	sendBtn->setHoverBg(on ? 0x7C848BFF : 0x3B5BDBFF);
}

void WinAiChat::refreshModelBtn()
{
	if (!modelBtn) return;
	// credFor 即使配不齐也会把"选了哪个接口、哪个模型"尽量填上（ok 只表示能不能真发请求），
	// 所以按钮上半句照常显示"现在指的是哪一套"，好让用户知道要去改哪儿
	const auto cred = Setting::get()->credFor(std::wstring{ AiScenario::chat });
	if (cred.providerName.empty() && cred.model.empty()) {
		modelLabel = Lang::get(L"ai.noProvider");
	}
	else {
		modelLabel = cred.providerName + L" - " + cred.model;
	}
	modelBtn->setText(modelLabel);
}

void WinAiChat::abortTask()
{
	// 在飞的那次请求的回调全部作废（迟到的回调不能再往消息区里写东西）
	++sendGen;
	if (task) {
		task->cancel();
		task.reset();
	}
	streamingBubble = nullptr;
	streaming.clear();
	if (busy) setBusy(false);
}

void WinAiChat::send()
{
	if (!AiService::ready(std::wstring{ AiScenario::chat })) {
		// 没配好就只说一句：这一条不该混进 msgs，也不该落历史
		tailText = Lang::get(L"ai.noKey");
		renderMsgs();
		return;
	}
	auto text = input->getText();
	// 首尾空白不算内容：粘进来的文字常常带一堆换行
	while (!text.empty() && (text.front() == L'\n' || text.front() == L' ' || text.front() == L'\r')) text.erase(0, 1);
	while (!text.empty() && (text.back() == L'\n' || text.back() == L' ' || text.back() == L'\r')) text.pop_back();
	if (text.empty() && pendingImg.empty()) return;
	// 第一次发言才真的建会话：开了一个却一句话没说就关掉，历史里不该留下空位
	if (curId == 0 && AiHistory::get()) curId = AiHistory::get()->create();

	AiService::Msg user;
	user.role = AiService::Role::User;
	user.content = text;
	user.image = std::move(pendingImg);
	user.imgW = pendingW;
	user.imgH = pendingH;
	user.time = AiHistory::now();
	// 回显用的那份缩略图：输入框里那份临时图马上要被 clearAttach 删掉，先按这条消息的
	// 时间戳另存一份（文件名唯一，不必担心重名）。图不进历史，这份只在当前会话里有用
	if (!user.image.empty() && !attachTmp.empty()) {
		const auto path = thumbPath(user.time);
		if (!path.empty()) {
			std::error_code ec;
			std::filesystem::copy_file(attachTmp, path,
				std::filesystem::copy_options::overwrite_existing, ec);
			if (!ec) msgThumbs.push_back(path);
		}
	}
	// 图已经交给这条消息了，输入框里的预览与那份临时文件都没用了
	pendingW = pendingH = 0;
	clearAttach();
	input->setText(L"");
	// 图留在 msgs 里：多轮对话时模型得一直"看得见"这张图，下一轮才会接着它答。
	// 代价是每轮都要重编一次 base64 —— 认一次几十毫秒，比答错划算
	msgs.push_back(std::move(user));
	if (AiHistory::get()) AiHistory::get()->append(curId, msgs.back());

	tailText.clear();
	streaming.clear();
	setBusy(true);
	renderMsgs();
	refreshSessions();     // 标题与"最近对话时间"都跟着变了

	const auto gen = ++sendGen;
	auto aliveFlag = alive;
	task = AiService::chat(std::wstring{ AiScenario::chat }, msgs,
		[this, aliveFlag, gen](const std::wstring& delta) {
			if (!*aliveFlag || gen != sendGen || !streamingBubble) return;
			streaming += delta;
			// 每来一小段就整条重画太浪费（markdown 要重新切块 + 重建控件），攒够间隔
			// 再画一次。收尾的 renderMsgs 一定会画最后一遍，所以这里跳过的那些次
			// 不会留下没渲染的内容
			const long long now = static_cast<long long>(GetTickCount64());
			if (now - lastStreamPaint < streamPaintMs) return;
			lastStreamPaint = now;
			fillBubble(streamingBubble, false, streaming);
			// 先把这一帧的布局算出来再滚：getMaxScrollY() 读的是 content->h，
			// 不先布局拿到的是上一轮的旧高度，贴在底部的那一行会来回弹（看着也是"闪"）
			Ling::WinBase::layout();
			// 内容长到刚把滚动条撑出来的那一下，右内边距要跟着让开，否则最新那几行会压到滑块底下
			const float padBefore = msgRightPad;
			applyMsgPadding();
			if (msgRightPad != padBefore) Ling::WinBase::layout();
			msgScroller->scrollTo(msgScroller->getMaxScrollY());
		},
		[this, aliveFlag, gen](const std::wstring& err) {
			if (!*aliveFlag || gen != sendGen) return;
			task.reset();
			setBusy(false);
			if (!err.empty()) {
				// 取消是把话说到一半掐断，前面那些已经出来的字应当留着 —— 但它不是
				// 模型完整的回答，所以不进 msgs、也不落历史
				tailText = streaming.empty() ? err : streaming + L"\n" + err;
			}
			else {
				AiService::Msg reply;
				reply.role = AiService::Role::Assistant;
				reply.content = streaming;
				reply.time = AiHistory::now();
				msgs.push_back(reply);
				if (AiHistory::get()) AiHistory::get()->append(curId, reply);
			}
			streamingBubble = nullptr;
			renderMsgs();
			refreshSessions();
		});
	msgScroller->scrollTo(msgScroller->getMaxScrollY());
}
