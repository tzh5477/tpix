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
	// ---- 空格判定 ----
	//
	// 引擎把每个汉字当成独立的一个"词"（见 ocrNeedSpace 的说明），
	// 所以拼行时必须自己决定词与词之间补不补空格，不能用 OcrLine::Text()
	// 标点符号。引擎把标点单独切成一个词，而且**认错的码点比汉字还五花八门**：
	// 实测同一个句号 U+3002 被认成 U+00B7（中点）、逗号 U+002C 被认成 U+FF0C（全角）。
	// 标点两侧一律不补空格 —— 补了就是"落地 。"这种，比认错码点难看得多
	bool isPunct(wchar_t c)
	{
		return (c >= 0x3000 && c <= 0x303F)		// 中文标点：。、《》【】
			|| (c >= 0xFF00 && c <= 0xFFEF)		// 全角：，．？！（）等
			|| (c >= 0x2010 && c <= 0x205E)		// 常用 Unicode 标点，含 U+2014 破折号
			|| c == 0x00B7 || c == 0x2022 || c == 0x2026;	// 中点/项目符号/省略号
	}

	bool isCjkChar(wchar_t c)
	{
		// 中日韩表意文字与假名、谚文。
		// 标点**不**算进来：标点自己走 isPunct 那条"两侧永不补空格"的规则，
		// 而"落地。"这种句号被误识成中点时，靠的就是这条兜住
		return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF);
	}

	// IMemoryBufferByteAccess 是纯 COM 接口，C++/WinRT 不为它生成投影，
	// 按文档给的 IID 自己声明一份。拿到它才能往 SoftwareBitmap 的像素区里直接写
	struct __declspec(uuid("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d")) __declspec(novtable)
		IMemoryBufferByteAccess : ::IUnknown
	{
		virtual HRESULT __stdcall GetBuffer(uint8_t** value, uint32_t* capacity) = 0;
	};

	// ---- 小图放大 ----
	//
	// 实测（2026-10-07，D:\tmp\tpixocr 下的独立探测程序，直接驱动 Windows OCR 引擎）：
	// **引擎认不出来的决定因素是图像总高度，不是字号**。同一行 px=16 的字，
	// 紧裁切高 21px 时认不出（空），上下各补 12px 白边变成 45px 就认出来了。
	// 多种内容（英文 / 数字 / 汉字）逐像素扫下来，门槛都落在
	// **h=38 失败、h=40 成功** 之间，与图宽无关（把 21px 高的图横向撑到
	// 1122px 依然失败）。
	//
	// 这正是"截图高度小于一行就提示没有识别到文字"的成因：一行文字紧裁出来
	// 正好落在门槛之下，不是字太小。
	//
	// 放大能救，但有两个坑，都踩过：
	//
	// 1) **必须等比**。早先按"高度拉到 48"的比例缩放，146x21 变成 334x48 ——
	//    高度够了，但横向被单独拉长 2.3 倍，汉字压扁变形，一行字被认成 "2 0 2"。
	//
	// 2) **倍数不能只取"刚好够门槛"**。在 21 个真实渲染样本（中英文、数字、
	//    货币、编号，单行紧裁切）上按字符 LCS 相似度打分：
	//        k=1 不放大 0.003   ← 就是用户遇到的"识别失败"
	//        k=2           0.707
	//        k=3           0.855
	//        k=4           0.907   ← 实测最优
	//        k=5           0.791
	//        k=6           0.773
	//    "取最小够门槛倍数"这条规则只有 0.844，比固定 4 倍低一截 ——
	//    门槛只是"能认"的及格线，不是"认得准"的及格线，字还得再放大一档。
	//    再往上又回落：字被撑得过大后笔画粘连。
	//
	// 所以：**统一 4 倍**，只在会撞上引擎尺寸上限时往下退。
	//
	// 够高的图一律原样送进引擎：常规截图（几百 px 高）本来就认得很好，
	// 白白插值一遍只会掉精度还慢。
	constexpr int kUpscale = 4;		// 固定放大倍数（实测最优，见上）
	constexpr int kMinHeight = 40;	// 实测门槛：>= 这个高度引擎自己就能认，不必插值
	constexpr int kMaxDim = 10000;	// OcrEngine::MaxImageDimension() 实测值

	// 双线性缩放 BGRA。只处理 RGB，alpha 一律写成不透明（引擎只认不透明的图）。
	// 采样按中心点对齐（+0.5 再减 0.5），否则每边会整体错半个像素。
	std::vector<BYTE> scaleBgra(const BYTE* src, const int w, const int h,
		const int nw, const int nh)
	{
		std::vector<BYTE> out((size_t)nw * nh * 4);
		for (int y = 0; y < nh; ++y)
		{
			double sy = (h == 1) ? 0.0 : (y + 0.5) * h / nh - 0.5;
			if (sy < 0) sy = 0;
			int y0 = (int)sy;
			if (y0 > h - 1) y0 = h - 1;
			const int y1 = (y0 + 1 < h) ? y0 + 1 : y0;
			const double fy = sy - y0;
			for (int x = 0; x < nw; ++x)
			{
				double sx = (w == 1) ? 0.0 : (x + 0.5) * w / nw - 0.5;
				if (sx < 0) sx = 0;
				int x0 = (int)sx;
				if (x0 > w - 1) x0 = w - 1;
				const int x1 = (x0 + 1 < w) ? x0 + 1 : x0;
				const double fx = sx - x0;
				auto dst = &out[((size_t)y * nw + x) * 4];
				for (int c = 0; c < 3; ++c)
				{
					const double p00 = src[((size_t)y0 * w + x0) * 4 + c];
					const double p10 = src[((size_t)y0 * w + x1) * 4 + c];
					const double p01 = src[((size_t)y1 * w + x0) * 4 + c];
					const double p11 = src[((size_t)y1 * w + x1) * 4 + c];
					const double v = p00 * (1 - fx) * (1 - fy) + p10 * fx * (1 - fy)
						+ p01 * (1 - fx) * fy + p11 * fx * fy;
					int iv = (int)(v + 0.5);
					if (iv < 0) iv = 0;
					if (iv > 255) iv = 255;
					dst[c] = (BYTE)iv;
				}
				dst[3] = 0xFF;
			}
		}
		return out;
	}

	// 算出该送进引擎的图：够高就原样返回（借用 data，不复制），
	// 不够高才双线性放大一份。scale 用来把词框坐标还原回原图。
	struct Prepared
	{
		const BYTE* data{ nullptr };	// 指向 data 或 owned
		int w{ 0 }, h{ 0 };
		double scale{ 1.0 };	// 引擎坐标 = 原图坐标 * scale（整数倍，无偏移）
		std::vector<BYTE> owned;
	};

	Prepared prepare(const int w, const int h, BYTE* data)
	{
		Prepared p;
		p.data = data;
		p.w = w;
		p.h = h;
		// 够高、或已经是 1 像素宽高（没有任何可插值的余地）时原样送
		if (h >= kMinHeight || h <= 1 || w <= 1) return p;
		// 等比放大 kUpscale 倍。整数倍是硬要求：非整数倍会把汉字压扁
		int k = kUpscale;
		// 不能撞上引擎的尺寸上限。极宽的矮图（如整行横幅 4000x20）放 4 倍就超了
		// 10000，这时按上限往下取整数倍；一倍都放不下就原样送，宁可认不出，
		// 也别让引擎直接拒绝整张图
		while (k >= 2 && ((long long)w * k > kMaxDim || (long long)h * k > kMaxDim)) --k;
		if (k < 2) return p;
		const int nw = w * k, nh = h * k;
		p.owned = scaleBgra(data, w, h, nw, nh);
		p.data = p.owned.data();
		p.w = nw;
		p.h = nh;
		p.scale = (double)k;	// 整数倍，坐标还原就是直接除
		return p;
	}

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

	// 建位图 + 跑引擎。整张图只认一次，行和词都从这一个结果里取。
	// 认不出来（没装语言包、参数不对、引擎内部抛了）返回空。
	// 放大过就把实际用的那张图连同系数一起交出去（out），词框要按它换算回原图
	wmo::OcrResult runEngine(const int w, const int h, BYTE* data,
		const std::wstring& langTag, Prepared* out = nullptr)
	{
		if (w <= 0 || h <= 0 || !data) return nullptr;
		try {
			auto engine = makeEngine(langTag);
			if (!engine) return nullptr;
			// 高度不够就先放大（见 kMinHeight 处的实测）。放大后的图是 owned，
			// 整个 RecognizeAsync 期间必须活着，所以 prepared 一直活到函数返回
			auto prepared = prepare(w, h, data);
			if (out) *out = prepared;
			SoftwareBitmap bitmap(BitmapPixelFormat::Bgra8, prepared.w, prepared.h);
			{
				auto buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
				auto desc = buffer.GetPlaneDescription(0);
				auto ref = buffer.CreateReference();
				auto access = ref.as<IMemoryBufferByteAccess>();
				uint8_t* dst{ nullptr };
				uint32_t cap{ 0 };
				if (FAILED(access->GetBuffer(&dst, &cap))) return nullptr;
				// 行距可能与 w*4 不等（对齐需要），所以按 Stride 一行一行拷
				auto stride = (size_t)desc.Stride;
				auto start = (size_t)desc.StartIndex;
				if (start + stride * (size_t)prepared.h > cap || stride < (size_t)prepared.w * 4) return nullptr;
				for (int y = 0; y < prepared.h; ++y)
				{
					auto srcRow = prepared.data + (size_t)y * prepared.w * 4;
					auto dstRow = dst + start + (size_t)y * stride;
					memcpy(dstRow, srcRow, (size_t)prepared.w * 4);
					// OCR 引擎只认不透明的图。截图里 alpha 未必是 255，逐像素压成不透明，
					// 否则半透明区域会被当成别的东西、整页结果都不对。
					// 放大出来的那份 alpha 已经是 0xFF 了，这里再写一遍不花什么钱
					for (int x = 3; x < prepared.w * 4; x += 4) dstRow[x] = 0xFF;
				}
			}
			return engine.RecognizeAsync(bitmap).get();
		}
		catch (...) {
			return nullptr;
		}
	}

	// 引擎坐标 -> 原图坐标。放大过的图必须过这一道，
	// 否则表格识别会把词全落错格子、选文会框不到字。
	// 整数倍放大没有平移，所以只是各分量除以 scale。
	void unmapRect(winrt::Windows::Foundation::Rect& r, const Prepared& p)
	{
		if (p.scale == 1.0) return;
		const double inv = 1.0 / p.scale;
		r.X = (float)(r.X * inv);
		r.Y = (float)(r.Y * inv);
		r.Width = (float)(r.Width * inv);
		r.Height = (float)(r.Height * inv);
	}

	// 两个词框之间的相对间隙：像素间隙 / 字高。-1 表示拿不到（字高为 0）。
	// 用字高而不是绝对像素：截图会被放大（prepare 里最多 4 倍），
	// 绝对间隙在不同字号下没有可比性
	float relativeGap(const float x0, const float w0, const float x1, const float h0, const float h1)
	{
		const float unit = std::max(h0, h1);
		if (unit <= 0.f) return -1.f;
		return (x1 - (x0 + w0)) / unit;
	}
}

