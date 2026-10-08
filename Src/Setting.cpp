#include "pch.h"
#include <include/Ling.h>
#include "Setting.h"
#include "Util.h"
#include "Lang.h"
#include "Win/WinAiChat.h"
#include "Win/WinAiTrans.h"
#include "Win/WinCap.h"
#include "Win/WinHistory.h"
#include "Win/WinBall.h"
#include "Win/WinOverlay.h"
#include "PinSource.h"
#include "App.h"

namespace {
    std::unique_ptr<Setting> setting;
    // 热键 id 挂在 Ling 的消息窗口上，是全局的；与托盘菜单 id（163 起）、
    // 各窗口自己的定时器 id 不是一套命名空间，互不干扰
    constexpr int capShortcutMsgId{ 100 };
    constexpr int pinLastMsgId{ 101 };
    constexpr int historyShortcutMsgId{ 102 };
    constexpr int ballShortcutMsgId{ 103 };
    constexpr int pinClipShortcutMsgId{ 104 };
    constexpr int rulerShortcutMsgId{ 105 };
    constexpr int crosshairShortcutMsgId{ 106 };
    constexpr int focusShortcutMsgId{ 107 };
    constexpr int aiTransShortcutMsgId{ 108 };
    constexpr int aiChatShortcutMsgId{ 109 };

    struct ShortcutDef { std::wstring_view type; int msgId; std::wstring_view def; };
    // 一张表管住"配置键名 → 消息 id → 默认组合"，加一个可配快捷键的动作只改这里一行。
    // def 留空表示"默认不占用"：这类动作不是人人都用，占掉一个组合反而碍事，用户想要自己设
    const ShortcutDef shortcutDefs[]{
        { L"cap",       capShortcutMsgId,       L"Ctrl+Alt+A" },
        { L"pinLast",   pinLastMsgId,           L"Ctrl+Alt+Z" },
        { L"history",   historyShortcutMsgId,   L"Ctrl+Alt+H" },
        { L"ball",      ballShortcutMsgId,      L"" },
        { L"pinClip",   pinClipShortcutMsgId,   L"" },
        { L"ruler",     rulerShortcutMsgId,     L"" },
        { L"crosshair", crosshairShortcutMsgId, L"" },
        { L"focus",     focusShortcutMsgId,     L"" },
        { L"aiTrans",   aiTransShortcutMsgId,   L"Alt+T" },
        { L"aiChat",    aiChatShortcutMsgId,    L"Alt+I" },
    };

    const ShortcutDef* findShortcutDef(const std::wstring& type)
    {
        for (auto& def : shortcutDefs) {
            if (def.type == type) return &def;
        }
        return nullptr;
    }
    // 从一个已确认是对象的 JsonObject 里读出一个接口。列表遍历与「按 id 取」共用这一份，
    // 免得"下拉框里看到的"和"实际发请求用的"哪天长成两套读法
    AiProvider readProvider(const JsonObject& obj)
    {
        AiProvider p;
        p.id = std::wstring{ obj.GetNamedString(L"id", L"") };
        p.name = std::wstring{ obj.GetNamedString(L"name", L"") };
        p.baseUrl = std::wstring{ obj.GetNamedString(L"baseUrl", L"") };
        p.apiKey = std::wstring{ obj.GetNamedString(L"apiKey", L"") };
        p.model = std::wstring{ obj.GetNamedString(L"model", L"") };
        auto models = obj.GetNamedArray(L"models", nullptr);
        if (models) {
            for (auto&& item : models) {
                // 手改过的配置里什么都可能躺在这一层，不是字符串就跳过，别让它把下拉框搞崩
                if (item.ValueType() != JsonValueType::String) continue;
                p.models.push_back(std::wstring{ item.GetString() });
            }
        }
        return p;
    }

    // 迁移老配置时第 0 个接口用的 id。ensureProviders 与 ensureAiMigration 只可能跑到一次，
    // 之后它就是一个普通接口的 id，与界面上「新增」生成的不冲突（那个从 p1 起，见 newProviderId）
    constexpr std::wstring_view firstProviderId{ L"p0" };

    // 配置文件的默认内容。空文件、坏 JSON、缺键都拿它兜底，所以这里列出的每一项
    // 都是代码里会直接按名字取的（见 getLang / getAutoStart / initShortcutKeys）
    constexpr std::wstring_view defaultConfig{ LR"""({"common":{"autoStart":false,"language":"zh-CN"},"shortcutKey":{"cap":"Ctrl+Alt+A","pinLast":"Ctrl+Alt+Z"}})""" };
}


