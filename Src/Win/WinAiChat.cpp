#include "pch.h"
#include <algorithm>
#include <cstddef>
#include <ctime>
#include "../AiHistory.h"
#include "../AiService.h"
#include "../Lang.h"
#include "../Setting.h"
#include "../Util.h"
#include "WinAiChat.h"

namespace {
	std::unique_ptr<WinAiChat> winAiChat;
	// 侧栏最多列多少条。超出就只显示最近的这些 —— 更老的那批靠"保留天数 / 条数"自己清掉
	constexpr int maxSessions{ 12 };
	// "这一条还没进 msgs"：流式输出中的占位气泡，以及没配好时的提示
	constexpr size_t noIndex{ static_cast<size_t>(-1) };
	// 输入区高低的上下限（逻辑像素）
	constexpr float minInputH{ 96.f }, maxInputH{ 420.f };
	// 侧栏那一行里名字最多显示几个字。行宽 200 上下，右边还要摆日期和两个按钮，
	// 不截的话文字会直接压到日期上（Label 既不折行也不会省略号）。
	// ⚠️ 这个数必须保证"文字自然宽度 < nameColW"，见 buildSidebar 里那段宽度配平：
	// 名字那枚按钮一旦被压得比文字还窄，Button 内部是居中排的，文字会从左边溢出去
	// （实测长标题会被裁在侧栏左边缘、还压住日期）。5 个字 × 12 号 = 60，加左内边距
	// 和省略号仍在 88 之内，留了余量。
	constexpr size_t nameMax{ 5 };
	// 名字那一格写死的宽度（逻辑像素）。200 - 左右 padding 16 - 日期 4+45
	// - ✎ 2+20 - × 2+20 = 91，取 88 留一点余量
	constexpr float nameColW{ 88.f };

	// 毫秒时间戳 -> 本地时间文本。withTime 为真时带上时分秒（模型回答的落款要精确到秒），
	// 否则只要年月日（会话列表右上那一列）
	std::wstring fmtTime(const long long ms, const bool withTime)
	{
		if (ms <= 0) return L"";
		const std::time_t t = static_cast<std::time_t>(ms / 1000);
		std::tm tm{};
		if (localtime_s(&tm, &t) != 0) return L"";
		wchar_t buf[64]{};
		if (wcsftime(buf, 64, withTime ? L"%Y-%m-%d %H:%M:%S" : L"%Y-%m-%d", &tm) == 0) return L"";
		return buf;
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
		if (isRight || !divider || !divider->isPosIn(pos)) return;
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
		const bool on = divider->isPosIn(pos);
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
	// （"回答完了按钮就消失"就是这么来的）
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

	msgScroller = right->makeChild<Ling::ScrollerBox>();
	msgScroller->setFlexGrow(1.f);
	msgScroller->setWidthPercent(100.f);
	msgBox = msgScroller->makeChild<Ling::Node>();
	msgBox->setWidthPercent(100.f);
	msgBox->setFlexDirection(Ling::FlexDirection::Column);
	msgBox->setPadding(12.f, 12.f, 12.f, 12.f);

	divider = right->makeChild<Ling::Node>();
	divider->setWidthPercent(100.f);
	divider->setHeight(6.f);
	divider->setBg(0xE8E8ECFF);

	bottom = right->makeChild<Ling::Node>();
	bottom->setWidthPercent(100.f);
	bottom->setHeight(inputAreaH);
	bottom->setFlexDirection(Ling::FlexDirection::Column);
	bottom->setPadding(8.f, 8.f, 8.f, 8.f);

	attachTip = bottom->makeChild<Ling::Label>();
	attachTip->setFontSize(12.f);
	attachTip->setColor(0x597EF7FF);
	attachTip->setMarginBottom(4.f);

	auto inputRow = bottom->makeChild<Ling::Node>();
	inputRow->setWidthPercent(100.f);
	inputRow->setFlexGrow(1.f);
	// 输入框要比它显示的文本高一点，压得太狠时允许缩
	inputRow->setFlexShrink(1.f);
	inputRow->setFlexDirection(Ling::FlexDirection::Row);

	input = inputRow->makeChild<Ling::TextBox>();
	input->setFlexGrow(1.f);
	input->setHeightPercent(100.f);
	input->setFontSize(14.f);
	input->setBorder(1.f, 0xE0E0E0FF);
	input->setPadding(8.f, 6.f, 8.f, 6.f);
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
			// 剪贴板里是图就直接挂到这一句上，别再要求用户先存成文件走"粘贴图片"按钮
			if (takeClipboardImage()) *handled = true;
		}
	});

	auto btnCol = inputRow->makeChild<Ling::Node>();
	btnCol->setWidth(84.f);
	btnCol->setMarginLeft(8.f);
	btnCol->setFlexDirection(Ling::FlexDirection::Column);
	// 输入区被拖高之后按钮还挤在顶上不好看，居中摆
	btnCol->setJustifyContent(Ling::Justify::Center);

	sendBtn = btnCol->makeChild<Ling::Button>();
	sendBtn->setHeight(28.f);
	sendBtn->setWidthPercent(100.f);
	sendBtn->setBorder(1.f, 0xE0E0E0FF);
	sendBtn->setHoverBg(0xF2F2F2FF);
	sendBtn->setText(Lang::get(L"ai.send"));
	sendBtn->onClick.add([this](Ling::Button*) {
		if (busy) {
			if (task) task->cancel();
			return;
		}
		send();
	});

	auto imgBtn = btnCol->makeChild<Ling::Button>();
	imgBtn->setHeight(28.f);
	imgBtn->setWidthPercent(100.f);
	imgBtn->setMarginTop(4.f);
	imgBtn->setBorder(1.f, 0xE0E0E0FF);
	imgBtn->setHoverBg(0xF2F2F2FF);
	imgBtn->setText(Lang::get(L"ai.attachImg"));
	imgBtn->onClick.add([this](Ling::Button*) { attachFromClipboard(); });

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
			if (dividerDragging || divider->isPosIn(pt)) {
				SetCursor(LoadCursor(nullptr, IDC_SIZENS));
				return TRUE;
			}
		}
	}
	return Ling::WinBase::setCursor();
}

