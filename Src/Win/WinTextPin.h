#pragma once
#include <include/Ling.h>
#include <vector>
#include <memory>
#include <string>

// 文本钉窗：把一段文字当成「贴在屏幕上的可编辑 txt」来用，而不是烤成一张死图。
// 顶部一条深色的标题栏（按住拖动整扇窗、右边一枚关闭按钮），下面整块是 Ling::TextBox，
// 自带 Ctrl+A / C / X / V、方向键 / Home / End、滚轮、IME，双击选词、三击选段（见 Ling TextBox）。
// 不做 OCR、不做图片标注 —— 文本就是文本。
class WinTextPin : public Ling::WinBase
{
public:
	// 在屏幕中央开一扇文本钉窗。空串什么都不做
	static void init(const std::wstring& text);
	// 现在还有没有文本钉窗开着
	static bool hasWindow();
	// 退出流程里调：窗口对象是文件级静态变量，交给静态析构就在 CoUninitialize 之后了
	static void dispose();
private:
	explicit WinTextPin(const std::wstring& text);
	void onCreated() override;
	// 文本区尺寸跟着窗口走（宽度撑满、高度 = 窗口高 - 标题栏高）。放在这里而不是 onCreated：
	// 拖边框缩放时 WinBase::sizeChange 会再调一次 layout()，尺寸据此重算
	void layout() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	// 标题栏拖窗 / 边框缩放都走这里：WS_POPUP 没有系统标题栏，自己认 HTCAPTION / HTxxxx 边界。
	// 交给系统走那套非客户区拖拽，比在自己窗口里按客户端坐标算位移稳得多（后者会和窗口
	// 自身移动互相追赶，抖得厉害）。见 WinAiChat::onHitTest 同款做法
	LRESULT onHitTest(const POINT pos) override;
	void onClosed();
private:
	Ling::TextBox* textBox{ nullptr };
	Ling::Node* titleBar{ nullptr };
	Ling::Label* titleLabel{ nullptr };
	Ling::Button* closeBtn{ nullptr };
	std::wstring content;
	bool isClosed{ false };
	// 标题栏逻辑高度（实际物理高度 = 它 × dpi）
	static constexpr float kTitleH{ 30.f };
	static std::vector<std::unique_ptr<WinTextPin>> winTextPins;
};
