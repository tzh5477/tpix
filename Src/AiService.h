#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// 云端 AI 服务（OpenAI 兼容接口）。配置项在系统设置的 ai 组：baseUrl / apiKey / model。
// S1 的对话窗是它第一个调用方；S2 的翻译后续复用同一个 chat（换一套消息拼法即可）。
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

	// baseUrl / apiKey / model 三样缺一样就不能用。UI 拿它决定要不要先引导去配置
	static bool ready();
	// 拉模型列表 == 验证连接：二者是同一个请求（OpenAI 兼容的 GET /models），
	// 拿到了就既能填下拉框，也说明地址、密钥、网络都通
	static TaskPtr models(std::function<void(const std::vector<std::wstring>& ids)> onModels,
		std::function<void(const std::wstring& err)> onDone);
	// 流式对话。onDelta 每收到一小段回调一次，顺序拼起来是完整回答；
	// onDone 的 err 为空表示正常结束，取消时是 ai.canceled。
	// 两个回调都在 UI 线程，且都排在 chat 返回之后 —— UI 必然先拿到 TaskPtr，不会错过回调
	static TaskPtr chat(const std::vector<Msg>& msgs,
		std::function<void(const std::wstring& delta)> onDelta,
		std::function<void(const std::wstring& err)> onDone);
};
