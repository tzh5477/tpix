#include "pch.h"
#include <wincodec.h>
#include <shobjidl.h>
#include <algorithm>
#include <format>
#include <fstream>
#include "Util.h"
#include "Lang.h"
#include "Setting.h"
#include "quirc/quirc.h"

using Microsoft::WRL::ComPtr;

namespace {
	// 把 BGRA top-down 像素按指定格式编码写进 stream。saveToClipboard 和 saveToFile 共用这段。
	// 只有 JPEG 需要真正动像素：它是 24bpp 且不带 alpha，得把 A 通道丢掉重排一行。
	// WebP / PNG 都能直接吃 32bppBGRA，编码时给的也是同一份数据。
	bool encodeImage(IStream* stream, const int w, const int h, BYTE* data,
		const Util::ImgFormat format, const float quality = 90.f)
	{
		WICPixelFormatGUID requestFmt{ GUID_WICPixelFormat32bppBGRA };
		UINT rowBytes = (UINT)w * 4;
		std::vector<BYTE> converted;
		BYTE* src = data;
		if (format == Util::ImgFormat::Jpeg) {
			requestFmt = GUID_WICPixelFormat24bppBGR;
			rowBytes = (UINT)w * 3;
			converted.resize((size_t)rowBytes * (size_t)h);
			for (int y = 0; y < h; ++y) {
				BYTE* s = data + (size_t)y * (size_t)w * 4;
				BYTE* d = converted.data() + (size_t)y * rowBytes;
				for (int x = 0; x < w; ++x) {
					d[x * 3] = s[x * 4];
					d[x * 3 + 1] = s[x * 4 + 1];
					d[x * 3 + 2] = s[x * 4 + 2];
				}
			}
			src = converted.data();
		}
		UINT imgBytes = rowBytes * (UINT)h;
		ComPtr<IWICImagingFactory> factory;
		auto hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
		if (FAILED(hr)) return false;
		GUID containerFmt{ GUID_ContainerFormatPng };
		if (format == Util::ImgFormat::Jpeg) containerFmt = GUID_ContainerFormatJpeg;
		else if (format == Util::ImgFormat::WebP) containerFmt = GUID_ContainerFormatWebp;
		ComPtr<IWICBitmapEncoder> encoder;
		hr = factory->CreateEncoder(containerFmt, nullptr, encoder.GetAddressOf());
		if (FAILED(hr)) return false;
		hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
		if (FAILED(hr)) return false;
		ComPtr<IWICBitmapFrameEncode> frame;
		ComPtr<IPropertyBag2> encoderOptions;
		hr = encoder->CreateNewFrame(frame.GetAddressOf(), encoderOptions.GetAddressOf());
		if (FAILED(hr)) return false;
		// 有损格式才认 ImageQuality；PNG 不给这个选项，给了也是白给
		if (encoderOptions && format != Util::ImgFormat::Png) {
			PROPBAG2 option{};
			option.pstrName = const_cast<wchar_t*>(L"ImageQuality");
			VARIANT varQuality{};
			VariantInit(&varQuality);
			varQuality.vt = VT_R4;
			varQuality.fltVal = std::clamp(quality, 0.f, 100.f) / 100.f;
			// 写不进去就用编码器的默认值，不该因此让整次存盘失败
			encoderOptions->Write(1, &option, &varQuality);
		}
		hr = frame->Initialize(encoderOptions.Get());
		if (FAILED(hr)) return false;
		hr = frame->SetSize((UINT)w, (UINT)h);
		if (FAILED(hr)) return false;
		WICPixelFormatGUID fmt = requestFmt;
		hr = frame->SetPixelFormat(&fmt);
		// 编码器会把它真正接受的格式写回 fmt，与请求的不是同一个就说明这个格式它不吃
		if (FAILED(hr) || !IsEqualGUID(fmt, requestFmt)) return false;
		hr = frame->WritePixels((UINT)h, rowBytes, imgBytes, src);
		if (FAILED(hr)) return false;
		hr = frame->Commit();
		if (FAILED(hr)) return false;
		return SUCCEEDED(encoder->Commit());
	}

