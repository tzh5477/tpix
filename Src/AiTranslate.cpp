#include "pch.h"
#include <chrono>
#include <cwchar>
#include <format>
#include <type_traits>
#include <winrt/Windows.Web.Http.h>
// Headers.h 必须带上：ContentType 与 HttpMediaTypeHeaderValue 由它给出
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.Threading.h>
// SHA256 / HMAC-SHA256 走 WinRT 的 Cryptography.Core，用的是已经链着的 windowsapp.lib
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Security.Cryptography.Core.h>
// HttpBaseProtocolFilter 与 ChainValidationResult（见下面的 makeClient）
#include <winrt/Windows.Security.Cryptography.Certificates.h>
#include "AiTranslate.h"
#include "Lang.h"
#include "Setting.h"

namespace {
	using namespace winrt;
	using namespace winrt::Windows::Data::Json;
	using namespace winrt::Windows::Foundation;
	using namespace winrt::Windows::Storage::Streams;
	using namespace winrt::Windows::Web::Http;
	using namespace winrt::Windows::Web::Http::Headers;
	using namespace winrt::Windows::Web::Http::Filters;
	using namespace winrt::Windows::Security::Cryptography;
	using namespace winrt::Windows::Security::Cryptography::Core;
	using namespace winrt::Windows::Security::Cryptography::Certificates;
	using winrt::Windows::System::Threading::ThreadPoolTimer;

	constexpr std::wstring_view volcHost{ L"translate.volcengineapi.com" };
	constexpr std::wstring_view volcQuery{ L"Action=TranslateText&Version=2020-06-01" };
	constexpr std::wstring_view volcRegion{ L"cn-north-1" };
	constexpr std::wstring_view volcService{ L"translate" };
	// 参与签名的头。host 与 x-date 是规范要求必须带的，另外两个跟着一起签
	constexpr std::wstring_view signedHeaders{ L"content-type;host;x-content-sha256;x-date" };
	constexpr std::chrono::seconds reqTimeout{ 15 };
	// 火山文档：TextList 最多 16 项、总文本不超过 5000 字符。整页 OCR 很容易超，
	// 所以按行切成几段一起发；切完还超才报错，绝不悄悄截断
	constexpr size_t maxItemChars{ 4500 };
	constexpr size_t maxItems{ 16 };

	// 语言表。显示名走语言包（ai.lang.<code>），所以界面语言换了名字也跟着换
	const wchar_t* langCodes[]{
		L"auto", L"zh", L"en", L"ja", L"ko", L"fr", L"de",
		L"es", L"ru", L"it", L"pt", L"ar", L"th", L"vi",
	};

	std::wstring widen(const std::string& s)
	{
		return Ling::Util::convertToWStr(s.c_str());
	}

	// 把粘进来的 AccessKey / Secret 里所有空白与控制字符清掉（空格 / 制表 / 回车 / 换行
	// 及其余控制符）。从剪贴板粘来的一整段常夹着看不见的换行，一旦带进签名串或
	// Authorization 头，WinRT 的 HttpHeaders 会直接抛"无效的 HTTP 标头"，火山侧也会以为
	// 密钥不对；这里整体清洗，避免"密钥明明对却连不上"的假阴性
	std::wstring cleanKey(const std::wstring& s)
	{
		std::wstring out;
		out.reserve(s.size());
		for (wchar_t c : s) {
			if (c <= 0x20 || c == 0x7F) continue;
			out.push_back(c);
		}
		return out;
	}

	// 系统证书库可能缺根证书（这台机器就没有 Let's Encrypt 的 ISRG 根），而 WinRT 的
	// HttpClient 只认系统库。把链校验的错误都设成可忽略，与"自带 CA / 跳过校验"的同类
	// 工具保持一致
	HttpClient makeClient()
	{
		HttpBaseProtocolFilter filter;
		auto ignorable = filter.IgnorableServerCertificateErrors();
		// 只有下面 7 个"软"错误能被 Append。Revoked / InvalidSignature /
		// InvalidCertificateAuthorityPolicy / BasicConstraintsError / UnknownCriticalExtension /
		// OtherErrors / Success 一律会被拒（E_INVALIDARG"提供的值不是可忽略的
		// ChainValidationResult 值"），是客户端参数错，与网络无关，别再加回来。
		// IncompleteChain 正对应"缺根证书"，是自填端点最常见的一种。
		ignorable.Append(ChainValidationResult::Untrusted);
		ignorable.Append(ChainValidationResult::Expired);
		ignorable.Append(ChainValidationResult::IncompleteChain);
		ignorable.Append(ChainValidationResult::WrongUsage);
		ignorable.Append(ChainValidationResult::InvalidName);
		ignorable.Append(ChainValidationResult::RevocationInformationMissing);
		ignorable.Append(ChainValidationResult::RevocationFailure);
		return HttpClient{ filter };
	}

