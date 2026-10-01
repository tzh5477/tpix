#pragma once
#include <string>
#include <vector>

// 系统里装好的一种识别语言
struct OcrLang
{
	// BCP-47 语言标签，如 L"zh-Hans-CN"，传给 recognize 用来指定引擎
	std::wstring tag;
	// 给人看的显示名（系统按界面语言给的，如"中文(简体)"）
	std::wstring name;
};

// 内置离线 OCR，走 Windows.Media.Ocr（系统自带，不用带模型文件，联网也不需要）。
//
// 前提：用户在"设置 - 时间和语言 - 语言"里装了某个语言的识别包。一个都没装时
// isAvailable() 返回 false，调用方应当退回原来的插件路径，而不是弹一句"识别失败"。
class Ocr
{
public:
	// 系统里有没有可用的识别引擎
	static bool isAvailable();
	// 已装的识别语言。一个都没装时返回空 —— 调用方应当据此退回插件路径
	static std::vector<OcrLang> languages();
	// 识别一张 BGRA top-down 的图，返回按行拼起来的文字；认不出来返回空串。
	// langTag 为空时按用户的语言档案挑引擎，挑不到就退回第一个已装的语言。
	// 可能耗时几百毫秒到几秒，别在 UI 线程上直接调
	static std::wstring recognize(const int w, const int h, BYTE* data,
		const std::wstring& langTag = L"");
};