	// quirc 交出来的是裸字节流：BYTE 类型的二维码现实中基本都是 UTF-8（微信、支付宝
	// 生成的都是），Kanji 类型按 ISO 18004 规定是 Shift-JIS。所以先按 UTF-8 严格解，
	// 解不通再退回对应的本地代码页，避免把中文变成一堆问号
	std::wstring qrPayloadToWStr(const uint8_t* payload, const int len, const int dataType)
	{
		if (len <= 0) return L"";
		auto convert = [payload, len](UINT codePage, DWORD flags) {
			auto str = (const char*)payload;
			auto count = MultiByteToWideChar(codePage, flags, str, len, nullptr, 0);
			if (count <= 0) return std::wstring();
			std::wstring result(count, 0);
			MultiByteToWideChar(codePage, flags, str, len, result.data(), count);
			return result;
		};
		auto result = convert(CP_UTF8, MB_ERR_INVALID_CHARS);
		if (!result.empty()) return result;
		return convert(dataType == QUIRC_DATA_TYPE_KANJI ? 932 : CP_ACP, 0);
	}

	// 插件的查找顺序：先本 exe 同目录（绿色包一起解压的情况），
	// 再 %appdata%\ScreenCapture\plugin（后来单独下载的情况）
	std::filesystem::path findImageReader()
	{
		wchar_t buffer[MAX_PATH]{};
		GetModuleFileName(nullptr, buffer, MAX_PATH);
		auto path = std::filesystem::path{ buffer }.parent_path().append(L"ImageReader.exe");
		if (std::filesystem::exists(path)) return path;
		path = Setting::get()->getDataPath().append(L"plugin").append(L"ImageReader.exe");
		if (std::filesystem::exists(path)) return path;
		return {};
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

	// 静态图取第 0 帧，统一转成 BGRA top-down 行紧凑。源可能是灰度 / CMYK / BGR 之类，
	// 走一次格式转换，调用方拿到的永远是同一套布局
	bool decodeFrame(IWICBitmapDecoder* decoder, std::vector<BYTE>& out, DWORD& w, DWORD& h)
	{
		ComPtr<IWICBitmapFrameDecode> frame;
		if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return false;
		return Util::decodeWicFrame(frame.Get(), out, w, h);
	}
}

void Util::saveToClipboard(const int w, const int h, BYTE* data)
{
	if (w <= 0 || h <= 0 || !data) return;
	DWORD rowBytes = (DWORD)w * 4;
	DWORD imgBytes = rowBytes * (DWORD)h;

	// ---------- 1) PNG 编码到内存流 ----------
	ComPtr<IStream> pngStream;
	if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, pngStream.GetAddressOf()))) return;
	if (!encodeImage(pngStream.Get(), w, h, data, Util::ImgFormat::Png)) return;
	// 流内部的 HGLOBAL 尺寸可能大于实际字节数，拷一份精确大小的出来给剪切板
	STATSTG stat{};
	if (FAILED(pngStream->Stat(&stat, STATFLAG_NONAME))) return;
	SIZE_T pngSize = (SIZE_T)stat.cbSize.QuadPart;
	if (pngSize == 0) return;
	HGLOBAL hPngSrc{ nullptr };
	if (FAILED(GetHGlobalFromStream(pngStream.Get(), &hPngSrc)) || !hPngSrc) return;
	auto srcPtr = GlobalLock(hPngSrc);
	if (!srcPtr) return;
	HGLOBAL hPng = GlobalAlloc(GMEM_MOVEABLE, pngSize);
	if (!hPng) { GlobalUnlock(hPngSrc); return; }
	auto dstPtr = GlobalLock(hPng);
	if (!dstPtr) { GlobalUnlock(hPngSrc); GlobalFree(hPng); return; }
	CopyMemory(dstPtr, srcPtr, pngSize);
	GlobalUnlock(hPng);
	GlobalUnlock(hPngSrc);

	// ---------- 2) 构造 CF_DIBV5（带 alpha） ----------
	HGLOBAL hDibV5 = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + imgBytes);
	if (!hDibV5) { GlobalFree(hPng); return; }
	auto pv5 = static_cast<BYTE*>(GlobalLock(hDibV5));
	if (!pv5) { GlobalFree(hDibV5); GlobalFree(hPng); return; }
	auto bv5 = reinterpret_cast<BITMAPV5HEADER*>(pv5);
	*bv5 = {};
	bv5->bV5Size = sizeof(BITMAPV5HEADER);
	bv5->bV5Width = w;
	bv5->bV5Height = -h;                  // 负 = top-down
	bv5->bV5Planes = 1;
	bv5->bV5BitCount = 32;
	bv5->bV5Compression = BI_BITFIELDS;   // 让接收端识别 alpha
	bv5->bV5SizeImage = imgBytes;
	bv5->bV5RedMask = 0x00FF0000;
	bv5->bV5GreenMask = 0x0000FF00;
	bv5->bV5BlueMask = 0x000000FF;
	bv5->bV5AlphaMask = 0xFF000000;
	bv5->bV5CSType = LCS_sRGB;
	bv5->bV5Intent = LCS_GM_GRAPHICS;
	CopyMemory(pv5 + sizeof(BITMAPV5HEADER), data, imgBytes);
	GlobalUnlock(hDibV5);

	// ---------- 3) 构造 CF_DIB（24bpp、BI_RGB、自下而上） ----------
	// 老软件（比如 Illustrator 2020）只认最传统的这一种 DIB：注册格式 PNG 它不查，
	// CF_DIBV5 它不认，32bpp + BI_BITFIELDS 和 top-down 也读不了。系统虽然能从 CF_DIBV5
	// 合成出 CF_DIB，合成出来的仍是那份带 alpha 的 32 位数据，一样不合它的口味。
	// 所以显式再放一份最保守的：丢掉 alpha 写成 24 位，行按 4 字节对齐，自下而上排列
	DWORD dibRowBytes = ((DWORD)w * 3 + 3) & ~3u;
	DWORD dibImgBytes = dibRowBytes * (DWORD)h;
	HGLOBAL hDib = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + dibImgBytes);
	if (!hDib) { GlobalFree(hDibV5); GlobalFree(hPng); return; }
	auto pDib = static_cast<BYTE*>(GlobalLock(hDib));
	if (!pDib) { GlobalFree(hDib); GlobalFree(hDibV5); GlobalFree(hPng); return; }
	auto bi = reinterpret_cast<BITMAPINFOHEADER*>(pDib);
	*bi = {};
	bi->biSize = sizeof(BITMAPINFOHEADER);
	bi->biWidth = w;
	bi->biHeight = h;                     // 正 = 自下而上
	bi->biPlanes = 1;
	bi->biBitCount = 24;
	bi->biCompression = BI_RGB;
	bi->biSizeImage = dibImgBytes;
	auto dibPixels = pDib + sizeof(BITMAPINFOHEADER);
	for (int row = 0; row < h; row++) {
		auto src = data + (size_t)row * rowBytes;                 //入参是 top-down
		auto dst = dibPixels + (size_t)(h - 1 - row) * dibRowBytes;
		for (int col = 0; col < w; col++) {
			dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];     //BGRA -> BGR
			src += 4;
			dst += 3;
		}
	}
	GlobalUnlock(hDib);

	// ---------- 4) 写入剪切板 ----------
	if (!OpenClipboard(nullptr)) {
		GlobalFree(hDib);
		GlobalFree(hDibV5);
		GlobalFree(hPng);
		return;
	}
	EmptyClipboard();
	// SetClipboardData 成功后 HGLOBAL 归剪切板所有，不能再 GlobalFree；失败了才要自己释放
	if (!SetClipboardData(CF_DIBV5, hDibV5)) {
		GlobalFree(hDibV5);
	}
	if (!SetClipboardData(CF_DIB, hDib)) {
		GlobalFree(hDib);
	}
	UINT cfPng = RegisterClipboardFormatW(L"PNG");
	if (cfPng == 0 || !SetClipboardData(cfPng, hPng)) {
		GlobalFree(hPng);
	}
	CloseClipboard();
}

