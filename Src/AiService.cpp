#include "pch.h"
#include <chrono>
#include <format>
#include <winrt/Windows.Web.Http.h>
// Headers.h 必须显式带上：Append 的返回类型由它给出，缺了它编译器看不到声明（C3779）
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.Threading.h>
// base64：把编好的 PNG 字节变成 data: URL 里的那一段，靠的是已经链着的 windowsapp.lib
#include <winrt/Windows.Security.Cryptography.h>
#include "AiService.h"
#include "Lang.h"
#include "Setting.h"
#include "Util.h"

namespace {
	using namespace winrt;
	using namespace winrt::Windows::Data::Json;
	using namespace winrt::Windows::Foundation;
	using namespace winrt::Windows::Storage::Streams;
	using namespace winrt::Windows::Web::Http;
	using winrt::Windows::System::Threading::ThreadPoolTimer;
	using winrt::Windows::Security::Cryptography::CryptographicBuffer;

	constexpr std::wstring_view modelsPath{ L"/models" };
	constexpr std::wstring_view chatPath{ L"/chat/completions" };
	// 连不上 / 域名不通 / 服务没起来：响应头迟迟不来，等多久就撤
	constexpr std::chrono::seconds headTimeout{ 15 };
	// 流已经开了，两次收到数据之间空多久算对方挂了
	constexpr std::chrono::seconds idleTimeout{ 30 };
	constexpr uint32_t chunkSize{ 32 * 1024 };

	std::wstring trimSlash(std::wstring url)
	{
		while (!url.empty() && url.back() == L'/') url.pop_back();
		return url;
	}

	// 用户粘进来的地址有两种：服务根（https://xxx/v1）与完整 endpoint（…/v1/chat/completions）。
	// 两种都认，各自拼出要用的那一个
	std::wstring baseRoot(const std::wstring& baseUrl)
	{
		auto str = trimSlash(baseUrl);
		if (str.ends_with(chatPath)) str.resize(str.size() - chatPath.size());
		return str;
	}

	std::wstring chatEndpoint(const std::wstring& baseUrl)
	{
		auto str = trimSlash(baseUrl);
		if (str.ends_with(chatPath)) return str;
		return str + std::wstring{ chatPath };
	}

	std::wstring modelsEndpoint(const std::wstring& baseUrl)
	{
		auto str = trimSlash(baseUrl);
		if (!str.ends_with(modelsPath)) str += modelsPath;
		return str;
	}

	std::wstring roleName(AiService::Role role)
	{
		switch (role) {
		case AiService::Role::System: return L"system";
		case AiService::Role::Assistant: return L"assistant";
		default: return L"user";
		}
	}

	// 片段：取消之后就不该再往 UI 送
	void postDelta(const std::shared_ptr<AiService::Task>& task,
		const std::function<void(const std::wstring&)>& cb, std::wstring text)
	{
		Ling::App::get()->dq.TryEnqueue([task, cb, text = std::move(text)]() {
			if (!task->isCanceled()) cb(text);
		});
	}

	// 收尾：取消也照送，UI 靠它解锁输入框
	void postDone(const std::shared_ptr<AiService::Task>& task,
		const std::function<void(const std::wstring&)>& cb, std::wstring err)
	{
		Ling::App::get()->dq.TryEnqueue([task, cb, err = std::move(err)]() { cb(err); });
	}

	// 超时是我们自己 Cancel 掉异步操作的结果，不是服务端的错。
	// winrt 没有给 hresult 与 hresult_canceled 之间定义 ==，只能比 value
	bool isCanceledErr(const winrt::hresult_error& e)
	{
		return e.code().value == HRESULT_FROM_WIN32(ERROR_CANCELLED);
	}

	std::wstring httpErr(const HttpStatusCode code)
	{
		// 带上状态码：401（密钥错）和 404（地址错）的处理办法完全不一样，
		// 用户得能区分，而不是只看到一句"失败了"
		return std::format(L"{} {}", Lang::get(L"ai.httpErr"), static_cast<int>(code));
	}

