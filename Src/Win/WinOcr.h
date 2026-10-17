#pragma once
#include <include/Ling.h>
#include <memory>
#include <string>
#include <vector>
#include "../AiService.h"
#include "../Ocr.h"
#include "../Table.h"

class WinPin;

// OCR 结果窗口。建好就开一条工作线程去识别，识别完把文字填进多行文本框 ——
// 识别一张满屏图可能要好几百毫秒，直接堵在 UI 线程上窗口会卡住不动。
class WinOcr : public Ling::WinBase
{
public:
	WinOcr(std::vector<BYTE>&& data, const int w, const int h, WinPin* owner = nullptr,
		bool silent = false);
	~WinOcr();
	// owner：从哪张贴图打开的。贴图那条路会把图自动藏进侧边栏（见 WinPin::ocrToWindow），
	// 识别窗关闭时负责把它放回来；截图编辑页那条路不传，没有"放回"一说
	static void init(std::vector<BYTE>&& data, const int w, const int h, WinPin* owner = nullptr);
	// 静默识别：窗口**不显示**，认出文字就直接进剪贴板并关窗。
	// 用在设置 - 文字识别「识别之后 = 直接复制文字并关闭截图」那一档。
	// 一个字都没认出来时照旧把窗口显出来 —— 那种情况必须让用户看见原因，
	// 否则点了"文字识别"就跟没反应一样
	static void initAndCopy(std::vector<BYTE>&& data, const int w, const int h);
	static void dispose();
private:
	void onCreated() override;
	// 标题行当拖拽区、放开 Ling 默认的 800×600 最小跟踪尺寸（这个窗只有 560×420）
	LRESULT onHitTest(const POINT pos) override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	void startRecognize();
	void setResult(const std::wstring& text);
	void setTable(const TableResult& table);
	void applyTableBtn();
	// 识别引擎：系统自带的离线 OCR，或者大模型（见 aiMode）。两者不是谁替代谁 ——
	// 前者不用联网也不花钱，后者对排版乱的图、以及"顺带把表格认出来"更灵
	void applyEngineBtn();
	// 文字 / 表格是**两个场景**，各自可以绑不同的大模型（见 Setting::setScenario）。
	// 下拉与按钮上的模型名要跟着当前这一档走，所以到处都得先问它
	std::wstring curScenario() const;
	// 「识别模型」下拉的当前项下标（-1 = 都不匹配，列表里不高亮）。本地模型按内部名
	// 比对、大模型按（接口, 模型）比对 —— 只看模型名会在两家的模型重名时指错行
	int curChoiceIndex(const std::vector<OcrModelChoice>& choices) const;
	// 用大模型认这一张图。走的是对话那条已经做好的图通路（AiService::Msg 的 image），
	// 输出要靠提示词约束成纯文本 / TSV 表格
	void startAiRecognize();
	// 语言选择：0 是"跟随系统"，后面依次是 Ocr::languages() 里的每一项
	int langIndex{ 0 };
	std::wstring curLangTag() const;
	void applyLangBtn();
private:
	std::vector<BYTE> pixels;
	int imgW{ 0 }, imgH{ 0 };
	// 从哪张贴图打开的（贴图那条路才有）。关闭时调它的 revealAfterOcr 放回侧边栏里藏的那张
	WinPin* pinOwner{ nullptr };
	// 静默识别：窗口不显示，认出字就复制 + 关窗（见 initAndCopy）。
	// 认不出东西时会自己放掉这一档、把窗口显出来
	bool silentCopy{ false };
	// 静默模式的收尾：有字 → 进剪贴板 + 关窗；没字 → 退回正常显示窗口
	void finishSilent(const std::wstring& text);
	// 把静默模式收掉并把窗口显出来（认不出字 / 没配密钥这类"必须告诉用户"的情况）
	void revealSilent();
	std::wstring result;
	// 表格模式下的 HTML：框里显示的是 tab 分隔的文本，复制时才把它一起放上剪贴板，
	// 这样粘到 Word / Excel 里是一张真表
	std::wstring html;
	bool tableMode{ false };
	Ling::Button* tableBtn{ nullptr };
	Ling::TextBox* box{ nullptr };
	std::vector<OcrLang> langs;
	Ling::Button* langBtn{ nullptr };
	Ling::Button* engineBtn{ nullptr };
	// false = 本地离线模型；true = 大模型。存在 toolPin.ocr.engine 里（与语言同一处）。
	// ⚠️ 它只说"走哪条路"，**具体是哪个模型**另有出处：本地那代在 toolPin.ocr.model，
	// 大模型在场景配置里（当前是文字还是表格场景见 curScenario）
	bool aiMode{ false };
	AiService::TaskPtr aiTask{ nullptr };
	// 窗口关掉之后迟到的回调不能再碰这些节点（postDone 是"取消也照送"的语义）
	std::shared_ptr<bool> alive{ std::make_shared<bool>(true) };
	// 每发起一次识别加一。连点换语言会并发跑好几个线程，回来的先后没保证，
	// 只认最后发起的那次，否则点了日语却显示上一次中文的结果
	int taskSeq{ 0 };
};