Setting::Setting() :dataPath{ initDataPath() }, configPath{ initConfigPath() }
{
    if (std::filesystem::exists(configPath)) {
        auto content = Ling::Util::readFileText(configPath);
        if (content.empty() || content.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
            configObj = JsonObject::Parse(defaultConfig);
            save();
        }
        else {
            JsonObject obj{ nullptr };
            if (JsonObject::TryParse(content, obj)) {
                configObj = obj;
            }
            else {
                MessageBox(nullptr, L"config.json parse error，use default config", L"tpix", MB_OK | MB_ICONWARNING);
                configObj = JsonObject::Parse(defaultConfig);
            }
        }
    }
    else {
        configObj = JsonObject::Parse(defaultConfig);
    }
    // 无论走了哪条路都要补一遍：接口列表的迁移与「至少有一个接口」的兜底都在这里
    ensureProviders();
}



Setting::~Setting()
{

}

void Setting::init()
{
    auto ptr = new Setting();
    setting.reset(ptr);
}

void Setting::dispose()
{
    setting.reset();
}

Setting* Setting::get()
{
    return setting.get();
}

std::filesystem::path Setting::getDataPath()
{
    return dataPath; //复制一份路径对象，不允许就地修改
}

const JsonObject Setting::getConfigObj()
{
    return configObj;
}

void Setting::setShortcutKey(const std::wstring& type, const std::vector<std::wstring>& keys)
{
    std::wstring str;
    for (size_t i = 0; i < keys.size(); i++)
    {
        str += L"+" + keys[i];
    }
    str.erase(0,1);
    auto shortcutKey = configObj.GetNamedObject(L"shortcutKey");
    shortcutKey.SetNamedValue(type, JsonValue::CreateStringValue(str));
    auto app = Ling::App::get();
    auto def = findShortcutDef(type);
    if (!def) return;
    // 清空就是不占这个组合了：只撤注册，不注册空的
    app->unRegHotKey(def->msgId);
    if (!str.empty()) app->regHotKey(str, def->msgId);
    save();
}

std::wstring Setting::getShortcutKey(const std::wstring& type)
{
    // 一路用带默认值的重载：启动时 ensureDefaults 已经补齐过，这里只是别让运行期
    // 意外（配置被外部改动、问了个没配过的 type）变成一次崩溃
    auto obj = configObj.GetNamedObject(L"shortcutKey", nullptr);
    if (!obj) return L"";
    return std::wstring{ obj.GetNamedString(type, L"") };
}

void Setting::setAutoStart(bool autoStart)
{
    std::wstring runKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (autoStart) {
        wchar_t buffer[MAX_PATH];
        GetModuleFileName(nullptr, buffer, MAX_PATH);
        auto curPath = std::filesystem::path(buffer);
        std::wstring commandLine = std::format(L"\"{}\" --auto-start", curPath.wstring());
        HKEY hKey;
        if (RegOpenKeyEx(HKEY_CURRENT_USER, runKey.data(), 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
            RegSetValueEx(hKey, L"tpix", 0, REG_SZ, (const BYTE*)commandLine.data(), (DWORD)((commandLine.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }
    }
    else {
        HKEY hKey;
        if (RegOpenKeyEx(HKEY_CURRENT_USER, runKey.data(), 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
            RegDeleteValue(hKey, L"tpix");
            RegCloseKey(hKey);
        }
    }
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) {
        common = JsonObject();
        configObj.SetNamedValue(L"common", common);
    }
    common.SetNamedValue(L"autoStart", JsonValue::CreateBooleanValue(autoStart));
    save();
}

bool Setting::getAutoStart()
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    return common && common.GetNamedBoolean(L"autoStart", false);
}

std::filesystem::path Setting::initDataPath()
{
    PWSTR pathTmp;
    auto hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &pathTmp);
    if (FAILED(hr)) {
        _ASSERT_EXPR(FALSE, L"get roaming path，error");
        return L"";
    }
    auto dataPath = std::filesystem::path{ pathTmp };
    CoTaskMemFree(pathTmp);
    dataPath.append("tpix");
    if (!std::filesystem::exists(dataPath)) {
        if (!std::filesystem::create_directories(dataPath)) {
            _ASSERT_EXPR(FALSE, L"create data path，error");
        }
    }
    return dataPath;
}

std::filesystem::path Setting::initConfigPath()
{
    // 与插件的查找顺序一致（见 Util.cpp 里的 findImageReader）：先看 exe 同目录。
    // 只有那份文件本来就存在时才认它 —— 不存在就不要在程序目录里新建，
    // 装在 Program Files 下时那儿通常没有写权限，况且默认位置该是 appdata
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileName(nullptr, buffer, MAX_PATH);
    auto path = std::filesystem::path{ buffer }.parent_path().append(L"config.json");
    if (std::filesystem::exists(path)) return path;
    auto fallback = this->dataPath; //复制一份路径对象，append 会就地改
    return fallback.append(L"config.json");
}

void Setting::save()
{
    std::wstring str{ configObj.Stringify() };
    Ling::Util::saveFile(configPath.wstring(), str);
}

std::wstring Setting::getLang()
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) return L"zh-CN";
    return std::wstring{ common.GetNamedString(L"language", L"zh-CN") };
}

