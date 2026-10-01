#include "pch.h"
#include <shobjidl.h>
#include "../Lang.h"
#include "../Ocr.h"
#include "../Setting.h"
#include "../Util.h"
#include "WinSetting.h"
#include "WinSettingCommon.h"
#include "WinHistory.h"

namespace {
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
    auto weakThis = getWeakThis();
    // 这个回调一直挂在窗口上，而本节点可能在窗口关闭之前就被菜单切换换掉了，
    // 所以先确认自己还活着再去碰成员
    win->onDestroy.add([this, weakThis]() {
        if (!weakThis.lock()) return;
        this->hideSelectBox();
    });
}

WinSettingCommon::~WinSettingCommon()
{
    win->onMouseDown.remove(onMouseDownToken);
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

    btn->onClick.add([this](Ling::Button* btn) {
        auto setting = Setting::get();
        auto isAutoStart = setting->getAutoStart();
        setting->setAutoStart(!isAutoStart);
        setAutoStartBtn(btn);
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
    std::wstring langName{ L"简体中文" };
    for (auto& pair:langs)
    {
        if (pair.second == langCode) {
            langName = pair.first;
            break;
        }
    }
    selectBtn = box->makeChild<Ling::Button>();
    selectBtn->setText(langName);
    selectBtn->setHeight(28.f);
    selectBtn->setWidth(160.f);
    selectBtn->setBorder(1.f, 0xE0E0E0FF);
    selectBtn->setHoverBg(0XFFFFFFFF);
    selectBtn->onClick.add([this](Ling::Button* btn) {
        if (selectBox) return;
        this->showSelectBox(btn);
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

void WinSettingCommon::initCapCtrls()
{
    // 开 / 关两套配色，与 ToolSub::applyToggleStyle、initPinCtrls 里的保持一致
    auto applySwitch = [](Ling::Button* btn, bool on) {
        btn->setText(on ? L"\ue688" : L"\ue687");
        btn->setColor(on ? 0x597ef7ff : 0x666666FF);
        btn->setHoverColor(on ? 0x597ef7ff : 0x666666FF);
    };
    // 延时：0 就是关。给固定几档而不是自由输入 —— 这几秒是用来摆菜单、等悬停态的，
    // 真要精确到 7 秒的场景不存在
    constexpr int delayOpts[]{ 0, 2, 3, 5, 10 };
    auto delayRow = makeRow(L"setting.capDelay");
    auto delayBtn = delayRow->makeChild<Ling::Button>();
    delayBtn->setHeight(28.f);
    delayBtn->setWidth(80.f);
    delayBtn->setBorder(1.f, 0xE0E0E0FF);
    delayBtn->setHoverBg(0xFFFFFFFF);
    auto applyDelay = [delayOpts](Ling::Button* btn) {
        auto cur = Setting::get()->getCapDelay();
        int idx{ 0 };
        for (int i = 0; i < 5; ++i) {
            if (delayOpts[i] == cur) { idx = i; break; }
        }
        btn->setText(delayOpts[idx] == 0 ? Lang::get(L"setting.delayOff")
            : std::to_wstring(delayOpts[idx]) + Lang::get(L"setting.sec"));
    };
    applyDelay(delayBtn);
    delayBtn->onClick.add([delayOpts, applyDelay](Ling::Button* btn) {
        auto cur = Setting::get()->getCapDelay();
        int idx{ 0 };
        for (int i = 0; i < 5; ++i) {
            if (delayOpts[i] == cur) { idx = i; break; }
        }
        Setting::get()->setCapDelay(delayOpts[(idx + 1) % 5]);
        applyDelay(btn);
    });

    // 框选形状。真要用的时候不必先来设置页改 —— 框选时按住 Alt 拖动就是手绘
    auto shapeRow = makeRow(L"setting.capShape");
    auto shapeBtn = shapeRow->makeChild<Ling::Button>();
    shapeBtn->setHeight(28.f);
    shapeBtn->setWidth(80.f);
    shapeBtn->setBorder(1.f, 0xE0E0E0FF);
    shapeBtn->setHoverBg(0xFFFFFFFF);
    auto applyShape = [](Ling::Button* btn) {
        btn->setText(Lang::get(Setting::get()->getCapShape() == 1
            ? L"setting.polyShape" : L"setting.rectShape"));
    };
    applyShape(shapeBtn);
    shapeBtn->onClick.add([applyShape](Ling::Button* btn) {
        Setting::get()->setCapShape(Setting::get()->getCapShape() == 1 ? 0 : 1);
        applyShape(btn);
    });

    // 固定尺寸区域：0 号预设是"不固定"
    auto fixRow = makeRow(L"setting.capFixed");
    auto fixBtn = fixRow->makeChild<Ling::Button>();
    fixBtn->setHeight(28.f);
    fixBtn->setWidth(100.f);
    fixBtn->setBorder(1.f, 0xE0E0E0FF);
    fixBtn->setHoverBg(0xFFFFFFFF);
    auto applyFix = [](Ling::Button* btn) {
        auto idx = Setting::get()->getCapFixedIdx();
        int w{ 0 }, h{ 0 };
        btn->setText(Setting::fixedSize(idx, w, h)
            ? std::to_wstring(w) + L" × " + std::to_wstring(h)
            : Lang::get(L"setting.delayOff"));
    };
    applyFix(fixBtn);
    fixBtn->onClick.add([applyFix](Ling::Button* btn) {
        auto& presets = Setting::fixedSizePresets();
        Setting::get()->setCapFixedIdx((Setting::get()->getCapFixedIdx() + 1) % (int)presets.size());
        applyFix(btn);
    });

    auto fxRow = makeRow(L"setting.clickFx");
    auto fxBtn = fxRow->makeChild<Ling::Button>();
    fxBtn->setFontFamily(L"icon");
    fxBtn->setHeightPercent(100.f);
    fxBtn->setFontSize(18.f);
    fxBtn->setWidth(60.f);
    applySwitch(fxBtn, Setting::get()->getClickFx());
    fxBtn->onClick.add([applySwitch](Ling::Button* btn) {
        auto next = !Setting::get()->getClickFx();
        Setting::get()->setClickFx(next);
        applySwitch(btn, next);
    });

    auto cursorRow = makeRow(L"setting.includeCursor");
    auto cursorBtn = cursorRow->makeChild<Ling::Button>();
    cursorBtn->setFontFamily(L"icon");
    cursorBtn->setHeightPercent(100.f);
    cursorBtn->setFontSize(18.f);
    cursorBtn->setWidth(60.f);
    applySwitch(cursorBtn, Setting::get()->getIncludeCursor());
    cursorBtn->onClick.add([applySwitch](Ling::Button* btn) {
        auto next = !Setting::get()->getIncludeCursor();
        Setting::get()->setIncludeCursor(next);
        applySwitch(btn, next);
    });

    auto shotRow = makeRow(L"setting.autoShot");
    auto shotBtn = shotRow->makeChild<Ling::Button>();
    shotBtn->setFontFamily(L"icon");
    shotBtn->setHeightPercent(100.f);
    shotBtn->setFontSize(18.f);
    shotBtn->setWidth(60.f);
    applySwitch(shotBtn, Setting::get()->getAutoShot());
    shotBtn->onClick.add([applySwitch](Ling::Button* btn) {
        auto next = !Setting::get()->getAutoShot();
        Setting::get()->setAutoShot(next);
        applySwitch(btn, next);
    });

    // 间隔：定时截图是无人值守的，太密会把硬盘堆满，给到分钟这一档
    constexpr int minOpts[]{ 1, 5, 10, 30, 60 };
    auto minRow = makeRow(L"setting.autoShotMin");
    auto minBtn = minRow->makeChild<Ling::Button>();
    minBtn->setHeight(28.f);
    minBtn->setWidth(80.f);
    minBtn->setBorder(1.f, 0xE0E0E0FF);
    minBtn->setHoverBg(0xFFFFFFFF);
    auto applyMin = [minOpts](Ling::Button* btn) {
        auto cur = Setting::get()->getAutoShotMin();
        int idx{ 0 };
        for (int i = 0; i < 5; ++i) {
            if (minOpts[i] == cur) { idx = i; break; }
        }
        btn->setText(std::to_wstring(minOpts[idx]) + Lang::get(L"setting.min"));
    };
    applyMin(minBtn);
    minBtn->onClick.add([minOpts, applyMin](Ling::Button* btn) {
        auto cur = Setting::get()->getAutoShotMin();
        int idx{ 0 };
        for (int i = 0; i < 5; ++i) {
            if (minOpts[i] == cur) { idx = i; break; }
        }
        Setting::get()->setAutoShotMin(minOpts[(idx + 1) % 5]);
        applyMin(btn);
    });

    // 滚动截图方向。只是个默认值：真滚起来发现这个方向滚不动，CapLong 会自己换一次向
    auto dirRow = makeRow(L"setting.longDir");
    auto dirBtn = dirRow->makeChild<Ling::Button>();
    dirBtn->setHeight(28.f);
    dirBtn->setWidth(80.f);
    dirBtn->setBorder(1.f, 0xE0E0E0FF);
    dirBtn->setHoverBg(0xFFFFFFFF);
    auto applyDir = [](Ling::Button* btn) {
        btn->setText(Lang::get(Setting::get()->getLongHorizontal()
            ? L"long.horizontal" : L"long.vertical"));
    };
    applyDir(dirBtn);
    dirBtn->onClick.add([applyDir](Ling::Button* btn) {
        Setting::get()->setLongHorizontal(!Setting::get()->getLongHorizontal());
        applyDir(btn);
    });
}

void WinSettingCommon::initSaveCtrls()
{
    // 保存格式：三种循环切换，按钮上直接写扩展名（大写），比另起一套译名更不容易对不上
    auto fmtRow = makeRow(L"setting.saveFormat");
    auto fmtBtn = fmtRow->makeChild<Ling::Button>();
    fmtBtn->setHeight(28.f);
    fmtBtn->setWidth(80.f);
    fmtBtn->setBorder(1.f, 0xE0E0E0FF);
    fmtBtn->setHoverBg(0xFFFFFFFF);
    auto applyFormat = [](Ling::Button* btn) {
        auto ext = Util::getExtOfFormat((Util::ImgFormat)Util::getSaveFormat());
        std::wstring upper;
        for (auto c : ext) upper += (wchar_t)(c >= L'a' && c <= L'z' ? c - 32 : c);
        btn->setText(upper);
    };
    applyFormat(fmtBtn);
    fmtBtn->onClick.add([applyFormat](Ling::Button* btn) {
        auto next = (Util::getSaveFormat() + 1) % 3;
        Setting::get()->setSaveFormat(next);
        applyFormat(btn);
    });

    auto autoRow = makeRow(L"setting.autoSave");
    auto autoBtn = autoRow->makeChild<Ling::Button>();
    autoBtn->setFontFamily(L"icon");
    autoBtn->setHeightPercent(100.f);
    autoBtn->setFontSize(18.f);
    autoBtn->setWidth(60.f);
    auto applyAutoSave = [](Ling::Button* btn, bool on) {
        btn->setText(on ? L"\ue688" : L"\ue687");
        btn->setColor(on ? 0x597ef7ff : 0x666666FF);
        btn->setHoverColor(on ? 0x597ef7ff : 0x666666FF);
    };
    applyAutoSave(autoBtn, Setting::get()->getAutoSave());
    autoBtn->onClick.add([applyAutoSave](Ling::Button* btn) {
        auto setting = Setting::get();
        auto next = !setting->getAutoSave();
        setting->setAutoSave(next);
        applyAutoSave(btn, next);
    });

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
}

void WinSettingCommon::initHistoryCtrls()
{
    // 上限不做自由输入：历史条目的成本是磁盘上一整张原图，给个滑杆反而容易填出个 10000
    constexpr int limitOpts[]{ 50, 100, 200, 500 };
    auto limitRow = makeRow(L"setting.historyLimit");
    auto limitBtn = limitRow->makeChild<Ling::Button>();
    limitBtn->setHeight(28.f);
    limitBtn->setWidth(80.f);
    limitBtn->setBorder(1.f, 0xE0E0E0FF);
    limitBtn->setHoverBg(0xFFFFFFFF);
    auto applyLimit = [limitOpts](Ling::Button* btn) {
        auto cur = Setting::get()->getHistoryLimit();
        int idx{ 0 };
        for (int i = 0; i < 4; ++i) {
            if (limitOpts[i] == cur) { idx = i; break; }
        }
        btn->setText(std::to_wstring(limitOpts[idx]));
    };
    applyLimit(limitBtn);
    limitBtn->onClick.add([limitOpts, applyLimit](Ling::Button* btn) {
        auto cur = Setting::get()->getHistoryLimit();
        int idx{ 0 };
        for (int i = 0; i < 4; ++i) {
            if (limitOpts[i] == cur) { idx = i; break; }
        }
        Setting::get()->setHistoryLimit(limitOpts[(idx + 1) % 4]);
        applyLimit(btn);
    });

    auto clipRow = makeRow(L"setting.clipboardHistory");
    auto clipBtn = clipRow->makeChild<Ling::Button>();
    clipBtn->setFontFamily(L"icon");
    clipBtn->setHeightPercent(100.f);
    clipBtn->setFontSize(18.f);
    clipBtn->setWidth(60.f);
    auto applyClip = [](Ling::Button* btn, bool on) {
        btn->setText(on ? L"\ue688" : L"\ue687");
        btn->setColor(on ? 0x597ef7ff : 0x666666FF);
        btn->setHoverColor(on ? 0x597ef7ff : 0x666666FF);
    };
    applyClip(clipBtn, Setting::get()->getClipboardHistory());
    clipBtn->onClick.add([applyClip](Ling::Button* btn) {
        auto next = !Setting::get()->getClipboardHistory();
        Setting::get()->setClipboardHistory(next);
        applyClip(btn, next);
    });

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
    auto row = makeRow(L"setting.restorePins");
    auto btn = row->makeChild<Ling::Button>();
    btn->setFontFamily(L"icon");
    btn->setHeightPercent(100.f);
    btn->setFontSize(18.f);
    btn->setWidth(60.f);
    auto apply = [](Ling::Button* btn, bool on) {
        btn->setText(on ? L"\ue688" : L"\ue687");
        btn->setColor(on ? 0x597ef7ff : 0x666666FF);
        btn->setHoverColor(on ? 0x597ef7ff : 0x666666FF);
    };
    apply(btn, Setting::get()->getRestorePins());
    btn->onClick.add([apply](Ling::Button* b) {
        auto next = !Setting::get()->getRestorePins();
        Setting::get()->setRestorePins(next);
        apply(b, next);
    });
}

void WinSettingCommon::initOcrCtrls()
{
    auto langs = Ocr::languages();
    auto row = makeRow(L"setting.ocrLang");
    auto btn = row->makeChild<Ling::Button>();
    btn->setHeight(28.f);
    btn->setWidth(140.f);
    btn->setBorder(1.f, 0xE0E0E0FF);
    btn->setHoverBg(0xFFFFFFFF);
    if (langs.empty()) {
        // 一个识别语言包都没装：这行只能当提示用，点了也没得切
        btn->setText(Lang::get(L"ocr.notInstalled"));
        return;
    }
    auto apply = [langs](Ling::Button* b) {
        auto tag = Setting::get()->getToolStr(L"ocr", L"lang", L"");
        auto name = std::wstring{ Lang::get(L"ocr.langAuto") };
        for (auto const& lang : langs) {
            if (lang.tag == tag) { name = lang.name; break; }
        }
        b->setText(name);
    };
    apply(btn);
    btn->onClick.add([langs, apply](Ling::Button* b) {
        auto tag = Setting::get()->getToolStr(L"ocr", L"lang", L"");
        // 空标签（跟随系统）算第 0 项，之后依次是列表里的每一项
        int idx{ 0 };
        for (int i = 0; i < static_cast<int>(langs.size()); ++i) {
            if (langs[i].tag == tag) { idx = i + 1; break; }
        }
        auto next = (idx + 1) % static_cast<int>(langs.size() + 1);
        Setting::get()->setToolStr(L"ocr", L"lang", next == 0 ? L"" : langs[next - 1].tag);
        apply(b);
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

void WinSettingCommon::hideSelectBox()
{
    if (!selectBox) return;
    win->onMouseDown.remove(onMouseDownToken);
    win->body->removeChild(selectBox);
    selectBox = nullptr;
}

void WinSettingCommon::showSelectBox(Ling::Button* btn)
{
    auto weakThis = getWeakThis();
    onMouseDownToken = win->onMouseDown.add([this,weakThis](POINT pos, bool isRight) {
        if (!weakThis.lock()) return;
        if (!this->selectBox) return;
        if (this->selectBtn->isPosIn(pos)) return;
        if (this->selectBox->isPosIn(pos)) return;
        win->body->removeChild(selectBox);
        this->selectBox = nullptr;
        this->win->onMouseDown.remove(this->onMouseDownToken);
    });
    if (selectBox) {
        win->body->removeChild(selectBox);
    }
    auto langs = Lang::get()->getSupportedLang();
    auto itemH{ 30.f };
    auto totalH = std::min(320.f, itemH * (langs.size()+1));

    selectBox = win->body->makeChild<Ling::ScrollerBox>();
    selectBox->setSize(btn->w/win->dpi, totalH);
    selectBox->setPositionType(Ling::Position::Absolute);
    selectBox->setPosition(Ling::Edge::Left, btn->x/win->dpi);
    selectBox->setPosition(Ling::Edge::Top, btn->y/win->dpi);
    selectBox->setBg(0xFFFFFFFF);
    selectBox->setBorder(1.f, 0x597ef766);
    for (auto& pair:langs)
    {
        auto btn = selectBox->makeChild<Ling::Button>();
        btn->setText(pair.first);
        btn->setHeight(itemH);
        btn->setWidthPercent(100.f);
        btn->setHoverBg(0Xf2f2f2FF);
        btn->setHoverColor(0X000000FF);
        btn->onClick.add([this](Ling::Button* btn) {
            auto lang = Lang::get();
            auto langName = btn->getText();
            auto langs = lang->getSupportedLang();
            for (auto& pair : langs)
            {
                if (pair.first == langName) {
                    Setting::get()->setLang(pair.second);
                    win->close();
                    Ling::App::get()->dq.TryEnqueue([this]() {
                        WinSetting::init();
                    });
                    break;
                }
            }
        });
    }
    auto lastItem = selectBox->makeChild<Ling::Button>();
    lastItem->setText(Lang::get(L"setting.getMoreLang"));
    lastItem->setHeight(itemH);
    lastItem->setWidthPercent(100.f);
    lastItem->setHoverBg(0Xf2f2f2FF);
    lastItem->setHoverColor(0X000000FF);
    lastItem->onClick.add([this](Ling::Button* btn) {
        win->onMouseDown.remove(onMouseDownToken);
        std::wstring downloadUrl{ L"https://github.com/xland/ScreenCapture/tree/main/Lang" };
        ShellExecute(win->hwnd, L"open", downloadUrl.data(), nullptr, nullptr, SW_SHOWNORMAL);
        win->body->removeChild(selectBox);
        selectBox = nullptr;
    });
}
