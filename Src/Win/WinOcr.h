#pragma once
#include <include/Ling.h>
#include <vector>
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
	void startRecognize();
	void setResult(const std::wstring& text);
	void setTable(const TableResult& table);
	void applyTableBtn();
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
	// 每发起一次识别加一。连点换语言会并发跑好几个线程，回来的先后没保证，
	// 只认最后发起的那次，否则点了日语却显示上一次中文的结果
	int taskSeq{ 0 };
};
