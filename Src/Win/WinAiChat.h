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
	// 重画左侧会话列表。按"30 天内 / 年-月"分组，组内与组间都是时间新的在前。
	// 列表刻意不套 ScrollerBox —— 滚动容器里可点控件的命中坐标要减去滚动量
	//（WinSetting 里那套补偿），这里犯不上为十几条历史引入那套补偿逻辑。
	// 正在改名的那一行会重建成输入框
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
	// 往消息区追加一条：气泡 + 底下那排图标。返回气泡容器本身（流式输出要往它上面追字）。
	// index 是它在 msgs 里的下标；noIndex 表示"还没进 msgs 的流式占位"（没有按钮）
	Ling::Node* addItem(AiService::Role role, const std::wstring& text, size_t index, long long time);
	// 往气泡容器里填内容：用户条是一段普通文字，回答条按 markdown 分块渲染。
	// 流式输出每收到一段就重跑一次这个（整条重画，不做增量维护）
	void fillBubble(Ling::Node* bubble, bool isUser, const std::wstring& text);
	void send();
	// 掐掉在飞的那次请求（换会话 / 删掉正在回答的那一轮之前必须先做）
	void abortTask();
	void loadIntoInput(size_t index);
	void removeRound(size_t index);
	void copyMsg(size_t index);
	// 挂附件：弹文件选择框挑一张图（Ctrl+V 那条"从剪贴板粘图"是另一条路）
	void attach();
	bool takeClipboardImage();
	bool takeImageFile(const std::wstring& path);
	// 把待发附件落成一张缩放前的原图 PNG（缩略图控件只认文件路径），
	// 返回是否写成功并记进 attachTmp
	bool saveAttachPreview();
	// 把待发附件重新摆到输入框里（缩略图 + 尺寸文字）；没有附件就把整条藏起来
	void applyAttachPreview();
	// 丢掉待发附件：清内存、删缩略图临时文件、收起预览
	void clearAttach();
	// 用户条上回显发出去的图，靠的是一张落盘的临时缩略图（ImageBox 只认文件路径）。
	// 路径由消息时间戳推出来，见 thumbPath
	std::wstring thumbPath(long long time) const;
	// 删掉所有回显用的临时缩略图。换会话、关窗时调 —— 图不进历史，重开旧会话时
	// 这些文件已经没有对应的消息了
	void clearMsgThumbs();
	void setBusy(bool on);
	// 输入区里「接口 - 模型」那一枚按钮。需求要的就是"问答之前可以换一个模型"，
	// 它显示的是 chat 这个场景当前绑的那一套，选完改的就是那套绑定（下次开还在）
	void refreshModelBtn();
	// 分割线以下那一整块的高度（逻辑像素）
	void setInputAreaH(float logical);
	// 消息区里的按钮都挂在 ScrollerBox 里，命中坐标要减去滚动量（同 WinSetting）
	void syncScrollHitCoords();
	// 给消息区右侧让出滚动条那一条竖带（滚动条浮在最右、不占布局宽度）。得在布局
	// 之后调 —— 它露不露取决于内容高不高
	void applyMsgPadding();
	// 图标按钮的悬停提示。Ling 没有 tooltip 控件，自己在 body 上摆一个浮层
	void showTip(Ling::Node* anchor, const std::wstring& text);
	void hideTip();
	// ---- 对话区文字框选 ----
	// 窗口坐标 -> (第几个文本块, 块内字符位置)。压住哪一块就取哪一块；都压不住时按纵向
	// 取最近的一块并落到它的首/尾 —— 拖出文字区（更上面的块、更下面的块）也要能接着选。
	//
	// allowFallback=false 时只要没真压在文字上就返回 false：起手用这个口径，不然在
	// 消息区里点一下图标、点一下空白，都会在"纵向最近"的那一块上糊出一片选区
	bool selHit(POINT pos, size_t& bi, unsigned int& cp, bool allowFallback = true);
	// 分割线的命中（含纵向余量）。线只有 3 逻辑像素高，不给余量则按住拖动要像素级对准
	bool onDivider(POINT pos) const;
	// 按当前 anchor/cur 把高亮刷到各块上，并把整段选中文字收进 selText
	void applySelVisual();
	// 抹掉高亮并清空选区（节点还活着时用：点空白取消选择）
	void clearSelVisual();
	// 只清状态、**绝不回头碰任何节点**：节点马上要被销毁时用（消息区重画 / 气泡重填）。
	// 顺带把 selDragging 收掉 —— 它成立的前提正是 selBlocks 全活着
	void resetSelState();