void Setting::setLang(const std::wstring& langCode)
{
    auto common = setting->configObj.GetNamedObject(L"common", nullptr);
    if (!common) {
        common = JsonObject();
        setting->configObj.SetNamedValue(L"common", common);
    }
    common.SetNamedValue(L"language", JsonValue::CreateStringValue(langCode));
    setting->save();
	Lang::get()->initLang(langCode);
}

JsonObject Setting::getToolObj(const std::wstring& tool)
{
    // 用带默认值的重载：这两层在旧配置文件里都不存在，直接 GetNamedObject 会抛异常，
    // 值被手工改成非对象时它也一样返回默认值，不会炸
    auto root = configObj.GetNamedObject(L"toolPin", nullptr);
    if (!root) {
        root = JsonObject();
        configObj.SetNamedValue(L"toolPin", root);
    }
    auto obj = root.GetNamedObject(tool, nullptr);
    if (!obj) {
        obj = JsonObject();
        root.SetNamedValue(tool, obj);
    }
    return obj;
}

bool Setting::getToolFlag(const std::wstring& tool, const std::wstring& key, bool def)
{
    return getToolObj(tool).GetNamedBoolean(key, def);
}

void Setting::setToolFlag(const std::wstring& tool, const std::wstring& key, bool val)
{
    getToolObj(tool).SetNamedValue(key, JsonValue::CreateBooleanValue(val));
    save();
}

float Setting::getToolNum(const std::wstring& tool, const std::wstring& key, float def)
{
    return static_cast<float>(getToolObj(tool).GetNamedNumber(key, def));
}

std::wstring Setting::getToolStr(const std::wstring& tool, const std::wstring& key, const std::wstring& def)
{
    return std::wstring{ getToolObj(tool).GetNamedString(key, def) };
}

void Setting::setToolStr(const std::wstring& tool, const std::wstring& key, const std::wstring& val)
{
    getToolObj(tool).SetNamedValue(key, JsonValue::CreateStringValue(val));
    save();
}

void Setting::setToolNum(const std::wstring& tool, const std::wstring& key, float val)
{
    getToolObj(tool).SetNamedValue(key, JsonValue::CreateNumberValue(val));
    save();
}

JsonObject Setting::getAiObj()
{
	auto obj = configObj.GetNamedObject(L"ai", nullptr);
	if (!obj) {
		obj = JsonObject();
		configObj.SetNamedValue(L"ai", obj);
	}
	return obj;
}

std::wstring Setting::getAiStr(const std::wstring& key, const std::wstring& def)
{
	return std::wstring{ getAiObj().GetNamedString(key, def) };
}

void Setting::setAiStr(const std::wstring& key, const std::wstring& val)
{
	getAiObj().SetNamedValue(key, JsonValue::CreateStringValue(val));
	save();
}

JsonArray Setting::getAiProvidersArray()
{
	auto arr = getAiObj().GetNamedArray(L"providers", nullptr);
	if (!arr) {
		arr = JsonArray();
		getAiObj().SetNamedValue(L"providers", arr);
	}
	return arr;
}

JsonObject Setting::getScenarioObj()
{
	auto obj = getAiObj().GetNamedObject(L"scenarios", nullptr);
	if (!obj) {
		obj = JsonObject();
		getAiObj().SetNamedValue(L"scenarios", obj);
	}
	return obj;
}

