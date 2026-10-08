#pragma once
#include <include/Ling.h>
#include <memory>
#include <string>
#include <vector>
#include "../AiService.h"
#include "../Ocr.h"
#include "../Table.h"

// OCR 结果窗口。建好就开一条工作线程去识别，识别完把文字填进多行文本框 ——
// 识别一张满屏图可能要好几百毫秒，直接堵在 UI 线程上窗口会卡住不动。
class WinOcr : public Ling::WinBase
{
public:
	WinOcr(std::vector<BYTE>&& data, const int w, const int h);
	~WinOcr();
	static void init(std::vector<BYTE>&& data, const int w, const int h);
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
	// false = 系统离线 OCR；true = 大模型。存在 toolPin.ocr.engine 里（与语言同一处）
	bool aiMode{ false };
	AiService::TaskPtr aiTask{ nullptr };
	// 窗口关掉之后迟到的回调不能再碰这些节点（postDone 是"取消也照送"的语义）
	std::shared_ptr<bool> alive{ std::make_shared<bool>(true) };
	// 每发起一次识别加一。连点换语言会并发跑好几个线程，回来的先后没保证，
	// 只认最后发起的那次，否则点了日语却显示上一次中文的结果
	int taskSeq{ 0 };
};
