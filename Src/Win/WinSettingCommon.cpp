#include "pch.h"
#include <shobjidl.h>
#include <algorithm>
#include "../Lang.h"
#include "../GlobalMouse.h"
#include "../Ocr.h"
#include "../SelectPopup.h"
#include "../Setting.h"
#include "../Util.h"
#include "WinSetting.h"
#include "WinSettingCommon.h"
#include "WinHistory.h"

namespace {
    // 开 / 关两项：图标字体里的叉与勾，和开关按钮上显示的是同一对码位，
    // 所以不用另起一套「开 / 关」译名。关在前开在后，下标正好能当 bool 用
    const std::vector<std::wstring> onOffItems{ L"\ue687", L"\ue688" };

    // 选目录对话框。返回 false 表示用户取消或调用失败，out 不动
    bool pickFolder(HWND hwnd, std::wstring& out)
    {
        Microsoft::WRL::ComPtr<IFileDialog> dialog;
        auto hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(dialog.GetAddressOf()));
        if (FAILED(hr)) return false;
        DWORD flags{ 0 };
        dialog->GetOptions(&flags);
        dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (FAILED(dialog->Show(hwnd))) return false;
        Microsoft::WRL::ComPtr<IShellItem> item;
        if (FAILED(dialog->GetResult(item.GetAddressOf()))) return false;
        PWSTR path{ nullptr };
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return false;
        out = path;
        CoTaskMemFree(path);
        return true;
    }
    // 存 / 取一个配置文件的对话框。类型只给 json：配置就是这一份，选别的格式没有意义。
    // 返回 false 表示用户取消或调用失败，out 不动
    bool pickJsonFile(HWND hwnd, std::wstring& out, const bool save)
    {
        Microsoft::WRL::ComPtr<IFileDialog> dialog;
        auto hr = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.GetAddressOf()));
        if (FAILED(hr)) return false;
        COMDLG_FILTERSPEC filter[]{ { L"JSON", L"*.json" } };
        dialog->SetFileTypes(1, filter);
        dialog->SetDefaultExtension(L"json");
        if (save) dialog->SetFileName(L"ScreenCapture-config.json");
        if (FAILED(dialog->Show(hwnd))) return false;
        Microsoft::WRL::ComPtr<IShellItem> item;
        if (FAILED(dialog->GetResult(item.GetAddressOf()))) return false;
        PWSTR path{ nullptr };
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return false;
        out = path;
        CoTaskMemFree(path);
        return true;
    }
    // 目录可能很长，按钮只有 240 宽，超出就从尾部截断保留文件名那一段
    std::wstring shortenPath(const std::wstring& path)
    {
        constexpr size_t maxLen{ 30 };
        if (path.size() <= maxLen) return path;
        return L"..." + path.substr(path.size() - maxLen);
    }
}

WinSettingCommon::WinSettingCommon(Ling::WinBase* parent):Ling::Node(parent)
{    
    initAutoStartCtrls();
    initLangCtrls();
    initCapBtnCtrls();
    initCapCtrls();
    initSaveCtrls();
    initHistoryCtrls();
    initPinCtrls();
    initOcrCtrls();
    initConfigCtrls();
    // 窗口关掉时把还开着的列表一起收掉。列表是独立窗口，不会跟着本节点走
    win->onDestroy.add([]() {
        SelectPopup::close();
    });
}

WinSettingCommon::~WinSettingCommon()
{
    SelectPopup::close();
}

void WinSettingCommon::initAutoStartCtrls()
{
    auto box = makeChild<Ling::Node>();
    box->setHeight(39.f);
    box->setFlexDirection(Ling::FlexDirection::Row);
    box->setAlignItems(Ling::Align::Center);

    auto label = box->makeChild<Ling::Label>();
    label->setText(Lang::get(L"setting.autoStart"));
    label->setHeightPercent(100.f);
    label->setJustifyContent(Ling::Justify::Center);
    label->setFlexGrow(1.f);

    auto btn = box->makeChild<Ling::Button>();
    btn->setText(L"\ue687");
    btn->setFontFamily(L"icon");
    btn->setHeightPercent(100.f);
    btn->setFontSize(18.f);
    btn->setWidth(60.f);
    setAutoStartBtn(btn);

    btn->onClick.add([this](Ling::Button* b) {
        SelectPopup::show(win, b, onOffItems, Setting::get()->getAutoStart() ? 1 : 0,
            [this, b](int idx) {
                Setting::get()->setAutoStart(idx == 1);
                // 写注册表可能失败，按钮上显示的是真正读回来的状态，不是刚想设的那个
                setAutoStartBtn(b);
            }, L"icon");
    });

    auto border = makeChild<Ling::Node>();
    border->setHeight(1.f);
    border->setBg(0xE0E0E0FF);
}