bool Util::loadImageBytes(const std::wstring& path, std::vector<BYTE>& out, DWORD& w, DWORD& h)
{
	ComPtr<IWICImagingFactory> factory;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(factory.GetAddressOf())))) return false;
	ComPtr<IWICBitmapDecoder> decoder;
	if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
		WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf()))) return false;
	return decodeFrame(decoder.Get(), out, w, h);
}

bool Util::decodeImageBytes(BYTE* buf, DWORD size, std::vector<BYTE>& out, DWORD& w, DWORD& h)
{
	if (!buf || size == 0) return false;
	ComPtr<IWICImagingFactory> factory;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(factory.GetAddressOf())))) return false;
	// 不用 SHCreateMemStream：那要额外链 shlwapi.lib，而工程里本来就有 CreateStreamOnHGlobal
	auto hGlobal = GlobalAlloc(GMEM_MOVEABLE, size);
	if (!hGlobal) return false;
	auto dst = GlobalLock(hGlobal);
	if (!dst) {
		GlobalFree(hGlobal);
		return false;
	}
	memcpy(dst, buf, size);
	GlobalUnlock(hGlobal);
	ComPtr<IStream> stream;
	if (FAILED(CreateStreamOnHGlobal(hGlobal, TRUE, stream.GetAddressOf()))) {
		GlobalFree(hGlobal);
		return false;
	}
	ComPtr<IWICBitmapDecoder> decoder;
	if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
		WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf()))) return false;
	return decodeFrame(decoder.Get(), out, w, h);
}

