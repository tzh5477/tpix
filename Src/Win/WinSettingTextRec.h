#pragma once
#include <memory>
#include <include/Ling.h>
#include "WinSetting.h"

// 「文本识别」设置大类：把原来散在「通用设置」里的 识别模型 / 识别语言 两项，
// 连同「用大模型识别」这一项，集中到这一个页面暴露出来。
// 识别模型下拉同时列出离线模型（PP-OCRv6 …）与「大模型」一项：选大模型置
// toolPin.ocr.engine=ai，选离线模型置回 sys（离线模型名保留，便于切回）。
// 识别语言只对系统离线 OCR 有意义：内置模型多语种、大模型也用不上，它们选中时这行藏起来
class WinSettingTextRec : public Ling::Node
{
public:
	WinSettingTextRec(Ling::WinBase* parent);
	~WinSettingTextRec();
private:
	Ling::Node* makeRow(const std::wstring& labelKey);
	Ling::Button* makeSelectBtn(Ling::Node* row, float width,
		const std::vector<std::wstring>& items, int cur, std::function<void(int)> onPick);
	void initModelCtrls();
	void initLangCtrls();
	// 模型目录：显示 <数据目录>\ocr 并提供「打开目录」，用户把新的 det/rec onnx
	// 放进 <数据目录>\ocr\<任意名>\ 就能在识别模型下拉里选到
	void initModelDirCtrl();
	// 选中一个内置代次而文件还没下全时，把文件下全（放后台线程，完事弹一句）。
	// 文件已全（用户自己放的 / 下过）直接返回，不弹东西
	void fetchModel(const std::wstring& name);
	// 识别语言那一行：选模型时可能要显隐，留个指针
	Ling::Node* langRow{ nullptr };
	// 后台下载线程只拿得到这个标志（对象随切页立即销毁，裸 this 用不得）。
	// 析构时放掉，回填那一支看到 false 就不碰任何成员
	std::shared_ptr<bool> fetchAlive{ std::make_shared<bool>(true) };
};
