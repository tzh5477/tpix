#include "pch.h"
#include <algorithm>
#include "../AiHistory.h"
#include "../AiService.h"
#include "../AiTranslate.h"
#include "../Lang.h"
#include "../SelectPopup.h"
#include "../Setting.h"
#include "WinSetting.h"
#include "WinSettingAi.h"

namespace {
	// 模型列表的最小宽度（逻辑像素）。模型 id 比那个箭头按钮宽得多（agnes-image-2.5-flash
	// 有 22 个字符），不给下限的话列表就按按钮的宽度定，名字全被截断
	constexpr float modelPopupMinW{ 240.f };

	// 模型那个组合框的尺寸（逻辑像素）。两个数必须自己配平：外框只画一圈边框，
	// 里面输入框与箭头按钮是"外框宽 - 按钮宽"，不给 flex 留任何分配余地
	constexpr float comboW{ 240.f };
	constexpr float pickBtnW{ 28.f };
}

WinSettingAi::WinSettingAi(Ling::WinBase* parent) :Ling::Node(parent)
{
	// 控件不在这里建：由设置窗口按菜单项分别调 buildLlm / buildTrans
	// 窗口关掉时把还开着的列表一起收掉。列表是独立窗口，不会跟着本节点走
	win->onDestroy.add([]() {
		SelectPopup::close();
	});
}

void WinSettingAi::buildLlm()
{
	initAiCtrls();
}

void WinSettingAi::buildTrans()
{
	initTransCtrls();
}

WinSettingAi::~WinSettingAi()
{
	*aiAlive = false;
	SelectPopup::close();
}

Ling::Node* WinSettingAi::makeRow(const std::wstring& labelKey)
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