bool Util::decodeWicFrame(IWICBitmapFrameDecode* frame, std::vector<BYTE>& out, DWORD& w, DWORD& h)
{
	if (!frame) return false;
	ComPtr<IWICImagingFactory> factory;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(factory.GetAddressOf())))) return false;
	UINT fw{ 0 }, fh{ 0 };
	if (FAILED(frame->GetSize(&fw, &fh)) || fw == 0 || fh == 0) return false;
	ComPtr<IWICFormatConverter> converter;
	if (FAILED(factory->CreateFormatConverter(converter.GetAddressOf()))) return false;
	if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
		WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
	out.assign((size_t)fw * fh * 4, 0);
	if (FAILED(converter->CopyPixels(nullptr, fw * 4, (UINT)out.size(), out.data()))) return false;
	w = fw;
	h = fh;
	return true;
}

Util::ClipContent Util::readClipboard(std::vector<BYTE>& img, int& w, int& h, std::wstring& text)
{
	if (!OpenClipboard(nullptr)) return ClipContent::None;
	DWORD dw{ 0 }, dh{ 0 };
	bool got{ false };
	auto cfPng = RegisterClipboardFormatW(L"PNG");
	if (cfPng != 0) {
		auto handle = GetClipboardData(cfPng);
		if (handle) {
			auto buf = reinterpret_cast<BYTE*>(GlobalLock(handle));
			auto size = GlobalSize(handle);
			if (buf && size) got = decodeImageBytes(buf, (DWORD)size, img, dw, dh);
			if (buf) GlobalUnlock(handle);
		}
	}
	if (!got) {
		auto handle = GetClipboardData(CF_DIBV5);
		if (!handle) handle = GetClipboardData(CF_DIB);
		if (handle) {
			auto buf = reinterpret_cast<BYTE*>(GlobalLock(handle));
			if (buf) got = dibToBGRA(buf, img, dw, dh);
			if (buf) GlobalUnlock(handle);
		}
	}
	if (got) {
		w = (int)dw;
		h = (int)dh;
		CloseClipboard();
		return ClipContent::Image;
	}
	if (auto handle = GetClipboardData(CF_UNICODETEXT)) {
		if (auto buf = reinterpret_cast<wchar_t*>(GlobalLock(handle))) {
			text = buf;
			GlobalUnlock(handle);
			CloseClipboard();
			return ClipContent::Text;
		}
	}
	CloseClipboard();
	return ClipContent::None;
}