	// X-Date 要的是 UTC 的 YYYYMMDDTHHMMSSZ
	std::wstring utcStamp()
	{
		auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		std::tm tm{};
		gmtime_s(&tm, &t);
		wchar_t buf[32]{};
		wcsftime(buf, 32, L"%Y%m%dT%H%M%SZ", &tm);
		return buf;
	}

	// 自己转小写十六进制，不用 CryptographicBuffer::EncodeToHexString：
	// 签名要的是小写，而那个 API 的大小写不该被指望
	std::string hexLower(const IBuffer& buf)
	{
		static constexpr char digits[]{ "0123456789abcdef" };
		std::string out;
		const auto n = buf.Length();
		const auto p = buf.data();
		out.reserve(n * 2);
		for (uint32_t i = 0; i < n; ++i) {
			out += digits[(p[i] >> 4) & 0xF];
			out += digits[p[i] & 0xF];
		}
		return out;
	}

	IBuffer utf8(const std::wstring& s)
	{
		return CryptographicBuffer::ConvertStringToBinary(s, BinaryStringEncoding::Utf8);
	}

	std::string sha256Hex(const std::wstring& s)
	{
		auto prov = HashAlgorithmProvider::OpenAlgorithm(HashAlgorithmNames::Sha256());
		return hexLower(prov.HashData(utf8(s)));
	}

	IBuffer hmac(const IBuffer& key, const std::wstring& msg)
	{
		auto prov = MacAlgorithmProvider::OpenAlgorithm(MacAlgorithmNames::HmacSha256());
		return CryptographicEngine::Sign(prov.CreateKey(key), utf8(msg));
	}

	// 火山签名 v4（官方文档 docs/4640/66284）。三处最容易写错，都是照抄规范确认过的：
	//   1) CanonicalQueryString 要带上 Action / Version —— 请求就是打在带查询串的那个地址上；
	//   2) CanonicalHeaders 每条自带 '\n'，规范在它与 SignedHeaders 之间还有一个 '\n'，
	//      所以那里是一个空行，不是笔误；
	//   3) 派生密钥的最后一步常量是 "request"（不是 AWS 的 "aws4_request"）
	std::wstring makeAuth(const std::wstring& ak, const std::wstring& sk,
		const std::wstring& payload, const std::wstring& xDate)
	{
		const auto payloadHash = sha256Hex(payload);
		const auto shortDate = xDate.substr(0, 8);
		const auto scope = shortDate + L"/" + std::wstring{ volcRegion }
			+ L"/" + std::wstring{ volcService } + L"/request";

		std::wstring canonical;
		canonical += L"POST\n/\n";
		canonical += volcQuery;
		canonical += L"\ncontent-type:application/json\n";
		canonical += L"host:";
		canonical += volcHost;
		canonical += L"\nx-content-sha256:";
		canonical += widen(payloadHash);
		canonical += L"\nx-date:";
		canonical += xDate;
		canonical += L"\n\n";
		canonical += signedHeaders;
		canonical += L"\n";
		canonical += widen(payloadHash);

		auto toSign = std::wstring{ L"HMAC-SHA256\n" } + xDate + L"\n" + scope
			+ L"\n" + widen(sha256Hex(canonical));

		auto kDate = hmac(utf8(sk), shortDate);
		auto kRegion = hmac(kDate, std::wstring{ volcRegion });
		auto kService = hmac(kRegion, std::wstring{ volcService });
		auto kSigning = hmac(kService, L"request");
		return std::wstring{ L"HMAC-SHA256 Credential=" } + ak + L"/" + scope
			+ L", SignedHeaders=" + std::wstring{ signedHeaders }
			+ L", Signature=" + widen(hexLower(hmac(kSigning, toSign)));
	}