void WinAiChat::syncScrollHitCoords()
{
	if (!msgScroller || !msgBox) return;
	const float scrollY = msgScroller->getScrollY();
	// 没滚动就什么都别动：鼠标移动时这里是热路径
	if (scrollY == 0.f) return;
	shiftForScroll(msgBox, msgBox->y, scrollY);
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
	renameBox = nullptr;
	while (!sessionBox->children.empty()) {
		sessionBox->removeChild(sessionBox->children.front().get());
	}
	auto sessions = AiHistory::get() ? AiHistory::get()->list() : std::vector<AiHistory::Session>{};
	const int n = std::min(static_cast<int>(sessions.size()), maxSessions);
	for (int i = 0; i < n; ++i) {
		auto& s = sessions[i];
		const auto id = s.id;
		auto row = sessionBox->makeChild<Ling::Node>();
		row->setWidthPercent(100.f);
		row->setHeight(30.f);
		row->setMarginBottom(2.f);
		row->setFlexDirection(Ling::FlexDirection::Row);
		row->setAlignItems(Ling::Align::Center);

		// 正在改名的那一条：名字那一格换成输入框。回车提交；点到别处由输入框自己
		// 失焦，也在那里提交
		if (renamingId != 0 && id == renamingId) {
			auto box = row->makeChild<Ling::TextBox>();
			box->setFlexGrow(1.f);
			box->setHeightPercent(100.f);
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

		auto nameBtn = row->makeChild<Ling::Button>();
		// 宽度写死，别用 flexGrow —— flexGrow 是在"内容自然宽度"之上再分剩余空间，
		// 长标题会把这一格撑到行宽之外
		nameBtn->setWidth(nameColW);
		nameBtn->setHeightPercent(100.f);
		nameBtn->setFontSize(12.f);
		nameBtn->setJustifyContent(Ling::Justify::Start);   // 名字靠左
		nameBtn->setPaddingLeft(6.f);
		// 一条都没说过话的会话没有标题（create 出来的空会话不写盘，所以这里只会是
		// 已经说过话但标题被清掉的情况），给个占位，别让按钮空着
		nameBtn->setText(clipName(s.title.empty() ? Lang::get(L"ai.newChat") : s.title));
		nameBtn->setBorder(1.f, id == curId ? 0x597EF7FF : 0xE0E0E0FF);
		nameBtn->setHoverBg(0xEEF4FFFF);
		nameBtn->onClick.add([this, id](Ling::Button*) { openSession(id); });

		auto date = row->makeChild<Ling::Label>();
		date->setFontSize(10.f);
		date->setColor(0x999999FF);
		date->setMarginLeft(4.f);
		date->setText(fmtTime(s.time, false));

		auto editBtn = row->makeChild<Ling::Button>();
		editBtn->setSize(20.f, 22.f);
		editBtn->setMarginLeft(2.f);
		editBtn->setFontSize(12.f);
		editBtn->setColor(0x888888FF);
		editBtn->setHoverColor(0x333333FF);
		editBtn->setHoverBg(0xEEEEEEFF);
		editBtn->setText(L"\u270E");   // ✎
		editBtn->onClick.add([this, id](Ling::Button*) { beginRename(id); });

		auto delBtn = row->makeChild<Ling::Button>();
		delBtn->setSize(20.f, 22.f);
		delBtn->setMarginLeft(2.f);
		delBtn->setFontSize(12.f);
		delBtn->setColor(0x888888FF);
		delBtn->setHoverColor(0xE81123FF);
		delBtn->setHoverBg(0xFFECECFF);
		delBtn->setText(L"\u00D7");    // ×
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
	curId = id;
	msgs.clear();
	pendingImg.clear();
	pendingW = pendingH = 0;
	tailText.clear();
	renamingId = 0;
	renameBox = nullptr;
	applyAttachTip();
	if (id != 0) {
		if (auto session = AiHistory::get() ? AiHistory::get()->find(id) : nullptr) msgs = session->msgs;
	}
	if (input) input->setText(L"");
	renderMsgs();
	refreshSessions();
}

Ling::Label* WinAiChat::addItem(const AiService::Role role, const std::wstring& text,
	const size_t index, const long long time)
{
	const bool isUser = role == AiService::Role::User;

	auto item = msgBox->makeChild<Ling::Node>();
	item->setWidthPercent(100.f);
	item->setFlexDirection(Ling::FlexDirection::Column);
	item->setMarginBottom(10.f);

	auto bubble = item->makeChild<Ling::Label>();
	// 折行必须开：不开的话整段回答排成一整行，既看不全，那一整行的宽度还会顺着
	// flex 基准把外层布局撑爆
	bubble->setWrap(true);
	bubble->setWidthPercent(100.f);
	bubble->setText(text);
	bubble->setFontSize(14.f);
	bubble->setColor(0x333333FF);
	bubble->setPadding(10.f, 8.f, 10.f, 8.f);
	bubble->setBorderRadius(6.f);
	bubble->setBg(isUser ? 0xE6F4FFFF : 0xF2F2F5FF);

	// 流式占位（还有"没配好"的提示）：还没有落款时间，也没有成品的按钮
	if (index == noIndex) return bubble;

	auto actions = item->makeChild<Ling::Node>();
	actions->setWidthPercent(100.f);
	actions->setHeight(22.f);
	actions->setMarginTop(4.f);
	actions->setFlexDirection(Ling::FlexDirection::Row);
	actions->setAlignItems(Ling::Align::Center);

	// 小按钮：宽度不写死，跟着文字量（Button 宽度 auto 时量出来的就是那两个字 + 内边距）
	auto makeSmall = [&actions](const std::wstring& label, const float marginLeft) {
		auto btn = actions->makeChild<Ling::Button>();
		btn->setHeight(20.f);
		btn->setFontSize(11.f);
		btn->setPadding(8.f, 0.f, 8.f, 0.f);
		btn->setMarginLeft(marginLeft);
		btn->setColor(0x888888FF);
		btn->setHoverColor(0x333333FF);
		btn->setHoverBg(0xF2F2F2FF);
		btn->setText(label);
		return btn;
	};

	if (isUser) {
		// 问题这一条：复制 / 编辑 / 删除（删除删的是整轮，见 removeRound）
		actions->setJustifyContent(Ling::Justify::Start);
		makeSmall(Lang::get(L"ai.copy"), 0.f)->onClick.add([this, index](Ling::Button*) { copyMsg(index); });
		makeSmall(Lang::get(L"ai.edit"), 6.f)->onClick.add([this, index](Ling::Button*) { loadIntoInput(index); });
		makeSmall(Lang::get(L"ai.delete"), 6.f)->onClick.add([this, index](Ling::Button*) { removeRound(index); });
	}
	else {
		// 回答这一条：落款（精确到秒）与复制，都贴右边
		actions->setJustifyContent(Ling::Justify::End);
		auto stamp = actions->makeChild<Ling::Label>();
		stamp->setFontSize(11.f);
		stamp->setColor(0xAAAAAAFF);
		stamp->setMarginRight(4.f);
		stamp->setText(fmtTime(time, true));
		makeSmall(Lang::get(L"ai.copy"), 0.f)->onClick.add([this, index](Ling::Button*) { copyMsg(index); });
	}
	return bubble;
}

void WinAiChat::renderMsgs()
{
	streamingBubble = nullptr;
	msgBox->removeAllChildren();
	for (size_t i = 0; i < msgs.size(); ++i) {
		addItem(msgs[i].role, msgs[i].content, i, msgs[i].time);
	}
	if (!tailText.empty()) addItem(AiService::Role::Assistant, tailText, noIndex, 0);
	if (busy) streamingBubble = addItem(AiService::Role::Assistant, streaming.empty() ? L"..." : streaming, noIndex, 0);
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

void WinAiChat::attachFromClipboard()
{
	if (!takeClipboardImage()) attachTip->setText(Lang::get(L"ai.imgNone"));
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
	applyAttachTip();
	return true;
}

void WinAiChat::applyAttachTip()
{
	if (pendingImg.empty()) {
		attachTip->setText(L"");
		return;
	}
	attachTip->setText(Lang::get(L"ai.imgAttached")
		+ std::to_wstring(pendingW) + L" × " + std::to_wstring(pendingH));
}

void WinAiChat::setBusy(const bool on)
{
	busy = on;
	if (sendBtn) sendBtn->setText(Lang::get(on ? L"ai.stop" : L"ai.send"));
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
	if (!AiService::ready()) {
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
	pendingImg.clear();
	pendingW = pendingH = 0;
	applyAttachTip();
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
	task = AiService::chat(msgs,
		[this, aliveFlag, gen](const std::wstring& delta) {
			if (!*aliveFlag || gen != sendGen || !streamingBubble) return;
			streaming += delta;
			streamingBubble->setText(streaming);
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