bool Util::saveToFile(const std::wstring& path, const int w, const int h, BYTE* data)
{
	return saveToFile(path, w, h, data, ImgFormat::Png);
}

bool Util::saveToFile(const std::wstring& path, const int w, const int h, BYTE* data,
	const ImgFormat format, const float quality)
{
	if (path.empty() || w <= 0 || h <= 0 || !data) return false;
	ComPtr<IWICImagingFactory> factory;
	auto hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
	if (FAILED(hr)) return false;
	ComPtr<IWICStream> stream;
	hr = factory->CreateStream(stream.GetAddressOf());
	if (FAILED(hr)) return false;
	hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
	if (FAILED(hr)) return false;
	return encodeImage(stream.Get(), w, h, data, format, quality);
}

std::wstring Util::getExtOfFormat(const ImgFormat format)
{
	switch (format)
	{
	case ImgFormat::Jpeg: return L"jpg";
	case ImgFormat::WebP: return L"webp";
	default: return L"png";
	}
}

int Util::getSaveFormat()
{
	// 夹一遍值域：配置文件可能被手工改坏，而下面直接拿它当枚举用
	auto val = Setting::get()->getSaveFormat();
	if (val < 0 || val > (int)ImgFormat::WebP) return (int)ImgFormat::Png;
	return val;
}

// 自动保存开着就不再弹另存为：每次截图都要点一次目录，是这个工具最高频的打断，
// 而"图去哪了"这个问题有目录 + 模板就够回答了
std::wstring Util::resolveSavePath(HWND hwnd)
{
	auto setting = Setting::get();
	if (!setting->getAutoSave()) {
		return getSaveFilePath(hwnd, getExtOfFormat((ImgFormat)getSaveFormat()));
	}
	return autoSavePath();
}

std::wstring Util::autoSavePath()
{
	auto setting = Setting::get();
	auto dir = std::filesystem::path{ setting->getSaveDir() };
	if (dir.empty()) dir = setting->getDataPath().append(L"screenshot");
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	auto ext = getExtOfFormat((ImgFormat)getSaveFormat());
	auto base = formatFileName(setting->getSaveNameTpl(), ext);
	auto full = dir / base;
	if (!std::filesystem::exists(full)) return full.wstring();
	// 同一秒内连着截两张：模板算出来的名字会重，往后加序号直到撞不上
	auto stem = std::filesystem::path{ base }.stem().wstring();
	for (int i = 1; i < 999; ++i) {
		auto candidate = dir / (stem + L"_" + std::to_wstring(i) + L"." + ext);
		if (!std::filesystem::exists(candidate)) return candidate.wstring();
	}
	return {};
}

