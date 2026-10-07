#include "pch.h"
#include <shobjidl.h>
#include <cmath>
#include <algorithm>
#include "PinSource.h"
#include "Util.h"
#include "AnimImage.h"
#include "Win/WinPin.h"
#include "Win/WinTextPin.h"
#include "ShotHistory.h"

using Microsoft::WRL::ComPtr;

namespace {
	// 贴图落在主显示器中间。贴出来的图往往比屏幕小得多，不居中就不知道跑哪去了
	void placeCenter(const int w, const int h, int& x, int& y)
	{
		auto monitor = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
		MONITORINFO mi{ sizeof(MONITORINFO) };
		GetMonitorInfo(monitor, &mi);
		x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
		y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
	}

	void pin(std::vector<BYTE>& data, const int w, const int h)
	{
		int x{ 0 }, y{ 0 };
		placeCenter(w, h, x, y);
		WinPin::initFromData(x, y, w, h, data);
	}

	// GPU 上的位图不能直接 Map，拷到一块带 CPU_READ 的再逐行读回来。
	// mapped.pitch 按 GPU 行对齐、可能大于 w*4，这里紧缩成 WIC 和剪贴板都认的步长
	bool readBack(ID2D1DeviceContext* ctx, ID2D1Bitmap1* bmp, const D2D1_SIZE_U size,
		std::vector<BYTE>& out)
	{
		D2D1_BITMAP_PROPERTIES1 cpuProps{
			.pixelFormat{ bmp->GetPixelFormat() },
			.dpiX{ 96.0f }, .dpiY{ 96.0f },
			.bitmapOptions{ D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW }
		};
		ComPtr<ID2D1Bitmap1> cpuBmp;
		if (FAILED(ctx->CreateBitmap(size, nullptr, 0, &cpuProps, cpuBmp.GetAddressOf()))) return false;
		if (FAILED(cpuBmp->CopyFromBitmap(nullptr, bmp, nullptr))) return false;
		D2D1_MAPPED_RECT mapped{};
		if (FAILED(cpuBmp->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
		const UINT32 rowBytes = size.width * 4;
		out.resize((size_t)rowBytes * size.height);
		for (UINT32 row = 0; row < size.height; ++row)
		{
			CopyMemory(out.data() + (size_t)row * rowBytes,
				mapped.bits + (size_t)row * mapped.pitch, rowBytes);
		}
		cpuBmp->Unmap();
		return true;
	}

	// 把一段文字画到一块离屏位图上再读回像素。
	// 用 d2d->deviceContext 做离屏是安全的：SetTarget → BeginDraw → EndDraw → SetTarget(nullptr)
	// 在本函数内闭环，与 WinPin::getImagePixels 同一套做法
	bool renderText(const std::wstring& text, std::vector<BYTE>& out, int& w, int& h)
	{
		constexpr float fontSize{ 16.f }, padding{ 18.f }, maxTextW{ 520.f };
		auto d2d = Ling::D2D::get();
		auto ctx = d2d->deviceContext.Get();
		auto layout = Ling::D2D::makeTextLayout(text, fontSize, maxTextW);
		if (!layout) return false;
		DWRITE_TEXT_METRICS metrics{};
		if (FAILED(layout->GetMetrics(&metrics))) return false;
		w = (int)std::ceil(metrics.width) + (int)(padding * 2);
		h = (int)std::ceil(metrics.height) + (int)(padding * 2);
		D2D1_SIZE_U size{ (UINT32)w, (UINT32)h };

		D2D1_BITMAP_PROPERTIES1 props{
			.pixelFormat{ D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED) },
			.dpiX{ 96.0f }, .dpiY{ 96.0f },
			.bitmapOptions{ D2D1_BITMAP_OPTIONS_TARGET }
		};
		ComPtr<ID2D1Bitmap1> bmp;
		if (FAILED(ctx->CreateBitmap(size, nullptr, 0, &props, bmp.GetAddressOf()))) return false;
		ComPtr<ID2D1SolidColorBrush> brush;
		if (FAILED(ctx->CreateSolidColorBrush(D2D1::ColorF(0x333333), brush.GetAddressOf()))) return false;

		ctx->SetTarget(bmp.Get());
		ctx->SetTransform(D2D1::Matrix3x2F::Identity());
		ctx->BeginDraw();
		ctx->Clear(D2D1::ColorF(D2D1::ColorF::White));
		ctx->DrawTextLayout({ padding, padding }, layout.Get(), brush.Get());
		auto hr = ctx->EndDraw();
		ctx->SetTarget(nullptr);
		if (FAILED(hr)) return false;
		return readBack(ctx, bmp.Get(), size, out);
	}

	// #RGB / #RRGGBB / #RRGGBBAA，返回 0xRRGGBBAA（与 Ling::Color 同一套通道序）
	bool parseColor(const std::wstring& s, uint32_t& rgba)
	{
		if (s.empty() || s[0] != L'#') return false;
		auto hex = [](wchar_t c) -> int {
			if (c >= L'0' && c <= L'9') return c - L'0';
			if (c >= L'a' && c <= L'f') return c - L'a' + 10;
			if (c >= L'A' && c <= L'F') return c - L'A' + 10;
			return -1;
			};
		auto digits = s.substr(1);
		if (digits.size() == 3) {
			for (auto c : digits) if (hex(c) < 0) return false;
			rgba = 0;
			for (auto c : digits) rgba = (rgba << 8) | (hex(c) * 17);
			rgba = (rgba << 8) | 0xFF;
			return true;
		}
		if (digits.size() == 6 || digits.size() == 8) {
			for (auto c : digits) if (hex(c) < 0) return false;
			rgba = 0;
			for (auto c : digits) rgba = (rgba << 4) | hex(c);
			if (digits.size() == 6) rgba = (rgba << 8) | 0xFF;
			return true;
		}
		return false;
	}

	// 读文本文件：先认 BOM（UTF-8 / UTF-16 LE / UTF-16 BE），没有 BOM 就先按 UTF-8 严格解析，
	// 解析不过再退回系统默认代码页（中文 Windows 上是 GBK）。过大（>8MB）不读，避免把巨型
	// 日志 / 打包文件塞进一个编辑框
	bool readTextFile(const std::wstring& path, std::wstring& out)
	{
		HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) return false;
		LARGE_INTEGER sz{};
		if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return false; }
		if (sz.QuadPart == 0) { out.clear(); CloseHandle(h); return true; }
		if (sz.QuadPart > 8 * 1024 * 1024) { CloseHandle(h); return false; }
		std::vector<BYTE> buf((size_t)sz.QuadPart);
		DWORD rd = 0;
		if (!ReadFile(h, buf.data(), (DWORD)sz.QuadPart, &rd, nullptr)) { CloseHandle(h); return false; }
		CloseHandle(h);
		auto toWide = [&](UINT cp, DWORD flags) -> bool {
			int n = MultiByteToWideChar(cp, flags, (char*)buf.data(), (int)buf.size(), nullptr, 0);
			if (n <= 0) return false;
			out.resize((size_t)n);
			return MultiByteToWideChar(cp, flags, (char*)buf.data(), (int)buf.size(), out.data(), n) > 0;
		};
		if (buf.size() >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF)
			return toWide(CP_UTF8, 0);
		if (buf.size() >= 2 && buf[0] == 0xFF && buf[1] == 0xFE) {
			size_t n = (buf.size() - 2) / 2; out.resize(n);
			for (size_t i = 0; i < n; ++i) out[i] = (wchar_t)(buf[2 + 2 * i] | (buf[2 + 2 * i + 1] << 8));
			return true;
		}
		if (buf.size() >= 2 && buf[0] == 0xFE && buf[1] == 0xFF) {
			size_t n = (buf.size() - 2) / 2; out.resize(n);
			for (size_t i = 0; i < n; ++i) out[i] = (wchar_t)((buf[2 + 2 * i] << 8) | buf[2 + 2 * i + 1]);
			return true;
		}
		if (toWide(CP_UTF8, MB_ERR_INVALID_CHARS)) return true;  // 无 BOM 且是合法 UTF-8
		return toWide(CP_ACP, 0);                                // 否则按系统代码页（GBK…）
	}
}

