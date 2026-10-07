#include "pch.h"
#include <winrt/Windows.Data.Json.h>
#include <algorithm>
#include <chrono>
#include <format>
#include "AiHistory.h"
#include "Setting.h"

using namespace winrt::Windows::Data::Json;

namespace {
	std::unique_ptr<AiHistory> aiHistory;
	// 列表里那句标题最多留多少字。再长就把列表撑成了正文预览
	constexpr size_t titleMax{ 40 };
	constexpr long long dayMs{ 24LL * 60 * 60 * 1000 };

	long long nowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	}

	// 列表里显示的那一句：换行在单行列表里会把行距撑开，先压成空格
	std::wstring makeTitle(const std::wstring& text)
	{
		std::wstring out;
		for (auto ch : text) {
			if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
			out.push_back(ch);
		}
		while (!out.empty() && out.front() == L' ') out.erase(0, 1);
		if (out.size() > titleMax) {
			out.resize(titleMax);
			out.push_back(L'…');
		}
		return out;
	}

	int roleNum(AiService::Role role)
	{
		return static_cast<int>(role);
	}

	AiService::Role roleFromNum(double num)
	{
		switch (static_cast<int>(num)) {
		case 0: return AiService::Role::System;
		case 2: return AiService::Role::Assistant;
		default: return AiService::Role::User;
		}
	}
}

void AiHistory::init()
{
	aiHistory.reset(new AiHistory());
}

void AiHistory::dispose()
{
	aiHistory.reset();
}

AiHistory* AiHistory::get()
{
	return aiHistory.get();
}

long long AiHistory::now()
{
	return nowMs();
}

AiHistory::AiHistory()
{
	dir = Setting::get()->getDataPath() / L"ai";
	file = dir / L"history.json";
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	load();
}

AiHistory::~AiHistory() = default;

long long AiHistory::create()
{
	auto id = nowMs();
	// 同一毫秒里连开两个（启动恢复时可能）会撞 id，往后顺一位。sessions 是按时间升序的，
	// 所以末位就是目前最大的那个
	if (!sessions.empty() && id <= sessions.back().id) id = sessions.back().id + 1;
	Session s;
	s.id = id;
	s.time = id;
	sessions.push_back(std::move(s));
	// 刻意不写盘：开了会话却一句话没说就关掉，不该在历史里留一条空的
	return id;
}

void AiHistory::append(const long long id, const Msg& msg)
{
	if (msg.content.empty()) return;
	if (!Setting::get()->getAiHistorySave()) return;
	auto it = std::ranges::find(sessions, id, &Session::id);
	if (it == sessions.end()) return;
	if (it->title.empty() && msg.role == AiService::Role::User) it->title = makeTitle(msg.content);
	// 只存 role + content（外加显示用的时间戳）：图不落盘也不留在内存里
	// （一张满屏截图的 base64 有几 MB）
	Msg stored{ msg.role, msg.content };
	stored.time = msg.time > 0 ? msg.time : nowMs();
	it->msgs.push_back(std::move(stored));
	it->time = nowMs();
	save();
}

void AiHistory::setMsgs(const long long id, const std::vector<Msg>& msgs)
{
	auto it = std::ranges::find(sessions, id, &Session::id);
	if (it == sessions.end()) return;
	it->msgs = msgs;
	// 标题跟着第一条用户消息重算：把开着的那条问题删掉之后，列表里不该还挂着它的标题
	it->title.clear();
	for (auto& msg : it->msgs) {
		if (msg.role == AiService::Role::User) {
			it->title = makeTitle(msg.content);
			break;
		}
	}
	save();
}

void AiHistory::rename(const long long id, const std::wstring& title)
{
	auto it = std::ranges::find(sessions, id, &Session::id);
	if (it == sessions.end()) return;
	if (it->title == title) return;
	it->title = title;
	save();
}

void AiHistory::remove(const long long id)
{
	auto n = sessions.size();
	std::erase_if(sessions, [id](const Session& s) { return s.id == id; });
	if (sessions.size() == n) return;
	save();
}

void AiHistory::clear()
{
	sessions.clear();
	std::error_code ec;
	std::filesystem::remove(file, ec);
}

