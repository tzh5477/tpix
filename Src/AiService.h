#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "Setting.h"

// 云端 AI 服务（OpenAI 兼容接口）。
// 调用方按需给两样东西之一：
//   1) 场景名（AiScenario::chat / translate / recognize / table）—— 由 Setting 里
//      「这个业务该用哪个接口、哪个模型」的绑定解出凭据；
//   2) 直接给一组 AiCred —— 界面上临时换模型（对话窗那个下拉框）走这条路。
// 底下真正的收发只有一份：Bearer + OpenAI 兼容的 /models 与 /chat/completions。
// 火山的机器翻译是另一套认证（签名，见 S3），不走这里。
class AiService
{
public:
	enum class Role { System, User, Assistant };
	struct Msg
	{
		Role role;
		std::wstring content;
		// 这一条附带的图（BGRA top-down 行紧凑，与 Util 同一套格式）。空 = 纯文本消息。
		// 发出去时按 OpenAI 兼容的 image_url 通道走：编 PNG → base64 → data: URL。
		// ⚠️ 不落历史（AiHistory 只存 role + content）：一张满屏截图 base64 之后几 MB，
		//    全部塞进 history.json 会把那个文件撑到看不懂的大小
		std::vector<BYTE> image;
		int imgW{ 0 }, imgH{ 0 };
		// 这条消息产生的时刻（毫秒时间戳，0 = 未知）。只给界面显示用，不发出去 ——
		// 请求体里每条消息只有 role + content
		long long time{ 0 };
	};
	// 一次请求的控制柄。cancel 之后不再回调片段，但收尾回调照来（取消也是一次收尾）——
	// UI 靠它解锁输入框，不必自己判断到底成没成。
	// ⚠️ 析构不会自动取消：窗口销毁前必须由调用方 cancel，否则迟到的回调会打到已析构的窗口上
	struct Task
	{
		void cancel() { canceled = true; }
		bool isCanceled() const { return canceled.load(); }
	private:
		std::atomic<bool> canceled{ false };
	};
	using TaskPtr = std::shared_ptr<Task>;

	// 某个业务场景当前能不能用（它绑的那个接口地址 / 密钥 / 模型都齐了）
	static bool ready(const std::wstring& scenario);
	// 把场景解成地址 / 密钥 / 模型。UI 拿它显示"现在用着的是哪一个"
	static AiCred credFor(const std::wstring& scenario);
	// 拉某个接口的模型列表 == 验证连接：二者是同一个请求（OpenAI 兼容的 GET /models），
	// 拿到了就既能填下拉框，也说明地址、密钥、网络都通
	static TaskPtr models(const AiCred& cred,
		std::function<void(const std::vector<std::wstring>& ids)> onModels,
		std::function<void(const std::wstring& err)> onDone);
	// 流式对话（按场景）。onDelta 每收到一小段回调一次，顺序拼起来是完整回答；
	// onDone 的 err 为空表示正常结束，取消时是 ai.canceled。
	// 两个回调都在 UI 线程，且都排在 chat 返回之后 —— UI 必然先拿到 TaskPtr，不会错过回调
	static TaskPtr chat(const std::wstring& scenario, const std::vector<Msg>& msgs,
		std::function<void(const std::wstring& delta)> onDelta,
		std::function<void(const std::wstring& err)> onDone);
	// 同上，但凭据由调用方直接给（临时换模型的那条路）
	static TaskPtr chat(const AiCred& cred, const std::vector<Msg>& msgs,
		std::function<void(const std::wstring& delta)> onDelta,
		std::function<void(const std::wstring& err)> onDone);
};
