#include "pch.h"
#include <shobjidl.h>
#include <filesystem>
#include <thread>
#include "../Lang.h"
#include "../Ocr.h"
#include "../OcrModel.h"
#include "../SelectPopup.h"
#include "../Setting.h"
#include "WinSetting.h"
#include "WinSettingTextRec.h"

WinSettingTextRec::WinSettingTextRec(Ling::WinBase* parent) :Ling::Node(parent)
{
	initModelCtrls();
	// 顺序即显示顺序：识别模型 -> 模型目录 -> 识别语言
	initModelDirCtrl();
	initLangCtrls();
	// 窗口关掉时把还开着的列表一起收掉。列表是独立窗口，不会跟着本节点走
	win->onDestroy.add([]() {
		SelectPopup::close();
	});
}

WinSettingTextRec::~WinSettingTextRec()
{
	// 后台下载线程只认这个标志：它手里是裸 this，对象没了（切页 / 关窗）就不能再碰
	if (fetchAlive) *fetchAlive = false;
}

void WinSettingTextRec::fetchModel(const std::wstring& name)
{
	// 文件已经下全（用户自己放的，或者这一代下过）：直接返回，不弹东西
	if (OcrModel::hasFiles(Ocr::modelDir(), name)) return;
	auto alive = fetchAlive;
	auto root = Ocr::modelDir();
	// 下载要几秒（tiny 6MB）到几十秒（small 31MB），压在 UI 线程上设置页会整页僵住。
	// 里面走 WinRT 的 HttpClient，线程得先 init_apartment（同 WinPin::startOcr / WinOcr）
	std::thread([this, alive, root, name]() {
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
		const auto err = OcrModel::fetch(root, name);
		Ling::App::get()->dq.TryEnqueue([this, alive, err]() {
			// 对象已经没了（切页 / 关窗）：什么都不弹
			if (!*alive) return;
			if (err == OcrModel::Err::ok)
				MessageBox(win->hwnd, Lang::get(L"ocr.modelFetchOk").c_str(),
					Lang::get(L"about.sysTip").c_str(), MB_OK | MB_ICONINFORMATION);
			else
				MessageBox(win->hwnd, Lang::get(L"ocr.modelDownload").c_str(),
					Lang::get(L"about.sysTip").c_str(), MB_OK | MB_ICONWARNING);
		});
	}).detach();
}

Ling::Node* WinSettingTextRec::makeRow(const std::wstring& labelKey)
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

	// ⚠️ 分隔线做成 box 的子节点并绝对定位于底部：这样整行被 hide()（识别语言行在
	// 大模型 / 内置模型下默认隐藏）时，分隔线跟着一起消失，不会在页面底部留一条悬空线。
	// 效果与 Common 页用 sibling 分隔线一致，但规避了"最后一行隐藏时尾巴悬空"那档事
	auto border = box->makeChild<Ling::Node>();
	border->setHeight(1.f);
	border->setPositionType(Ling::Position::Absolute);
	border->setPosition(Ling::Edge::Left, 0.f);
	border->setPosition(Ling::Edge::Right, 0.f);
	border->setPosition(Ling::Edge::Bottom, 0.f);
	border->setBg(0xE0E0E0FF);
	return box;
}

Ling::Button* WinSettingTextRec::makeSelectBtn(Ling::Node* row, float width,
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
	// 两项的（这里没有）单击即在两项间切换；其余都弹列表，选完把字换掉
	btn->onClick.add([this, btn, items, onPick](Ling::Button*) {
		SelectPopup::show(win, btn, items, -1, [btn, items, onPick](int idx) {
			onPick(idx);
			btn->setText(items[idx]);
		});
	});
	return btn;
}