void WinSettingCommon::initLangCtrls()
{
    auto box = makeChild<Ling::Node>();
    box->setHeight(39.f);
    box->setFlexDirection(Ling::FlexDirection::Row);
    box->setAlignItems(Ling::Align::Center);

    auto label = box->makeChild<Ling::Label>();
    label->setText(Lang::get(L"setting.language"));
    label->setHeightPercent(100.f);
    label->setJustifyContent(Ling::Justify::Center);
    label->setFlexGrow(1.f);

    auto langCode = Setting::get()->getLang();
    auto langs = Lang::get()->getSupportedLang();
    std::vector<std::wstring> items;
    int idx{ 0 };
    for (auto& pair:langs)
    {
        if (pair.second == langCode) idx = (int)items.size();
        items.push_back(pair.first);
    }
    // 最后另起一项去下载更多语言包：它不是选项，选中了只是打开仓库目录
    auto moreIdx = (int)items.size();
    items.push_back(Lang::get(L"setting.getMoreLang"));

    auto btn = box->makeChild<Ling::Button>();
    btn->setText(items[idx]);
    btn->setHeight(28.f);
    btn->setWidth(160.f);
    btn->setBorder(1.f, 0xE0E0E0FF);
    btn->setHoverBg(0XFFFFFFFF);
    btn->onClick.add([this, langs, items, moreIdx](Ling::Button* b) {
        SelectPopup::show(win, b, items, -1, [this, langs, moreIdx](int i) {
            if (i == moreIdx) {
                std::wstring url{ L"https://github.com/xland/ScreenCapture/tree/main/Lang" };
                ShellExecute(win->hwnd, L"open", url.data(), nullptr, nullptr, SW_SHOWNORMAL);
                return;
            }
            Setting::get()->setLang(langs[i].second);
            // 界面上每一句都要换成新语言，逐个节点改不如关掉重开
            win->close();
            Ling::App::get()->dq.TryEnqueue([]() {
                WinSetting::init();
            });
        });
        });
    auto border = makeChild<Ling::Node>();
    border->setHeight(1.f);
    border->setBg(0xE0E0E0FF);
}

// ToolCap 上可配的标注工具。图标码位与 ToolMain 一一对应，
// 默认主行五项与 ToolCap::defaultShapeIds 必须保持一致 —— 两处（工具条摆按钮、设置界面显示默认态）
// 各写一份默认值的话，改一边就会冒出"设置里看着是关的、工具条上却出现了"这种事。
namespace {
    struct CapBtnDef { const wchar_t* id; const wchar_t* code; const wchar_t* tip; };
    const CapBtnDef capBtnDefs[]{
        { L"rect",    L"\ue8e8", L"tool.rect" },
        { L"ellipse", L"\ue6bc", L"tool.ellipse" },
        { L"arrow",   L"\ue603", L"tool.arrow" },
        { L"number",  L"\ue776", L"tool.number" },
        { L"line",    L"\ue601", L"tool.line" },
        { L"text",    L"\ue6ec", L"tool.text" },
        { L"mosaic",  L"\ue82e", L"tool.mosaic" },
        { L"eraser",  L"\ue6be", L"tool.eraser" },
        { L"watermark", L"\ue607", L"tool.watermark" },
    };
    // 默认值在这份文件里也必须再写一遍：这里是"配置从来没写过"时的兜底，
    // 与 ToolCap::defaultShapeIds 是同一套语义
    const std::vector<std::wstring> capBtnDefaultIds{
        L"rect", L"arrow", L"text", L"mosaic", L"number"
    };
}