void Setting::ensureProviders()
{
	if (getAiProvidersArray().Size() > 0) return;
	// 老版本把地址 / 密钥 / 模型 / 模型列表平铺在 ai 组里，搬进来当第 0 个接口 ——
	// 用户早就填好的东西不该因为换了版本就变成"没配过"
	AiProvider legacy;
	auto arr = getAiObj().GetNamedArray(L"models", nullptr);
	if (arr) {
		for (auto&& item : arr) {
			if (item.ValueType() != JsonValueType::String) continue;
			legacy.models.push_back(std::wstring{ item.GetString() });
		}
	}
	legacy.baseUrl = getAiStr(L"baseUrl", L"");
	legacy.apiKey = getAiStr(L"apiKey", L"");
	legacy.model = getAiStr(L"model", L"");
	legacy.id = firstProviderId;
	// 名字刻意留空，由 providerName 在界面那一层给默认名 —— 这里还不能碰语言包：
	// Setting::init 排在 Lang::init 之前（见 App.cpp 的构造顺序）
	setAiProvider(legacy);
}

std::wstring Setting::newProviderId()
{
	const auto list = getAiProviders();
	// 从 p1 起（p0 留给迁移出来的那一个），每次现查一遍有没有被占 ——
	// 删过中间某一项之后条数会对不上，不能按 list.size() 推
	for (int i = 1; i < 10000; ++i) {
		auto id = std::wstring{ L"p" } + std::to_wstring(i);
		bool taken{ false };
		for (const auto& p : list) {
			if (p.id == id) { taken = true; break; }
		}
		if (!taken) return id;
	}
	return std::wstring{ L"p" } + std::to_wstring(list.size() + 1);
}

std::vector<AiProvider> Setting::getAiProviders()
{
	std::vector<AiProvider> list;
	for (auto&& item : getAiProvidersArray()) {
		// 手改过的配置里这一层什么都可能躺着，不是对象就跳过
		if (item.ValueType() != JsonValueType::Object) continue;
		auto one = readProvider(item.GetObject());
		// 没有 id 就没法被场景引用，等于没法用
		if (one.id.empty()) continue;
		list.push_back(std::move(one));
	}
	return list;
}

bool Setting::getAiProvider(const std::wstring& id, AiProvider& out)
{
	if (id.empty()) return false;
	for (auto&& item : getAiProvidersArray()) {
		if (item.ValueType() != JsonValueType::Object) continue;
		auto one = readProvider(item.GetObject());
		if (one.id != id) continue;
		out = std::move(one);
		return true;
	}
	return false;
}

void Setting::setAiProvider(const AiProvider& provider)
{
	if (provider.id.empty()) return;
	JsonObject obj;
	obj.SetNamedValue(L"id", JsonValue::CreateStringValue(provider.id));
	obj.SetNamedValue(L"name", JsonValue::CreateStringValue(provider.name));
	obj.SetNamedValue(L"baseUrl", JsonValue::CreateStringValue(provider.baseUrl));
	obj.SetNamedValue(L"apiKey", JsonValue::CreateStringValue(provider.apiKey));
	obj.SetNamedValue(L"model", JsonValue::CreateStringValue(provider.model));
	JsonArray models;
	for (const auto& id : provider.models) models.Append(JsonValue::CreateStringValue(id));
	obj.SetNamedValue(L"models", models);

	auto arr = getAiProvidersArray();
	for (uint32_t i = 0; i < arr.Size(); ++i) {
		auto item = arr.GetAt(i);
		if (item.ValueType() != JsonValueType::Object) continue;
		if (std::wstring{ item.GetObject().GetNamedString(L"id", L"") } != provider.id) continue;
		arr.SetAt(i, obj);
		save();
		return;
	}
	arr.Append(obj);
	save();
}

void Setting::removeAiProvider(const std::wstring& id)
{
	auto arr = getAiProvidersArray();
	for (uint32_t i = 0; i < arr.Size(); ++i) {
		auto item = arr.GetAt(i);
		if (item.ValueType() != JsonValueType::Object) continue;
		if (std::wstring{ item.GetObject().GetNamedString(L"id", L"") } != id) continue;
		arr.RemoveAt(i);
		save();
		// 绑定在这个接口上的场景不用单独收拾：getScenarioProvider 每次读都会核对它
		// 还在不在，没了就自然退回现有的第 0 个接口
		return;
	}
}

std::wstring Setting::getScenarioProvider(const std::wstring& scenario)
{
	auto obj = getScenarioObj().GetNamedObject(scenario, nullptr);
	if (obj) {
		auto id = std::wstring{ obj.GetNamedString(L"provider", L"") };
		AiProvider tmp;
		if (!id.empty() && getAiProvider(id, tmp)) return id;   // 绑的那个还在
	}
	// 没绑过 / 绑的那个已经被删了：退回现有的第 0 个接口，调用方不必先判空
	auto list = getAiProviders();
	return list.empty() ? std::wstring{} : list.front().id;
}

