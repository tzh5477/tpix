#include "pch.h"
#include <include/Ling.h>
#include "Setting.h"
#include "Util.h"
#include "Lang.h"
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
    };

    const ShortcutDef* findShortcutDef(const std::wstring& type)
    {
        for (auto& def : shortcutDefs) {
            if (def.type == type) return &def;
        }
        return nullptr;
    }
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
            return;
        }
        JsonObject obj{ nullptr };
        if (JsonObject::TryParse(content, obj)) {
            configObj = obj;
            return;
        }
        MessageBox(nullptr, L"config.json parse error，use default config", L"ScreenCapture", MB_OK | MB_ICONWARNING);
    }
    configObj = JsonObject::Parse(defaultConfig); 
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
            RegSetValueEx(hKey, L"ScreenCapture", 0, REG_SZ, (const BYTE*)commandLine.data(), (commandLine.size() + 1) * sizeof(wchar_t));
            RegCloseKey(hKey);
        }
    }
    else {
        HKEY hKey;
        if (RegOpenKeyEx(HKEY_CURRENT_USER, runKey.data(), 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
            RegDeleteValue(hKey, L"ScreenCapture");
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
    dataPath.append("ScreenCapture");
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

void Setting::initShortcutKeys()
{
    auto lingApp = Ling::App::get();
    for (auto& def : shortcutDefs) {
        // 取不到就用默认的那个组合：热键注册不上顶多是快捷键不好用，不该让程序起不来
        std::wstring str{ getShortcutKey(std::wstring{ def.type }) };
        if (str.empty()) str = std::wstring{ def.def };
        if (str.empty()) continue;   // 这一项既没配也没有默认，等于关着
        lingApp->regHotKey(str, def.msgId);
    }

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
        }
    });
    lingApp->onSecondInstance.add([this]() {
        WinCap::init();
    });
}