std::wstring Util::formatFileName(const std::wstring& tpl, const std::wstring& ext)
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	std::wstring result;
	for (size_t i = 0; i < tpl.size(); ++i)
	{
		if (tpl[i] != L'%' || i + 1 >= tpl.size()) {
			result += tpl[i];
			continue;
		}
		auto c = tpl[i + 1];
		i++;
		switch (c)
		{
		case L'y': result += std::format(L"{:04d}", st.wYear); break;
		case L'm': result += std::format(L"{:02d}", st.wMonth); break;
		case L'd': result += std::format(L"{:02d}", st.wDay); break;
		case L'H': result += std::format(L"{:02d}", st.wHour); break;
		case L'M': result += std::format(L"{:02d}", st.wMinute); break;
		case L'S': result += std::format(L"{:02d}", st.wSecond); break;
		case L'n': result += std::format(L"{:03d}", st.wMilliseconds); break;
		case L'%': result += L'%'; break;
		default:  result += c; break;   // 认不出的占位符原样留下，别把字符吃掉
		}
	}
	// 模板里一个占位符都没有（或者被清空了）时退化成时间戳，
	// 否则会产出一个只有扩展名的文件名
	if (result.empty()) result = createFileName(L"");
	if (!result.empty() && result.back() != L'.') result += L'.';
	return result + ext;
}

std::wstring Util::getSaveFilePath(HWND hwnd, const std::wstring& ext)
{
	std::wstring result;
	ComPtr<IFileSaveDialog> saveDialog;
	auto hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(saveDialog.GetAddressOf()));
	if (FAILED(hr)) return result;
	DWORD dwFlags{ 0 };
	saveDialog->GetOptions(&dwFlags);
	saveDialog->SetOptions(dwFlags | FOS_OVERWRITEPROMPT | FOS_STRICTFILETYPES);
	auto pattern = L"*." + ext;
	auto typeName = Lang::get(L"util.file");
	COMDLG_FILTERSPEC filterSpec[]{ { typeName.c_str(), pattern.c_str() } };
	saveDialog->SetFileTypes(_countof(filterSpec), filterSpec);
	saveDialog->SetFileTypeIndex(1);
	saveDialog->SetDefaultExtension(ext.c_str());
	auto fileName = createFileName(ext);
	saveDialog->SetFileName(fileName.c_str());
	// 用户取消时 Show 返回 HRESULT_FROM_WIN32(ERROR_CANCELLED)，一样走 FAILED 分支
	hr = saveDialog->Show(hwnd);
	if (FAILED(hr)) return result;
	ComPtr<IShellItem> item;
	hr = saveDialog->GetResult(item.GetAddressOf());
	if (FAILED(hr)) return result;
	PWSTR filePath{ nullptr };
	hr = item->GetDisplayName(SIGDN_FILESYSPATH, &filePath);
	if (FAILED(hr)) return result;
	result = filePath;
	CoTaskMemFree(filePath);
	return result;
}

std::wstring Util::createFileName(const std::wstring& ext)
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	return std::format(L"{:04d}{:02d}{:02d}{:02d}{:02d}{:02d}{:03d}.{}",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, ext);
}

namespace {
// 指针的快照。hCursor 是系统共享的句柄，等到画的时候它可能已经换人了，所以拷一份图标存着
HICON snapshotIcon{ nullptr };
POINT snapshotPos{ 0, 0 };

// 把指针画到 hdc 上。hdc 的原点对应屏幕上的 (x, y)
void drawCursor(HDC hdc, const int x, const int y)
{
	HICON icon{ snapshotIcon };
	POINT pos{ snapshotPos };
	if (!icon) {
		// 没快照：现取一个。定时自动截图走这条路 —— 那时屏幕上没有 tpix 的窗口，
		// 取到的就是用户真正在用的那个指针
		CURSORINFO ci{ .cbSize = sizeof(CURSORINFO) };
		if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING)) return;
		icon = ci.hCursor;
		pos = ci.ptScreenPos;
	}
	ICONINFO ii{};
	if (!GetIconInfo(icon, &ii)) return;
	// GetIconInfo 造出来的这两张位图得自己删；单色指针没有 hbmColor
	DrawIconEx(hdc, pos.x - x - (int)ii.xHotspot, pos.y - y - (int)ii.yHotspot,
		icon, 0, 0, 0, nullptr, DI_NORMAL);
	if (ii.hbmMask) DeleteObject(ii.hbmMask);
	if (ii.hbmColor) DeleteObject(ii.hbmColor);
}
}

