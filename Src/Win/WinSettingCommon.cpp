#include "pch.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <filesystem>
#include <algorithm>
#include "../Lang.h"
#include "../App.h"
#include "../GlobalMouse.h"
#include "../Ocr.h"
#include "../SelectPopup.h"
#include "../Setting.h"
#include "../Util.h"
#include "WinSetting.h"
#include "WinSettingCommon.h"
#include "WinColorPicker.h"
#include "WinHistory.h"

namespace {
    // 主题色的八个预设色。设置页那排色块与"哪一个还亮着"（applyThemeSwatchStyle）共用一份 ——
    // 各写一遍的话，改了预设顺序就会出现"点第 3 个、亮的是第 5 个"
    const unsigned int kThemePresets[]{ 0x1677FF, 0x2F54EB, 0x722ED1, 0xEB2F96,
                                       0xF5222D, 0xFA8C16, 0x52C41A, 0x13C2C2 };

    // 开关「开」态的颜色 = 主题色（原来是写死的 0x597ef7）。makeSwitchBtn / setAutoStartBtn /
    // applyTheme 三处共用 —— 强调色改成跟主题色走之后，各写一份必然漏掉一处
    unsigned int kSwitchOnColor() { return (Setting::getThemeColor() << 8) | 0xFF; }

    // 对勾的墨色 / 浅色描边判据，与 WinColorPicker.cpp 那份同一套算式。刻意不复用：
    // 不想让设置页与取色器互相 include（同 ToolSub 与它之前的取舍）
    unsigned int themeInkOn(unsigned int rgba)
    {
        const auto r = (rgba >> 24) & 0xFF, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF;
        return (0.299 * r + 0.587 * g + 0.114 * b) > 150 ? 0x000000FF : 0xFFFFFFFF;
    }
    bool themeLight(unsigned int rgba)
    {
        const auto r = (rgba >> 24) & 0xFF, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF;
        return (0.299 * r + 0.587 * g + 0.114 * b) > 200;
    }

    // 「自定义」块在"还没挑过自定义色"时铺的七彩渐变画刷。八档取的就是 kThemePresets
    // 八个预设色。起止点用 0~1：Composition 的线性渐变画刷默认按**包围盒相对**映射，
    // 与方块尺寸无关 —— 建一次一直用，不必等布局拿到尺寸
    winrt::Windows::UI::Composition::CompositionLinearGradientBrush themeRainbowBrush(Ling::Node* n)
    {
        // WinRT 投影类型没有默认构造，用 {nullptr} 起步当"还没建"的判据
        static winrt::Windows::UI::Composition::CompositionLinearGradientBrush brush{ nullptr };
        if (!brush) {
            auto comp = n->visual.Compositor();
            brush = comp.CreateLinearGradientBrush();
            brush.StartPoint({ 0.f, 0.f });
            brush.EndPoint({ 1.f, 1.f });
            auto stops = brush.ColorStops();
            for (int i = 0; i < 8; i++)
                stops.Append(comp.CreateColorGradientStop(
                    i / 7.f, Ling::Color((kThemePresets[i] << 8) | 0xFF).getUIColor()));
        }
        return brush;
    }

    // 换数据目录时把旧目录整棵树复制过去。**只在目标是空目录时做** —— 非空说明用户
    // 已经在那儿放了东西（或者搬过一次），硬塞会混。返回是否真的复制了
    bool copyDataTree(const std::wstring& from, const std::wstring& to)
    {
        std::error_code ec;
        if (!std::filesystem::is_empty(to, ec)) return false;
        std::filesystem::create_directories(to, ec);
        if (ec) return false;
        std::filesystem::copy(from, to, std::filesystem::copy_options::recursive, ec);
        return !ec;
    }