	// SSE 的 data 是一小段 JSON：{"choices":[{"delta":{"content":"…"}}]}。
	// 非流式的兼容网关会给 {"choices":[{"message":{"content":"…"}}]}，两条路都收着
	std::wstring parseDelta(const std::string& payload)
	{
		JsonObject obj{ nullptr };
		if (!JsonObject::TryParse(Ling::Util::convertToWStr(payload.c_str()), obj) || !obj) return {};
		auto choices = obj.GetNamedArray(L"choices", nullptr);
		if (!choices || choices.Size() == 0) return {};
		// 先取值再看类型：GetObjectAt 对"不是对象"是直接抛的，服务端返回的形状不该被信任
		auto first = choices.GetAt(0);
		if (first.ValueType() != JsonValueType::Object) return {};
		auto one = first.GetObject();
		auto node = one.GetNamedObject(L"delta", nullptr);
		if (!node) node = one.GetNamedObject(L"message", nullptr);
		if (!node) return {};
		return std::wstring{ node.GetNamedString(L"content", L"") };
	}

	// 带图的消息，content 得是数组而不是字符串：
	// [{"type":"text","text":…},{"type":"image_url","image_url":{"url":"data:image/png;base64,…"}}]
	// 纯文本那条路仍给字符串 —— 各家网关对这两种形状的支持程度不一样，能简单就简单
	IJsonValue makeContent(const AiService::Msg& msg)
	{
		if (msg.image.empty()) return JsonValue::CreateStringValue(msg.content);
		std::vector<BYTE> png;
		// 编不出来就退化成纯文本：宁可少一张图，也别把一个坏 data URL 发出去，
		// 那种情况服务端只会回一句看不懂的 400
		if (!Util::encodeImageBytes(msg.imgW, msg.imgH, msg.image.data(), png)) {
			return JsonValue::CreateStringValue(msg.content);
		}
		auto view = winrt::array_view<uint8_t const>{ png.data(), static_cast<uint32_t>(png.size()) };
		auto url = std::wstring{ L"data:image/png;base64," }
			+ std::wstring{ CryptographicBuffer::EncodeToBase64String(CryptographicBuffer::CreateFromByteArray(view)) };
		JsonArray parts;
		if (!msg.content.empty()) {
			JsonObject text;
			text.SetNamedValue(L"type", JsonValue::CreateStringValue(L"text"));
			text.SetNamedValue(L"text", JsonValue::CreateStringValue(msg.content));
			parts.Append(text);
		}
		JsonObject imgUrl, img;
		imgUrl.SetNamedValue(L"url", JsonValue::CreateStringValue(url));
		img.SetNamedValue(L"type", JsonValue::CreateStringValue(L"image_url"));
		img.SetNamedValue(L"image_url", imgUrl);
		parts.Append(img);
		return parts;
	}

	// GET /models 的响应体 {"data":[{"id":"…"}]}
	std::vector<std::wstring> parseModels(const std::wstring& body)
	{
		std::vector<std::wstring> ids;
		JsonObject obj{ nullptr };
		if (!JsonObject::TryParse(body, obj) || !obj) return ids;
		auto arr = obj.GetNamedArray(L"data", nullptr);
		if (!arr) return ids;
		for (auto&& item : arr) {
			if (item.ValueType() != JsonValueType::Object) continue;
			auto id = std::wstring{ item.GetObject().GetNamedString(L"id", L"") };
			if (!id.empty()) ids.push_back(std::move(id));
		}
		return ids;
	}

