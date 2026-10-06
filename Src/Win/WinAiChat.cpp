#include "pch.h"
#include <algorithm>
#include <format>
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
			if (!preset.empty()) winAiChat->input->setText(preset);
			winAiChat->show();
			SetForegroundWindow(winAiChat->hwnd);
			return;
		}
		winAiChat.reset();
	}
	winAiChat.reset(new WinAiChat());
	winAiChat->presetText = preset;
	if (winAiChat->hwnd) SetForegroundWindow(winAiChat->hwnd);
}

void WinAiChat::dispose()
{
	winAiChat.reset();
}

void WinAiChat::onCreated()
{
	enableShadow();
	body->setBg(0xFFFFFFFF);
	body->setFlexDirection(Ling::FlexDirection::Row);

	auto side = body->makeChild<Ling::Node>();
	side->setWidth(180.f);
	side->setHeightPercent(100.f);
	side->setBg(0xF7F7F9FF);
	side->setFlexDirection(Ling::FlexDirection::Column);
	side->setPadding(8.f, 8.f, 8.f, 8.f);
	buildSidebar(side);

	auto right = body->makeChild<Ling::Node>();
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

	auto bottom = right->makeChild<Ling::Node>();
	bottom->setWidthPercent(100.f);
	bottom->setFlexDirection(Ling::FlexDirection::Column);
	bottom->setPadding(8.f, 8.f, 8.f, 8.f);

	attachTip = bottom->makeChild<Ling::Label>();
	attachTip->setFontSize(12.f);
	attachTip->setColor(0x597EF7FF);
	attachTip->setMarginBottom(4.f);

	auto inputRow = bottom->makeChild<Ling::Node>();
	inputRow->setWidthPercent(100.f);
	inputRow->setFlexDirection(Ling::FlexDirection::Row);
	inputRow->setHeight(60.f);

	input = inputRow->makeChild<Ling::TextBox>();
	input->setFlexGrow(1.f);
	input->setHeightPercent(100.f);
	input->setFontSize(14.f);
	input->setBorder(1.f, 0xE0E0E0FF);
	input->setPadding(8.f, 6.f, 8.f, 6.f);
	input->setPlaceholder(Lang::get(L"ai.placeholder"));

	auto btnCol = inputRow->makeChild<Ling::Node>();
	btnCol->setWidth(84.f);
	btnCol->setMarginLeft(8.f);
	btnCol->setFlexDirection(Ling::FlexDirection::Column);

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

	if (!presetText.empty()) {
		input->setText(presetText);
		presetText.clear();
	}
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
	while (!sessionBox->children.empty()) {
		sessionBox->removeChild(sessionBox->children.front().get());
	}
	auto sessions = AiHistory::get() ? AiHistory::get()->list() : std::vector<AiHistory::Session>{};
	auto n = std::min((int)sessions.size(), maxSessions);
	for (int i = 0; i < n; ++i) {
		auto& s = sessions[i];
		auto btn = sessionBox->makeChild<Ling::Button>();
		btn->setHeight(30.f);
		btn->setWidthPercent(100.f);
		btn->setMarginBottom(2.f);
		btn->setFontSize(12.f);
		// 一条都没说过话的会话没有标题（create 出来的空会话不写盘，所以这里只会是
		// 已经说过话但标题被清掉的情况），给个占位，别让按钮空着
		btn->setText(s.title.empty() ? Lang::get(L"ai.newChat") : s.title);
		btn->setBorder(1.f, s.id == curId ? 0x597EF7FF : 0xE0E0E0FF);
		btn->setHoverBg(0xFFFFFFFF);
		auto id = s.id;
		btn->onClick.add([this, id](Ling::Button*) { openSession(id); });
	}
}