    // 选目录对话框。initial 是打开时应该停在那儿的目录，传空 = 不指定。
    // **必须指定**：不指定的话系统按对话框类型记"最近访问过哪儿"，于是停在用户
    // 上次碰过的另一个目录选择上（数据目录点开停在录制临时目录那儿就是这么来的）。
    // 目录不存在时 SHCreateItemFromParsingName 会失败，静默不设、回落系统行为
    bool pickFolder(HWND hwnd, std::wstring& out, const std::wstring& initial = L"")
    {
        Microsoft::WRL::ComPtr<IFileDialog> dialog;
        auto hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(dialog.GetAddressOf()));
        if (FAILED(hr)) return false;
        DWORD flags{ 0 };
        dialog->GetOptions(&flags);
        dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (!initial.empty()) {
            Microsoft::WRL::ComPtr<IShellItem> item;
            if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr,
                    IID_PPV_ARGS(item.GetAddressOf())))) {
                dialog->SetFolder(item.Get());
            }
        }
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
    // initial 同 pickFolder：导出 / 导入都默认停在数据目录（config.json 的住处）
    bool pickJsonFile(HWND hwnd, std::wstring& out, const bool save,
        const std::wstring& initial = L"")
    {
        Microsoft::WRL::ComPtr<IFileDialog> dialog;
        auto hr = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.GetAddressOf()));
        if (FAILED(hr)) return false;
        COMDLG_FILTERSPEC filter[]{ { L"JSON", L"*.json" } };
        dialog->SetFileTypes(1, filter);
        dialog->SetDefaultExtension(L"json");
        if (save) dialog->SetFileName(L"tpix-config.json");
        if (!initial.empty()) {
            Microsoft::WRL::ComPtr<IShellItem> item;
            if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr,
                    IID_PPV_ARGS(item.GetAddressOf())))) {
                dialog->SetFolder(item.Get());
            }
        }
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
    initThemeCtrls();
    initCapCtrls();
    initVideoCtrls();
    initSaveCtrls();
    initHistoryCtrls();
    initPinCtrls();
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
    // 自启动这一枚是手工搭的（它不落工具配置、只写注册表），进不了 makeSwitchBtn，
    // 但主题色改了同样要按当前状态重上一层 —— 这里补登记一次
    switchBtns.push_back({ btn, [] { return Setting::get()->getAutoStart(); } });

    // 开 / 关只有两种状态，按钮上的图标（勾 / 叉）与配色已经表示清楚了，单击直接翻转
    btn->onClick.add([this](Ling::Button* b) {
        Setting::get()->setAutoStart(!Setting::get()->getAutoStart());
        // 写注册表可能失败，按钮上显示的是真正读回来的状态，不是刚想设的那个
        setAutoStartBtn(b);
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
                std::wstring url{ L"https://github.com/tzh5477/tpix/tree/main/Lang" };
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
    // 两项的（框选形状 / 滚动截图方向）单击即在两项间切换：按钮上的字就是当前那一项，
    // 再弹一个只有两项的列表让用户"点开、看清、再点一次"是多余的一步。
    // cur 用 mutable 的闭包副本记着 —— 这个按钮只建一次，副本就是它的当前状态
    if (items.size() == 2) {
        btn->onClick.add([btn, items, onPick, cur](Ling::Button*) mutable {
            cur = 1 - cur;
            btn->setText(items[cur]);
            onPick(cur);
        });
        return btn;
    }
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
        btn->setColor(on ? kSwitchOnColor() : 0x666666FF);
        btn->setHoverColor(on ? kSwitchOnColor() : 0x666666FF);
    };
    apply(read());
    // 同上：按钮本身已经显示出开关状态，单击即翻转，不必再弹一个两项列表
    btn->onClick.add([read, write, apply](Ling::Button*) {
        bool next = !read();
        apply(next);
        write(next);
    });
    // 记进清单：主题色改了要按"当前状态"重上一层（那层色是上面 apply 里上的）
    switchBtns.push_back({ btn, read });
    return btn;
}

