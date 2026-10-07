#pragma once
#include <include/Ling.h>
class WinSettingShortcut :public Ling::Node
{
public:
	WinSettingShortcut(Ling::WinBase* parent);
	~WinSettingShortcut();
private:
	void onBtnClick(Ling::Button* btn);
	void beginCapture(Ling::Button* btn);
	void endCapture();
	void onKeyDown(UINT key);
	void onKeyUp(UINT key);
	// 录键过程中把已按下的键实时显示到那个按钮上
	void updateCaptureText();
	std::wstring keyToStr(UINT vkCode);
private:
	std::vector<Ling::Button*> btns;
	std::wstring curKey;
	winrt::event_token onMouseDownToken, onKeyDownToken, onKeyUpToken;
	std::vector<std::wstring> tempKeys;
};