	// 一个 SSE 事件。格式是若干行 field: value，我们只认 data 那一行。
	// 返回是否读到了流结束的 [DONE]
	bool handleEvent(const std::shared_ptr<AiService::Task>& task, const std::string& evt,
		const std::function<void(const std::wstring&)>& onDelta)
	{
		size_t pos{ 0 };
		while (pos < evt.size()) {
			auto nl = evt.find('\n', pos);
			auto line = evt.substr(pos, nl == std::string::npos ? nl : nl - pos);
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
			auto payload = line;
			if (payload.starts_with("data:")) {
				payload.erase(0, 5);
				while (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
				if (payload == "[DONE]") return true;
				if (!payload.empty()) {
					auto text = parseDelta(payload);
					if (!text.empty()) postDelta(task, onDelta, std::move(text));
				}
			}
			if (nl == std::string::npos) break;
			pos = nl + 1;
		}
		return false;
	}

	// 把 pending 里能凑成完整事件的部分吃掉。pending 必须留着那些还没收到分隔符的字节：
	// 一次 ReadAsync 拿到的可能只是半个事件，甚至半个 UTF-8 字符（每字节单独存，天然没事）
	bool drainEvents(const std::shared_ptr<AiService::Task>& task, std::string& pending,
		const std::function<void(const std::wstring&)>& onDelta)
	{
		bool done{ false };
		while (true) {
			auto pos = pending.find("\n\n");
			if (pos == std::string::npos) break;
			std::string evt = pending.substr(0, pos);
			pending.erase(0, pos + 2);
			if (handleEvent(task, evt, onDelta)) done = true;
		}
		return done;
	}

	// GET /models：既是模型下拉框的数据源，也是"这个值不通"的判据
	winrt::fire_and_forget doFetchModels(std::shared_ptr<AiService::Task> task, std::wstring url,
		std::wstring auth, std::function<void(const std::vector<std::wstring>& ids)> onModels,
		std::function<void(const std::wstring& err)> onDone)
	{
		// 网络挪到后台线程：这一步可能卡十几秒，挂在 UI 线程上就是整个应用不动了
		co_await winrt::resume_background();
		std::vector<std::wstring> ids;
		std::wstring err;
		try {
			HttpClient client;
			HttpRequestMessage req{ HttpMethod::Get(), Uri{ url } };
			req.Headers().Append(L"Authorization", auth);
			auto op = client.SendRequestAsync(req);
			auto guard = ThreadPoolTimer::CreateTimer([op](const ThreadPoolTimer&) { op.Cancel(); }, headTimeout);
			auto resp = co_await op;
			guard.Cancel();
			if (!resp.IsSuccessStatusCode()) {
				err = httpErr(resp.StatusCode());
			}
			else {
				auto body = std::wstring{ co_await resp.Content().ReadAsStringAsync() };
				ids = parseModels(body);
				if (ids.empty()) err = Lang::get(L"ai.noModel");
			}
		}
		catch (winrt::hresult_error const& e) {
			err = isCanceledErr(e) ? Lang::get(L"ai.timeout") : Lang::get(L"ai.fail");
		}
		catch (...) {
			err = Lang::get(L"ai.fail");
		}
		if (!ids.empty()) {
			Ling::App::get()->dq.TryEnqueue([task, onModels, ids = std::move(ids)]() {
				if (!task->isCanceled()) onModels(ids);
			});
		}
		postDone(task, onDone, std::move(err));
	}

	// POST /chat/completions，stream = true，逐段读 SSE
	winrt::fire_and_forget doChat(std::shared_ptr<AiService::Task> task, std::wstring url,
		std::wstring auth, std::wstring model, JsonArray msgs,
		std::function<void(const std::wstring& delta)> onDelta,
		std::function<void(const std::wstring& err)> onDone)
	{
		co_await winrt::resume_background();
		std::wstring err;
		std::string pending;
		bool done{ false };
		try {
			HttpClient client;
			HttpRequestMessage req{ HttpMethod::Post(), Uri{ url } };
			req.Headers().Append(L"Authorization", auth);
			JsonObject body;
			body.SetNamedValue(L"model", JsonValue::CreateStringValue(model));
			body.SetNamedValue(L"messages", msgs);
			body.SetNamedValue(L"stream", JsonValue::CreateBooleanValue(true));
			req.Content(HttpStringContent{ std::wstring{ body.Stringify() },
				UnicodeEncoding::Utf8, L"application/json" });
			// ResponseHeadersRead：响应头一到就返回，响应体留给下面分批读 —— 流式要的就是这个
			auto sendOp = client.SendRequestAsync(req, HttpCompletionOption::ResponseHeadersRead);
			auto headGuard = ThreadPoolTimer::CreateTimer(
				[sendOp](const ThreadPoolTimer&) { sendOp.Cancel(); }, headTimeout);
			auto resp = co_await sendOp;
			headGuard.Cancel();
			if (!resp.IsSuccessStatusCode()) {
				err = httpErr(resp.StatusCode());
			}
			else {
				auto stream = co_await resp.Content().ReadAsInputStreamAsync();
				Buffer buffer{ chunkSize };
				while (!task->isCanceled() && !done) {
					auto readOp = stream.ReadAsync(buffer, buffer.Capacity(), InputStreamOptions::Partial);
					// 空太久就把这次读撤掉，免得请求永远挂着、回调再也不来
					auto idleGuard = ThreadPoolTimer::CreateTimer(
						[readOp](const ThreadPoolTimer&) { readOp.Cancel(); }, idleTimeout);
					auto read = co_await readOp;
					idleGuard.Cancel();
					if (read.Length() == 0) break;
					pending.append(reinterpret_cast<const char*>(read.data()), read.Length());
					done = drainEvents(task, pending, onDelta);
				}
				// 少数网关收尾不带换行，最后那一个事件得手动捞出来
				if (!done && !task->isCanceled() && !pending.empty()) drainEvents(task, pending, onDelta);
			}
		}
		catch (winrt::hresult_error const& e) {
			err = isCanceledErr(e) ? Lang::get(L"ai.timeout") : Lang::get(L"ai.fail");
		}
		catch (...) {
			err = Lang::get(L"ai.fail");
		}
		postDone(task, onDone, task->isCanceled() ? Lang::get(L"ai.canceled") : std::move(err));
	}
}

bool AiService::ready()
{
	auto setting = Setting::get();
	if (setting->getAiStr(L"baseUrl", L"").empty()) return false;
	if (setting->getAiStr(L"apiKey", L"").empty()) return false;
	if (setting->getAiStr(L"model", L"").empty()) return false;
	return true;
}

AiService::TaskPtr AiService::models(std::function<void(const std::vector<std::wstring>& ids)> onModels,
	std::function<void(const std::wstring& err)> onDone)
{
	auto task = std::make_shared<Task>();
	auto setting = Setting::get();
	auto baseUrl = setting->getAiStr(L"baseUrl", L"");
	auto key = setting->getAiStr(L"apiKey", L"");
	if (baseUrl.empty() || key.empty()) {
		postDone(task, onDone, Lang::get(L"ai.noKey"));
		return task;
	}
	doFetchModels(task, modelsEndpoint(baseUrl), L"Bearer " + key, std::move(onModels), std::move(onDone));
	return task;
}

AiService::TaskPtr AiService::chat(const std::vector<Msg>& msgs,
	std::function<void(const std::wstring& delta)> onDelta,
	std::function<void(const std::wstring& err)> onDone)
{
	auto task = std::make_shared<Task>();
	auto setting = Setting::get();
	auto baseUrl = setting->getAiStr(L"baseUrl", L"");
	auto key = setting->getAiStr(L"apiKey", L"");
	auto model = setting->getAiStr(L"model", L"");
	if (baseUrl.empty() || key.empty() || model.empty()) {
		postDone(task, onDone, Lang::get(L"ai.noKey"));
		return task;
	}
	JsonArray arr;
	for (const auto& msg : msgs) {
		JsonObject one;
		one.SetNamedValue(L"role", JsonValue::CreateStringValue(roleName(msg.role)));
		one.SetNamedValue(L"content", makeContent(msg));
		arr.Append(one);
	}
	doChat(task, chatEndpoint(baseUrl), L"Bearer " + key, model, arr,
		std::move(onDelta), std::move(onDone));
	return task;
}