void WinSettingCommon::applyCapBtnStyle(Ling::Button* btn, bool selected)
{
    if (selected) {
        btn->setBg(0xe6f4ffff);
        btn->setHoverBg(0xe6f4ffff);
    }
    else {
        btn->setBg(0);
        btn->setHoverBg(0xF2F2F2ff);
    }
}

void WinSettingCommon::initCapBtnCtrls()
{
    auto box = makeChild<Ling::Node>();
    box->setHeight(39.f);
    box->setFlexDirection(Ling::FlexDirection::Row);
    box->setAlignItems(Ling::Align::Center);

    auto label = box->makeChild<Ling::Label>();
    label->setText(Lang::get(L"setting.capBtn"));
    label->setHeightPercent(100.f);
    label->setJustifyContent(Ling::Justify::Center);
    label->setFlexGrow(1.f);

    auto setting = Setting::get();
    for (auto& def : capBtnDefs)
    {
        bool onMain = false;
        for (auto& id : capBtnDefaultIds) {
            if (id == def.id) { onMain = true; break; }
        }
        auto btn = box->makeChild<Ling::Button>();
        btn->setWidth(28.f);
        btn->setHeight(28.f);
        btn->setText(def.code);
        btn->setFontFamily(L"icon");
        btn->setFontSize(13.f);
        auto selected = setting->getToolFlag(L"toolCap", def.id, onMain);
        applyCapBtnStyle(btn, selected);
        // 每次都按同一个默认值去读：没写过的键读出来就是 onMain（与初始显示一致），
        // 写过之后读到的就是上次写进去的值，所以这里不能换成"上次显示的那个布尔"
        btn->onClick.add([this, id = std::wstring(def.id), onMain](Ling::Button* b) {
            auto s = Setting::get();
            auto next = !s->getToolFlag(L"toolCap", id, onMain);
            s->setToolFlag(L"toolCap", id, next);
            applyCapBtnStyle(b, next);
        });
    }

    auto border = makeChild<Ling::Node>();
    border->setHeight(1.f);
    border->setBg(0xE0E0E0FF);
}

Ling::Node* WinSettingCommon::makeRow(const std::wstring& labelKey)
{
    auto box = makeChild<Ling::Node>();
    box->setHeight(39.f);
    box->setFlexDirection(Ling::FlexDirection::Row);
    box->setAlignItems(Ling::Align::Center);

    auto label = box->makeChild<Ling::Label>();
    label->setText(Lang::get(labelKey));
    label->setHeightPercent(100.f);
    label->setJustifyContent(Ling::Justify::Center);
    label->setFlexGrow(1.f);

    auto border = makeChild<Ling::Node>();
    border->setHeight(1.f);
    border->setBg(0xE0E0E0FF);
    return box;
}

Ling::Button* WinSettingCommon::makeSelectBtn(Ling::Node* row, float width,
    const std::vector<std::wstring>& items, int cur, std::function<void(int)> onPick)
{
    // 夹一下：cur 多半是从配置文件读回来的，被手工改坏就会取到表外
    cur = std::clamp(cur, 0, (int)items.size() - 1);
    auto btn = row->makeChild<Ling::Button>();
    btn->setHeight(28.f);
    btn->setWidth(width);
    btn->setBorder(1.f, 0xE0E0E0FF);
    btn->setHoverBg(0xFFFFFFFF);
    btn->setText(items[cur]);
    // items 按值进闭包：选完要拿它把按钮上的字换掉，而那时列表已经收了、调用方也不再持有它
    btn->onClick.add([this, btn, items, onPick](Ling::Button*) {
        SelectPopup::show(win, btn, items, -1, [btn, items, onPick](int idx) {
            onPick(idx);
            btn->setText(items[idx]);
        });
    });
    return btn;
}

Ling::Button* WinSettingCommon::makeSwitchBtn(Ling::Node* row,
    std::function<bool()> read, std::function<void(bool)> write)
{
    auto btn = row->makeChild<Ling::Button>();
    btn->setFontFamily(L"icon");
    btn->setHeightPercent(100.f);
    btn->setFontSize(18.f);
    btn->setWidth(60.f);
    auto apply = [btn](bool on) {
        btn->setText(on ? L"\ue688" : L"\ue687");
        btn->setColor(on ? 0x597ef7ff : 0x666666FF);
        btn->setHoverColor(on ? 0x597ef7ff : 0x666666FF);
    };
    apply(read());
    btn->onClick.add([this, btn, read, write, apply](Ling::Button*) {
        SelectPopup::show(win, btn, onOffItems, read() ? 1 : 0,
            [write, apply](int idx) {
                apply(idx == 1);
                write(idx == 1);
            }, L"icon");
    });
    return btn;
}

