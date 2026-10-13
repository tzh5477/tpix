#pragma once
#include <include/Ling.h>
#include <memory>
#include <string>
#include "../AiService.h"
#include "../Ocr.h"

// 翻译窗。左上是语言方向，中间是原文与译文，下面是动作按钮。
//
// 取词翻译走的是「OCR 结果 → 这里」：WinOcr 上那个「翻译」按钮把识别出来的文字带进来，
// 于是"截一块屏幕 → 认字 → 翻译"整条路是通的，不必去模拟 Ctrl+C 抓别人程序的选区。
class WinAiTrans : public Ling::WinBase
{
public:
	~WinAiTrans();
	// preset 非空时填进输入框并立刻翻一次（OCR 那条路就是这么进来的）。
	// owner：从哪个窗口唤起的（比如识别窗）。它唤起时把自己藏了，这里关闭时要放它回来
	static void init(const std::wstring& preset = L"", Ling::WinBase* owner = nullptr);
	static void dispose();
private:
	WinAiTrans();
	void onCreated() override;
	LRESULT onHitTest(const POINT pos) override;
	// 带进来的原文（截屏取词 / 选中文本的快捷键）。必须在构造之后调 ——
	// 控件是在 onCreated 里建出来的，而 onCreated 在构造函数里就跑完了
	void applyPreset(const std::wstring& text);
	void applyLangBtns();
	// 行尾那枚按钮上的字 + 它的下拉内容：当前这一单走的是哪条路（火山 / 某个大模型）。
	// 恒在、不藏 —— 见成员 srcBtn 的注释
	void applySrcBtn();
	// 下拉里高亮哪一项。下标按 applySrcBtn 那份列表算：第 0 项是火山引擎，之后才是
	// choices 里的模型，所以这里回的是 choices 的下标 +1
	int curSrcIndex(const std::vector<OcrModelChoice>& choices) const;
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
	// 行尾那枚（见 applySrcBtn）：当前这一单翻译走的是哪条路 —— 火山引擎 / 某个大模型。
	// 它同时是**切换入口**：点开的下拉第 0 项固定是火山引擎，之后是已配的全部大模型，
	// 选中即换路并立刻用新路把当前原文重翻一次。
	// 必须恒在 —— 早先的写法是"走火山引擎就把按钮藏起来、换一行静态文字"，而那行字
	// 是 Label、点不动，用户点上去毫无反应，看着就是"这个下拉坏了"
	Ling::Button* srcBtn{ nullptr };
	Ling::Button* runBtn{ nullptr };
	AiService::TaskPtr task{ nullptr };
	bool busy{ false };
	std::wstring result;
	// 窗口关掉之后迟到的回调不能再碰这些节点（收尾回调是"取消也照送"的）
	std::shared_ptr<bool> alive{ std::make_shared<bool>(true) };
	// 从哪个窗口唤起的（识别窗 / 无）。关闭时把它 show 回来 —— 两个置顶窗叠着
	// 只会互相挡，唤起方都是先藏了自己再开这一扇
	Ling::WinBase* ownerWin{ nullptr };
};