void WinAiChat::openSession(const long long id)
{
	curId = id;
	msgs.clear();
	pendingImg.clear();
	pendingW = pendingH = 0;
	streamingBubble = nullptr;
	streaming.clear();
	applyAttachTip();
	while (!msgBox->children.empty()) {
		msgBox->removeChild(msgBox->children.front().get());
	}
	if (id == 0) {
		refreshSessions();
		return;
	}
	auto session = AiHistory::get() ? AiHistory::get()->find(id) : nullptr;
	if (session) {
		for (auto& msg : session->msgs) {
			msgs.push_back(msg);
			addBubble(msg.role, msg.content);
		}
	}
	refreshSessions();
}

Ling::Label* WinAiChat::addBubble(const AiService::Role role, const std::wstring& text)
{
	auto bubble = msgBox->makeChild<Ling::Label>();
	bubble->setText(text);
	bubble->setFontSize(14.f);
	bubble->setPadding(10.f, 8.f, 10.f, 8.f);
	bubble->setMarginBottom(8.f);
	bubble->setBorderRadius(6.f);
	bubble->setBg(role == AiService::Role::User ? 0xE6F4FFFF : 0xF2F2F5FF);
	return bubble;
}

void WinAiChat::attachFromClipboard()
{
	std::vector<BYTE> img;
	int w{ 0 }, h{ 0 };
	std::wstring text;
	if (Util::readClipboard(img, w, h, text) != Util::ClipContent::Image) {
		attachTip->setText(Lang::get(L"ai.imgNone"));
		return;
	}
	pendingImg = std::move(img);
	pendingW = w;
	pendingH = h;
	applyAttachTip();
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
	sendBtn->setText(Lang::get(on ? L"ai.stop" : L"ai.send"));
}

void WinAiChat::send()
{
	if (!AiService::ready()) {
		addBubble(AiService::Role::Assistant, Lang::get(L"ai.noKey"));
		return;
	}
	auto text = input->getText();
	if (text.empty() && pendingImg.empty()) return;
	// 第一次发言才真的建会话：开了一个却一句话没说就关掉，历史里不该留下空位
	if (curId == 0 && AiHistory::get()) {
		curId = AiHistory::get()->create();
		refreshSessions();
	}

	AiService::Msg user;
	user.role = AiService::Role::User;
	user.content = text;
	user.image = std::move(pendingImg);
	user.imgW = pendingW;
	user.imgH = pendingH;
	pendingW = pendingH = 0;
	applyAttachTip();
	addBubble(AiService::Role::User, text);
	input->setText(L"");

	// 图留在 msgs 里：多轮对话时模型得一直"看得见"这张图，下一轮才会接着它答。
	// 代价是每轮都要重编一次 base64 —— 认一次几十毫秒，比答错划算
	msgs.push_back(user);
	if (AiHistory::get()) AiHistory::get()->append(curId, user);

	streaming.clear();
	streamingBubble = addBubble(AiService::Role::Assistant, L"...");
	setBusy(true);
	auto aliveFlag = alive;
	task = AiService::chat(msgs,
		[this, aliveFlag](const std::wstring& delta) {
			if (!*aliveFlag || !streamingBubble) return;
			streaming += delta;
			streamingBubble->setText(streaming);
			msgScroller->scrollTo(msgScroller->getMaxScrollY());
		},
		[this, aliveFlag](const std::wstring& err) {
			if (!*aliveFlag) return;
			task.reset();
			setBusy(false);
			if (!err.empty()) {
				// 取消是把话说到一半掐断，前面那些已经出来的字应当留着
				streaming += L"\n" + err;
				if (streamingBubble) streamingBubble->setText(streaming);
			}
			else {
				msgs.push_back(AiService::Msg{ AiService::Role::Assistant, streaming });
				if (AiHistory::get()) AiHistory::get()->append(curId,
					AiService::Msg{ AiService::Role::Assistant, streaming });
			}
			streamingBubble = nullptr;
			msgScroller->scrollTo(msgScroller->getMaxScrollY());
		});
	msgScroller->scrollTo(msgScroller->getMaxScrollY());
}