void WinSettingCommon::initCapCtrls()
{
    // 延时：0 就是关。给固定几档而不是自由输入 —— 这几秒是用来摆菜单、等悬停态的，
    // 真要精确到 7 秒的场景不存在
    constexpr int delayOpts[]{ 0, 2, 3, 5, 10 };
    std::vector<std::wstring> delayItems;
    for (auto v : delayOpts) {
        delayItems.push_back(v == 0 ? Lang::get(L"setting.delayOff")
            : std::to_wstring(v) + Lang::get(L"setting.sec"));
    }
    auto curDelay = Setting::get()->getCapDelay();
    int delayIdx{ 0 };
    for (int i = 0; i < 5; ++i) {
        if (delayOpts[i] == curDelay) { delayIdx = i; break; }
    }
    auto delayRow = makeRow(L"setting.capDelay");
    makeSelectBtn(delayRow, 80.f, delayItems, delayIdx,
        [delayOpts](int idx) { Setting::get()->setCapDelay(delayOpts[idx]); });

    // 框选形状。真要用的时候不必先来设置页改 —— 框选时按住 Alt 拖动就是手绘
    auto shapeRow = makeRow(L"setting.capShape");
    makeSelectBtn(shapeRow, 80.f,
        { Lang::get(L"setting.rectShape"), Lang::get(L"setting.polyShape") },
        Setting::get()->getCapShape() == 1 ? 1 : 0,
        [](int idx) { Setting::get()->setCapShape(idx); });

    // 固定尺寸区域：0 号预设是"不固定"
    auto& presets = Setting::fixedSizePresets();
    std::vector<std::wstring> fixItems;
    for (int i = 0; i < (int)presets.size(); ++i) {
        int w{ 0 }, h{ 0 };
        fixItems.push_back(Setting::fixedSize(i, w, h)
            ? std::to_wstring(w) + L" × " + std::to_wstring(h)
            : Lang::get(L"setting.delayOff"));
    }
    auto fixRow = makeRow(L"setting.capFixed");
    makeSelectBtn(fixRow, 100.f, fixItems, Setting::get()->getCapFixedIdx(),
        [](int idx) { Setting::get()->setCapFixedIdx(idx); });

    makeSwitchBtn(makeRow(L"setting.clickFx"),
        [] { return Setting::get()->getClickFx(); },
        [](bool on) { Setting::get()->setClickFx(on); });

    // 全局鼠标：按住 Win 键拖动就出结果。开关一动就装 / 卸钩子
    makeSwitchBtn(makeRow(L"setting.globalMouse"),
        [] { return Setting::get()->getGlobalMouse(); },
        [](bool on) {
            Setting::get()->setGlobalMouse(on);
            GlobalMouse::setEnabled(on);
        });

    makeSwitchBtn(makeRow(L"setting.includeCursor"),
        [] { return Setting::get()->getIncludeCursor(); },
        [](bool on) { Setting::get()->setIncludeCursor(on); });

    makeSwitchBtn(makeRow(L"setting.autoShot"),
        [] { return Setting::get()->getAutoShot(); },
        [](bool on) { Setting::get()->setAutoShot(on); });

    // 间隔：定时截图是无人值守的，太密会把硬盘堆满，给到分钟这一档
    constexpr int minOpts[]{ 1, 5, 10, 30, 60 };
    std::vector<std::wstring> minItems;
    for (auto v : minOpts) minItems.push_back(std::to_wstring(v) + Lang::get(L"setting.min"));
    auto curMin = Setting::get()->getAutoShotMin();
    int minIdx{ 0 };
    for (int i = 0; i < 5; ++i) {
        if (minOpts[i] == curMin) { minIdx = i; break; }
    }
    auto minRow = makeRow(L"setting.autoShotMin");
    makeSelectBtn(minRow, 80.f, minItems, minIdx,
        [minOpts](int idx) { Setting::get()->setAutoShotMin(minOpts[idx]); });

    // 滚动截图方向。只是个默认值：真滚起来发现这个方向滚不动，CapLong 会自己换一次向
    auto dirRow = makeRow(L"setting.longDir");
    makeSelectBtn(dirRow, 80.f,
        { Lang::get(L"long.vertical"), Lang::get(L"long.horizontal") },
        Setting::get()->getLongHorizontal() ? 1 : 0,
        [](int idx) { Setting::get()->setLongHorizontal(idx == 1); });
}