std::wstring Setting::getScenarioModel(const std::wstring& scenario)
{
	auto obj = getScenarioObj().GetNamedObject(scenario, nullptr);
	if (obj) {
		auto providerId = std::wstring{ obj.GetNamedString(L"provider", L"") };
		AiProvider owner;
		// 只有当这一整套还指着某个活着的接口时，它记的那个模型才算数
		if (!providerId.empty() && getAiProvider(providerId, owner)) {
			auto model = std::wstring{ obj.GetNamedString(L"model", L"") };
			if (!model.empty()) return model;
			return owner.model;
		}
	}
	AiProvider fallback;
	if (getAiProvider(getScenarioProvider(scenario), fallback)) return fallback.model;
	return std::wstring{};
}

void Setting::setScenario(const std::wstring& scenario, const std::wstring& providerId,
	const std::wstring& model)
{
	JsonObject obj;
	obj.SetNamedValue(L"provider", JsonValue::CreateStringValue(providerId));
	obj.SetNamedValue(L"model", JsonValue::CreateStringValue(model));
	getScenarioObj().SetNamedValue(scenario, obj);
	save();
}

AiCred Setting::credFor(const std::wstring& scenario)
{
	AiCred cred;
	auto providerId = getScenarioProvider(scenario);
	AiProvider provider;
	if (!getAiProvider(providerId, provider)) {
		// 一个接口都没有：三个串都留空，让调用方去提示"请先到设置里新增接口"
		return cred;
	}
	cred.providerId = provider.id;
	cred.providerName = providerName(provider);
	cred.baseUrl = provider.baseUrl;
	cred.apiKey = provider.apiKey;
	cred.model = getScenarioModel(scenario);
	if (cred.model.empty()) cred.model = provider.model;
	// 三样缺一样就不能发：少一个地址或者少一个模型，发出去只会收获一句看不懂的报错
	if (cred.baseUrl.empty() || cred.apiKey.empty() || cred.model.empty()) return cred;
	cred.ok = true;
	return cred;
}

std::wstring Setting::providerName(const AiProvider& provider)
{
	// 名称是用户自己填的，允许为空 —— 那时给个语言包里的默认名，让用户知道这一项是什么。
	// 放在这里兜而不是让每个调用方各判一次：设置页与对话窗都要用
	return provider.name.empty() ? Lang::get(L"ai.defProvider") : provider.name;
}

bool Setting::getAiHistorySave()
{
	return getAiObj().GetNamedBoolean(L"historySave", true);
}

void Setting::setAiHistorySave(const bool val)
{
	getAiObj().SetNamedValue(L"historySave", JsonValue::CreateBooleanValue(val));
	save();
}

int Setting::getAiHistoryLimit()
{
	// 夹到 [1, 500]：0 会让"刚发出去的一问一答"立刻被清掉，那已经不是历史了；
	// 上限则是给磁盘兜底 —— 会话里存的是纯文本，几百条之后文件会有几 MB
	auto val = (int)getAiObj().GetNamedNumber(L"historyLimit", 50.0);
	if (val < 1) return 1;
	if (val > 500) return 500;
	return val;
}

void Setting::setAiHistoryLimit(const int val)
{
	getAiObj().SetNamedValue(L"historyLimit", JsonValue::CreateNumberValue((double)val));
	save();
}

int Setting::getAiHistoryDays()
{
	auto val = (int)getAiObj().GetNamedNumber(L"historyDays", 30.0);
	if (val < 1) return 1;
	if (val > 3650) return 3650;
	return val;
}

void Setting::setAiHistoryDays(const int val)
{
	getAiObj().SetNamedValue(L"historyDays", JsonValue::CreateNumberValue((double)val));
	save();
}

JsonObject Setting::getSaveObj()
{
	auto obj = configObj.GetNamedObject(L"save", nullptr);
	if (!obj) {
		obj = JsonObject();
		configObj.SetNamedValue(L"save", obj);
	}
	return obj;
}

int Setting::getSaveFormat()
{
	return static_cast<int>(getSaveObj().GetNamedNumber(L"format", 0.0));
}

void Setting::setSaveFormat(int val)
{
	getSaveObj().SetNamedValue(L"format", JsonValue::CreateNumberValue(static_cast<double>(val)));
	save();
}