std::vector<AiHistory::Session> AiHistory::list() const
{
	// sessions 里是升序（旧的在前），列表要新的在前
	std::vector<Session> out{ sessions };
	std::ranges::sort(out, [](const Session& a, const Session& b) { return a.time > b.time; });
	return out;
}

const AiHistory::Session* AiHistory::find(const long long id) const
{
	auto it = std::ranges::find(sessions, id, &Session::id);
	if (it == sessions.end()) return nullptr;
	return &*it;
}

void AiHistory::load()
{
	if (!Setting::get()->getAiHistorySave()) return;
	if (!std::filesystem::exists(file)) return;
	JsonObject obj{ nullptr };
	if (!JsonObject::TryParse(Ling::Util::readFileText(file), obj) || !obj) return;
	auto arr = obj.GetNamedArray(L"sessions", nullptr);
	if (!arr) return;
	for (auto&& val : arr) {
		// 手改过的文件里什么都可能躺在这一层，不是对象就跳过，别让它把整个历史搞崩
		if (val.ValueType() != JsonValueType::Object) continue;
		auto one = val.GetObject();
		Session s;
		s.id = static_cast<long long>(one.GetNamedNumber(L"id", 0.0));
		if (s.id == 0) continue;
		s.time = static_cast<long long>(one.GetNamedNumber(L"time", 0.0));
		s.title = std::wstring{ one.GetNamedString(L"title", L"") };
		auto msgs = one.GetNamedArray(L"msgs", nullptr);
		if (msgs) {
			for (auto&& mv : msgs) {
				if (mv.ValueType() != JsonValueType::Object) continue;
				auto mo = mv.GetObject();
				Msg msg;
				msg.role = roleFromNum(mo.GetNamedNumber(L"role", 1.0));
				msg.content = std::wstring{ mo.GetNamedString(L"content", L"") };
				if (msg.content.empty()) continue;
				// 老文件里没有 t（那时还没记时间），读出来是 0 —— 界面上按"时间未知"处理
				msg.time = static_cast<long long>(mo.GetNamedNumber(L"t", 0.0));
				s.msgs.push_back(std::move(msg));
			}
		}
		sessions.push_back(std::move(s));
	}
	std::ranges::sort(sessions, [](const Session& a, const Session& b) { return a.time < b.time; });
	// 配置可能在关掉程序的这段时间里被改小了，读进来就按新配置清一次
	auto before = sessions.size();
	trim();
	if (sessions.size() != before) save();
}

void AiHistory::save()
{
	// 关掉"保存历史"之后磁盘上不该还留着聊天记录。这里删文件，而不是留一个空数组
	if (!Setting::get()->getAiHistorySave()) {
		std::error_code ec;
		std::filesystem::remove(file, ec);
		sessions.clear();
		return;
	}
	trim();
	JsonArray arr;
	for (auto& s : sessions) {
		JsonObject one;
		one.SetNamedValue(L"id", JsonValue::CreateNumberValue(static_cast<double>(s.id)));
		one.SetNamedValue(L"time", JsonValue::CreateNumberValue(static_cast<double>(s.time)));
		one.SetNamedValue(L"title", JsonValue::CreateStringValue(s.title));
		JsonArray msgs;
		for (auto& msg : s.msgs) {
			JsonObject mo;
			mo.SetNamedValue(L"role", JsonValue::CreateNumberValue((double)roleNum(msg.role)));
			mo.SetNamedValue(L"content", JsonValue::CreateStringValue(msg.content));
			mo.SetNamedValue(L"t", JsonValue::CreateNumberValue(static_cast<double>(msg.time)));
			msgs.Append(mo);
		}
		one.SetNamedValue(L"msgs", msgs);
		arr.Append(one);
	}
	JsonObject root;
	root.SetNamedValue(L"sessions", arr);
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	Ling::Util::saveFile(file.wstring(), std::wstring{ root.Stringify() });
}

void AiHistory::trim()
{
	auto days = Setting::get()->getAiHistoryDays();
	auto cutoff = nowMs() - static_cast<long long>(days) * dayMs;
	std::erase_if(sessions, [cutoff](const Session& s) { return s.time < cutoff; });
	auto limit = Setting::get()->getAiHistoryLimit();
	// sessions 是升序，超了就从最旧那条（队首）开始删
	while (static_cast<int>(sessions.size()) > limit) sessions.erase(sessions.begin());
}