void WinSettingCommon::initSaveCtrls()
{
    // 保存格式：选项直接写扩展名（大写），比另起一套译名更不容易对不上
    std::vector<std::wstring> fmtItems;
    for (int i = 0; i < 3; ++i) {
        auto ext = Util::getExtOfFormat((Util::ImgFormat)i);
        for (auto& c : ext) if (c >= L'a' && c <= L'z') c -= 32;
        fmtItems.push_back(ext);
    }
    auto fmtRow = makeRow(L"setting.saveFormat");
    makeSelectBtn(fmtRow, 80.f, fmtItems, Util::getSaveFormat(),
        [](int idx) { Setting::get()->setSaveFormat(idx); });

    makeSwitchBtn(makeRow(L"setting.autoSave"),
        [] { return Setting::get()->getAutoSave(); },
        [](bool on) { Setting::get()->setAutoSave(on); });

    auto dirRow = makeRow(L"setting.saveDir");
    auto dirBtn = dirRow->makeChild<Ling::Button>();
    dirBtn->setHeight(28.f);
    dirBtn->setWidth(240.f);
    dirBtn->setBorder(1.f, 0xE0E0E0FF);
    dirBtn->setHoverBg(0xFFFFFFFF);
    auto applyDir = [](Ling::Button* btn) {
        auto dir = Setting::get()->getSaveDir();
        // 没设过目录时 Util::resolveSavePath 会落到数据目录下的 screenshot，这里照实说清
        btn->setText(dir.empty() ? Lang::get(L"setting.saveDirDefault") : shortenPath(dir));
    };
    applyDir(dirBtn);
    dirBtn->onClick.add([this, applyDir](Ling::Button* btn) {
        std::wstring dir;
        if (!pickFolder(win->hwnd, dir)) return;
        Setting::get()->setSaveDir(dir);
        applyDir(btn);
    });

    auto tplRow = makeRow(L"setting.saveNameTpl");
    auto tplBox = tplRow->makeChild<Ling::TextBox>();
    tplBox->setHeight(28.f);
    tplBox->setWidth(200.f);
    tplBox->setBorder(1.f, 0xE0E0E0FF);
    tplBox->setVerticalCenter(true);
    tplBox->setText(Setting::get()->getSaveNameTpl());
    // setText 也会触发一次，写回的是同一个值，多存一次配置而已
    tplBox->onTextChanged.add([](Ling::TextBox*, const std::wstring& val) {
        Setting::get()->setSaveNameTpl(val);
    });

    // 复制后自动粘贴到截图前那个窗口
    makeSwitchBtn(makeRow(L"setting.autoPaste"),
        [] { return Setting::get()->getAutoPaste(); },
        [](bool on) { Setting::get()->setAutoPaste(on); });
}

void WinSettingCommon::initHistoryCtrls()
{
    // 上限不做自由输入：历史条目的成本是磁盘上一整张原图，给个滑杆反而容易填出个 10000
    constexpr int limitOpts[]{ 50, 100, 200, 500 };
    std::vector<std::wstring> limitItems;
    int limitIdx{ 0 };
    for (int i = 0; i < 4; ++i) {
        limitItems.push_back(std::to_wstring(limitOpts[i]));
        if (limitOpts[i] == Setting::get()->getHistoryLimit()) limitIdx = i;
    }
    auto limitRow = makeRow(L"setting.historyLimit");
    makeSelectBtn(limitRow, 80.f, limitItems, limitIdx,
        [limitOpts](int idx) { Setting::get()->setHistoryLimit(limitOpts[idx]); });

    makeSwitchBtn(makeRow(L"setting.clipboardHistory"),
        [] { return Setting::get()->getClipboardHistory(); },
        [](bool on) { Setting::get()->setClipboardHistory(on); });

    auto openRow = makeRow(L"setting.openHistory");
    auto openBtn = openRow->makeChild<Ling::Button>();
    openBtn->setText(Lang::get(L"history.open"));
    openBtn->setHeight(28.f);
    openBtn->setWidth(120.f);
    openBtn->setBorder(1.f, 0xE0E0E0FF);
    openBtn->setHoverBg(0xFFFFFFFF);
    openBtn->onClick.add([](Ling::Button*) { WinHistory::init(); });
}

