#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include "AiService.h"

// AI 对话历史。一个会话（Session）是一组消息，全部落在 数据目录/ai/history.json 一个文件里，
// 结构与读写方式照抄 ShotHistory（进程启动时读一次，之后每次增删整体写回）。
//
// 为什么不塞进 config.json：它会被「导出配置」整个带走，聊天记录跟着配置去别的机器不合适；
// 而且对话随时在追加，config.json 是任何一项设置改动都整体重写的，跟着频繁重写不划算。
//
// 清理策略（trim）：每次写盘前跑一遍，两个维度各管一段 ——
//   超过 historyDays 天的按年龄删；剩下的超过 historyLimit 条，从最旧那条开始删。
// 两条都留着是因为它们防的不是一回事：天天聊的人会被条数卡住，偶尔聊的人会被天数清掉。
class AiHistory
{
public:
	using Msg = AiService::Msg;
	struct Session
	{
		long long id{ 0 };     // 创建时刻的毫秒时间戳，同时是排序键
		long long time{ 0 };   // 最后一次追加消息的时刻，按年龄清理就看它
		std::wstring title;    // 列表里显示的那一句，取首条用户消息截断
		std::vector<Msg> msgs;
	};
public:
	static void init();
	static void dispose();
	static AiHistory* get();
	// 现在（毫秒时间戳）。消息上的时间戳由调用方打好再交进来（界面要立刻拿到它显示，
	// 不能等 append 内部自己填）
	static long long now();
	// 开一个空会话，返回 id。标题要等第一条用户消息进来才有，列表里先显示占位
	long long create();
	// 追加一条消息。会话不存在、或内容为空则什么都不做（空回答不该在历史里留一个空气泡）
	void append(long long id, const Msg& msg);
	// 整体覆盖一个会话的消息。删掉某一轮对话（问题 + 对应的回答）之后用它写回
	void setMsgs(long long id, const std::vector<Msg>& msgs);
	// 改名。传空串表示清掉标题，列表里会退回占位文案
	void rename(long long id, const std::wstring& title);
	void remove(long long id);
	// 全清，磁盘文件一起删掉
	void clear();
	// 时间新的在前
	std::vector<Session> list() const;
	// 取一个会话。没有这个 id 返回 nullptr。返回的指针只在下次改动之前有效
	const Session* find(long long id) const;
private:
	AiHistory();
	~AiHistory();
	friend struct std::default_delete<AiHistory>;
	void load();
	void save();
	// 按天数与条数各删一轮。只动内存，写盘由 save 负责
	void trim();
private:
	std::vector<Session> sessions;
	std::filesystem::path dir;
	std::filesystem::path file;
};