bool Setting::getAutoSave()
{
	return getSaveObj().GetNamedBoolean(L"auto", false);
}

void Setting::setAutoSave(bool val)
{
	getSaveObj().SetNamedValue(L"auto", JsonValue::CreateBooleanValue(val));
	save();
}

std::wstring Setting::getSaveDir()
{
	return std::wstring{ getSaveObj().GetNamedString(L"dir", L"") };
}

void Setting::setSaveDir(const std::wstring& dir)
{
	getSaveObj().SetNamedValue(L"dir", JsonValue::CreateStringValue(dir));
	save();
}

std::wstring Setting::getSaveNameTpl()
{
	return std::wstring{ getSaveObj().GetNamedString(L"nameTpl", L"%y%m%d_%H%M%S") };
}

void Setting::setSaveNameTpl(const std::wstring& tpl)
{
	getSaveObj().SetNamedValue(L"nameTpl", JsonValue::CreateStringValue(tpl));
	save();
}

bool Setting::getAutoPaste()
{
	// 默认关：它会把焦点从 tpix 挪走，还往别人的窗口里塞东西，不该静默生效
	return getSaveObj().GetNamedBoolean(L"autoPaste", false);
}

void Setting::setAutoPaste(bool val)
{
	getSaveObj().SetNamedValue(L"autoPaste", JsonValue::CreateBooleanValue(val));
	save();
}

int Setting::getHistoryLimit()
{
	// 夹到 [10, 2000]：太小了历史没意义，太大了数据目录会堆出几个 G 的图片
	auto val = (int)getSaveObj().GetNamedNumber(L"historyLimit", 100.0);
	if (val < 10) return 10;
	if (val > 2000) return 2000;
	return val;
}

void Setting::setHistoryLimit(int val)
{
	getSaveObj().SetNamedValue(L"historyLimit", JsonValue::CreateNumberValue((double)val));
	save();
}

bool Setting::getClipboardHistory()
{
	return getSaveObj().GetNamedBoolean(L"clipHistory", false);
}

void Setting::setClipboardHistory(bool val)
{
	getSaveObj().SetNamedValue(L"clipHistory", JsonValue::CreateBooleanValue(val));
	save();
}

JsonObject Setting::getPinObj()
{
	auto obj = configObj.GetNamedObject(L"pin", nullptr);
	if (!obj) {
		obj = JsonObject();
		configObj.SetNamedValue(L"pin", obj);
	}
	return obj;
}

winrt::Windows::Data::Json::JsonArray Setting::getPins()
{
	// 缺 items 时现建一个挂上去：GetNamedArray 对不存在的键返回的是"空对象"而不是
	// 空 JsonArray，直接拿去迭代/清空会在启动时必崩
	auto arr = getPinObj().GetNamedArray(L"items", nullptr);
	if (!arr) {
		arr = JsonArray();
		getPinObj().SetNamedValue(L"items", arr);
	}
	return arr;
}

void Setting::setPins(const winrt::Windows::Data::Json::JsonArray& arr)
{
	getPinObj().SetNamedValue(L"items", arr);
	save();
}

bool Setting::getRestorePins()
{
	return getPinObj().GetNamedBoolean(L"restore", true);
}

void Setting::setRestorePins(bool val)
{
	getPinObj().SetNamedValue(L"restore", JsonValue::CreateBooleanValue(val));
	save();
}

JsonObject Setting::getCapObj()
{
	auto obj = configObj.GetNamedObject(L"cap", nullptr);
	if (!obj) {
		obj = JsonObject();
		configObj.SetNamedValue(L"cap", obj);
	}
	return obj;
}

int Setting::getCapDelay()
{
	// 夹到 [0, 60]：延时是给"摆好菜单/悬停态再截"用的，再长就没有意义了
	auto val = (int)getCapObj().GetNamedNumber(L"delay", 0.0);
	if (val < 0) return 0;
	if (val > 60) return 60;
	return val;
}

void Setting::setCapDelay(int val)
{
	getCapObj().SetNamedValue(L"delay", JsonValue::CreateNumberValue((double)val));
	save();
}

bool Setting::getAutoShot()
{
	return getCapObj().GetNamedBoolean(L"autoShot", false);
}

void Setting::setAutoShot(bool val)
{
	getCapObj().SetNamedValue(L"autoShot", JsonValue::CreateBooleanValue(val));
	save();
}

int Setting::getAutoShotMin()
{
	auto val = (int)getCapObj().GetNamedNumber(L"autoShotMin", 5.0);
	if (val < 1) return 1;
	if (val > 1440) return 1440;
	return val;
}

