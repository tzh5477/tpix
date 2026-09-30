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

	// 把 DIB（CF_DIB / CF_DIBV5 给的那块内存）转成 BGRA top-down。
	// 只处理 BI_RGB 与 BI_BITFIELDS 的 24/32 bpp —— 剪贴板里出现的 4/8 bpp 调色板图
	// 基本只来自老程序，认不出来就当没有图片，不强撑
	bool dibToBGRA(BYTE* dib, std::vector<BYTE>& out, DWORD& w, DWORD& h)
	{
		auto header = reinterpret_cast<BITMAPINFOHEADER*>(dib);
		if (header->biBitCount != 24 && header->biBitCount != 32) return false;
		if (header->biCompression != BI_RGB && header->biCompression != BI_BITFIELDS) return false;
		const int iw = header->biWidth;
		const int ih = std::abs(header->biHeight);
		if (iw <= 0 || ih <= 0) return false;
		const bool bottomUp = header->biHeight > 0;
		const int bpp = header->biBitCount / 8;
		// 行按 4 字节对齐，这是 DIB 的规矩，与我们的行紧凑布局不一样
		const int srcPitch = ((iw * bpp + 3) / 4) * 4;
		UINT32 maskR{ 0x00FF0000 }, maskG{ 0x0000FF00 }, maskB{ 0x000000FF }, maskA{ 0 };
		if (header->biCompression == BI_BITFIELDS) {
			// BITMAPV5HEADER 的掩码就排在头后面；BITMAPINFOHEADER 带 BI_BITFIELDS 时同样如此
			auto masks = reinterpret_cast<UINT32*>(dib + sizeof(BITMAPINFOHEADER));
			maskR = masks[0]; maskG = masks[1]; maskB = masks[2];
			if (header->biSize >= sizeof(BITMAPV5HEADER)) {
				maskA = reinterpret_cast<BITMAPV5HEADER*>(dib)->bV5AlphaMask;
			}
		}
		auto shift = [](UINT32 mask) -> int {
			int n{ 0 };
			while (mask && (mask & 1) == 0) { mask >>= 1; n++; }
			return n;
		};
		const int sR = shift(maskR), sG = shift(maskG), sB = shift(maskB), sA = shift(maskA);
		out.assign((size_t)iw * ih * 4, 0);
		for (int y = 0; y < ih; ++y)
		{
			auto sy = bottomUp ? (ih - 1 - y) : y;
			auto srcRow = dib + header->biSize + (size_t)sy * srcPitch;
			auto dstRow = out.data() + (size_t)y * iw * 4;
			for (int x = 0; x < iw; ++x)
			{
				UINT32 px{ 0 };
				memcpy(&px, srcRow + x * bpp, bpp);
				UINT32 r = (px & maskR) >> sR;
				UINT32 g = (px & maskG) >> sG;
				UINT32 b = (px & maskB) >> sB;
				// 掩码位数可能不足 8（比如 565），按比例拉回 0–255，否则颜色整体偏暗
				auto norm = [](UINT32 v, UINT32 mask) -> UINT32 {
					if (mask == 0) return 0;
					int bits{ 0 };
					for (UINT32 m = mask; m; m >>= 1) bits++;
					if (bits >= 8) return v & 0xFF;
					auto maxV = (1u << bits) - 1;
					return (v * 255 + maxV / 2) / maxV;
				};
				r = norm(r, maskR); g = norm(g, maskG); b = norm(b, maskB);
				dstRow[x * 4 + 0] = (BYTE)b;
				dstRow[x * 4 + 1] = (BYTE)g;
				dstRow[x * 4 + 2] = (BYTE)r;
				// 32bpp 且没给 alpha 掩码时，那 8 位大多是不透明，按不透明处理 ——
				// 照掩码算出来是 0，整张图会变全透明
				dstRow[x * 4 + 3] = (BYTE)(maskA ? (px & maskA) >> sA : 0xFF);
			}
		}
		w = (DWORD)iw;
		h = (DWORD)ih;
		return true;
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
	if (!OpenClipboard(nullptr)) return;
	// 优先拿 PNG：浏览器和不少现代程序放的就是它，alpha 也保得住
	std::vector<BYTE> data;
	DWORD w{ 0 }, h{ 0 };
	auto cfPng = RegisterClipboardFormatW(L"PNG");
	bool got{ false };
	if (cfPng != 0) {
		auto handle = GetClipboardData(cfPng);
		if (handle) {
			auto size = GlobalSize(handle);
			auto buf = reinterpret_cast<BYTE*>(GlobalLock(handle));
			if (buf && size) {
				got = Util::decodeImageBytes(buf, (DWORD)size, data, w, h);
			}
			if (buf) GlobalUnlock(handle);
		}
	}
	if (!got) {
		auto format = CF_DIBV5;
		auto handle = GetClipboardData(format);
		if (!handle) {
			format = CF_DIB;
			handle = GetClipboardData(format);
		}
		if (handle) {
			auto buf = reinterpret_cast<BYTE*>(GlobalLock(handle));
			if (buf) got = dibToBGRA(buf, data, w, h);
			if (buf) GlobalUnlock(handle);
		}
	}
	if (got) {
		addImage(Source::Clipboard, (int)w, (int)h, data.data());
		CloseClipboard();
		return;
	}
	auto textHandle = GetClipboardData(CF_UNICODETEXT);
	if (textHandle) {
		auto buf = reinterpret_cast<wchar_t*>(GlobalLock(textHandle));
		if (buf) addText(Source::Clipboard, buf);
		if (buf) GlobalUnlock(textHandle);
	}
	CloseClipboard();
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
