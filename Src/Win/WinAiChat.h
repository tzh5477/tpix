#pragma once
#include <include/Ling.h>
#include <memory>
#include <vector>
#include "../AiService.h"

// AI 对话窗。左侧是历史会话，右侧是消息区 + 输入区。
//
// 上下文的处理：**每次都把整个会话的 messages 全发出去** —— 兼容接口是无状态的，
// 想让它"记得前面说过什么"只有这一条路。会话内容来自 AiHistory（内存里那份），
// 图不落历史，所以重开一个旧会话时那张图是不在的。
//
// 流式：收到的每个 delta 追加到同一个气泡上，收尾（onDone）才把完整这一条写进历史。
// 收尾时整个消息区会重画一遍 —— 那样这一条才拿得到"正式消息"才有的时间与按钮，
// 也省掉了一堆增量维护的老消息索引。
class WinAiChat : public Ling::WinBase
{
public:
	~WinAiChat();
	// preset 非空时填进输入框等用户自己按发送（不代发：对话是要花钱的，
	// 而且多半还要再加一句要求）。热键带选中内容进来走的就是这条路
	static void init(const std::wstring& preset = L"");
	static void dispose();
private:
	WinAiChat();
	void onCreated() override;
	LRESULT onHitTest(const POINT pos) override;
	BOOL setCursor() override;
	void buildSidebar(Ling::Node* side);
	// 带进来的选中文本（热键那条路）。必须在构造之后调：控件是在 onCreated 里建出来的，
	// 而 onCreated 在构造函数里就跑完了
	void applyPreset(const std::wstring& text);
	// 重画左侧会话列表。最多显示 maxSessions 条：列表刻意不套 ScrollerBox ——
	// 滚动容器里可点控件的命中坐标要减去滚动量（WinSetting 里那套补偿），
	// 这里犯不上为十几条历史引入那套补偿逻辑。正在改名的那一行会重建成输入框
	void refreshSessions();
	// 打开一个会话。id 为 0 表示"新对话"（还没建）
	void openSession(long long id);
	void removeSession(long long id);
	void beginRename(long long id);
	// raw 是输入框里的原文。id 用来核对"这一下改的还是同一条"——回车确认与
	// 点到别处提交都走它，改名中途切走时靠它作废
	void commitRename(long long id, const std::wstring& raw);
	// 重画整个消息区（正在流式输出时末尾会多一条占位气泡）
	void renderMsgs();
	// 往消息区追加一条：气泡 + 底下那排按钮。返回气泡本身（流式输出要往它上面追字）。
	// index 是它在 msgs 里的下标；noIndex 表示"还没进 msgs 的流式占位"（没有按钮）
	Ling::Label* addItem(AiService::Role role, const std::wstring& text, size_t index, long long time);
	void send();
	// 掐掉在飞的那次请求（换会话 / 删掉正在回答的那一轮之前必须先做）
	void abortTask();
	void loadIntoInput(size_t index);
	void removeRound(size_t index);
	void copyMsg(size_t index);
	void attachFromClipboard();
	// 剪贴板里是图就挂到下一句上，返回是否拿到了
	bool takeClipboardImage();
	void applyAttachTip();
	void setBusy(bool on);
	// 分割线以下那一整块的高度（逻辑像素）
	void setInputAreaH(float logical);
	// 消息区里的按钮都挂在 ScrollerBox 里，命中坐标要减去滚动量（同 WinSetting）
	void syncScrollHitCoords();
private:
	long long curId{ 0 };
	std::vector<AiService::Msg> msgs;
	AiService::TaskPtr task{ nullptr };
	Ling::Node* sessionBox{ nullptr };
	Ling::ScrollerBox* msgScroller{ nullptr };
	Ling::Node* msgBox{ nullptr };
	Ling::Node* divider{ nullptr };
	Ling::Node* bottom{ nullptr };
	Ling::TextBox* input{ nullptr };
	Ling::Button* sendBtn{ nullptr };
	Ling::Label* attachTip{ nullptr };
	// 正在流式输出的那条气泡。收尾之前所有 delta 都追加到它上面
	Ling::Label* streamingBubble{ nullptr };
	std::wstring streaming;
	// 不属于历史、只临时挂在末尾的一段话：没配好时的提示，或者被取消/失败时已经
	// 吐出来的半截回答。下次重画（发消息 / 换会话）就没了
	std::wstring tailText;
	std::vector<BYTE> pendingImg;
	int pendingW{ 0 }, pendingH{ 0 };
	bool busy{ false };
	// 每次发问 +1。掐掉在飞的请求时也 +1，让那一次的回调认不出自己、直接作废
	long long sendGen{ 0 };
	// 会话列表里正在改名的那一条（0 = 没有）
	long long renamingId{ 0 };
	Ling::TextBox* renameBox{ nullptr };
	float inputAreaH{ 130.f };
	bool dividerDragging{ false }, dividerHover{ false };
	float dragStartY{ 0.f }, dragStartH{ 0.f };
	// 窗口关掉 / 被销毁之后，迟到的回调不能再碰这些节点（postDone 是"取消也照送"的）
	std::shared_ptr<bool> alive{ std::make_shared<bool>(true) };
};
