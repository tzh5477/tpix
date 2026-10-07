#pragma once
#include <include/Ling.h>
#include <functional>
#include <string>
#include <vector>
class WinSettingCommon:public Ling::Node
{
public:
	WinSettingCommon(Ling::WinBase* parent);
	~WinSettingCommon();
private:
	void initAutoStartCtrls();
	void initLangCtrls();
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
	// 配置备份与恢复：导出成一份 json / 从一份 json 导入
	void initConfigCtrls();
	// 导入导出失败时的提示（系统提示标题 + 一行说明）
	void showConfigTip(const std::wstring& key);
	// 一行「标签 + 控件」。生成的行节点作为返回值交给调用方塞控件，分隔线是本节点的
	// 子节点而不是行内的，必须在下一行入列之前加好，所以顺手在这里加掉
	Ling::Node* makeRow(const std::wstring& labelKey);
	// 一行里那个「点一下弹出全部选项」的按钮。按钮上显示当前这一档，
	// 选完由 onPick 落盘（按钮上的字这里自己换掉）。
	// 原来这些位置都是「点一次切一档」的循环按钮，档位一多就得点好几下才转到想要的那个
	Ling::Button* makeSelectBtn(Ling::Node* row, float width,
		const std::vector<std::wstring>& items, int cur,
		std::function<void(int)> onPick);
	// 行尾那个开 / 关开关：按钮上是勾或叉，点一下弹出两项直接选。
	// read 取当前是否开着，write 把新状态落盘（顺带做装钩子这类副作用）
	Ling::Button* makeSwitchBtn(Ling::Node* row,
		std::function<bool()> read, std::function<void(bool)> write);
	void setAutoStartBtn(Ling::Button* btn);
};