void WinSettingCommon::initThemeCtrls()
{
    auto row = makeRow(L"setting.theme");
    auto box = row->makeChild<Ling::Node>();
    box->setFlexDirection(Ling::FlexDirection::Row);
    box->setAlignItems(Ling::Align::Center);
    // 八个预设 + 末尾一块「自定义」入口（点开取色器）。贴图边框、按钮选中底色、控制点、
    // 悬浮球那条细线都从这一个色派生
    // 主题色那排色块的清单是成员不是局部变量：onClick 回调要遍历它改描边，
    // 局部 vector 按引用抓进 lambda，函数一返回就是悬垂 —— 点击即崩（探针实测）
    themeSwatches.clear();
    for (auto c : kThemePresets) {
        auto btn = box->makeChild<Ling::Button>();
        btn->setWidth(26.f);
        btn->setHeight(26.f);
        btn->setMarginRight(8.f);
        btn->setBg((c << 8) | 0xFF);
        // ⚠️ setBg 了就必须给 setHoverBg，否则一悬停整块变空白（Button 的规矩，见 TOPIC）
        btn->setHoverBg((c << 8) | 0xFF);
        btn->onClick.add([this, c](Ling::Button*) {
            Setting::setThemeColor(c);
            // 选中描边（哪个色块还亮着）与"边上的东西"一起刷：后者是 App::applyTheme ——
            // 所有当前活着的窗口各自重读一遍颜色。只刷贴图窗的话，悬浮球那条细线、
            // 截图框选那条线、设置页自己的开关都得等重启才变（作者报的正是这个）
            applyThemeSwatchStyle();
            App::applyTheme();
        });
        themeSwatches.push_back(btn);
    }
    // 末尾那块「自定义」入口：点开取色器任意调。颜色画在子 Label 上而不是 Button 自己 ——
    // 两态（七彩渐变 ↔ 回显）要换 visual 的画刷，挂 Button 上会被 hover 抹掉
    //（与 ToolSub 末尾块同一套做法，见 TOPIC-color §2）
    themeMoreBtn = box->makeChild<Ling::Button>();
    themeMoreBtn->setWidth(26.f);
    themeMoreBtn->setHeight(26.f);
    themeMoreBtn->setMarginRight(8.f);
    themeMoreBtn->setAlignItems(Ling::Align::Center);
    themeMoreBtn->setJustifyContent(Ling::Justify::Center);
    // 颜色画在子 Label 上、灰底只当框：⚠️ setBg 了就必须给 setHoverBg（Button 的规矩）
    themeMoreBtn->setHoverBg(0xF2F2F2FF);
    themeMoreLabel = themeMoreBtn->makeChild<Ling::Label>();
    themeMoreLabel->setSize(18.f, 18.f);
    themeMoreLabel->setAlignItems(Ling::Align::Center);
    themeMoreLabel->setJustifyContent(Ling::Justify::Center);
    themeMoreLabel->setBorderRadius(2.f);
    themeMoreBtn->onClick.add([this](Ling::Button*) { toggleThemePicker(themeMoreBtn); });
    applyThemeSwatchStyle();
}

// "哪个色块还亮着"那圈描边。onClick 与 applyTheme 共用 —— 两处各写一遍必然对不上
void WinSettingCommon::applyThemeSwatchStyle()
{
    const unsigned int cur = Setting::getThemeColor();
    bool onPreset{ false };
    for (size_t i = 0; i < themeSwatches.size(); ++i) {
        const bool on = (kThemePresets[i] == cur);
        if (on) onPreset = true;
        themeSwatches[i]->setBorder(on ? 2.f : 1.f, on ? 0x333333FF : 0xE0E0E0FF);
    }
    // 末尾「自定义」块的两种态，判据与 ToolSub 末尾块同一套（"当前色在不在预设里"）：
    //   ① 预设色 → 七彩渐变、不打勾：选中由预设里那一格的描边表达，这里再回显就成了
    //      两个地方说同一件事；"这块 = 挑任意颜色的入口"的含义只有一直铺七彩才立得住
    //   ② 自定义色 → 回显 + 打勾：预设行里没有任何一格能反映它，这里是唯一的预览，
    //      选中描边也亮在这块上
    if (!themeMoreLabel) return;
    const auto rgba = (cur << 8) | 0xFF;
    if (onPreset) {
        themeMoreLabel->visual.Brush(themeRainbowBrush(themeMoreLabel));
        themeMoreLabel->setText(L"");
        themeMoreLabel->setBorder(0.f, 0);
        themeMoreBtn->setBorder(1.f, 0xE0E0E0FF);
        return;
    }
    // 两态都直接换 visual 的画刷，不走 setBg —— 渐变没法用 setBg 表达
    themeMoreLabel->visual.Brush(
        themeMoreLabel->visual.Compositor().CreateColorBrush(Ling::Color(rgba).getUIColor()));
    themeMoreLabel->setText(L"\ue6ad");
    themeMoreLabel->setColor(themeInkOn(rgba));
    themeMoreLabel->setBorder(themeLight(rgba) ? 1.f : 0.f, 0xA8A8A8FF);
    themeMoreBtn->setBorder(2.f, 0x333333FF);
}

// 应用一个主题色：落盘 + 描边 + 广播。预设格的 onClick 与取色器的写回共用 ——
// 各写一遍的话，改一处忘一处（同 applyThemeSwatchStyle 的理由）
void WinSettingCommon::applyThemeColor(unsigned int rgba)
{
    Setting::setThemeColor(rgba >> 8);
    applyThemeSwatchStyle();
    App::applyTheme();
}