void WinSettingTextRec::initModelCtrls()
{
	// 离线模型 + 大模型两条路并在「识别模型」这一个下拉里选。
	// 离线模型来自 ocr\<名字>\ 下成对的 det / rec onnx（含还没下下来的内置代次）；
	// 大模型是列表末尾追加的一项，选中即切到 AI 引擎
	auto models = Ocr::models();
	const int aiIndex = (int)models.size();
	// 下拉里看到的名字用显示名（PP-OCRv6-small），但 onPick 落盘 / 建会话用的
	// 还是内部名 models[i]（PP-OCRv6）—— 老用户 ocr\PP-OCRv6\ 里已是 small，不能动
	std::vector<std::wstring> items;
	items.reserve(models.size() + 1);
	for (auto& m : models) items.push_back(Ocr::modelLabel(m));
	items.push_back(Lang::get(L"setting.ocrModelAi"));

	// 当前选中：AI 就指向末尾那项，否则指向当前离线模型名所在的下标
	const bool ai = Ocr::useAi();
	int cur = ai ? aiIndex : 0;
	if (!ai) {
		auto curName = Ocr::curModel();
		for (int i = 0; i < (int)models.size(); ++i) if (models[i] == curName) { cur = i; break; }
	}

	auto row = makeRow(L"setting.ocrModel");
	makeSelectBtn(row, 160.f, items, cur, [this, models, aiIndex](int i) {
		if (i == aiIndex) {
			// 大模型：切到 AI 引擎，离线模型名保留以便切回
			Setting::get()->setToolStr(L"ocr", L"engine", L"ai");
		}
		else {
			Ocr::setModel(models[i]);
			Setting::get()->setToolStr(L"ocr", L"engine", L"sys");
			// 内置代次可能还没下过文件（下拉里列出了它们，见 OcrModel::listModels）。
			// 选中即去下，别等第一次识别才懒触发 —— 那会儿用户只会看到"没有识别到文字"
			fetchModel(models[i]);
		}
		// 识别语言只对系统离线 OCR 有意义：大模型 / 内置模型都不用选语言，切完顺手把那行显隐调一下
		if (langRow) {
			bool showLang = !Ocr::useAi() && Ocr::engine() == OcrEngine::System;
			if (showLang) langRow->show();
			else langRow->hide();
		}
	});
}

void WinSettingTextRec::initLangCtrls()
{
	// 语言包只对系统 OCR 有意义：内置模型是多语种的、大模型用不上，它们选中时这行藏起来
	if (Ocr::useAi() || Ocr::engine() != OcrEngine::System) {
		// 先建着但藏起来，切模型时再由 initModelCtrls 里的回调决定是否显示
		langRow = makeRow(L"setting.ocrLang");
		langRow->hide();
		return;
	}
	auto langs = Ocr::languages();
	langRow = makeRow(L"setting.ocrLang");
	// 一个识别语言包都没装：这行只能当提示用，点开了也没得选
	if (langs.empty()) {
		auto btn = langRow->makeChild<Ling::Button>();
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
	makeSelectBtn(langRow, 140.f, items, idx, [langs](int i) {
		Setting::get()->setToolStr(L"ocr", L"lang", i == 0 ? L"" : langs[i - 1].tag);
	});
}

void WinSettingTextRec::initModelDirCtrl()
{
	// 模型目录这一行是给「用户自己下模型」用的：点开就是 <数据目录>\ocr，
	// 往里面放 <任意名字>\{xxx_det.onnx, xxx_rec.onnx}，重开这一页就会出现在
	// 识别模型下拉里（listModels 是按目录扫的，不认目录名）。按钮只放"打开目录"，
	// 不把路径画在按钮上 —— 路径带用户名，太长且没有必要
	auto row = makeRow(L"setting.ocrModelDir");
	auto btn = row->makeChild<Ling::Button>();
	btn->setHeight(28.f);
	btn->setWidth(100.f);
	btn->setBorder(1.f, 0xE0E0E0FF);
	btn->setHoverBg(0xFFFFFFFF);
	btn->setText(Lang::get(L"setting.openDir"));
	btn->onClick.add([this](Ling::Button*) {
		auto dir = Ocr::modelDir();
		// 一次都没下过内置模型时这个目录还不存在：先建出来，否则点开没反应
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		ShellExecute(win->hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	});
}