void Setting::setAutoShotMin(int val)
{
	getCapObj().SetNamedValue(L"autoShotMin", JsonValue::CreateNumberValue((double)val));
	save();
}

int Setting::getCapShape()
{
	// 0 = 矩形，1 = 手绘（自由多边形）
	auto val = (int)getCapObj().GetNamedNumber(L"shape", 0.0);
	return val == 1 ? 1 : 0;
}

void Setting::setCapShape(int val)
{
	getCapObj().SetNamedValue(L"shape", JsonValue::CreateNumberValue((double)(val == 1 ? 1 : 0)));
	save();
}

const std::vector<std::pair<int, int>>& Setting::fixedSizePresets()
{
	// 0 号留空代表"不固定"，与 getCapFixedIdx 的返回值对齐
	static const std::vector<std::pair<int, int>> presets{
		{ 0, 0 }, { 1920, 1080 }, { 1280, 720 }, { 800, 600 }, { 640, 480 }
	};
	return presets;
}

bool Setting::fixedSize(int idx, int& w, int& h)
{
	auto& presets = fixedSizePresets();
	if (idx <= 0 || idx >= (int)presets.size()) return false;
	w = presets[idx].first;
	h = presets[idx].second;
	return true;
}

int Setting::getCapFixedIdx()
{
	auto val = (int)getCapObj().GetNamedNumber(L"fixedIdx", 0.0);
	const auto& presets = fixedSizePresets();
	if (val < 0 || val >= (int)presets.size()) return 0;
	return val;
}

bool Setting::getClickFx()
{
	return getCapObj().GetNamedBoolean(L"clickFx", false);
}

bool Setting::getGlobalMouse()
{
	return getCapObj().GetNamedBoolean(L"globalMouse", false);
}

void Setting::setGlobalMouse(bool val)
{
	getCapObj().SetNamedValue(L"globalMouse", JsonValue::CreateBooleanValue(val));
	save();
}

void Setting::setClickFx(bool val)
{
	getCapObj().SetNamedValue(L"clickFx", JsonValue::CreateBooleanValue(val));
	save();
}

void Setting::setCapFixedIdx(int val)
{
	const auto& presets = fixedSizePresets();
	if (val < 0 || val >= (int)presets.size()) val = 0;
	getCapObj().SetNamedValue(L"fixedIdx", JsonValue::CreateNumberValue((double)val));
	save();
}

bool Setting::getIncludeCursor()
{
	return getCapObj().GetNamedBoolean(L"cursor", false);
}

void Setting::setIncludeCursor(bool val)
{
	getCapObj().SetNamedValue(L"cursor", JsonValue::CreateBooleanValue(val));
	save();
}

bool Setting::getLongHorizontal()
{
	return getCapObj().GetNamedBoolean(L"longHorizontal", false);
}

void Setting::setLongHorizontal(bool val)
{
	getCapObj().SetNamedValue(L"longHorizontal", JsonValue::CreateBooleanValue(val));
	save();
}

long long Setting::getUpdateCheckDay()
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) return 0;
    return static_cast<long long>(common.GetNamedNumber(L"updateCheckDay", 0));
}

void Setting::setUpdateCheckDay(long long day)
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) return;
    // 这项不写进 defaultConfig：它是程序自己的记账，不是给用户改的配置
    common.SetNamedValue(L"updateCheckDay", JsonValue::CreateNumberValue(static_cast<double>(day)));
    save();
}

void Setting::applyShortcutKeys()
{
    auto lingApp = Ling::App::get();
    for (auto& def : shortcutDefs) {
        // 旧的组合必须先撤：同一 id 上重复 RegisterHotKey 不会覆盖，换了一份配置
        // 不撤的话新旧两个组合都指向同一个动作
        lingApp->unRegHotKey(def.msgId);
        // 取不到就用默认的那个组合：热键注册不上顶多是快捷键不好用，不该让程序起不来
        std::wstring str{ getShortcutKey(std::wstring{ def.type }) };
        if (str.empty()) str = std::wstring{ def.def };
        if (str.empty()) continue;   // 这一项既没配也没有默认，等于关着
        lingApp->regHotKey(str, def.msgId);
    }
}

void Setting::setShortcutCapture(const bool on)
{
    if (!on) {
        applyShortcutKeys();
        return;
    }
    auto lingApp = Ling::App::get();
    for (auto& def : shortcutDefs) lingApp->unRegHotKey(def.msgId);
}