void Util::snapshotCursor()
{
	CURSORINFO ci{ .cbSize = sizeof(CURSORINFO) };
	if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) {
		snapshotIcon = nullptr;
		return;
	}
	if (snapshotIcon) DestroyIcon(snapshotIcon);
	snapshotIcon = CopyIcon(ci.hCursor);
	snapshotPos = ci.ptScreenPos;
}

std::vector<BYTE> Util::captureScreen(const int x, const int y, const int w, const int h,
	bool withCursor)
{
	std::vector<BYTE> data;
	if (w <= 0 || h <= 0) return data;
	HDC hScreen = GetDC(nullptr);
	HDC hDC = CreateCompatibleDC(hScreen);
	HBITMAP hBitmap = CreateCompatibleBitmap(hScreen, w, h);
	auto oldObj = SelectObject(hDC, hBitmap);
	BitBlt(hDC, 0, 0, w, h, hScreen, x, y, SRCCOPY);
	ReleaseDC(nullptr, hScreen);
	if (withCursor) drawCursor(hDC, x, y);
	data.resize((size_t)w * 4 * h);
	BITMAPINFO bmi{};
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = w;
	// 负高度 = top-down，第一行就是屏幕最上面那行，省掉后续所有翻转
	bmi.bmiHeader.biHeight = -h;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;
	GetDIBits(hDC, hBitmap, 0, h, data.data(), &bmi, DIB_RGB_COLORS);
	SelectObject(hDC, oldObj);
	DeleteDC(hDC);
	DeleteObject(hBitmap);
	return data;
}

void Util::addFileToClipboard(const std::wstring& filePath)
{
	if (!OpenClipboard(nullptr)) return;
	EmptyClipboard();
	// DROPFILES 之后紧跟双 \0 结尾的路径列表，这里只放一条
	auto totalSize = sizeof(DROPFILES) + (filePath.length() + 2) * sizeof(wchar_t);
	auto hGlobal = GlobalAlloc(GMEM_MOVEABLE, totalSize);
	if (!hGlobal) {
		CloseClipboard();
		return;
	}
	auto pDropFiles = static_cast<DROPFILES*>(GlobalLock(hGlobal));
	if (!pDropFiles) {
		GlobalFree(hGlobal);
		CloseClipboard();
		return;
	}
	pDropFiles->pFiles = sizeof(DROPFILES);
	pDropFiles->fWide = TRUE;
	auto dest = reinterpret_cast<wchar_t*>(pDropFiles + 1);
	wcscpy_s(dest, filePath.length() + 1, filePath.c_str());
	dest[filePath.length() + 1] = L'\0';
	GlobalUnlock(hGlobal);
	// 成功后 HGLOBAL 归剪切板所有，只在失败时自己释放
	if (!SetClipboardData(CF_HDROP, hGlobal)) {
		GlobalFree(hGlobal);
	}
	CloseClipboard();
}

