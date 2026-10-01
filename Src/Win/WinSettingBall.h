#pragma once
#include <include/Ling.h>
#include <string>
#include <vector>
#include "../BallAction.h"

// 设置页的「悬浮球」分组：开关悬浮球本身，勾选展开条上摆哪几个操作项。
// 悬浮球的其余参数（贴哪条边、延时几秒、滚动方向）都在悬浮球自己身上点右键调 ——
// 那些是"用的时候顺手改"的东西，塞进设置页反而绕远
class WinSettingBall : public Ling::Node
{
public:
	WinSettingBall(Ling::WinBase* parent);
	~WinSettingBall();
private:
	// 一行「标签 + 控件」。生成的行节点作为返回值交给调用方塞控件，分隔线是本节点的
	// 子节点而不是行内的，必须在下一行入列之前加好，所以顺手在这里加掉
	Ling::Node* makeRow(const std::wstring& labelKey);
	// 一行「图标 + 名称 + 勾选框」
	Ling::Node* makeActionRow(const BallActionDef& def);
	void initShowCtrl();
	void initActionCtrls();
	void applySwitch(Ling::Button* btn, bool on);
	bool isPicked(const std::wstring& id) const;
	void togglePick(const std::wstring& id);
private:
	std::vector<std::wstring> picked;
};