void Setting::initShortcutKeys()
{
    applyShortcutKeys();
    auto lingApp = Ling::App::get();
    lingApp->onHotKey.add([this](UINT msg) {
        switch (msg) {
        case capShortcutMsgId:
            WinCap::init();
            break;
        case pinLastMsgId:
            // 依次贴历史截图：连按一次多贴一张更早的，见 PinSource::pinNextOlder
            PinSource::pinNextOlder();
            break;
        case historyShortcutMsgId:
            WinHistory::init();
            break;
        case ballShortcutMsgId:
            WinBall::toggle();
            break;
        case pinClipShortcutMsgId:
            PinSource::fromClipboard();
            break;
        case rulerShortcutMsgId:
            WinOverlay::toggle(OverlayMode::Ruler);
            break;
        case crosshairShortcutMsgId:
            WinOverlay::toggle(OverlayMode::Crosshair);
            break;
        case focusShortcutMsgId:
            WinOverlay::toggle(OverlayMode::Focus);
            break;
        case aiTransShortcutMsgId:
            // 选中了文字就带进去直接翻一次；没选中（或前台取不到）就开个空窗手动输
            WinAiTrans::init(Util::copyFromForeground());
            break;
        case aiChatShortcutMsgId:
            // 只把选中文字填进输入框，不替用户按发送：对话是要花钱的，
            // 而且十有八九还想再加一句"用表格总结"之类的要求
            WinAiChat::init(Util::copyFromForeground());
            break;
        }
    });
    lingApp->onSecondInstance.add([this]() {
        WinCap::init();
    });
}

bool Setting::exportConfig(const std::wstring& path) const
{
    JsonObject out;
    // 逐项抄一份而不是直接 Stringify 原件：要跳过 pin，而 JsonObject 没有"删键"这回事
    for (auto&& pair : configObj) {
        if (pair.Key() == L"pin") continue; //贴图是运行时状态，图片文件不在配置里
        if (pair.Key() == L"ai") {
            // 地址与模型跟着走，密钥一个都不跟：它们是本机的凭据，导出去给别人既用不上，
            // 也容易被人手滑贴到别处。这与「pin 不导出」是同一条规矩：搬不到别处的东西就不搬
            JsonObject ai;
            for (auto&& p : pair.Value().GetObject()) {
                if (p.Key() == L"apiKey" || p.Key() == L"volcSk") continue;
                if (p.Key() == L"providers") {
                    // 接口列表里的每一项各自还带着一个密钥，同一个标准 —— 逐个摘掉再抄过去
                    JsonArray out;
                    if (p.Value().ValueType() == JsonValueType::Array) {
                        for (auto&& item : p.Value().GetArray()) {
                            if (item.ValueType() != JsonValueType::Object) { out.Append(item); continue; }
                            JsonObject one;
                            for (auto&& f : item.GetObject()) {
                                if (f.Key() == L"apiKey") continue;
                                one.SetNamedValue(f.Key(), f.Value());
                            }
                            out.Append(one);
                        }
                    }
                    ai.SetNamedValue(p.Key(), out);
                    continue;
                }
                ai.SetNamedValue(p.Key(), p.Value());
            }
            out.SetNamedValue(pair.Key(), ai);
            continue;
        }
        out.SetNamedValue(pair.Key(), pair.Value());
    }
    Ling::Util::saveFile(path, std::wstring{ out.Stringify() });
    return std::filesystem::exists(path);
}

bool Setting::importConfig(const std::wstring& path)
{
    auto content = Ling::Util::readFileText(path);
    if (content.empty()) return false;
    JsonObject obj{ nullptr };
    // 解析不出来就一个字都不改：导入失败最多是没导成，把配置清成默认那才是灾难
    if (!JsonObject::TryParse(content, obj) || !obj) return false;
    configObj = obj;
    save();
    // 只有这几类设置是"写进系统里 / 已经分发到各处"的，得按新的重来一遍；
    // 其余的都是每次现读 configObj，下次用到自然生效
    applyShortcutKeys();
    setAutoStart(getAutoStart());
    // 语言也跟上，但只在这份配置里的语言码确实装了的时候才切：写了个没装的语言码，
    // initLang 会把界面整个退成内置英文，看着像是导入把界面弄坏了
    auto langCode = getLang();
    for (auto& pair : Lang::get()->getSupportedLang()) {
        if (pair.second != langCode) continue;
        Lang::get()->initLang(langCode);
        break;
    }
    return true;
}
