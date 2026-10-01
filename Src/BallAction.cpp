#include "pch.h"
#include "BallAction.h"

namespace {
	const std::vector<BallActionDef> defs{
		{ L"cap",          L"\ue8e8", L"ball.cap" },
		{ L"long",         L"\ue73e", L"ball.long" },
		{ L"delay",        L"\ue900", L"ball.delay" },
		{ L"video",        L"\ue660", L"ball.video" },
		{ L"ocr",          L"\ue67b", L"ball.ocr" },
		{ L"qr",           L"\ue71e", L"ball.qr" },
		{ L"pinDirect",    L"\ue6a2", L"ball.pinDirect" },
		{ L"pinClip",      L"\ue901", L"ball.pinClip" },
		{ L"pinFile",      L"\ue608", L"ball.pinFile" },
		{ L"pinOlder",     L"\ue907", L"ball.pinOlder" },
		{ L"pinClipOlder", L"\ue908", L"ball.pinClipOlder" },
		{ L"history",      L"\ue902", L"ball.history" },
		{ L"ruler",        L"\ue903", L"ball.ruler" },
		{ L"crosshair",    L"\ue904", L"ball.crosshair" },
		{ L"focus",        L"\ue905", L"ball.focus" },
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
	return L"cap,long,delay,video,ocr,pinClip,pinFile,history";
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