Ling::Button* WinSettingAi::makeSelectBtn(Ling::Node* row, float width,
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

Ling::Button* WinSettingAi::makeSwitchBtn(Ling::Node* row,
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
	// 同上：按钮本身已经显示出开关状态，单击即翻转，不必再弹一个两项列表
	btn->onClick.add([read, write, apply](Ling::Button*) {
		bool next = !read();
		apply(next);
		write(next);
	});
	return btn;
}

void WinSettingAi::initAiCtrls()
{
	auto setting = Setting::get();

	// 地址与密钥都是手工填：兼容 OpenAI 的服务地址没有"列表"这回事，各家长得都不一样
	auto urlRow = makeRow(L"setting.aiBaseUrl");
	auto urlBox = urlRow->makeChild<Ling::TextBox>();
	urlBox->setHeight(28.f);
	urlBox->setWidth(240.f);
	urlBox->setBorder(1.f, 0xE0E0E0FF);
	urlBox->setVerticalCenter(true);
	urlBox->setPlaceholder(L"https://api.deepseek.com/v1");
	urlBox->setText(setting->getAiStr(L"baseUrl", L""));
	urlBox->onTextChanged.add([](Ling::TextBox*, const std::wstring& val) {
		Setting::get()->setAiStr(L"baseUrl", val);
	});

	// 密码模式：框里画的是圆点，getText 拿到的仍是真实值。它挡的是"旁边有人 / 被截屏"
	// 这一层 —— 值本身还是明文落在 config.json 上的（见 Setting.h 的注释），别把它当成保护
	auto keyRow = makeRow(L"setting.aiApiKey");
	auto keyBox = keyRow->makeChild<Ling::TextBox>();
	keyBox->setHeight(28.f);
	keyBox->setWidth(240.f);
	keyBox->setBorder(1.f, 0xE0E0E0FF);
	keyBox->setVerticalCenter(true);
	keyBox->setPasswordMode(true);
	keyBox->setText(setting->getAiStr(L"apiKey", L""));
	keyBox->onTextChanged.add([](Ling::TextBox*, const std::wstring& val) {
		Setting::get()->setAiStr(L"apiKey", val);
	});

	// 模型：一键一框合成一个组合框 —— 边框只由 combo 画一次，里面两件都不设边框
	// （照 WinBall 的 itemBox：外框容器 + 子控件保持自身默认外观）
	//
	// 两件的宽度都写死、由 comboW 配平，不能靠 flexGrow：TextBox 的构造函数自带
	// setWidth(240)，那是个"确定宽度"，而 flexGrow 只在有剩余空间时才有发言权 ——
	// 240 已经把外框占满了；yoga 的 flexShrink 默认又是 0（不是 CSS 的 1），
	// 这个框一个像素都不会让出来。结果是箭头按钮被摆到外框的右边界之外 35px 处，
	// 被圆角 clip 一裁，整个按钮就"消失"了（模型下拉框点不开、屏幕上找不到）
	auto modelRow = makeRow(L"setting.aiModel");
	auto combo = modelRow->makeChild<Ling::Node>();
	combo->setHeight(28.f);
	combo->setWidth(comboW);   // 与上面地址 / 密钥两个框同宽，右边缘对齐
	combo->setBorder(1.f, 0xE0E0E0FF);
	combo->setBorderRadius(4.f);
	combo->setFlexDirection(Ling::FlexDirection::Row);
	combo->setAlignItems(Ling::Align::Center);

	auto modelBox = combo->makeChild<Ling::TextBox>();
	modelBox->setWidth(comboW - pickBtnW);   // 让出箭头按钮那一格
	modelBox->setHeightPercent(100.f);
	modelBox->setVerticalCenter(true);
	modelBox->setText(setting->getAiStr(L"model", L""));
	modelBox->onTextChanged.add([](Ling::TextBox*, const std::wstring& val) {
		Setting::get()->setAiStr(L"model", val);
	});
	auto pickBtn = combo->makeChild<Ling::Button>();
	pickBtn->setWidth(pickBtnW);
	pickBtn->setHeightPercent(100.f);
	pickBtn->setHoverBg(0xF2F2F2FF);
	pickBtn->setText(L"\u25BE");
	pickBtn->onClick.add([this, modelBox](Ling::Button* btn) {
		auto items = Setting::get()->getAiModels();
		if (items.empty()) {
			MessageBox(win->hwnd, Lang::get(L"ai.fetchFirst").data(),
				Lang::get(L"about.sysTip").data(), MB_OK | MB_ICONINFORMATION);
			return;
		}
		// 选完只往框里填，落盘由 onTextChanged 那一支做 —— 两条路共用一个出口，
		// 免得"下拉选的"和"手打的"哪天走到不同的键上
		SelectPopup::show(win, btn, items, -1, [modelBox, items](int idx) {
			modelBox->setText(items[idx]);
		}, {}, modelPopupMinW);
	});

	// 验证 == 拉一次 /models。拿到列表就说明地址、密钥、网络三者都通，顺带把下拉框的数据源更新掉
	auto verifyRow = makeRow(L"setting.aiVerify");
	auto verifyBtn = verifyRow->makeChild<Ling::Button>();
	verifyBtn->setText(Lang::get(L"setting.aiVerifyBtn"));
	verifyBtn->setHeight(28.f);
	verifyBtn->setWidth(80.f);
	verifyBtn->setBorder(1.f, 0xE0E0E0FF);
	verifyBtn->setHoverBg(0xFFFFFFFF);
	auto statusLabel = verifyRow->makeChild<Ling::Label>();
	statusLabel->setMarginLeft(8.f);
	statusLabel->setFlexGrow(1.f);
	verifyBtn->onClick.add([this, statusLabel](Ling::Button*) {
		statusLabel->setText(Lang::get(L"ai.verifying"));
		auto alive = aiAlive;
		AiService::models(
			[this, statusLabel, alive](const std::vector<std::wstring>& ids) {
				if (!*alive) return;
				Setting::get()->setAiModels(ids);
				statusLabel->setText(Lang::get(L"ai.ok") + std::to_wstring(ids.size()));
			},
			[statusLabel, alive](const std::wstring& err) {
				if (!*alive) return;
				// 成功时 err 是空的，那时别把刚写上去的"连接正常"擦掉
				if (!err.empty()) statusLabel->setText(err);
			});
	});

	makeSwitchBtn(makeRow(L"setting.aiHistorySave"),
		[] { return Setting::get()->getAiHistorySave(); },
		[](bool on) {
			Setting::get()->setAiHistorySave(on);
			// 关掉就把已有记录一起删掉：留一堆旧聊天在磁盘上，"不保存"就是假的
			if (!on && AiHistory::get()) AiHistory::get()->clear();
		});

	// 上限与天数都不给自由输入：这两个数决定磁盘上留多少东西，档位够用了
	constexpr int limitOpts[]{ 20, 50, 100, 200 };
	std::vector<std::wstring> limitItems;
	int limitIdx{ 0 };
	for (int i = 0; i < 4; ++i) {
		limitItems.push_back(std::to_wstring(limitOpts[i]));
		if (limitOpts[i] == Setting::get()->getAiHistoryLimit()) limitIdx = i;
	}
	auto limitRow = makeRow(L"setting.aiHistoryLimit");
	makeSelectBtn(limitRow, 80.f, limitItems, limitIdx,
		[limitOpts](int idx) { Setting::get()->setAiHistoryLimit(limitOpts[idx]); });

	constexpr int dayOpts[]{ 7, 30, 90, 365 };
	std::vector<std::wstring> dayItems;
	int dayIdx{ 0 };
	for (int i = 0; i < 4; ++i) {
		dayItems.push_back(std::to_wstring(dayOpts[i]));
		if (dayOpts[i] == Setting::get()->getAiHistoryDays()) dayIdx = i;
	}
	auto dayRow = makeRow(L"setting.aiHistoryDays");
	makeSelectBtn(dayRow, 80.f, dayItems, dayIdx,
		[dayOpts](int idx) { Setting::get()->setAiHistoryDays(dayOpts[idx]); });
}

void WinSettingAi::initTransCtrls()
{
	auto setting = Setting::get();

	// 两条路：火山是专用翻译接口（便宜、快、语种固定），大模型是"顺便能翻"（不另配凭据，
	// 但慢、且吃 token）。切换只改一个字符串，其余配置各自留着，切回来不用重填
	std::vector<std::wstring> providerItems{ Lang::get(L"ai.providerVolc"), Lang::get(L"ai.providerModel") };
	int providerIdx{ setting->getAiStr(L"transProvider", L"volc") == L"model" ? 1 : 0 };
	makeSelectBtn(makeRow(L"setting.transProvider"), 160.f, providerItems, providerIdx,
		[](int idx) { Setting::get()->setAiStr(L"transProvider", idx == 1 ? L"model" : L"volc"); });

	auto akRow = makeRow(L"setting.transVolcAk");
	auto akBox = akRow->makeChild<Ling::TextBox>();
	akBox->setHeight(28.f);
	akBox->setWidth(240.f);
	akBox->setBorder(1.f, 0xE0E0E0FF);
	akBox->setVerticalCenter(true);
	akBox->setText(setting->getAiStr(L"volcAk", L""));
	akBox->onTextChanged.add([](Ling::TextBox*, const std::wstring& val) {
		Setting::get()->setAiStr(L"volcAk", val);
	});

	// Secret 与上面的 API Key 同理：框里画圆点，值本身仍是明文落盘的
	auto skRow = makeRow(L"setting.transVolcSk");
	auto skBox = skRow->makeChild<Ling::TextBox>();
	skBox->setHeight(28.f);
	skBox->setWidth(240.f);
	skBox->setBorder(1.f, 0xE0E0E0FF);
	skBox->setVerticalCenter(true);
	skBox->setPasswordMode(true);
	skBox->setText(setting->getAiStr(L"volcSk", L""));
	skBox->onTextChanged.add([](Ling::TextBox*, const std::wstring& val) {
		Setting::get()->setAiStr(L"volcSk", val);
	});

	// 验证就是真翻一句。翻译接口没有"ping"这回事，能翻出东西就说明密钥与签名都对
	auto verifyRow = makeRow(L"setting.transVerify");
	auto verifyBtn = verifyRow->makeChild<Ling::Button>();
	verifyBtn->setText(Lang::get(L"setting.aiVerifyBtn"));
	verifyBtn->setHeight(28.f);
	verifyBtn->setWidth(80.f);
	verifyBtn->setBorder(1.f, 0xE0E0E0FF);
	verifyBtn->setHoverBg(0xFFFFFFFF);
	auto statusLabel = verifyRow->makeChild<Ling::Label>();
	statusLabel->setMarginLeft(8.f);
	statusLabel->setFlexGrow(1.f);
	verifyBtn->onClick.add([this, statusLabel](Ling::Button*) {
		if (!AiTranslate::volcReady()) {
			statusLabel->setText(Lang::get(L"ai.noKey"));
			return;
		}
		statusLabel->setText(Lang::get(L"ai.verifying"));
		auto alive = aiAlive;
		AiTranslate::run(L"hello", L"en", L"zh",
			[statusLabel, alive](const std::wstring& result, const std::wstring&, const std::wstring& err) {
				if (!*alive) return;
				if (!err.empty()) { statusLabel->setText(err); return; }
				statusLabel->setText(Lang::get(L"ai.ok") + result);
			});
	});
}
