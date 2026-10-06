#include "pch.h"
#include "../Lang.h"
#include "WinSetting.h"
#include "WinSettingAbout.h"
#include "../Util.h"

WinSettingAbout::WinSettingAbout(Ling::WinBase* parent):Ling::Node(parent)
{
    std::vector<std::wstring> keys = { L"version",L"project" };
    for (auto& key : keys)
    {
        auto box = makeChild<Ling::Node>();
        box->setHeight(39.f);
        box->setFlexDirection(Ling::FlexDirection::Row);
        box->setAlignItems(Ling::Align::Center);

        auto label = box->makeChild<Ling::Label>();
        label->setText(Lang::get(L"about." + key));
        label->setHeightPercent(100.f);
        label->setJustifyContent(Ling::Justify::Center);
        label->setFlexGrow(1.f);

        auto btn = box->makeChild<Ling::Button>();
        btn->setId(key);
        if (key == L"version") {
            auto ver = Ling::Util::getVerNum();
            auto verStr = std::format(L"{}.{}.{}", ver[0], ver[1], ver[2]);
            btn->setText(verStr);
        }
        else if (key == L"project") {
            btn->setText(L"github.com/tzh5477/tpix");
            btn->setColor(0x597ef7ff);
            btn->setHoverColor(0x597ef7ff);
            btn->onClick.add([this](Ling::Button* btn) {
                std::wstring downloadUrl{ L"https://github.com/tzh5477/tpix" };
                ShellExecute(win->hwnd, L"open", downloadUrl.data(), nullptr, nullptr, SW_SHOWNORMAL);
                });
        }
        btn->setAlignItems(Ling::Align::FlexEnd);
        btn->setHeight(28.f);
        btn->setWidth(120.f);
        btn->setBg(0);
        btn->setHoverBg(0);
        btns.push_back(btn);

        auto border = makeChild<Ling::Node>();
        border->setHeight(1.f);
        border->setBg(0xE0E0E0FF);
    }
    // 项目简介：贴在两行信息下面的整宽文本。固定给两三行的高度 ——
    // 不设高的话 yoga 量不出文字高度， Label 会被压成一条缝
    auto desc = makeChild<Ling::Label>();
    desc->setText(Lang::get(L"about.desc"));
    desc->setMarginTop(8.f);
    desc->setHeight(56.f);
}

WinSettingAbout::~WinSettingAbout()
{

}
