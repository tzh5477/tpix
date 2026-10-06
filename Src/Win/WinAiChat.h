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
	void buildSidebar(Ling::Node* side);
	// 重画左侧会话列表。最多显示 maxSessions 条：列表刻意不套 ScrollerBox ——
	// 滚动容器里可点控件的命中坐标要减去滚动量（WinSetting 里那套补偿），
	// 这里犯不上为十几条历史引入那套补偿逻辑
	void refreshSessions();
	// 打开一个会话。id 为 0 表示"新对话"（还没建）
	void openSession(long long id);
	Ling::Label* addBubble(AiService::Role role, const std::wstring& text);
	void send();
	// 从剪贴板取一张图挂到下一次发送上（tpix 截完图就在剪贴板上，表格提取走的正是这条路）
	void attachFromClipboard();
	void applyAttachTip();
	void setBusy(bool on);
private:
	long long curId{ 0 };
	std::vector<AiService::Msg> msgs;
	AiService::TaskPtr task{ nullptr };
	Ling::Node* sessionBox{ nullptr };
	Ling::ScrollerBox* msgScroller{ nullptr };
	Ling::Node* msgBox{ nullptr };
	Ling::TextBox* input{ nullptr };
	Ling::Button* sendBtn{ nullptr };
	Ling::Label* attachTip{ nullptr };
	// 正在流式输出的那条气泡。收尾之前所有 delta 都追加到它上面
	Ling::Label* streamingBubble{ nullptr };
	std::wstring streaming;
	// 窗口构造之后才能填进输入框（控件在 onCreated 里才建出来），所以先存在这里
	std::wstring presetText;
	std::vector<BYTE> pendingImg;
	int pendingW{ 0 }, pendingH{ 0 };
	bool busy{ false };
	// 窗口关掉 / 被销毁之后，迟到的回调不能再碰这些节点（postDone 是"取消也照送"的）
	std::shared_ptr<bool> alive{ std::make_shared<bool>(true) };
};
