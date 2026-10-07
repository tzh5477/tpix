#pragma once
#include <include/Ling.h>
#include <memory>
#include <string>
#include "../AiService.h"

// 翻译窗。左上是语言方向，中间是原文与译文，下面是动作按钮。
//
// 取词翻译走的是「OCR 结果 → 这里」：WinOcr 上那个「翻译」按钮把识别出来的文字带进来，
// 于是"截一块屏幕 → 认字 → 翻译"整条路是通的，不必去模拟 Ctrl+C 抓别人程序的选区。
class WinAiTrans : public Ling::WinBase
{
public:
	~WinAiTrans();
	// preset 非空时填进输入框并立刻翻一次（OCR 那条路就是这么进来的）
	static void init(const std::wstring& preset = L"");
	static void dispose();
private:
	WinAiTrans();
	void onCreated() override;
	LRESULT onHitTest(const POINT pos) override;
	// 带进来的原文（截屏取词 / 选中文本的快捷键）。必须在构造之后调 ——
	// 控件是在 onCreated 里建出来的，而 onCreated 在构造函数里就跑完了
	void applyPreset(const std::wstring& text);
	void applyLangBtns();
	// 把目标语言切到 code 并同步按钮显示（"自动检测"推出的目标语言走这里）
	void setToLang(const std::wstring& code);
	void refreshResult(const std::wstring& text);
	void run();
	void setBusy(bool on);
	// 当前选中的源 / 目标语言代码
	std::wstring fromCode() const;
	std::wstring toCode() const;
private:
	// 下标对的是 AiTranslate::langs()；目标语言从 1 起（"自动检测"不能当目标）
	int fromIdx{ 0 };
	int toIdx{ 0 };
	Ling::TextBox* input{ nullptr };
	// 译文区也是个 TextBox 而不是 Label：Label 不折行、也没法选中，
	// 而译文经常很长、用户还想抠其中一段
	Ling::TextBox* resultBox{ nullptr };
	Ling::Label* statusLabel{ nullptr };
	Ling::Button* fromBtn{ nullptr };
	Ling::Button* toBtn{ nullptr };
	Ling::Button* runBtn{ nullptr };
	AiService::TaskPtr task{ nullptr };
	bool busy{ false };
	std::wstring result;
	// 窗口关掉之后迟到的回调不能再碰这些节点（收尾回调是"取消也照送"的）
	std::shared_ptr<bool> alive{ std::make_shared<bool>(true) };
};
