#include "pch.h"
#include <algorithm>
#include <iterator>
#include "BallAction.h"
#include "Setting.h"

namespace {
	// 后加进来、要替老配置补上的操作项。
	// 「翻译」「AI 对话」是明确要求放到球上的，但老配置里存的那一串里没有它们 ——
	// 不补的话设置页能看见（未勾选），球上却永远不出现。
	const std::wstring migrateActions[]{ L"aiTrans", L"aiChat" };

	// 表顺序 = 展开条上的排列顺序，也是设置页里勾选列表的顺序。
	// icon 必须在 Res/iconfont.ttf 里真的存在，否则按钮上是一个方块（由 Doc/tools/mkicons.py 生成）：
	//   e90c 相框（图片）   e90d 回旋箭头（历史）   e90e 相框+下箭头   e90f 剪贴板+下箭头
	//   e910 地球（翻译）   e911 对话气泡（AI 对话）
	const std::vector<BallActionDef> defs{
		{ L"cap",          L"\ue8e8", L"ball.cap" },
		{ L"long",         L"\ue73e", L"ball.long" },
		{ L"delay",        L"\ue900", L"ball.delay" },
		{ L"video",        L"\ue660", L"ball.video" },
		{ L"ocr",          L"\ue67b", L"ball.ocr" },
		{ L"qr",           L"\ue71e", L"ball.qr" },
		{ L"pinDirect",    L"\ue6a2", L"ball.pinDirect" },
		{ L"pinClip",      L"\ue901", L"ball.pinClip" },
		// 「从图片文件贴图」原先是"箭头落进托盘"（e608），那画的是**保存**图片，
		// 和"贴一张图上来"正好反着 —— 换成相框（带山与太阳）就是"一张图片"本身
		{ L"pinFile",      L"\ue90c", L"ball.pinFile" },
		// 下面两个原来都是"一张空卡片 / 剪贴板 + 同一根下箭头"，缩到 17 像素基本分不出来。
		// 主体拉开就行：截图那边换成相框（图），剪贴板那边保留夹子 —— 箭头仍共用，
		// 它表达的是"依次往下叠"这层共同语义
		{ L"pinOlder",     L"\ue90e", L"ball.pinOlder" },
		{ L"pinClipOlder", L"\ue90f", L"ball.pinClipOlder" },
		// 「历史记录」原先是两张错位的卡片 = 复制（和 AiChat 里的复制图标一模一样）。
		// 换成回旋箭头：一圈缺口 + 箭头，是"往前翻记录"的通用写法，也不会和时钟撞
		{ L"history",      L"\ue90d", L"ball.history" },
		{ L"ruler",        L"\ue903", L"ball.ruler" },
		{ L"crosshair",    L"\ue904", L"ball.crosshair" },
		{ L"focus",        L"\ue905", L"ball.focus" },
		{ L"aiTrans",      L"\ue910", L"ball.aiTrans" },
		{ L"aiChat",       L"\ue911", L"ball.aiChat" },
		{ L"setting",      L"\ue906", L"ball.setting" },
		{ L"hide",         L"\ue62d", L"ball.hide" },
	};
}

const std::vector<BallActionDef>& ballActionDefs()
{
	return defs;
}

std::wstring ballDefaultActions()
{
	// 顺序照 ballActionDefs 的定义表写：togglePick 会按表重排，写乱顺序也走不出别的排列。
	// 只影响"还没有这个键"的全新配置；老配置照自己存的那一串走
	return L"cap,long,delay,video,ocr,pinClip,pinFile,history,aiTrans,aiChat,setting";
}

std::vector<std::wstring> ballParseActions(const std::wstring& raw)
{
	std::vector<std::wstring> ids;
	size_t pos{ 0 };
	while (pos <= raw.size())
	{
		auto end = raw.find(L',', pos);
		if (end == std::wstring::npos) end = raw.size();
		auto id = raw.substr(pos, end - pos);
		// 认不出来的键丢掉：配置可能是手改的，也可能是删掉了某个操作项的旧版本留下的。
		// 顺手去个重，重复勾上会在展开条里出现两个一样的图标
		if (ballFindAction(id) && std::find(ids.begin(), ids.end(), id) == ids.end()) {
			ids.push_back(id);
		}
		if (end == raw.size()) break;
		pos = end + 1;
	}
	return ids;
}

void ballEnsureNewActions()
{
	auto setting = Setting::get();
	if (!setting) return;
	auto ids = ballParseActions(setting->getToolStr(L"ball", L"actions", ballDefaultActions()));
	// 每一项都插在 setting 前面。必须**正序**走：两轮都插在同一个锚点上时，
	// 后插的那一项落在更靠近锚点的位置，倒序插会得到 aiChat,aiTrans 的反向顺序
	bool changed{ false };
	for (const auto& id : migrateActions) {
		if (std::find(ids.begin(), ids.end(), id) != ids.end()) continue;
		ids.insert(std::find(ids.begin(), ids.end(), L"setting"), id);
		changed = true;
	}
	// 只有真的补进去了才写盘：否则每次启动都会覆盖一遍用户的配置。
	// 补完之后那两项就在存的那串里了，往后这一句不再成立 —— 用户手动取消勾选也不会被加回来
	if (changed) setting->setToolStr(L"ball", L"actions", ballJoinActions(ids));
}

std::wstring ballJoinActions(const std::vector<std::wstring>& ids)
{
	std::wstring out;
	for (auto& id : ids) {
		if (!out.empty()) out += L',';
		out += id;
	}
	return out;
}

const BallActionDef* ballFindAction(const std::wstring& id)
{
	for (auto& def : defs) {
		if (id == def.id) return &def;
	}
	return nullptr;
}
