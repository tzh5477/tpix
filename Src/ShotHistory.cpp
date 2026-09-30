#include "pch.h"
#include <winrt/Windows.Data.Json.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <ranges>
#include "ShotHistory.h"
#include "Setting.h"
#include "Util.h"

using namespace winrt::Windows::Data::Json;

namespace {
	std::unique_ptr<ShotHistory> shotHistory;
	// 缩略图的宽度。高度按原图比例算，不硬编码 —— 长条截图压成方的认不出来
	constexpr int thumbW{ 200 };

	long long nowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	}

	// 剪贴板监听窗口的消息处理。只认 WM_CLIPBOARDUPDATE，其余交给默认过程
	LRESULT CALLBACK clipWinProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (msg == WM_CLIPBOARDUPDATE) {
			auto self = reinterpret_cast<ShotHistory*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
			if (self) self->onClipboardUpdate();
			return 0;
		}
		return DefWindowProc(hwnd, msg, wParam, lParam);
	}

}

void ShotHistory::init()
{
	auto ptr = new ShotHistory();
	shotHistory.reset(ptr);
}

void ShotHistory::dispose()
{
	shotHistory.reset();
}

ShotHistory* ShotHistory::get()
{
	return shotHistory.get();
}

ShotHistory::ShotHistory()
{
	dir = Setting::get()->getDataPath() / L"history";
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	load();
	initClipboardListener();
}

ShotHistory::~ShotHistory()
{
	if (!clipHwnd) return;
	RemoveClipboardFormatListener(clipHwnd);
	DestroyWindow(clipHwnd);
}

