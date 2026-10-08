#pragma once
#include <string>
#include <vector>

// 悬浮球展开条上的操作项。id 是落盘用的稳定键（改 id 等于把老配置作废），
// icon 是 iconfont 的码位，nameKey 是语言键 —— 图标上不写名字，鼠标停上去才出提示。
// 表里的顺序就是展开条上的排列顺序
struct BallActionDef
{
	const wchar_t* id;
	const wchar_t* icon;
	const wchar_t* nameKey;
};

// 全部可选项。设置页照着它逐行生成勾选框，改这里的顺序就是改设置页里的顺序
const std::vector<BallActionDef>& ballActionDefs();
// 第一次装出来摆哪几项。挑的是最高频的那几个 —— 全摆上的话展开条会拖出屏幕
std::wstring ballDefaultActions();
// 给老配置补齐后加的操作项。ballDefaultActions() 只管"还没有这个键"的全新配置，
// 老配置走自己存的那一串（toolPin.ball.actions），新加的操作项不会自己冒出来。
// 想让某一项对老用户也生效，把它的 id 加进这里的名单，启动时补一次。
void ballEnsureNewActions();
// "cap,long,delay" <-> id 列表。认不出来的 id 直接丢掉，配置被手工改坏时不该带崩界面
std::vector<std::wstring> ballParseActions(const std::wstring& raw);
std::wstring ballJoinActions(const std::vector<std::wstring>& ids);
// 按 id 找定义，找不到返回 nullptr
const BallActionDef* ballFindAction(const std::wstring& id);
