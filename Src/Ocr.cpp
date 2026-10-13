#include "pch.h"
#include "Ocr.h"
#include <optional>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Globalization.h>
#include <algorithm>
#include "OcrModel.h"
#include "Setting.h"
#include "AiService.h"

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

	// ---- 内置模型那条路 ----
	//
	// 优先跑它：中英日韩都能认，不要求系统装语言包。跑不起来才回落系统 OCR
	OcrModelState lastState{ OcrModelState::unknown };

	std::wstring modelRoot()
	{
		return (Setting::get()->getDataPath() / L"ocr").wstring();
	}

	std::wstring modelName()
	{
		// 没选过就是空串 —— 用内置清单里的默认代次
		return Setting::get()->getToolStr(L"ocr", L"model", OcrModel::defaultModel());
	}

	// 认一次。成了给词表；模型这条路走不通给 nullopt，调用方据此回落系统 OCR
	std::optional<std::vector<OcrModelBox>> wordsByModel(const int w, const int h, BYTE* data)
	{
		std::vector<OcrModelBox> boxes;
		const auto err = OcrModel::recognize(modelRoot(), modelName(), w, h, data, boxes);
		switch (err) {
		case OcrModel::Err::ok: lastState = OcrModelState::ok; break;
		case OcrModel::Err::noModel: lastState = OcrModelState::noModel; break;
		case OcrModel::Err::download: lastState = OcrModelState::download; break;
		case OcrModel::Err::badModel: lastState = OcrModelState::badModel; break;
		default: lastState = OcrModelState::runtime; break;
		}
		if (err != OcrModel::Err::ok) return std::nullopt;
		return boxes;
	}

	// 词表拼成整段文字：同一行的词之间补一个空格，行与行之间换行。
	// 中文一行本来就只有一个词（没有空格可切），所以不会退化成"退 出 tpix"
	std::wstring joinBoxes(const std::vector<OcrModelBox>& boxes)
	{
		std::wstring text;
		int lastLine{ -1 };
		for (auto const& b : boxes) {
			if (b.text.empty()) continue;
			if (!text.empty()) {
				if (b.line != lastLine) text += L'\n';
				else text += L' ';
			}
			text += b.text;
			lastLine = b.line;
		}
		return text;
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

namespace {
	// 让大模型看图识字用的三段提示词（与 WinOcr 里那套协议一致，且只能给模型看、
	// 不进语言包 —— 换界面语言不该改变识别结果的格式）
	constexpr const wchar_t* kAiPromptSys{
		L"你是一个精确的 OCR 引擎，严格按用户的格式要求输出图片里的内容。" };
	constexpr const wchar_t* kAiPromptText{
		L"请完整识别这张图片里的文字，按原有的阅读顺序与分段输出，保留换行。"
		L"只输出识别到的原文，不要翻译、不要解释、不要加任何说明，也不要用代码块包裹。" };
	constexpr const wchar_t* kAiPromptTable{
		L"请识别这张图片里的表格，用制表符分隔的纯文本输出：第一行是表头，之后每行一条记录，"
		L"相邻单元格之间用一个制表符，行与行之间换行；单元格内部不要出现制表符与换行。"
		L"只输出表格内容，不要解释。" };
}

bool Ocr::useAi()
{
	return Setting::get()->getToolStr(L"ocr", L"engine", L"sys") == L"ai";
}

void Ocr::recognizeByAi(const int w, const int h, const BYTE* data, bool asTable,
	std::function<void(const std::wstring&)> onDone)
{
	if (!onDone) return;
	if (!data || w <= 0 || h <= 0) { onDone(L""); return; }
	const auto scenario = asTable ? std::wstring{ AiScenario::table }
		: std::wstring{ AiScenario::recognize };
	// 解成接口地址 / 密钥 / 模型；没配齐就空手回去，调用方按"没识别到"处理
	auto cred = Setting::get()->credFor(scenario);
	if (!cred.ok) { onDone(L""); return; }
	std::vector<AiService::Msg> msgs;
	AiService::Msg sys;
	sys.role = AiService::Role::System;
	sys.content = kAiPromptSys;
	AiService::Msg user;
	user.role = AiService::Role::User;
	user.content = asTable ? kAiPromptTable : kAiPromptText;
	// 同步拷一份像素进请求体（编 PNG / base64 由 AiService 负责），调用方随后即可释放缓冲
	user.image.assign(data, data + (size_t)w * h * 4);
	user.imgW = w;
	user.imgH = h;
	msgs.push_back(std::move(sys));
	msgs.push_back(std::move(user));
	auto acc = std::make_shared<std::wstring>();
	// 流式累积，收尾时一次性交付。回调都在 UI 线程，可直接碰 UI
	AiService::chat(cred, msgs,
		[acc](const std::wstring& delta) { *acc += delta; },
		[acc, onDone](const std::wstring& err) {
			(void)err;
			// 模型回的东西首尾常带空白（粘了段说明、多打了几个换行），剥干净再用
			auto s = *acc;
			while (!s.empty() && (s.front() == L'\n' || s.front() == L' ')) s.erase(0, 1);
			while (!s.empty() && (s.back() == L'\n' || s.back() == L' ')) s.pop_back();
			onDone(s);
		});
}

bool Ocr::isAvailable()
{
	// 内置模型这条路不挑系统语言包（缺文件会自己下），所以选中的那代能用就总有一条路；
	// 只有"连模型都拿不到、系统又没装语言包"这一种情况才是真的没得用
	if (engine() == OcrEngine::Model) return true;
	try {
		return wmo::OcrEngine::AvailableRecognizerLanguages().Size() > 0;
	}
	catch (...) {
		return false;
	}
}

OcrEngine Ocr::engine()
{
	// 内置清单里登记过就能自己下载、目录里已经有的就能直接用 ——
	// 两者都不是（比如用户填了个并不存在的目录名）才回落到系统 OCR
	const auto name = modelName();
	if (name.empty()) return OcrEngine::System;
	for (auto const& m : OcrModel::listModels(modelRoot())) if (m == name) return OcrEngine::Model;
	return OcrEngine::System;
}

OcrModelState Ocr::modelState()
{
	return lastState;
}

std::vector<std::wstring> Ocr::models()
{
	return OcrModel::listModels(modelRoot());
}

std::wstring Ocr::curModel()
{
	return modelName();
}

std::wstring Ocr::modelLabel(const std::wstring& name)
{
	return OcrModel::modelLabel(name);
}

void Ocr::setModel(const std::wstring& name)
{
	// 换掉之后下一次识别才会重建会话：OcrModel 内部按名字认，名字变了自然重来
	Setting::get()->setToolStr(L"ocr", L"model", name);
}

std::vector<OcrModelChoice> Ocr::aiModelChoices(const std::wstring& scenario)
{
	// **只列这一档场景在设置里绑的那一个模型**（AI 对话 / 翻译 / 文字识别 / 表格识别 各一份
	// 独立绑定，见 WinSettingAi）。早先这里把"所有已配接口下的所有模型"都摊出来，结果窗的
	// 下拉一长串，用户以为那是模型总目录，随手一选就把设置页的绑定改掉了 —— 结果窗只该做
	// "在设置绑的模型之间换个引擎试试"，挑模型是设置页的事。
	//
	// getScenarioProvider / getScenarioModel 自带兜底（没绑过就退到第 0 个接口与它当前那个
	// 模型），所以拿到的正是设置页那两格现在显示的东西 —— 两处必须一致。接口或模型任一为空
	// （一个接口都没配）就返回空，下拉里自然不出现 AI 那一项
	std::vector<OcrModelChoice> items;
	auto setting = Setting::get();
	const auto pid = setting->getScenarioProvider(scenario);
	const auto model = setting->getScenarioModel(scenario);
	if (pid.empty() || model.empty()) return items;
	OcrModelChoice c;
	c.ai = true;
	c.providerId = pid;
	c.model = model;
	// 只列一项，不必再缀接口名 —— 那是"两个接口下有同名模型、需要区分是哪一家"时的做法
	c.label = model;
	items.push_back(std::move(c));
	return items;
}

std::vector<OcrModelChoice> Ocr::modelChoices(const std::wstring& scenario)
{
	std::vector<OcrModelChoice> items;
	// 1) 本地离线模型。listModels 会把内置那几代**即使还没下下来**也列出来
	//    （选中它会自己去下），所以这里不用另外补
	for (auto& m : OcrModel::listModels(modelRoot())) {
		OcrModelChoice c;
		c.ai = false;
		c.model = m;
		c.label = OcrModel::modelLabel(m);
		items.push_back(std::move(c));
	}
	// 2) 大模型：只有这一档场景在设置里绑的那一个（理由见 aiModelChoices）
	for (auto& c : aiModelChoices(scenario)) items.push_back(std::move(c));
	return items;
}

void Ocr::migrate()
{
	// 一次性迁移：把老用户内部的「PP-OCRv6」这个名字改成真实模型名「PP-OCRv6-small」。
	// 之前内部名与显示名故意分开（显示 PP-OCRv6-small、内部 PP-OCRv6），是为了不破坏
	// 老用户已经下好的 ocr\PP-OCRv6\ 与存盘的 ocr.model=PP-OCRv6；现在统一成真实名，
	// 必须把这两处一起搬，否则：① 目录不重命名的老用户会被当成"没下过"去重新下载；
	// ② config 里 PP-OCRv6 指不到目录，识别直接走不通。两处都做"新名不存在才动手"，幂等。
	std::error_code ec;
	const std::filesystem::path dir = std::filesystem::path{ modelRoot() };
	const auto oldDir = dir / L"PP-OCRv6";
	const auto newDir = dir / L"PP-OCRv6-small";
	if (std::filesystem::is_directory(oldDir, ec) && !std::filesystem::exists(newDir, ec)) {
		std::filesystem::rename(oldDir, newDir, ec);
	}
	auto s = Setting::get();
	const auto m = s->getToolStr(L"ocr", L"model", L"");
	if (m == L"PP-OCRv6") s->setToolStr(L"ocr", L"model", L"PP-OCRv6-small");
}

std::wstring Ocr::modelDir()
{
	return modelRoot();
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

// 系统 OCR 那条路。Ocr::recognize / recognizeWords 在内置模型走不通时回落到这里
static std::wstring recognizeBySystem(const int w, const int h, BYTE* data, const std::wstring& langTag)
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

static std::vector<OcrWord> wordsBySystem(const int w, const int h, BYTE* data, const std::wstring& langTag)
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

std::wstring Ocr::recognize(const int w, const int h, BYTE* data, const std::wstring& langTag)
{
	// 内置模型优先。它认出来的是空的也认账 —— 图里真的没有字就是没有字，
	// 再去问一遍系统 OCR 只会白等一秒；只有模型这条路自己走不通才回落
	if (auto boxes = wordsByModel(w, h, data)) return joinBoxes(*boxes);
	return recognizeBySystem(w, h, data, langTag);
}

std::vector<OcrWord> Ocr::recognizeWords(const int w, const int h, BYTE* data, const std::wstring& langTag)
{
	// 表格填字（Table::fillText）与贴图窗口的选文都走这里 —— 换模型这件事
	// 对它们同样生效，不用各改一遍
	if (auto boxes = wordsByModel(w, h, data)) {
		std::vector<OcrWord> words;
		words.reserve(boxes->size());
		for (auto const& b : *boxes)
			words.push_back({ b.text, b.x, b.y, b.w, b.h });
		return words;
	}
	return wordsBySystem(w, h, data, langTag);
}
