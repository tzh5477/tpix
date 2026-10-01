#pragma once
#include <include/Ling.h>
class WinSettingCommon:public Ling::Node
{
public:
	WinSettingCommon(Ling::WinBase* parent);
	~WinSettingCommon();
	// 收掉语言下拉框。它挂在 win->body 上而不是挂在本节点里，所以本节点被换掉 / 销毁时
	// 它不会跟着走，得由外面在合适的时机显式收掉
	void hideSelectBox();
private:
	void initAutoStartCtrls();
	void initLangCtrls();
	// 一级工具条（ToolCap）上显示哪些标注工具。开关走 Setting 的 toolCap 组
	void initCapBtnCtrls();
	// 捕获：延时 / 包含鼠标指针 / 定时自动截图。都走 Setting 的 cap 组
	void initCapCtrls();
	// 输出格式、自动保存目录与命名模板。存盘路径由 Util::resolveSavePath 消费
	void initSaveCtrls();
	// 历史上限、剪贴板监听开关、打开历史窗口
	void initHistoryCtrls();
	// 贴图持久化开关：退出时存、启动时恢复
	void initPinCtrls();
	// OCR 默认识别语言。与 WinOcr 窗口上的语言按钮共用 Setting 的 ocr 组
	void initOcrCtrls();
	// 开 / 关两套配色，与 ToolSub::applyToggleStyle 保持一致
	static void applyCapBtnStyle(Ling::Button* btn, bool selected);
	// 一行「标签 + 控件」。生成的行节点作为返回值交给调用方塞控件，分隔线是本节点的
	// 子节点而不是行内的，必须在下一行入列之前加好，所以顺手在这里加掉
	Ling::Node* makeRow(const std::wstring& labelKey);
	void setAutoStartBtn(Ling::Button* btn);
	void showSelectBox(Ling::Button* btn);
private:
	Ling::Button* selectBtn{ nullptr };
	Ling::ScrollerBox* selectBox{ nullptr };
	winrt::event_token onMouseDownToken;
};