void PinSource::fromClipboard()
{
	std::vector<BYTE> img;
	int w{ 0 }, h{ 0 };
	std::wstring text;
	auto kind = Util::readClipboard(img, w, h, text);
	if (kind == Util::ClipContent::Image) {
		pin(img, w, h);
		return;
	}
	if (kind != Util::ClipContent::Text || text.empty()) return;
	// 剪贴板里是文本就一律贴成文本 —— 连 "复制来的颜色值"（取色器 / CSS 里拷的 #rrggbb、
	// rgb(...)）也不例外：以前那种会贴成一块色，可那块色改不了字面值，用户想拿回原始文本
	// 反而没地方取。贴成可编辑文本钉窗之后，颜色字符串照样看得见、选得中、复制得走
	WinTextPin::init(text);
}

void PinSource::fromFile(HWND hwnd)
{
	ComPtr<IFileDialog> dialog;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(dialog.GetAddressOf())))) return;
	COMDLG_FILTERSPEC filters[]{
		{ L"图片", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tif;*.tiff;*.ico" }
	};
	dialog->SetFileTypes(1, filters);
	DWORD flags{ 0 };
	dialog->GetOptions(&flags);
	dialog->SetOptions(flags | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
	if (FAILED(dialog->Show(hwnd))) return;
	ComPtr<IShellItem> item;
	if (FAILED(dialog->GetResult(item.GetAddressOf()))) return;
	PWSTR rawPath{ nullptr };
	if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath))) return;
	std::wstring path{ rawPath };
	CoTaskMemFree(rawPath);
	fromPath(path);
}

