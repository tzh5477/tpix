#include "pch.h"
#include "Ocr.h"
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Globalization.h>
#include <algorithm>

using namespace winrt;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Globalization;
// 不写 using namespace Windows::Media::Ocr：那个命名空间的名字也叫 Ocr，
// 与本类同名，写全了省得将来有人看岔
namespace wmo = winrt::Windows::Media::Ocr;

namespace {
	// IMemoryBufferByteAccess 是纯 COM 接口，C++/WinRT 不为它生成投影，
	// 按文档给的 IID 自己声明一份。拿到它才能往 SoftwareBitmap 的像素区里直接写
	struct __declspec(uuid("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d")) __declspec(novtable)
		IMemoryBufferByteAccess : ::IUnknown
	{
		virtual HRESULT __stdcall GetBuffer(uint8_t** value, uint32_t* capacity) = 0;
	};

	wmo::OcrEngine makeEngine(const std::wstring& langTag)
	{
		// hstring 没有直接吃 std::wstring 的构造，先转一道，否则这里要靠两次自定义转换才能对上
		if (!langTag.empty()) {
			auto lang = Language(hstring(langTag));
			return wmo::OcrEngine::TryCreateFromLanguage(lang);
		}
		// 按用户在系统里排的语言顺序挑，最符合"用户期望识别哪种文字"
		auto engine = wmo::OcrEngine::TryCreateFromUserProfileLanguages();
		if (engine) return engine;
		// 一个都没对上（比如系统是英文但只装了中文包）：总比识别不了强
		for (auto const& lang : wmo::OcrEngine::AvailableRecognizerLanguages()) {
			engine = wmo::OcrEngine::TryCreateFromLanguage(lang);
			if (engine) return engine;
		}
		return nullptr;
	}
}

bool Ocr::isAvailable()
{
	try {
		return wmo::OcrEngine::AvailableRecognizerLanguages().Size() > 0;
	}
	catch (...) {
		return false;
	}
}

std::vector<OcrLang> Ocr::languages()
{
	std::vector<OcrLang> list;
	try {
		for (auto const& lang : wmo::OcrEngine::AvailableRecognizerLanguages()) {
			auto tag = std::wstring{ std::wstring_view{ lang.LanguageTag() } };
			// 显示名在个别语言包上是空的（只有标签），那就直接显示标签，
			// 总比在下拉里留一个空白项强
			auto name = std::wstring{ std::wstring_view{ lang.DisplayName() } };
			list.push_back({ tag, name.empty() ? tag : name });
		}
	}
	catch (...) {
	}
	// 顺序按标签排：系统给的顺序没有保证，不排的话每次开窗口语言列表都在跳
	std::sort(list.begin(), list.end(),
		[](const OcrLang& a, const OcrLang& b) { return a.tag < b.tag; });
	return list;
}

std::wstring Ocr::recognize(const int w, const int h, BYTE* data, const std::wstring& langTag)
{
	if (w <= 0 || h <= 0 || !data) return {};
	try {
		auto engine = makeEngine(langTag);
		if (!engine) return {};
		SoftwareBitmap bitmap(BitmapPixelFormat::Bgra8, w, h);
		{
			auto buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
			auto desc = buffer.GetPlaneDescription(0);
			auto ref = buffer.CreateReference();
			auto access = ref.as<IMemoryBufferByteAccess>();
			uint8_t* dst{ nullptr };
			uint32_t cap{ 0 };
			if (FAILED(access->GetBuffer(&dst, &cap))) return {};
			// 行距可能与 w*4 不等（对齐需要），所以按 Stride 一行一行拷
			auto stride = (size_t)desc.Stride;
			auto start = (size_t)desc.StartIndex;
			if (start + stride * (size_t)h > cap || stride < (size_t)w * 4) return {};
			for (int y = 0; y < h; ++y)
			{
				auto srcRow = data + (size_t)y * w * 4;
				auto dstRow = dst + start + (size_t)y * stride;
				memcpy(dstRow, srcRow, (size_t)w * 4);
				// OCR 引擎只认不透明的图。截图里 alpha 未必是 255，逐像素压成不透明，
				// 否则半透明区域会被当成别的东西、整页结果都不对
				for (int x = 3; x < w * 4; x += 4) dstRow[x] = 0xFF;
			}
		}
		auto result = engine.RecognizeAsync(bitmap).get();
		std::wstring text;
		for (auto const& line : result.Lines())
		{
			if (!text.empty()) text += L"\n";
			text += std::wstring_view{ line.Text() };
		}
		return text;
	}
	catch (...) {
		return {};
	}
}