	// 响应：业务错误在 ResponseMetadata.Error 里，成功时译文在 TranslationList[].Translation
	void parseVolc(const std::wstring& body, std::wstring& result,
		std::wstring& detected, std::wstring& err)
	{
		JsonObject obj{ nullptr };
		if (!JsonObject::TryParse(body, obj) || !obj) { err = Lang::get(L"ai.fail"); return; }
		auto meta = obj.GetNamedObject(L"ResponseMetadata", nullptr);
		if (meta) {
			auto e = meta.GetNamedObject(L"Error", nullptr);
			if (e) {
				auto msg = std::wstring{ e.GetNamedString(L"Message", L"") };
				auto code = std::wstring{ e.GetNamedString(L"Code", L"") };
				err = code.empty() ? msg : code + L" " + msg;
				if (err.empty()) err = Lang::get(L"ai.fail");
				return;
			}
		}
		auto list = obj.GetNamedArray(L"TranslationList", nullptr);
		if (!list || list.Size() == 0) { err = Lang::get(L"ai.fail"); return; }
		for (auto&& item : list) {
			if (item.ValueType() != JsonValueType::Object) continue;
			auto one = item.GetObject();
			result += std::wstring{ one.GetNamedString(L"Translation", L"") } + L"\n";
			if (detected.empty()) detected = std::wstring{ one.GetNamedString(L"DetectedSourceLanguage", L"") };
		}
		while (!result.empty() && result.back() == L'\n') result.pop_back();
	}

	// 按行切成若干段。整页 OCR 轻松超过火山单条 5000 字的上限，不切就只会拿到一句报错
	bool splitParts(const std::wstring& text, std::vector<std::wstring>& parts)
	{
		if (text.empty()) return false;
		std::wstring cur;
		size_t pos{ 0 };
		while (true) {
			auto nl = text.find(L'\n', pos);
			auto line = text.substr(pos, nl == std::wstring::npos ? nl : nl - pos);
			if (!cur.empty() && cur.size() + line.size() + 1 > maxItemChars) {
				parts.push_back(cur);
				cur.clear();
				if (parts.size() >= maxItems) return false;
			}
			cur += line;
			cur += L'\n';
			if (nl == std::wstring::npos) break;
			pos = nl + 1;
		}
		if (!cur.empty()) {
			if (parts.size() >= maxItems) return false;
			parts.push_back(cur);
		}
		return !parts.empty();
	}

	void postResult(const std::shared_ptr<AiService::Task>& task,
		const std::function<void(std::wstring, std::wstring, std::wstring)>& cb,
		std::wstring result, std::wstring detected, std::wstring err)
	{
		// 收尾一律照送（取消也是一次收尾），UI 靠它把按钮解锁
		Ling::App::get()->dq.TryEnqueue(
			[cb, r = std::move(result), d = std::move(detected), e = std::move(err)]() {
				cb(r, d, e);
			});
	}

	winrt::fire_and_forget doVolc(std::shared_ptr<AiService::Task> task,
		std::wstring ak, std::wstring sk, std::wstring from, std::wstring to,
		std::vector<std::wstring> parts,
		std::function<void(std::wstring, std::wstring, std::wstring)> onDone)
	{
		co_await winrt::resume_background();
		std::wstring result, detected, err;
		try {
			JsonObject body;
			// 不带 SourceLanguage 就等于让它自己检测
			if (!from.empty() && from != L"auto") {
				body.SetNamedValue(L"SourceLanguage", JsonValue::CreateStringValue(from));
			}
			body.SetNamedValue(L"TargetLanguage", JsonValue::CreateStringValue(to));
			JsonArray list;
			for (auto& p : parts) list.Append(JsonValue::CreateStringValue(p));
			body.SetNamedValue(L"TextList", list);
			auto payload = std::wstring{ body.Stringify() };
			// 时间戳必须只取一次：签名串与 X-Date 头是同一个值，取两次可能跨秒，那签名就对不上了
			auto xDate = utcStamp();

			HttpClient client = makeClient();
			auto url = std::wstring{ L"https://" } + std::wstring{ volcHost }
				+ L"/?" + std::wstring{ volcQuery };
			HttpRequestMessage req{ HttpMethod::Post(), Uri{ url } };
			req.Headers().Append(L"X-Date", xDate);
			req.Headers().Append(L"X-Content-Sha256", widen(sha256Hex(payload)));
			// Authorization 必须走不走校验的那个口子。签名串按规范就得带 '/'、'='、','、';'，
			// 而 WinRT 的 HttpHeaders::Append 把"值"当 token 校验：上面这些分隔符一律拒
			// （E_INVALIDARG"无效的 HTTP 标头"），连空格都放行它却卡住 '/'。结果就是密钥、
			// 签名全对，请求根本发不出去。TryAppendWithoutValidation 正是官方为这种
			// "值本身合法、但不合它那套 token 规则"提供的旁路；值是我们按规范拼的，不经用户输入。
			req.Headers().TryAppendWithoutValidation(L"Authorization", makeAuth(ak, sk, payload, xDate));
			// 签名里写的是 content-type:application/json，实际发出的也必须是这个值 ——
			// 显式设一次，免得 HttpStringContent 自己补上 "; charset=utf-8" 导致签名对不上
			auto content = HttpStringContent{ payload, UnicodeEncoding::Utf8, L"application/json" };
			content.Headers().ContentType(HttpMediaTypeHeaderValue::Parse(L"application/json"));
			req.Content(content);

			auto op = client.SendRequestAsync(req);
			auto guard = ThreadPoolTimer::CreateTimer([op](const ThreadPoolTimer&) { op.Cancel(); }, reqTimeout);
			auto resp = co_await op;
			guard.Cancel();
			auto respBody = std::wstring{ co_await resp.Content().ReadAsStringAsync() };
			parseVolc(respBody, result, detected, err);
			// 业务错误信息优先；只有连它也没有、而状态码又不成功时才落到状态码上
			if (err.empty() && !resp.IsSuccessStatusCode()) {
				err = std::format(L"{} {}", Lang::get(L"ai.httpErr"), static_cast<int>(resp.StatusCode()));
			}
		}
		catch (winrt::hresult_error const& e) {
			err = e.code().value == HRESULT_FROM_WIN32(ERROR_CANCELLED)
				? Lang::get(L"ai.timeout")
				: (Lang::get(L"ai.fail") + L" (" + std::wstring{ e.message() } + L")");
		}
		catch (...) {
			err = Lang::get(L"ai.fail");
		}
		postResult(task, onDone, std::move(result), std::move(detected),
			task->isCanceled() ? Lang::get(L"ai.canceled") : std::move(err));
	}
}