void WinSettingCommon::toggleThemePicker(Ling::Node* anchor)
{
    WinColorPicker::Source src;
    src.presets.assign(std::begin(kThemePresets), std::end(kThemePresets));
    for (auto c : Setting::getThemeCustoms())
        src.customs.push_back((c << 8) | 0xFF);
    src.current = []() { return (Setting::getThemeColor() << 8) | 0xFF; };
    src.onUse = [this](UINT32 c) { applyThemeColor(c); };
    src.onConfirm = [this](UINT32 c) {
        applyThemeColor(c);
        // 与预设重复的不进自定义栅格：预设格自己能回显，存一份纯属多余
        const auto rgb = c >> 8;
        if (std::find(std::begin(kThemePresets), std::end(kThemePresets), rgb) == std::end(kThemePresets))
            Setting::addThemeCustom(rgb);
    };
    WinColorPicker::show(src, anchor, win);
}

// 主题色改了：这一页上的强调色都要重上一层。
// 开关的「开」态是建按钮那一刻按当时的状态上的色（见 makeSwitchBtn 里的 apply），
// 主题色块那圈描边同理由 applyThemeSwatchStyle 重算
void WinSettingCommon::applyTheme()
{
    for (auto& s : switchBtns) {
        if (!s.btn) continue;
        const bool on = s.read ? s.read() : false;
        s.btn->setColor(on ? kSwitchOnColor() : 0x666666FF);
        s.btn->setHoverColor(on ? kSwitchOnColor() : 0x666666FF);
    }
    applyThemeSwatchStyle();
    win->refresh();
}