void PinSource::fromPath(const std::wstring& path)
{
	if (path.empty()) return;
	// 文本文件不再烤成一张死图，而是开一扇可编辑的文本钉窗
	if (isTextFile(path)) { fromTextFile(path); return; }
	// 动图走另一条路：解出帧序列交给贴图窗口自己播，静态图才合成一块像素
	std::vector<AnimFrame> frames;
	if (AnimImage::load(path, frames)) {
		int px{ 0 }, py{ 0 };
		placeCenter((int)frames[0].w, (int)frames[0].h, px, py);
		WinPin::initFromAnim(px, py, path, frames);
		return;
	}
	std::vector<BYTE> data;
	DWORD w{ 0 }, h{ 0 };
	if (!Util::loadImageBytes(path, data, w, h)) return;
	pin(data, (int)w, (int)h);
}

void PinSource::fromText(const std::wstring& text)
{
	if (text.empty()) return;
	std::vector<BYTE> data;
	int w{ 0 }, h{ 0 };
	if (!renderText(text, data, w, h)) return;
	pin(data, w, h);
}

bool PinSource::isTextFile(const std::wstring& path)
{
	auto dot = path.rfind(L'.');
	if (dot == std::wstring::npos) return false;
	std::wstring ext = path.substr(dot + 1);
	std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
	static const std::wstring_view kExts[] = {
		L"txt", L"md", L"markdown", L"log", L"csv", L"tsv", L"json", L"xml",
		L"html", L"htm", L"ini", L"yaml", L"yml", L"toml", L"cfg", L"conf",
		L"cpp", L"cxx", L"cc", L"c", L"h", L"hpp", L"hxx",
		L"py", L"js", L"jsx", L"ts", L"tsx", L"java", L"go", L"rs", L"cs",
		L"sh", L"bat", L"cmd", L"ps1", L"sql", L"tex", L"rtf",
		L"gitignore", L"editorconfig", L"properties"
	};
	for (auto e : kExts) if (ext == e) return true;
	return false;
}