bool ocrNeedSpace(wchar_t left, wchar_t right, float relGap)
{
	// 标点两侧永不补空格。"落地。"的句号被引擎认成中点 U+00B7 时，
	// 这里就是唯一拦住"落地 。"那道门
	if (isPunct(left) || isPunct(right)) return false;
	// 汉字之间本来就没有空格 —— 这条是主判据，纯靠它就修掉了"退 出 tpix 后 重 跑"
	if (isCjkChar(left) && isCjkChar(right)) return false;
	// 拿不到间隙就只看字符类：两端都不是中日韩文字时补一个，
	// 与旧的字符类判断一致（表格识别与选文复制一直就是这个行为）
	if (relGap < 0.f) return true;
	// 实测：真空格 ≥ 0.29，引擎无中生有的词间拆分只有 0.07~0.11。
	// 阈值取 0.20 离两边都有余量
	return relGap >= 0.20f;
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
	// prepared 只是为了拿放大系数把词框换算回原图 —— 拼字符串只需要原图坐标下的
	// 相对间隙，而等比缩放不改变间隙与字高的比值，所以这里换算回原图算也一样对
	Prepared prepared;
	auto result = runEngine(w, h, data, langTag, &prepared);
	if (!result) return {};
	std::wstring text;
	for (auto const& line : result.Lines())
	{
		if (!text.empty()) text += L"\n";
		// 不能用 line.Text()：引擎把每个汉字当成一个独立的词，而 line.Text()
		// 是拿空格把这些词拼起来的，中文会变成"退 出 tpix 后 重 跑 一 次 …"
		OcrWord prev;
		bool hasPrev{ false };
		for (auto const& word : line.Words())
		{
			OcrWord cur;
			cur.text = std::wstring{ std::wstring_view{ word.Text() } };
			if (cur.text.empty()) continue;
			auto rect = word.BoundingRect();
			unmapRect(rect, prepared);
			cur.x = rect.X;
			cur.y = rect.Y;
			cur.w = rect.Width;
			cur.h = rect.Height;
			if (hasPrev && !cur.text.empty() && !prev.text.empty()
				&& ocrNeedSpace(prev.text.back(), cur.text.front(),
					relativeGap(prev.x, prev.w, cur.x, prev.h, cur.h))) {
				text += L' ';
			}
			text += cur.text;
			prev = std::move(cur);
			hasPrev = true;
		}
	}
	return text;
}

std::vector<OcrWord> Ocr::recognizeWords(const int w, const int h, BYTE* data, const std::wstring& langTag)
{
	std::vector<OcrWord> words;
	if (w <= 0 || h <= 0 || !data) return words;
	// prepared 跟着引擎实际用的那张图，所以词框坐标一定对得上，
	// 也不会为了拿系数把双线性放大白做一遍
	Prepared prepared;
	auto result = runEngine(w, h, data, langTag, &prepared);
	if (!result) return words;
	for (auto const& line : result.Lines())
	{
		for (auto const& word : line.Words())
		{
			auto rect = word.BoundingRect();
			// 引擎给的是放大后那套坐标，直接用会让表格识别落错格子、选文框不到字
			unmapRect(rect, prepared);
			words.push_back({ std::wstring{ std::wstring_view{ word.Text() } },
				rect.X, rect.Y, rect.Width, rect.Height });
		}
	}
	return words;
}