void WinSettingCommon::initCapCtrls()
{
    // 启动程序后开始截图。默认关：程序起来只挂托盘图标静默待命，用户按快捷键 / 点托盘才截。
    // 排在最前面 —— 它是这一页里唯一"决定程序起来长什么样"的一项
    makeSwitchBtn(makeRow(L"setting.startCapture"),
        [] { return Setting::get()->getStartCapture(); },
        [](bool on) { Setting::get()->setStartCapture(on); });

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

void WinSettingCommon::initVideoCtrls()
{
    // 录制时长上限。0 = 不限时，默认 120 分钟（与老版本硬编码的 MP4 上限一致，
    // 所以老用户升上来行为不变）。GIF 也读这一个 —— 原来它写死 6 分钟
    auto limitRow = makeRow(L"setting.videoTimeLimit");
    auto limitBox = limitRow->makeChild<Ling::TextBox>();
    limitBox->setHeight(28.f);
    limitBox->setWidth(96.f);
    // 单行输入框的上下内边距要让位给 setVerticalCenter：TextBox 构造默认上下各 6，
    // 28 高的框只剩 16 逻辑像素装文本，而一行 14 号字就有 ~19 —— 内容永远"溢出"半行，
    // 滚动条就常驻在框上（没换行也在）。同 WinSettingAi 里那六个框
    limitBox->setPadding(6.f, 0.f, 6.f, 0.f);
    limitBox->setBorder(1.f, 0xE0E0E0FF);
    limitBox->setVerticalCenter(true);
    limitBox->setText(std::to_wstring(Setting::get()->getVideoTimeLimitMin()));
    // 落盘时机是"离开输入框 / 回车"，不是每次按键：逐字落盘的话，把 120 改成 30 的
    // 中途会先存下 1、12，而 1 分钟意味着录制一秒后就自动停止 —— 那是真的会丢内容
    auto commitLimit = [limitBox]() {
        const int val = Setting::parseTimeLimitText(limitBox->getText());
        // 一个数字都没敲出来（清空了 / 敲了串别的）：恢复原值，不改配置
        if (val < 0) {
            limitBox->setText(std::to_wstring(Setting::get()->getVideoTimeLimitMin()));
            return;
        }
        Setting::get()->setVideoTimeLimitMin(val);
        // 回显规范化后的值：填了 999999 会被夹住，得让用户看见真正生效的那个数
        limitBox->setText(std::to_wstring(Setting::get()->getVideoTimeLimitMin()));
    };
    limitBox->onFocusChanged.add([commitLimit](Ling::TextBox*, bool focused) {
        if (!focused) commitLimit();
    });
    limitBox->onKeyDown.add([commitLimit](Ling::TextBox*, UINT key, bool* handled) {
        if (key == VK_RETURN) { *handled = true; commitLimit(); }
    });

    auto limitUnit = limitRow->makeChild<Ling::Label>();
    limitUnit->setText(Lang::get(L"setting.videoTimeLimitUnit"));
    limitUnit->setFontSize(13.f);
    limitUnit->setColor(0x666666FF);
    limitUnit->setMarginLeft(6.f);

    auto limitHint = limitRow->makeChild<Ling::Label>();
    limitHint->setText(Lang::get(L"setting.videoTimeLimitHint"));
    limitHint->setFontSize(12.f);
    limitHint->setColor(0x999999FF);
    limitHint->setMarginLeft(12.f);

    // 录制临时目录：temp.mp4 / temp.gif / 裁剪产物都落这里，默认是数据目录
    // （%appdata%\tpix，在系统盘上）。存在的意义就是"录一天"——4Mbps 一天几十个 G，
    // 得能指到大分区去。跟保存目录那一行完全同款：只列当前值，点开选目录
    auto dirRow = makeRow(L"setting.videoTempDir");
    auto dirBtn = dirRow->makeChild<Ling::Button>();
    dirBtn->setHeight(28.f);
    dirBtn->setWidth(240.f);
    dirBtn->setBorder(1.f, 0xE0E0E0FF);
    dirBtn->setHoverBg(0xFFFFFFFF);
    auto applyTempDir = [](Ling::Button* btn) {
        auto dir = Setting::get()->getVideoTempDir();
        // 空 = 没配过，实际落到数据目录（getRecordTempDir 的回落分支），照实说清
        btn->setText(dir.empty() ? Lang::get(L"setting.videoTempDirDefault") : shortenPath(dir));
    };
    applyTempDir(dirBtn);
    dirBtn->onClick.add([this, applyTempDir](Ling::Button* btn) {
        std::wstring dir;
        auto cur = Setting::get()->getVideoTempDir();
        // 空 = 没配过，实际落到数据目录（getRecordTempDir 的回落分支），对话框也停那儿
        if (!pickFolder(win->hwnd, dir,
                cur.empty() ? Setting::get()->getDataPath().wstring() : cur)) return;
        Setting::get()->setVideoTempDir(dir);
        applyTempDir(btn);
    });
}

void WinSettingCommon::initSaveCtrls()
{
    // 保存格式：选项直接写扩展名（大写），比另起一套译名更不容易对不上
    std::vector<std::wstring> fmtItems;
    for (int i = 0; i < 4; ++i) {
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
        auto cur = Setting::get()->getSaveDir();
        // 空 = 没配过，Util::autoSavePath 会落到数据目录下的 screenshot，对话框也停那儿
        auto init = cur.empty()
            ? (Setting::get()->getDataPath() / L"screenshot").wstring()
            : cur;
        if (!pickFolder(win->hwnd, dir, init)) return;
        Setting::get()->setSaveDir(dir);
        applyDir(btn);
    });

    // 数据目录：配置 / 历史 / 模型（ocr\）都在它下面，默认 %appdata%\tpix（在 C 盘）。
    // 与保存目录完全同款：一个按钮把当前值当文本显示，点开选目录。换它要写 exe 同目录的
    // data.json 且重启才生效，所以换完弹一次提示
    auto dataRow = makeRow(L"setting.dataDir");
    auto dataBtn = dataRow->makeChild<Ling::Button>();
    dataBtn->setHeight(28.f);
    dataBtn->setWidth(240.f);
    dataBtn->setBorder(1.f, 0xE0E0E0FF);
    dataBtn->setHoverBg(0xFFFFFFFF);
    auto applyDataDirText = [](Ling::Button* btn) {
        // 显示"当前配置值"（data.json 里那个），不是 getDataPath()：后者是构造期定死的
        // const 成员，改完 data.json 后要重启才变。用它回显的话，用户刚选完新目录、
        // 甚至只是重开一次设置页，界面都跳回老目录 —— 看着就是"没改成功"
        btn->setText(shortenPath(Setting::get()->pendingDataPath()));
    };
    applyDataDirText(dataBtn);
    // 换目录的统一收尾：写盘 + 弹提示 + **立刻回显新目录**。复制旧数据只在目标是空
    // 目录时做 —— 不复制的话重启后配置 / 历史 / 模型全变默认，那是"设置莫名其妙没了"
    // 最常踩的坑；目标非空就不动，让用户自己决定怎么合
    auto applyDataDir = [this, dataBtn](const std::wstring& dir) {
        const auto oldDir = Setting::get()->getDataPath().wstring();
        const bool moved = copyDataTree(oldDir, dir);
        if (!Setting::get()->setDataPath(dir)) {
            // 写盘失败（exe 装在只读位置）：data.json 没落成，重启也不会变。这时画上
            // 新目录就是骗人，明确告知失败、保持原样
            std::wstring warn{ L"Could not write data.json next to tpix.exe.\n\nThe data folder was NOT changed.\n\n" + dir };
            MessageBox(win->hwnd, warn.c_str(), L"tpix", MB_OK | MB_ICONWARNING);
            return;
        }
        std::wstring msg{ L"Data folder changed to:\n" + dir + L"\n\nRestart tpix for it to take effect.\n\n" };
        msg += moved ? L"Existing data has been copied to the new folder."
            : L"The target folder was not empty, so existing data was NOT copied.";
        MessageBox(win->hwnd, msg.c_str(), L"tpix", MB_OK | MB_ICONINFORMATION);
        // 成员 dataPath 要重启才变，所以直接把目标路径画上去，不等 applyDataDirText
        dataBtn->setText(shortenPath(dir));
    };
    dataBtn->onClick.add([this, applyDataDir](Ling::Button* btn) {
        std::wstring dir;
        // 回显当前配置的数据目录（截图里那次停在录制临时目录，就是缺这个指定）
        if (!pickFolder(win->hwnd, dir, Setting::get()->pendingDataPath())) return;
        if (std::filesystem::path{ dir } == Setting::get()->getDataPath()) return;
        applyDataDir(dir);
    });
    // 恢复默认摆在文本框后面。本来想"只在自定义目录时才摆"，但那样默认状态下这一行
    // 只剩一个文本框、看不出还能恢复默认；常驻 + 已是默认时给个反馈更直白
    auto resetDataBtn = dataRow->makeChild<Ling::Button>();
    resetDataBtn->setHeight(28.f);
    resetDataBtn->setWidth(88.f);
    resetDataBtn->setMarginLeft(8.f);
    resetDataBtn->setBorder(1.f, 0xE0E0E0FF);
    resetDataBtn->setHoverBg(0xFFFFFFFF);
    resetDataBtn->setText(Lang::get(L"setting.dataDirReset"));
    resetDataBtn->onClick.add([this, applyDataDir](Ling::Button*) {
        if (!Setting::get()->isCustomDataPath()) {
            MessageBox(win->hwnd, L"Already using the default data folder.", L"tpix", MB_OK | MB_ICONINFORMATION);
            return;
        }
        applyDataDir(Setting::defaultDataPath());
    });

    auto tplRow = makeRow(L"setting.saveNameTpl");
    auto tplBox = tplRow->makeChild<Ling::TextBox>();
    tplBox->setHeight(28.f);
    tplBox->setWidth(200.f);
    // 同上：不加这一行，框右边会常驻一条滚动条 —— WinSettingAi 那轮清扫漏了这一只
    tplBox->setPadding(6.f, 0.f, 6.f, 0.f);
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
    // 双击贴图做什么。三档有各自明确的用途，所以用下拉而不是开关：
    // 缩略图（腾地方）/ 复制并关闭（截完就走，老行为）/ 藏到书签条（既不丢也不挡道）
    auto dblRow = makeRow(L"setting.doubleClick");
    makeSelectBtn(dblRow, 180.f,
        { Lang::get(L"setting.dblThumb"), Lang::get(L"setting.dblCopyClose"), Lang::get(L"setting.dblHide") },
        Setting::get()->getDoubleClickAction(),
        [](int idx) { Setting::get()->setDoubleClickAction(idx); });

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
        if (!pickJsonFile(win->hwnd, path, true, Setting::get()->getDataPath().wstring())) return;
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
        if (!pickJsonFile(win->hwnd, path, false, Setting::get()->getDataPath().wstring())) return;
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

void WinSettingCommon::setAutoStartBtn(Ling::Button* btn)
{
    auto setting = Setting::get();
    auto isAutoStart = setting->getAutoStart();
    if (isAutoStart) {
        btn->setText(L"\ue688");
        btn->setColor(kSwitchOnColor());
        btn->setHoverColor(kSwitchOnColor());
    }
    else {
        btn->setText(L"\ue687");
        btn->setColor(0x666666FF);
        btn->setHoverColor(0x666666FF);
    }
}