bool Util::openWithImageReader(const int w, const int h, BYTE* data)
{
	auto exePath = findImageReader();
	if (exePath.empty()) {
		// 插件没装，直接把用户带到下载页，不再多弹一层提示
		ShellExecute(nullptr, L"open", L"https://github.com/xland/ImageReader/releases", nullptr, nullptr, SW_SHOWNORMAL);
		return false;
	}
	auto imgPath = Setting::get()->getDataPath().append(L"ocr_" + createFileName(L"png")).wstring();
	if (!saveToFile(imgPath, w, h, data)) return false;
	// --del-image=true：插件读完自己把缓存图删掉，免得在数据目录里越攒越多
	auto cmd = std::format(L"\"{}\" --image-path=\"{}\" --del-image=true", exePath.wstring(), imgPath);
	// 工作目录设成插件所在目录，它才找得到自己身边的依赖
	auto workDir = exePath.parent_path().wstring();
	STARTUPINFO si{ .cb = sizeof(STARTUPINFO) };
	PROCESS_INFORMATION pi{};
	if (!CreateProcess(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, workDir.data(), &si, &pi)) {
		std::error_code ec;
		std::filesystem::remove(imgPath, ec); //插件没起来，别留下垃圾文件
		return false;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
}

std::wstring Util::decodeQrCode(const int w, const int h, BYTE* data)
{
	std::wstring result;
	if (w <= 0 || h <= 0 || !data) return result;
	// quirc_new 和 quirc_resize 是这个库里唯一会申请内存的两个函数，选区大的时候
	// 那块灰度缓冲不小，所以下面每条返回路径都得走到 quirc_destroy
	auto qr = quirc_new();
	if (!qr) return result;
	if (quirc_resize(qr, w, h) < 0) {
		quirc_destroy(qr);
		return result;
	}
	// quirc_begin 给的就是它内部那块缓冲，一个像素一字节，直接把灰度写进去
	int bufW{ 0 }, bufH{ 0 };
	auto buffer = quirc_begin(qr, &bufW, &bufH);
	const size_t count = (size_t)w * h;
	for (size_t i = 0; i < count; i++) {
		auto px = data + i * 4; //入参是 BGRA
		buffer[i] = (uint8_t)((px[2] * 77 + px[1] * 150 + px[0] * 29) >> 8);
	}
	quirc_end(qr);
	auto codeCount = quirc_count(qr);
	for (int i = 0; i < codeCount; i++) {
		quirc_code code{};
		quirc_data qrData{};
		quirc_extract(qr, i, &code);
		auto err = quirc_decode(&code, &qrData);
		if (err == QUIRC_ERROR_DATA_ECC) {
			// 可能是镜像的码（ISO 18004:2015 允许），翻过来再试一次
			quirc_flip(&code);
			err = quirc_decode(&code, &qrData);
		}
		if (err != QUIRC_SUCCESS) continue;
		auto text = qrPayloadToWStr(qrData.payload, qrData.payload_len, qrData.data_type);
		if (text.empty()) continue;
		if (!result.empty()) result += L"\n";
		result += text;
	}
	quirc_destroy(qr);
	return result;
}

bool Util::resizeBGRA(const int srcW, const int srcH, BYTE* srcData,
	const int dstW, const int dstH, std::vector<BYTE>& dstData)
{
	if (srcW <= 0 || srcH <= 0 || !srcData || dstW <= 0 || dstH <= 0) return false;
	dstData.assign((size_t)dstW * dstH * 4, 0);
	for (int y = 0; y < dstH; ++y)
	{
		// 目标一行对应源图的哪几行：放大时这个区间可能只有一行，靠下面的 max 兜住
		auto sy0 = y * srcH / dstH;
		auto sy1 = std::max((y + 1) * srcH / dstH, sy0 + 1);
		for (int x = 0; x < dstW; ++x)
		{
			auto sx0 = x * srcW / dstW;
			auto sx1 = std::max((x + 1) * srcW / dstW, sx0 + 1);
			UINT sumB{ 0 }, sumG{ 0 }, sumR{ 0 }, sumA{ 0 }, n{ 0 };
			for (int sy = sy0; sy < sy1; ++sy) {
				auto row = srcData + (size_t)sy * srcW * 4;
				for (int sx = sx0; sx < sx1; ++sx) {
					auto px = row + sx * 4;
					sumB += px[0]; sumG += px[1]; sumR += px[2]; sumA += px[3];
					n++;
				}
			}
			auto d = dstData.data() + ((size_t)y * dstW + x) * 4;
			d[0] = (BYTE)(sumB / n);
			d[1] = (BYTE)(sumG / n);
			d[2] = (BYTE)(sumR / n);
			d[3] = (BYTE)(sumA / n);
		}
	}
	return true;
}