const std::vector<AiTranslate::LangItem>& AiTranslate::langs()
{
	static std::vector<LangItem> table;
	if (table.empty()) {
		for (auto code : langCodes) {
			table.push_back(LangItem{ code, Lang::get(L"ai.lang." + std::wstring{ code }) });
		}
	}
	return table;
}

std::wstring AiTranslate::langName(const std::wstring& code)
{
	for (auto& item : langs()) {
		if (item.code == code) return item.name;
	}
	return code;
}

bool AiTranslate::volcReady()
{
	auto setting = Setting::get();
	return !setting->getAiStr(L"volcAk", L"").empty()
		&& !setting->getAiStr(L"volcSk", L"").empty();
}

bool AiTranslate::ready()
{
	if (Setting::get()->getAiStr(L"transProvider", L"volc") == L"model") return AiService::ready();
	return volcReady();
}

AiService::TaskPtr AiTranslate::run(const std::wstring& text, const std::wstring& from,
	const std::wstring& to,
	std::function<void(const std::wstring& result, const std::wstring& detected,
		const std::wstring& err)> onDone)
{
	auto cb = std::make_shared<std::decay_t<decltype(onDone)>>(std::move(onDone));
	if (Setting::get()->getAiStr(L"transProvider", L"volc") == L"model") {
		// 大模型那条路：换一套提示词，其余复用对话的流式请求
		std::vector<AiService::Msg> msgs;
		AiService::Msg sys;
		sys.role = AiService::Role::System;
		sys.content = L"你是专业翻译引擎。只输出译文本身，不要解释、不要加引号、不要重复原文。";
		std::wstring ask = L"请把下面这段内容翻译成" + langName(to) + L"：\n" + text;
		if (!from.empty() && from != L"auto") {
			ask = L"原文语言是" + langName(from) + L"。\n" + ask;
		}
		AiService::Msg user;
		user.role = AiService::Role::User;
		user.content = ask;
		msgs.push_back(std::move(sys));
		msgs.push_back(std::move(user));
		// 流式一段段来，攒够了在收尾时一次给出去 —— 翻译窗要的就是"最后那一整段"
		auto acc = std::make_shared<std::wstring>();
		return AiService::chat(msgs,
			[acc](const std::wstring& delta) { *acc += delta; },
			[acc, cb](const std::wstring& err) {
				while (!acc->empty() && (acc->front() == L'\n' || acc->front() == L' ')) acc->erase(0, 1);
				while (!acc->empty() && (acc->back() == L'\n' || acc->back() == L' ')) acc->pop_back();
				(*cb)(*acc, std::wstring{}, err);
			});
	}

	auto task = std::make_shared<AiService::Task>();
	auto setting = Setting::get();
	auto ak = cleanKey(setting->getAiStr(L"volcAk", L""));
	auto sk = cleanKey(setting->getAiStr(L"volcSk", L""));
	if (ak.empty() || sk.empty()) {
		postResult(task, *cb, {}, {}, Lang::get(L"ai.noKey"));
		return task;
	}
	std::vector<std::wstring> parts;
	if (!splitParts(text, parts)) {
		postResult(task, *cb, {}, {}, Lang::get(L"ai.transTooLong"));
		return task;
	}
	doVolc(task, std::move(ak), std::move(sk), from, to, std::move(parts), *cb);
	return task;
}