void ShotHistory::initClipboardListener()
{
	// STATIC 是系统预置窗口类，不用注册就能建；HWND_MESSAGE 让它只收消息不参与显示
	clipHwnd = CreateWindow(L"STATIC", nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
	if (!clipHwnd) return;
	SetWindowLongPtr(clipHwnd, GWLP_WNDPROC, (LONG_PTR)clipWinProc);
	SetWindowLongPtr(clipHwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
	AddClipboardFormatListener(clipHwnd);
}

void ShotHistory::onClipboardUpdate()
{
	if (skipOnce) {
		skipOnce = false;
		return;
	}
	if (!Setting::get()->getClipboardHistory()) return;
	// 剪贴板的读法（PNG 优先、DIB 回退、最后拿文本）统一在 Util 里，贴图来源也走那一条
	std::vector<BYTE> data;
	int w{ 0 }, h{ 0 };
	std::wstring text;
	switch (Util::readClipboard(data, w, h, text))
	{
	case Util::ClipContent::Image:
		addImage(Source::Clipboard, w, h, data.data());
		break;
	case Util::ClipContent::Text:
		addText(Source::Clipboard, text);
		break;
	default:
		break;
	}
}

void ShotHistory::skipNextClipboard()
{
	skipOnce = true;
}

void ShotHistory::addImage(Source source, const int w, const int h, BYTE* data)
{
	if (w <= 0 || h <= 0 || !data) return;
	Item item;
	item.id = std::format(L"{}_{}", nowMs(), seq++);
	item.source = source;
	item.time = nowMs();
	item.w = w;
	item.h = h;
	item.file = item.id + L".png";
	item.thumb = item.id + L"_t.png";
	if (!Util::saveToFile(imagePath(item), w, h, data)) return;
	// 缩略图只在比原图小的时候才有意义：本来就不大的图再存一份是白占地方
	if (w > thumbW) {
		auto thumbH = std::max(1, (int)std::lround((float)h * thumbW / w));
		std::vector<BYTE> thumbData;
		if (Util::resizeBGRA(w, h, data, thumbW, thumbH, thumbData)) {
			Util::saveToFile(thumbPath(item), thumbW, thumbH, thumbData.data());
		}
	}
	items.push_back(item);
	trim(source);
	save();
}

void ShotHistory::addText(Source source, const std::wstring& text)
{
	if (text.empty()) return;
	// 连着复制同一段文字不重复记：按住 Ctrl+C 不放、或者某些程序会连发两次更新
	if (!items.empty()) {
		auto& last = items.back();
		if (last.source == source && last.isText && last.text == text) return;
	}
	Item item;
	item.id = std::format(L"{}_{}", nowMs(), seq++);
	item.source = source;
	item.time = nowMs();
	item.isText = true;
	item.text = text;
	items.push_back(item);
	trim(source);
	save();
}

void ShotHistory::removeById(const std::wstring& id)
{
	for (auto it = items.begin(); it != items.end(); ++it)
	{
		if (it->id != id) continue;
		removeFiles(*it);
		items.erase(it);
		save();
		return;
	}
}

void ShotHistory::clear(Source source)
{
	for (auto& item : items)
	{
		if (item.source == source) removeFiles(item);
	}
	std::erase_if(items, [source](const Item& item) { return item.source == source; });
	save();
}

std::vector<ShotHistory::Item> ShotHistory::list(Source source) const
{
	std::vector<Item> result;
	for (auto& item : items) {
		if (item.source == source) result.push_back(item);
	}
	std::ranges::sort(result, [](const Item& a, const Item& b) { return a.time > b.time; });
	return result;
}

std::wstring ShotHistory::imagePath(const Item& item) const
{
	if (item.file.empty()) return {};
	return (dir / item.file).wstring();
}

std::wstring ShotHistory::thumbPath(const Item& item) const
{
	if (item.thumb.empty()) return {};
	return (dir / item.thumb).wstring();
}

bool ShotHistory::loadImage(const Item& item, std::vector<BYTE>& data, int& w, int& h)
{
	auto path = imagePath(item);
	if (path.empty()) return false;
	std::vector<BYTE> bytes;
	DWORD imgW{ 0 }, imgH{ 0 };
	if (!Util::loadImageBytes(path, bytes, imgW, imgH)) return false;
	data = std::move(bytes);
	w = (int)imgW;
	h = (int)imgH;
	return true;
}

void ShotHistory::trim(Source source)
{
	auto limit = Setting::get()->getHistoryLimit();
	int count{ 0 };
	for (auto& item : items) {
		if (item.source == source) count++;
	}
	// 同类条目按时间升序，从最旧那条开始删
	while (count > limit)
	{
		auto oldest = items.end();
		for (auto it = items.begin(); it != items.end(); ++it) {
			if (it->source != source) continue;
			if (oldest == items.end() || it->time < oldest->time) oldest = it;
		}
		if (oldest == items.end()) break;
		removeFiles(*oldest);
		items.erase(oldest);
		count--;
	}
}

void ShotHistory::removeFiles(const Item& item)
{
	std::error_code ec;
	auto img = imagePath(item);
	if (!img.empty()) std::filesystem::remove(img, ec);
	auto thumb = thumbPath(item);
	if (!thumb.empty()) std::filesystem::remove(thumb, ec);
}

void ShotHistory::load()
{
	auto indexPath = dir / L"index.json";
	if (!std::filesystem::exists(indexPath)) return;
	JsonObject obj{ nullptr };
	if (!JsonObject::TryParse(Ling::Util::readFileText(indexPath), obj)) return;
	auto arr = obj.GetNamedArray(L"items", nullptr);
	if (!arr) return;
	for (auto val : arr)
	{
		auto itemObj = val.GetObject();
		Item item;
		item.id = std::wstring{ itemObj.GetNamedString(L"id", L"") };
		if (item.id.empty()) continue;
		item.source = (Source)(int)itemObj.GetNamedNumber(L"source", 0.0);
		item.time = (long long)itemObj.GetNamedNumber(L"time", 0.0);
		item.isText = itemObj.GetNamedBoolean(L"isText", false);
		item.text = std::wstring{ itemObj.GetNamedString(L"text", L"") };
		item.file = std::wstring{ itemObj.GetNamedString(L"file", L"") };
		item.thumb = std::wstring{ itemObj.GetNamedString(L"thumb", L"") };
		item.w = (int)itemObj.GetNamedNumber(L"w", 0.0);
		item.h = (int)itemObj.GetNamedNumber(L"h", 0.0);
		items.push_back(item);
	}
}

void ShotHistory::save()
{
	JsonArray arr;
	for (auto& item : items)
	{
		JsonObject obj;
		obj.SetNamedValue(L"id", JsonValue::CreateStringValue(item.id));
		obj.SetNamedValue(L"source", JsonValue::CreateNumberValue((double)item.source));
		obj.SetNamedValue(L"time", JsonValue::CreateNumberValue((double)item.time));
		obj.SetNamedValue(L"isText", JsonValue::CreateBooleanValue(item.isText));
		obj.SetNamedValue(L"text", JsonValue::CreateStringValue(item.text));
		obj.SetNamedValue(L"file", JsonValue::CreateStringValue(item.file));
		obj.SetNamedValue(L"thumb", JsonValue::CreateStringValue(item.thumb));
		obj.SetNamedValue(L"w", JsonValue::CreateNumberValue((double)item.w));
		obj.SetNamedValue(L"h", JsonValue::CreateNumberValue((double)item.h));
		arr.Append(obj);
	}
	JsonObject root;
	root.SetNamedValue(L"items", arr);
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	Ling::Util::saveFile((dir / L"index.json").wstring(), std::wstring{ root.Stringify() });
}