void WinSettingCommon::initPinCtrls()
{
    makeSwitchBtn(makeRow(L"setting.restorePins"),
        [] { return Setting::get()->getRestorePins(); },
        [](bool on) { Setting::get()->setRestorePins(on); });
}

void WinSettingCommon::initConfigCtrls()
{
    auto row = makeRow(L"setting.config");
    auto exportBtn = row->makeChild<Ling::Button>();
    exportBtn->setText(Lang::get(L"setting.configExport"));
    exportBtn->setHeight(28.f);
    exportBtn->setWidth(100.f);
    exportBtn->setBorder(1.f, 0xE0E0E0FF);
    exportBtn->setHoverBg(0xFFFFFFFF);
    exportBtn->onClick.add([this](Ling::Button*) {
        std::wstring path;
        if (!pickJsonFile(win->hwnd, path, true)) return;
        if (Setting::get()->exportConfig(path)) return;
        showConfigTip(L"setting.configExportFail");
    });

    auto importBtn = row->makeChild<Ling::Button>();
    importBtn->setText(Lang::get(L"setting.configImport"));
    importBtn->setHeight(28.f);
    importBtn->setWidth(100.f);
    importBtn->setMarginLeft(8.f);
    importBtn->setBorder(1.f, 0xE0E0E0FF);
    importBtn->setHoverBg(0xFFFFFFFF);
    importBtn->onClick.add([this](Ling::Button*) {
        std::wstring path;
        if (!pickJsonFile(win->hwnd, path, false)) return;
        if (Setting::get()->importConfig(path)) {
            // 整页都是按旧配置显示出来的，就地改每一行的值不如重建一遍省事
            win->close();
            Ling::App::get()->dq.TryEnqueue([]() { WinSetting::init(); });
            return;
        }
        showConfigTip(L"setting.configImportFail");
    });
}

void WinSettingCommon::showConfigTip(const std::wstring& key)
{
    MessageBox(win->hwnd, Lang::get(key).data(),
        Lang::get(L"about.sysTip").data(), MB_OK | MB_ICONWARNING);
}

void WinSettingCommon::initOcrCtrls()
{
    auto langs = Ocr::languages();
    auto row = makeRow(L"setting.ocrLang");
    if (langs.empty()) {
        // 一个识别语言包都没装：这行只能当提示用，点开了也没得选
        auto btn = row->makeChild<Ling::Button>();
        btn->setHeight(28.f);
        btn->setWidth(140.f);
        btn->setBorder(1.f, 0xE0E0E0FF);
        btn->setText(Lang::get(L"ocr.notInstalled"));
        return;
    }
    // 空标签（跟随系统）是第 0 项，之后依次是每个已装的语言包
    std::vector<std::wstring> items{ Lang::get(L"ocr.langAuto") };
    for (auto const& lang : langs) items.push_back(lang.name);
    auto tag = Setting::get()->getToolStr(L"ocr", L"lang", L"");
    int idx{ 0 };
    for (int i = 0; i < (int)langs.size(); ++i) {
        if (langs[i].tag == tag) { idx = i + 1; break; }
    }
    makeSelectBtn(row, 140.f, items, idx, [langs](int i) {
        Setting::get()->setToolStr(L"ocr", L"lang", i == 0 ? L"" : langs[i - 1].tag);
    });
}

void WinSettingCommon::setAutoStartBtn(Ling::Button* btn)
{
    auto setting = Setting::get();
    auto isAutoStart = setting->getAutoStart();
    if (isAutoStart) {
        btn->setText(L"\ue688");
        btn->setColor(0x597ef7ff);
        btn->setHoverColor(0x597ef7ff);
    }
    else {
        btn->setText(L"\ue687");
        btn->setColor(0x666666FF);
        btn->setHoverColor(0x666666FF);
    }
}

