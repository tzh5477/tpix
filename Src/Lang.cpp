#include "pch.h"
#include <filesystem>
#include "Lang.h"
#include "Util.h"
#include "Setting.h"

std::unique_ptr<Lang> lang;

namespace {
	// 语言文件优先从 exe 同目录的 Lang 子目录加载（绿色部署 / 调试方便），
	// 那个目录不存在才退回 %appdata%\tpix\Lang。
	// directory_iterator 碰到不存在的目录会抛 filesystem_error，
	// 所以用带 error_code 的重载，目录不在就当没有额外语言。
	std::vector<std::filesystem::path> getLangFiles()
	{
		std::vector<std::filesystem::path> result;
		wchar_t buffer[MAX_PATH]{};
		GetModuleFileName(nullptr, buffer, MAX_PATH);
		auto langPath = std::filesystem::path{ buffer }.parent_path().append(L"Lang");
		if (!std::filesystem::is_directory(langPath)) {
			langPath = Setting::get()->getDataPath().append(L"Lang");
		}
		std::error_code ec;
		std::filesystem::directory_iterator it{ langPath, ec }, end{};
		for (; !ec && it != end; it.increment(ec)) {
			result.push_back(it->path());
		}
		return result;
	}

	// 按点分路径逐级下钻，末段取字符串。任一级缺失 / 不是对象 / 末段不是字符串，
	// 一律返回空串，交给 Lang::get 继续找兜底。
	//
	// 这里刻意不用带默认值的 GetNamedObject / GetNamedString：它们只兜"键不存在"，
	// **兜不住"类型不对"** —— 名字在、值却是另一种类型时，内部会走到
	// IJsonValue::GetObject / GetString，那两个在 winrt\Windows.Data.Json.h 里就是
	// 一句 check_hresult，类型不符直接返回 E_ILLEGAL_METHOD_CALL
	// （0x8000000E，"A method was called at an unexpected time"），cppwinrt 抛出
	// hresult_illegal_method_call。这个异常从 DispatcherQueue 回调里穿出去就是
	// 0xC000027B stowed fail-fast，进程当场没 —— 2026-10-05 的托盘"设置"崩溃即此：
	// Lang::get(L"setting.ball.title") 只按前两段找，把对象 setting.ball 当字符串取。
	std::wstring getNestedString(JsonObject root, const std::vector<std::wstring>& path)
	{
		JsonObject cur{ root };
		for (size_t i = 0; i + 1 < path.size(); i++) {
			if (!cur) return {};
			auto value = cur.GetNamedValue(path[i], nullptr);
			if (!value || value.ValueType() != JsonValueType::Object) return {};
			cur = value.GetObject();
		}
		if (!cur) return {};
		auto leaf = cur.GetNamedValue(path.back(), nullptr);
		if (!leaf || leaf.ValueType() != JsonValueType::String) return {};
		return std::wstring{ leaf.GetString() };
	}
}

Lang::Lang()
{
}

Lang::~Lang()
{
}

void Lang::init()
{
	auto ptr = new Lang();
	lang.reset(ptr);
	lang->initLang(Setting::get()->getLang());
}

void Lang::dispose()
{
	lang.reset();
}

Lang* Lang::get()
{
	return lang.get();
}

std::wstring Lang::get(const std::wstring& keyPath)
{
	// keyPath 是 "组.键" 或 "组.子组.键" 这种点分路径，按段数逐级下钻（setting.ball.title
	// 是三层）。第三方语言文件缺键是常态（程序加了新文案，人家的文件还是老的），
	// 缺了先找内置的 en-US，连那儿也没有就把键名本身显示出来 —— 界面上难看，
	// 但比崩掉好，也一眼能看出缺哪个键。类型不符也走同一条兜底（见 getNestedString）
	auto arr = Ling::Util::splitStr(keyPath, L'.');
	if (arr.size() < 2) return keyPath;
	auto& self = *lang;
	auto text = getNestedString(self.langObj, arr);
	if (!text.empty()) return text;
	if (!self.fallbackObj) return keyPath;
	text = getNestedString(self.fallbackObj, arr);
	if (text.empty()) return keyPath;
	return text;
}

void Lang::initLang(const std::wstring& langCode)
{
	auto [pData, size] = Ling::Util::getRes(langCode);
	std::wstring builtinJson;
	if (pData && size > 0)
		builtinJson = Ling::Util::readTextFromBytes(pData, size);
	JsonObject builtinObj{ nullptr };
	if (!builtinJson.empty())
		builtinObj = JsonObject::Parse(builtinJson);

	if (langCode == L"zh-CN" || langCode == L"en-US") {
		langObj = builtinObj;
		return;
	}
	// 第三方语言文件：用户自己放进来的，可能是老版本的（缺新加的键）、也可能手工改坏了。
	// 所以一是用 TryParse（Parse 解析失败直接抛，一路抛出去就是进程没了），
	// 二是把内置的 en-US 留在 fallbackObj 里给 Lang::get 兜底
	fallbackObj = builtinObj;
	bool loaded{ false };
	for (const auto& entry : getLangFiles()) {
		std::wstring filename = entry.filename().wstring();
		if (filename.find(langCode) == std::wstring::npos) continue;
		auto pathStr = entry.wstring();
		std::wstring content = Ling::Util::readFileText(entry);
		JsonObject obj{ nullptr };
		if (JsonObject::TryParse(content, obj)) {
			langObj = obj;
			loaded = true;
		}
		else {
			auto msg = L"lang pare error：" + pathStr + L"\n use English";
			MessageBox(nullptr, msg.data(), L"tpix", MB_OK | MB_ICONWARNING);
		}
		break;
	}
	//文件没找到、或者内容不是合法 JSON：整份都用内置的 en-US
	if (!loaded) langObj = fallbackObj;
}

std::vector<std::pair<std::wstring, std::wstring>> Lang::getSupportedLang()
{
	std::vector<std::pair<std::wstring, std::wstring>> result = { {L"简体中文",L"zh-CN"},{L"English",L"en-US"} };
	for (const auto& entry : getLangFiles()) {
		std::wstring filename = entry.filename().wstring();
		auto arr = Ling::Util::splitStr(filename, L'.');
		if (arr.size() == 3 && arr[2] == L"json") {
			result.push_back({ arr[0] ,arr[1] });
		}
	}
	return result;
}
