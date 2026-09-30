#pragma once
#include <include/Ling.h>
#include <vector>

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
private:
	std::vector<BYTE> pixels;
	int imgW{ 0 }, imgH{ 0 };
	std::wstring result;
	Ling::TextBox* box{ nullptr };
};
