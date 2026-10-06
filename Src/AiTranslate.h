#pragma once
#include <functional>
#include <string>
#include <vector>
#include "AiService.h"

// 翻译。两条路，由 ai.transProvider 决定：
//   1) volc  —— 火山引擎机器翻译。签名认证（HMAC-SHA256，见 AiTranslate.cpp 里的注释），
//               与 AiService 那套 Bearer 完全不是一回事，所以不复用它的请求代码。
//   2) model —— 交给大模型，复用 AiService::chat（流式），换一套提示词而已。
//
// 返回的是 AiService::TaskPtr：取消语义与对话共用（取消也会回调 onDone）。
class AiTranslate
{
public:
	// 一种语言。code 是火山的语言代码，也是落盘的值；name 是给人看的中文名，
	// 大模型那条路把它写进提示词里（模型不认 "zh" 这种代码也能工作，但认名字更稳）
	struct LangItem
	{
		std::wstring code;
		std::wstring name;
	};
	// 语言表。第 0 项是「自动检测」，只能当源语言 —— 火山要求 TargetLanguage 必填
	static const std::vector<LangItem>& langs();
	static std::wstring langName(const std::wstring& code);

	// 当前这条路能不能用（密钥 / 地址填齐了没有）。UI 拿它决定要不要先引导去配置
	static bool ready();
	// 火山那条路单独判：设置页只在这一组上显示"验证"按钮
	static bool volcReady();

	// 翻一段文字。from 为空或 "auto" 表示自动检测源语言。
	// onDone(err 为空 = 成功)：result 是译文，detected 是检测到的源语言（只有自动检测时才有）。
	// 回调都在 UI 线程；取消时 err = ai.canceled，result 是已经收到的那部分
	static AiService::TaskPtr run(const std::wstring& text, const std::wstring& from,
		const std::wstring& to,
		std::function<void(const std::wstring& result, const std::wstring& detected,
			const std::wstring& err)> onDone);
};