void PinSource::fromTextFile(const std::wstring& path)
{
	if (path.empty()) return;
	std::wstring text;
	if (!readTextFile(path, text)) return;
	WinTextPin::init(text);
}

void PinSource::fromColor(const std::wstring& color)
{
	uint32_t rgba{ 0 };
	if (!parseColor(color, rgba)) return;
	// 转成 Ling::Color 再取通道，省得自己去数 rgba 的位移 —— 那个顺序是 0xRRGGBBAA，容易记反
	auto c = Ling::Color(rgba).getD2DColor();
	const int w{ 240 }, h{ 160 };
	std::vector<BYTE> data((size_t)w * h * 4);
	for (size_t i = 0; i < (size_t)w * h; ++i)
	{
		data[i * 4 + 0] = (BYTE)std::lround(c.b * 255);
		data[i * 4 + 1] = (BYTE)std::lround(c.g * 255);
		data[i * 4 + 2] = (BYTE)std::lround(c.r * 255);
		data[i * 4 + 3] = (BYTE)std::lround(c.a * 255);
	}
	pin(data, w, h);
}

namespace {
	// 「依次贴」的记账：连着贴到第几张、上次是什么时候、上一张贴在了哪。
	// 截图历史与剪贴板历史各一份，两条线互不干扰
	struct PinRun
	{
		// 下一条取第几张（1 起数）。停手超过 2 秒就当是新的一轮，从第二新那张开始
		int nextIndex(const int count)
		{
			auto now = GetTickCount64();
			cursor = (now - lastAt > 2000) ? 1 : cursor + 1;
			lastAt = (long long)now;
			if (cursor > count) cursor = count;
			return cursor;
		}
		// 连着贴的那几张往右下错开一点，不偏移的话会严丝合缝叠在一起，看不出贴了几张
		void settle(int& x, int& y)
		{
			if (hasLast) {
				x = lastX + 24;
				y = lastY + 24;
			}
			hasLast = true;
			lastX = x;
			lastY = y;
		}
		int cursor{ 0 };
		long long lastAt{ 0 };
		int lastX{ 0 }, lastY{ 0 };
		bool hasLast{ false };
	};
	PinRun shotRun, clipRun;
}

void PinSource::pinNextOlder()
{
	auto shots = ShotHistory::get();
	if (!shots) return;
	auto list = shots->list(ShotHistory::Source::Shot);
	if (list.empty()) return;
	std::vector<BYTE> data;
	int w{ 0 }, h{ 0 };
	if (!shots->loadImage(list[shotRun.nextIndex((int)list.size()) - 1], data, w, h)) return;
	int x{ 0 }, y{ 0 };
	placeCenter(w, h, x, y);
	shotRun.settle(x, y);
	WinPin::initFromData(x, y, w, h, data);
}

void PinSource::pinNextOlderClip()
{
	auto shots = ShotHistory::get();
	if (!shots) return;
	auto list = shots->list(ShotHistory::Source::Clipboard);
	if (list.empty()) return;
	auto& item = list[clipRun.nextIndex((int)list.size()) - 1];
	int x{ 0 }, y{ 0 };
	std::vector<BYTE> data;
	int w{ 0 }, h{ 0 };
	if (item.isText) {
		// 历史里的文本条目存的是字不是像素，跟"从剪贴板贴图"贴文本时一样现渲染一张
		if (!renderText(item.text, data, w, h)) return;
	}
	else if (!shots->loadImage(item, data, w, h)) return;
	placeCenter(w, h, x, y);
	clipRun.settle(x, y);
	WinPin::initFromData(x, y, w, h, data);
}