private:
	long long curId{ 0 };
	std::vector<AiService::Msg> msgs;
	AiService::TaskPtr task{ nullptr };
	Ling::Node* sessionBox{ nullptr };
	Ling::ScrollerBox* msgScroller{ nullptr };
	Ling::Node* msgBox{ nullptr };
	Ling::Node* divider{ nullptr };
	Ling::Node* bottom{ nullptr };
	Ling::Node* inputBox{ nullptr };
	Ling::TextBox* input{ nullptr };
	Ling::Button* sendBtn{ nullptr };
	Ling::Button* attachBtn{ nullptr };
	// 附件按钮右边那一枚「接口名 - 模型名」，点开是所有可选组合
	Ling::Button* modelBtn{ nullptr };
	// 模型按钮上那一整串标签。名字长的时候按钮会截断，悬停提示要拿完整的这串
	std::wstring modelLabel;
	Ling::Node* attachStrip{ nullptr };
	Ling::ImageBox* attachThumb{ nullptr };
	Ling::Label* attachInfo{ nullptr };
	Ling::Label* tipLabel{ nullptr };
	// 正在流式输出的那条气泡。收尾之前所有 delta 都追加到它上面
	Ling::Node* streamingBubble{ nullptr };
	std::wstring streaming;
	// 流式输出重画 markdown 的最小间隔（毫秒）。每来一小段就整条重画太浪费，
	// 攒够这个间隔才重画一次；收尾时 renderMsgs 还会再画最后一次
	long long lastStreamPaint{ 0 };
	// 不属于历史、只临时挂在末尾的一段话：没配好时的提示，或者被取消/失败时已经
	// 吐出来的半截回答。下次重画（发消息 / 换会话）就没了
	std::wstring tailText;
	std::vector<BYTE> pendingImg;
	int pendingW{ 0 }, pendingH{ 0 };
	// 缩略图只能从文件加载（Image::loadImg 只认路径），这里是它落盘的那个临时文件
	std::wstring attachTmp;
	bool attachVisible{ false };
	// 已经发出去、需要回显的那些图落下的临时文件（每发一张追加一条），换会话/关窗时统一删
	std::vector<std::wstring> msgThumbs;
	// 消息区当前的右内边距（逻辑像素）。见 applyMsgPadding
	float msgRightPad{ 0.f };
	// 消息区里按视觉顺序排好的文本块（每个 markdown 块 / 用户条正文就是一个 Label）。
	// 拖动开始时重建一次。⚠️ 它只是"借用"这些指针 —— 谁销毁这些节点，谁负责先 resetSelState
	std::vector<Ling::Label*> selBlocks;
	bool selDragging{ false };
	// 连击计数：1 = 单点（拖选）、2 = 双击选词、3 = 三击选整段。
	// 判据同 Ling::TextBox —— 与上一次按下的间隔在系统双击时间内、且位置几乎没动
	ULONGLONG lastClickTick{ 0 };
	POINT lastClickPt{ 0, 0 };
	int clickCount{ 0 };
	// 选区两端，都写成 (块下标, 块内字符位置)：anchor 是按下那一下，cur 是当前位置
	size_t selAnchorBlock{ 0 }, selCurBlock{ 0 };
	unsigned int selAnchorPos{ 0 }, selCurPos{ 0 };
	// 当前选中的文字，Ctrl+C 直接抄它
	std::wstring selText;
	bool busy{ false };
	// 每次发问 +1。掐掉在飞的请求时也 +1，让那一次的回调认不出自己、直接作废
	long long sendGen{ 0 };
	// 会话列表里正在改名的那一条（0 = 没有）
	long long renamingId{ 0 };
	Ling::TextBox* renameBox{ nullptr };
	float inputAreaH{ 152.f };
	bool dividerDragging{ false }, dividerHover{ false };
	float dragStartY{ 0.f }, dragStartH{ 0.f };
	// 窗口关掉 / 被销毁之后，迟到的回调不能再碰这些节点（postDone 是"取消也照送"的）
	std::shared_ptr<bool> alive{ std::make_shared<bool>(true) };
};
