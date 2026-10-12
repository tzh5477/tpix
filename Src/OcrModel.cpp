#include "pch.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <onnxruntime_cxx_api.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>
#include "OcrModel.h"

namespace {
	using namespace winrt;
	using namespace winrt::Windows::Foundation;	// Uri 在这个命名空间里
	using namespace winrt::Windows::Storage::Streams;
	using namespace winrt::Windows::Web::Http;

	// ---- 内置模型清单 ----
	//
	// 一代模型就是一行：目录名 + det / rec 两个下载地址。
	// <数据目录>\ocr\<目录名>\ 下只要能找到一对 det / rec 的 onnx 就能用，
	// 所以**用户自己下的、或者将来 PP-OCRv7 的，往里一丢就能在设置里选**，
	// 不必改代码 —— 清单只负责"目录是空的时去哪儿下"
	struct ModelDef
	{
		const wchar_t* name;
		const wchar_t* det;
		const wchar_t* rec;
	};

	constexpr ModelDef kModelDefs[]{
		{ L"PP-OCRv6",
		  L"https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv6/det/PP-OCRv6_det_small.onnx",
		  L"https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv6/rec/PP-OCRv6_rec_small.onnx" },
	};
	// 认不出目录名时按它下载。与 kModelDefs[0] 保持一致
	constexpr std::wstring_view kDefModel{ L"PP-OCRv6" };
	constexpr size_t kMinModelBytes{ 1024 * 1024 };	// 比这小基本是下回来一个错误页

	// ---- 两条路线的分界（2026-10-11 实测，56 张紧裁单行样本）----
	//
	// det 的默认规则是"短边不足 736 就放大到 736"。整页文档短边本来就 > 736 不触发，
	// 可 tpix 里用户框的往往就是**一行字**（如 286×23）—— 一放大就是 30 倍，
	// 概率图糊掉、一行字被劈成好几框，实测只有 6/56 全对、3.9 秒一张。
	// 同一批样本：det（不放大）38/56、225ms；**整图直接 rec 48/56、73ms**。
	// 所以"只有一行"的一律不走 det
	//
	// ⚠️ "只有一行"当初是用宽高比（>5 就算一行）判的，2026-10-12 证明是错的：
	// 976×192 的 5 行短文本比值 5.08，照样落进"一行"的区间，整幅送 rec 吐空
	// （界面上就是"没有识别到文字"）。现在交给 countTextLines() 真数行数。
	// 也**不能**按"矮就一定是单行"短路 —— 小字号的两行（如 300×40）一样是多行
	constexpr int kDetMinH{ 128 };		// det 输入的高度下限，不到就放大（见 detect）
	// det 输入的最长边上限。超过就等比缩到这个数（**只缩不放**，理由同上）
	constexpr int kDetLimit{ 960 };
	constexpr float kDetThresh{ 0.2f };		// 概率图二值化阈值（官方 yml）
	constexpr float kDetBoxThresh{ 0.45f };	// 行框得分下限（官方 yml）
	constexpr float kDetUnclip{ 1.4f };	// 行框外扩系数（官方 yml）
	constexpr int kDetMaxCand{ 3000 };
	constexpr float kDetMinSide{ 3.f };
	// rec 的输入固定 48 高，宽度跟着原图宽高比走（官方 RecResizeImg）
	constexpr int kRecH{ 48 };
	constexpr int kRecMinW{ 320 };
	constexpr int kRecMaxW{ 3200 };		// 官方给出的动态上限，再宽没意义
	constexpr float kMinConf{ 0.5f };		// 行框认出来的字置信度低于这个就丢（官方默认）
	constexpr float kRetrySingleConf{ 0.82f };	// 单行低于这个值就值得再试一次增强图
	constexpr float kRetryMultiConf{ 0.72f };	// 多行阈值放宽：det+rec 本来就比单行更吃上下文
	constexpr int kThreads{ 2 };			// onnxruntime 每次 Run 的线程数（实测最优，见文件末）

	// ---- 会话 ----
	//
	// 全局一份：建一次要读两个模型（合计 30MB）还要预热，每次识别都重建太亏。
	// 下面这些只在 recognize 里碰，而 recognize 全程持锁 —— 连点两次也只是排队
	std::mutex mtx;
	std::unique_ptr<Ort::Env> env;
	std::unique_ptr<Ort::Session> detSess;
	std::unique_ptr<Ort::Session> recSess;
	std::string detIn, detOut, recIn, recOut;
	// 字典（rec 输出下标 - 1）。模型自带的 character 元数据，与 Table 里 slanet-plus 同款
	std::vector<std::wstring> dict;
	std::wstring loadedName;

	// ---- 小工具 ----

	float edgeMedianLuma(const int w, const int h, const BYTE* data)
	{
		if (w <= 0 || h <= 0 || !data) return 255.f;
		auto lumOf = [](const BYTE* p) {
			return 0.114f * p[0] + 0.587f * p[1] + 0.299f * p[2];	// BGRA
		};
		std::vector<float> edge;
		edge.reserve((size_t)w * 2 + (size_t)h * 2);
		for (int x = 0; x < w; ++x) {
			edge.push_back(lumOf(data + (size_t)x * 4));
			edge.push_back(lumOf(data + ((size_t)(h - 1) * w + x) * 4));
		}
		for (int y = 0; y < h; ++y) {
			edge.push_back(lumOf(data + (size_t)y * w * 4));
			edge.push_back(lumOf(data + ((size_t)y * w + w - 1) * 4));
		}
		std::nth_element(edge.begin(), edge.begin() + edge.size() / 2, edge.end());
		return edge[edge.size() / 2];
	}

	// 图里有几"行"文字 —— 靠水平投影数，不靠宽高比。
	//
	// ⚠️ 宽高比判不出这件事（2026-10-12 实测）：976×192 那种 5 行短文本比值 5.08，
	// 和"一行长句"完全重叠，被当成单行整幅送进 rec，rec 只认一行的，直接吐空
	// （界面上就是"没有识别到文字"，38ms 就返回）。改成逐行数前景像素、数连续的段：
	// 几段就是几行。一行字从上到下笔画是连着的，行间的空白把它断开 —— 行距只要
	// 有 2 个像素就能分开（要求空白连着两行才算断，挡抗锯齿的淡影）
	int countTextLines(const int w, const int h, const BYTE* data)
	{
		if (w <= 0 || h <= 0 || !data) return 1;
		// 背景亮度取四边一圈的中位数：文档图大片是白底、文字是少数，边上一圈基本
		// 落在背景上（比"取全图众数"便宜，也不怕标题占掉大半幅）
		auto lumOf = [](const BYTE* p) {
			return 0.114f * p[0] + 0.587f * p[1] + 0.299f * p[2];	// BGRA
		};
		const float bg = edgeMedianLuma(w, h, data);
		const float tol = std::max(40.f, bg * 0.25f);	// 与背景差多少算文字

		const int minPix = std::max(2, w / 100);	// 一行至少得有这么多像素，挡噪点
		int lines = 0;
		bool in = false;
		int gap = 0;
		for (int y = 0; y < h; ++y) {
			const BYTE* p = data + (size_t)y * w * 4;
			int cnt = 0;
			for (int x = 0; x < w; ++x, p += 4)
				if (std::abs(lumOf(p) - bg) > tol) ++cnt;
			if (cnt >= minPix) {
				if (!in) { ++lines; in = true; }
				gap = 0;
			}
			else if (in && ++gap > 1) {
				in = false;
			}
		}
		return std::max(lines, 1);
	}

	struct RetryImage
	{
		int w{ 0 }, h{ 0 };
		int padX{ 0 }, padY{ 0 };
		std::vector<BYTE> data;
	};

	RetryImage buildRetryImage(const int w, const int h, const BYTE* data)
	{
		RetryImage out;
		if (w <= 0 || h <= 0 || !data) return out;
		const float edgeBg = edgeMedianLuma(w, h, data);
		const bool invert = edgeBg < 110.f;	// 深色界面里常是亮字暗底，翻成暗字亮底更稳
		const int padX = std::clamp(w / 24, 8, 24);
		const int padY = std::clamp(h / 3, 8, 28);
		const int nw = w + padX * 2;
		const int nh = h + padY * 2;
		float bg = invert ? (255.f - edgeBg) : edgeBg;
		bg = std::clamp(bg, 0.f, 255.f);
		const BYTE bg8 = (BYTE)(bg + 0.5f);
		out.w = nw;
		out.h = nh;
		out.padX = padX;
		out.padY = padY;
		out.data.assign((size_t)nw * nh * 4, 0xFF);
		for (size_t i = 0; i < out.data.size(); i += 4) {
			out.data[i] = bg8;
			out.data[i + 1] = bg8;
			out.data[i + 2] = bg8;
		}
		auto clampByte = [](float v) {
			if (v < 0.f) v = 0.f;
			if (v > 255.f) v = 255.f;
			return (BYTE)(v + 0.5f);
		};
		for (int y = 0; y < h; ++y) {
			for (int x = 0; x < w; ++x) {
				const BYTE* src = data + ((size_t)y * w + x) * 4;
				float lum = 0.114f * src[0] + 0.587f * src[1] + 0.299f * src[2];
				if (invert) lum = 255.f - lum;
				// 只做一档温和拉伸：把低对比文字再往背景两侧拉开一点，别上二值化把细笔画打碎
				lum = bg + (lum - bg) * 1.35f;
				BYTE v = clampByte(lum);
				BYTE* dst = out.data.data() + ((size_t)(y + padY) * nw + (x + padX)) * 4;
				dst[0] = v;
				dst[1] = v;
				dst[2] = v;
				dst[3] = 0xFF;
			}
		}
		return out;
	}

	void unpadBoxes(std::vector<OcrModelBox>& boxes, const int padX, const int padY,
		const int w, const int h)
	{
		for (auto& b : boxes) {
			const auto left = b.x - (float)padX;
			const auto top = b.y - (float)padY;
			const auto right = left + b.w;
			const auto bottom = top + b.h;
			b.x = std::clamp(left, 0.f, (float)w - 1.f);
			b.y = std::clamp(top, 0.f, (float)h - 1.f);
			b.w = std::max(std::clamp(right, 0.f, (float)w) - b.x, 1.f);
			b.h = std::max(std::clamp(bottom, 0.f, (float)h) - b.y, 1.f);
		}
	}

	std::wstring toWide(const std::string& s)
	{
		if (s.empty()) return {};
		const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
		if (n <= 0) return {};
		std::wstring out((size_t)n, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
		return out;
	}

	bool containsIci(const std::wstring& s, const std::wstring& sub)
	{
		auto hit = std::search(s.begin(), s.end(), sub.begin(), sub.end(),
			[](wchar_t a, wchar_t b) { return towlower(a) == towlower(b); });
		return hit != s.end();
	}

	// 在目录里找 det / rec 那个 onnx。按**名字里有没有 det / rec** 认，不写死文件名 ——
	// 官方包里叫 PP-OCRv6_det_small.onnx，用户自己下的可能就叫 det.onnx
	bool findOnnx(const std::filesystem::path& dir, const std::wstring& token,
		std::filesystem::path& out)
	{
		std::error_code ec;
		for (std::filesystem::directory_iterator it{ dir, ec }, end{}; it != end; it.increment(ec)) {
			if (!it->is_regular_file()) continue;
			auto path = it->path();
			if (path.extension() != L".onnx") continue;
			if (!containsIci(path.filename().wstring(), token)) continue;
			out = path;
			return true;
		}
		return false;
	}

	bool download(const std::wstring& url, const std::filesystem::path& path)
	{
		try {
			HttpClient client;
			auto buffer = client.GetBufferAsync(Uri{ url }).get();
			std::vector<BYTE> bytes(buffer.Length());
			DataReader::FromBuffer(buffer).ReadBytes(bytes);
			if (bytes.size() < kMinModelBytes) return false;
			std::error_code ec;
			std::filesystem::create_directories(path.parent_path(), ec);
			// 先写临时文件再改名：下载中断不会留下一个半截的 .onnx，骗过下一次的存在检查
			auto tmp = path;
			tmp += L".downloading";
			{
				std::ofstream file{ tmp, std::ios::binary | std::ios::trunc };
				if (!file) return false;
				file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
			}
			std::filesystem::rename(tmp, path, ec);
			return !ec;
		}
		catch (...) {
			return false;
		}
	}

	const ModelDef* findDef(const std::wstring& name)
	{
		for (auto& d : kModelDefs) if (name == d.name) return &d;
		return nullptr;
	}

	// 取输出里第 n 个张量的 float 数据（不管它叫什么名字）
	bool outTensor(std::vector<Ort::Value>& outs, const int n,
		std::vector<int64_t>& dims, const float*& p)
	{
		if (n < 0 || n >= (int)outs.size()) return false;
		auto info = outs[n].GetTensorTypeAndShapeInfo();
		dims = info.GetShape();
		p = outs[n].GetTensorData<float>();
		return true;
	}

	// ---- 缩放 / 采样 ----
	//
	// 双线性缩放到 dw×dh，按 CHW 排布写进 out（每行 stride 个，右边补 0）。
	// imageNet=true 用 det 那套均值标准差；false 用 rec 的 (x/255-0.5)/0.5 ——
	// **两边不一样，别套错**（官方 yml：det 有 NormalizeImage，rec 没有）
	void resizeChw(const BYTE* src, const int w, const int h, const int dw, const int dh,
		const int stride, const bool imageNet, std::vector<float>& out)
	{
		constexpr float mean[3]{ 0.485f, 0.456f, 0.406f };
		constexpr float stdv[3]{ 0.229f, 0.224f, 0.225f };
		out.assign((size_t)3 * stride * dh, 0.f);
		for (int y = 0; y < dh; ++y) {
			const auto fy = std::clamp((y + 0.5f) * (h / (float)dh) - 0.5f, 0.f, (float)h - 1.f);
			const int y0 = (int)fy;
			const int y1 = std::min(y0 + 1, h - 1);
			const auto ay = fy - y0;
			for (int x = 0; x < dw; ++x) {
				const auto fx = std::clamp((x + 0.5f) * (w / (float)dw) - 0.5f, 0.f, (float)w - 1.f);
				const int x0 = (int)fx;
				const int x1 = std::min(x0 + 1, w - 1);
				const auto ax = fx - x0;
				for (int c = 0; c < 3; ++c) {
					auto at = [&](const int sx, const int sy) {
						return (float)src[((size_t)sy * w + sx) * 4 + c] / 255.f;
					};
					const auto top = at(x0, y0) * (1.f - ax) + at(x1, y0) * ax;
					const auto bottom = at(x0, y1) * (1.f - ax) + at(x1, y1) * ax;
					auto v = top * (1.f - ay) + bottom * ay;
					if (imageNet) v = (v - mean[c]) / stdv[c];
					else v = (v - 0.5f) / 0.5f;
					out[((size_t)c * dh + y) * stride + x] = v;
				}
			}
		}
	}

	// 把一个平行四边形区域拉直采成 dh 高的 CHW。原点 o，两条边向量 u（宽）与 v（高），
	// 输出 (i,j) 对应原图 o + (i+0.5)/dw·u + (j+0.5)/dh·v —— 等价于把那块裁下来再缩放，
	// 只是不用真的先落一张中间图
	void warpChw(const BYTE* src, const int w, const int h,
		const float ox, const float oy, const float ux, const float uy,
		const float vx, const float vy, const int dw, const int dh, const int stride,
		std::vector<float>& out)
	{
		out.assign((size_t)3 * stride * dh, 0.f);
		for (int y = 0; y < dh; ++y) {
			const auto sv = (y + 0.5f) / dh;
			for (int x = 0; x < dw; ++x) {
				const auto su = (x + 0.5f) / dw;
				const auto fx = std::clamp(ox + su * ux + sv * vx, 0.f, (float)w - 1.f);
				const auto fy = std::clamp(oy + su * uy + sv * vy, 0.f, (float)h - 1.f);
				const int x0 = (int)fx, y0 = (int)fy;
				const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
				const auto ax = fx - x0, ay = fy - y0;
				for (int c = 0; c < 3; ++c) {
					auto at = [&](const int sx, const int sy) {
						return (float)src[((size_t)sy * w + sx) * 4 + c] / 255.f;
					};
					const auto top = at(x0, y0) * (1.f - ax) + at(x1, y0) * ax;
					const auto bottom = at(x0, y1) * (1.f - ax) + at(x1, y1) * ax;
					const auto v = top * (1.f - ay) + bottom * ay;
					out[((size_t)c * dh + y) * stride + x] = (v - 0.5f) / 0.5f;
				}
			}
		}
	}

	// ---- 几何：凸包 + 旋转卡壳 ----
	//
	// 没有 OpenCV，minAreaRect 得自己来。对连通域的所有像素求凸包，再枚举凸包的每条边
	// 当矩形的一条边，取面积最小的那个 —— 这就是旋转卡壳，只是用枚举代替双指针
	//（凸包一般几十到几百个点，O(m²) 完全够快）
	struct Pt { float x{ 0 }, y{ 0 }; };

	std::vector<Pt> convexHull(std::vector<Pt> pts)
	{
		std::sort(pts.begin(), pts.end(), [](const Pt& a, const Pt& b) {
			return a.x < b.x || (a.x == b.x && a.y < b.y);
			});
		pts.erase(std::unique(pts.begin(), pts.end(), [](const Pt& a, const Pt& b) {
			return a.x == b.x && a.y == b.y;
			}), pts.end());
		const int n = (int)pts.size();
		if (n < 3) return pts;
		auto cross = [](const Pt& o, const Pt& a, const Pt& b) {
			return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
		};
		std::vector<Pt> hull((size_t)2 * n);
		int k{ 0 };
		for (int i = 0; i < n; ++i) {
			while (k >= 2 && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0) --k;
			hull[k++] = pts[i];
		}
		for (int i = n - 2, t = k + 1; i >= 0; --i) {
			while (k >= t && cross(hull[k - 2], hull[k - 1], pts[i]) <= 0) --k;
			hull[k++] = pts[i];
		}
		hull.resize(k - 1);
		return hull;
	}

	// 一个旋转矩形：中心 + 两条半轴方向（u 为宽方向，单位向量）+ 宽高
	struct RotRect
	{
		float cx{ 0 }, cy{ 0 }, w{ 0 }, h{ 0 }, ux{ 1 }, uy{ 0 };
	};

	RotRect minAreaRect(const std::vector<Pt>& pts)
	{
		auto hull = convexHull(pts);
		const int n = (int)hull.size();
		if (n == 0) return {};
		if (n == 1) return { hull[0].x, hull[0].y, 1.f, 1.f, 1.f, 0.f };
		if (n == 2) {
			const auto dx = hull[1].x - hull[0].x, dy = hull[1].y - hull[0].y;
			const auto len = std::max(std::sqrt(dx * dx + dy * dy), 1e-6f);
			const auto ux = dx / len, uy = dy / len;
			return { (hull[0].x + hull[1].x) / 2.f, (hull[0].y + hull[1].y) / 2.f,
				len, 1.f, ux, uy };
		}
		float bestArea{ FLT_MAX };
		RotRect best;
		for (int i = 0; i < n; ++i) {
			const auto& a = hull[i];
			const auto& b = hull[(i + 1) % n];
			const auto dx = b.x - a.x, dy = b.y - a.y;
			const auto len = std::sqrt(dx * dx + dy * dy);
			if (len < 1e-6f) continue;
			const auto ux = dx / len, uy = dy / len;
			const auto vx = -uy, vy = ux;
			float minU{ FLT_MAX }, maxU{ -FLT_MAX }, minV{ FLT_MAX }, maxV{ -FLT_MAX };
			for (auto& p : hull) {
				const auto du = p.x * ux + p.y * uy;
				const auto dv = p.x * vx + p.y * vy;
				minU = std::min(minU, du); maxU = std::max(maxU, du);
				minV = std::min(minV, dv); maxV = std::max(maxV, dv);
			}
			const auto w = maxU - minU, h = maxV - minV;
			const auto area = w * h;
			if (area >= bestArea) continue;
			bestArea = area;
			// u、v 是正交单位向量，点 = du·u + dv·v，所以中心就是把两边的中点加起来
			best = { (minU + maxU) / 2.f * ux + (minV + maxV) / 2.f * vx,
				(minU + maxU) / 2.f * uy + (minV + maxV) / 2.f * vy,
				w, h, ux, uy };
		}
		return best;
	}

	// ---- 行框 ----
	//
	// 四个角按 左上 / 右上 / 右下 / 左下 排好，单位是与 recognize 入参图一致的像素
	struct Quad { float x[4]{ 0, 0, 0, 0 }, y[4]{ 0, 0, 0, 0 }; };

	Quad rectToQuad(const RotRect& r)
	{
		// 宽方向 u，高方向 v = 把 u 转 90°
		const auto vx = -r.uy, vy = r.ux;
		const auto hw = r.w / 2.f, hh = r.h / 2.f;
		Quad q;
		const float sx[4]{ -1.f, 1.f, 1.f, -1.f };
		const float sy[4]{ -1.f, -1.f, 1.f, 1.f };
		for (int i = 0; i < 4; ++i) {
			q.x[i] = r.cx + sx[i] * hw * r.ux + sy[i] * hh * vx;
			q.y[i] = r.cy + sx[i] * hw * r.uy + sy[i] * hh * vy;
		}
		return q;
	}

	// 角落顺序统一成 左上 / 右上 / 右下 / 左下（按 x 分左右两组，组内按 y 分上下）
	void orderQuad(Quad& q)
	{
		int idx[4]{ 0, 1, 2, 3 };
		std::sort(std::begin(idx), std::end(idx),
			[&](const int a, const int b) { return q.x[a] < q.x[b]; });
		int left[2]{ idx[0], idx[1] }, right[2]{ idx[2], idx[3] };
		if (q.y[left[0]] > q.y[left[1]]) std::swap(left[0], left[1]);
		if (q.y[right[0]] > q.y[right[1]]) std::swap(right[0], right[1]);
		const int order[4]{ left[0], right[0], right[1], left[1] };
		Quad out;
		for (int i = 0; i < 4; ++i) {
			out.x[i] = q.x[order[i]];
			out.y[i] = q.y[order[i]];
		}
		q = out;
	}

	struct QuadMetrics
	{
		float left{ 0 }, top{ 0 }, right{ 0 }, bottom{ 0 };
		float cx{ 0 }, cy{ 0 };
		float w{ 0 }, h{ 0 };
		float angle{ 0 };	// 顶边方向，弧度
	};

	QuadMetrics quadMetrics(const Quad& q)
	{
		QuadMetrics m;
		m.left = std::min(std::min(q.x[0], q.x[1]), std::min(q.x[2], q.x[3]));
		m.top = std::min(std::min(q.y[0], q.y[1]), std::min(q.y[2], q.y[3]));
		m.right = std::max(std::max(q.x[0], q.x[1]), std::max(q.x[2], q.x[3]));
		m.bottom = std::max(std::max(q.y[0], q.y[1]), std::max(q.y[2], q.y[3]));
		m.cx = (m.left + m.right) / 2.f;
		m.cy = (m.top + m.bottom) / 2.f;
		auto len = [](const float x0, const float y0, const float x1, const float y1) {
			const auto dx = x1 - x0, dy = y1 - y0;
			return std::sqrt(dx * dx + dy * dy);
		};
		m.w = (len(q.x[0], q.y[0], q.x[1], q.y[1]) + len(q.x[3], q.y[3], q.x[2], q.y[2])) / 2.f;
		m.h = (len(q.x[0], q.y[0], q.x[3], q.y[3]) + len(q.x[1], q.y[1], q.x[2], q.y[2])) / 2.f;
		m.angle = std::atan2(q.y[1] - q.y[0], q.x[1] - q.x[0]);
		return m;
	}

	float angleDiff(const float a, const float b)
	{
		auto d = std::fabs(a - b);
		while (d > 3.14159265f) d -= 3.14159265f * 2.f;
		return std::fabs(d);
	}

	// det 在截图里偶尔会把同一行横向切成两三个相邻的小框；逐框送 rec 会变慢，
	// 而且上下文变短后更容易丢空格/错字。这里只合并"几乎贴在一起"的水平相邻框：
	// 间距稍大就宁可不合，避免把表格的邻列、或者本来分开的两块内容并成一行。
	bool canMergeNeighborQuad(const Quad& a, const Quad& b)
	{
		const auto ma = quadMetrics(a);
		const auto mb = quadMetrics(b);
		const auto h = std::max(ma.h, mb.h);
		if (h < 1.f) return false;
		// 只动接近水平的文字，旋转 / 竖排还是保留原框，别为了省一次 rec 把方向搞坏
		constexpr float kMaxAbsAngle{ 15.f * 3.14159265f / 180.f };
		constexpr float kMaxAngleDiff{ 10.f * 3.14159265f / 180.f };
		if (std::fabs(ma.angle) > kMaxAbsAngle || std::fabs(mb.angle) > kMaxAbsAngle) return false;
		if (angleDiff(ma.angle, mb.angle) > kMaxAngleDiff) return false;
		// 高度差太大通常不是同一行；y 中心与垂直重叠同时约束，防止串到上下两行
		if (std::min(ma.h, mb.h) / std::max(ma.h, mb.h) < 0.7f) return false;
		if (std::fabs(ma.cy - mb.cy) > 0.25f * h) return false;
		const auto overlap = std::min(ma.bottom, mb.bottom) - std::max(ma.top, mb.top);
		if (overlap < 0.7f * std::min(ma.h, mb.h)) return false;
		// 只合并几乎挨着的相邻框：留一点负值给 det 的轻微外扩重叠，正值只放到半个字高
		const auto gap = mb.left - ma.right;
		return gap >= -0.15f * h && gap <= 0.45f * h;
	}

	Quad mergeNeighborQuad(const Quad& a, const Quad& b)
	{
		const auto ma = quadMetrics(a);
		const auto mb = quadMetrics(b);
		Quad q;
		const auto left = std::min(ma.left, mb.left);
		const auto right = std::max(ma.right, mb.right);
		const auto top = std::min(ma.top, mb.top);
		const auto bottom = std::max(ma.bottom, mb.bottom);
		q.x[0] = left; q.y[0] = top;
		q.x[1] = right; q.y[1] = top;
		q.x[2] = right; q.y[2] = bottom;
		q.x[3] = left; q.y[3] = bottom;
		return q;
	}

	std::vector<Quad> mergeNeighborQuads(std::vector<Quad> quads)
	{
		if (quads.size() < 2) return quads;
		std::vector<Quad> merged;
		merged.reserve(quads.size());
		merged.push_back(quads[0]);
		for (size_t i = 1; i < quads.size(); ++i) {
			if (canMergeNeighborQuad(merged.back(), quads[i])) {
				merged.back() = mergeNeighborQuad(merged.back(), quads[i]);
				continue;
			}
			merged.push_back(quads[i]);
		}
		return merged;
	}

	// 矩形内概率图的均值。DBNet 用它过滤掉"看着像字其实不是"的那些团
	//
	// 矩形的外扩（unclip）不需要 pyclipper：**输入本来就是矩形**时，polygon 偏移
	// 距离 d 就等于四条边各往外推 d。d 照官方公式取 area × ratio / perimeter
	float boxScore(const float* pred, const int w, const int h, const RotRect& r)
	{
		const auto vx = -r.uy, vy = r.ux;
		const auto hw = r.w / 2.f, hh = r.h / 2.f;
		const float cs[4][2]{ { -hw, -hh }, { hw, -hh }, { hw, hh }, { -hw, hh } };
		float minX{ FLT_MAX }, maxX{ -FLT_MAX }, minY{ FLT_MAX }, maxY{ -FLT_MAX };
		for (auto& c : cs) {
			const auto px = r.cx + c[0] * r.ux + c[1] * vx;
			const auto py = r.cy + c[0] * r.uy + c[1] * vy;
			minX = std::min(minX, px); maxX = std::max(maxX, px);
			minY = std::min(minY, py); maxY = std::max(maxY, py);
		}
		const int x0 = std::clamp((int)std::floor(minX), 0, w - 1);
		const int x1 = std::clamp((int)std::ceil(maxX), 0, w - 1);
		const int y0 = std::clamp((int)std::floor(minY), 0, h - 1);
		const int y1 = std::clamp((int)std::ceil(maxY), 0, h - 1);
		if (x1 < x0 || y1 < y0) return 0.f;
		double sum{ 0 };
		int cnt{ 0 };
		for (int y = y0; y <= y1; ++y) {
			for (int x = x0; x <= x1; ++x) {
				const auto dx = (float)x + 0.5f - r.cx, dy = (float)y + 0.5f - r.cy;
				const auto du = dx * r.ux + dy * r.uy;
				const auto dv = dx * vx + dy * vy;
				if (std::fabs(du) > hw || std::fabs(dv) > hh) continue;
				sum += pred[(size_t)y * w + x];
				++cnt;
			}
		}
		return cnt ? (float)(sum / cnt) : 0.f;
	}

	// 8 邻域连通域，返回每个团的像素下标
	std::vector<std::vector<int>> findBlobs(const std::vector<uint8_t>& mask,
		const int w, const int h)
	{
		std::vector<int> label((size_t)w * h, -1);
		std::vector<std::vector<int>> blobs;
		std::vector<int> stack;
		for (int i = 0; i < w * h; ++i) {
			if (!mask[i] || label[i] >= 0) continue;
			std::vector<int> blob;
			stack.clear();
			stack.push_back(i);
			label[i] = (int)blobs.size();
			while (!stack.empty()) {
				const int p = stack.back();
				stack.pop_back();
				blob.push_back(p);
				const int x = p % w, y = p / w;
				for (int dy = -1; dy <= 1; ++dy) {
					const int ny = y + dy;
					if (ny < 0 || ny >= h) continue;
					for (int dx = -1; dx <= 1; ++dx) {
						const int nx = x + dx;
						if (nx < 0 || nx >= w) continue;
						const int q = ny * w + nx;
						if (!mask[q] || label[q] >= 0) continue;
						label[q] = (int)blobs.size();
						stack.push_back(q);
					}
				}
			}
			blobs.push_back(std::move(blob));
			if ((int)blobs.size() >= kDetMaxCand) break;
		}
		return blobs;
	}

	// ---- det ----
	std::vector<Quad> detect(const int w, const int h, const BYTE* data)
	{
		// det 的输入尺寸：默认只缩不放（见常量区的实测），缩完按官方规则取整到 32 的倍数。
		// ⚠️ 唯一的例外是"太矮"的图要放大 —— 网络下采样 32 倍，高不到 kDetMinH 的话
		// 特征图只剩两三格，两行小字整个检不出来（实测 300×40 的两行原样进去一个框
		// 都没有、放大 2 倍后 100% 正确）。放大倍率同时受长边上限约束，别把宽度撑爆
		const int maxSide = std::max(w, h);
		float ratio{ 1.f };
		if (maxSide > kDetLimit) ratio = (float)kDetLimit / (float)maxSide;
		if ((float)h * ratio < (float)kDetMinH) {
			const float up = std::min((float)kDetMinH / ((float)h * ratio),
				(float)kDetLimit / ((float)maxSide * ratio));
			ratio *= std::max(up, 1.f);
		}
		const int rw = std::max(32, (int)llround(w * ratio / 32) * 32);
		const int rh = std::max(32, (int)llround(h * ratio / 32) * 32);
		std::vector<float> input;
		resizeChw(data, w, h, rw, rh, rw, true, input);
		std::vector<Ort::Value> outs;
		{
			Ort::AllocatorWithDefaultOptions allocator;
			auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
			std::array<int64_t, 4> shape{ 1, 3, rh, rw };
			Ort::Value tensor = Ort::Value::CreateTensor<float>(memInfo,
				input.data(), input.size(), shape.data(), shape.size());
			std::array<const char*, 1> inNames{ detIn.c_str() };
			std::array<const char*, 1> outNames{ detOut.c_str() };
			outs = detSess->Run(Ort::RunOptions{ nullptr }, inNames.data(), &tensor, 1,
				outNames.data(), outNames.size());
		}
		std::vector<int64_t> dims;
		const float* pred{ nullptr };
		// 概率图是那个四维、第二维为 1 的张量
		for (int i = 0; i < (int)outs.size(); ++i) {
			std::vector<int64_t> d;
			const float* p{ nullptr };
			if (!outTensor(outs, i, d, p)) continue;
			if (d.size() == 4 && d[1] == 1) { dims = d; pred = p; break; }
		}
		if (dims.size() != 4) return {};
		const int mw = (int)dims[3], mh = (int)dims[2];
		if (mw <= 0 || mh <= 0) return {};

		std::vector<uint8_t> mask((size_t)mw * mh);
		for (size_t i = 0; i < mask.size(); ++i) mask[i] = pred[i] > kDetThresh ? 1 : 0;

		std::vector<Quad> quads;
		for (auto& blob : findBlobs(mask, mw, mh)) {
			std::vector<Pt> pts;
			pts.reserve(blob.size());
			for (const int p : blob) pts.push_back({ (float)(p % mw), (float)(p / mw) });
			auto rect = minAreaRect(std::move(pts));
			if (std::min(rect.w, rect.h) < kDetMinSide) continue;
			if (boxScore(pred, mw, mh, rect) < kDetBoxThresh) continue;
			// 外扩：矩形四条边各推 d。det 给的是"字芯"，不扩一层会把笔画边缘切掉
			const auto d = rect.w * rect.h * kDetUnclip / (2.f * (rect.w + rect.h));
			rect.w += 2.f * d;
			rect.h += 2.f * d;
			auto q = rectToQuad(rect);
			// 概率图 -> 原图。中间那次缩放的两级比例正好约掉，直接按原图/图宽算
			const auto sx = (float)w / (float)mw, sy = (float)h / (float)mh;
			for (int i = 0; i < 4; ++i) {
				q.x[i] = std::clamp(q.x[i] * sx, 0.f, (float)w - 1.f);
				q.y[i] = std::clamp(q.y[i] * sy, 0.f, (float)h - 1.f);
			}
			orderQuad(q);
			quads.push_back(q);
		}
		// 阅读顺序：先按左上角 y、再按 x 排；挨得近的那几行（y 差不到 10 像素）按 x 微调
		std::sort(quads.begin(), quads.end(), [](const Quad& a, const Quad& b) {
			if (std::fabs(a.y[0] - b.y[0]) > 10.f) return a.y[0] < b.y[0];
			return a.x[0] < b.x[0];
			});
		for (int i = 0; i + 1 < (int)quads.size(); ++i) {
			for (int j = i; j >= 0; --j) {
				if (std::fabs(quads[j + 1].y[0] - quads[j].y[0]) < 10.f
					&& quads[j + 1].x[0] < quads[j].x[0]) std::swap(quads[j], quads[j + 1]);
				else break;
			}
		}
		return mergeNeighborQuads(std::move(quads));
	}

	// ---- rec ----
	struct RecOut
	{
		std::wstring text;
		std::vector<std::pair<float, float>> span;	// 每个字在"行宽"上的起止（0~1）
		float conf{ 0.f };
	};

	struct PassStats
	{
		bool oneLine{ false };
		int quads{ 0 };			// 检出来多少行框
		int textLines{ 0 };		// rec 真的吐出过文字的行数（不管置信度）
		int acceptedLines{ 0 };	// 置信度过线、最终纳入结果的行数
		int textChars{ 0 };
		float confSum{ 0.f };

		float avgConf() const { return textLines > 0 ? confSum / textLines : 0.f; }
		bool empty() const { return acceptedLines == 0 || textChars == 0; }
	};

	bool shouldRetry(const PassStats& s)
	{
		if (s.empty()) return true;
		if (s.oneLine) return s.avgConf() < kRetrySingleConf;
		if (s.avgConf() < kRetryMultiConf) return true;
		// 行框很多但过线的很少，常见于深色截图 / 低对比度 / 边缘裁得太紧
		return s.quads >= 2 && s.acceptedLines * 2 < s.quads;
	}

	bool retryBeats(const PassStats& base, const PassStats& retry)
	{
		if (retry.empty()) return false;
		if (base.empty()) return true;
		if (retry.acceptedLines > base.acceptedLines && retry.avgConf() + 0.02f >= base.avgConf()) return true;
		if (retry.textChars >= base.textChars + std::max(2, base.textChars / 5)
			&& retry.avgConf() + 0.02f >= base.avgConf()) return true;
		if (retry.avgConf() > base.avgConf() + 0.08f
			&& retry.textChars * 5 >= base.textChars * 4) return true;
		return false;
	}

	bool recognizeQuad(const int w, const int h, const BYTE* data, const Quad& q, RecOut& out)
	{
		// u = 宽方向（左上->右上），v = 高方向（左上->左下）
		float ux = q.x[1] - q.x[0], uy = q.y[1] - q.y[0];
		float vx = q.x[3] - q.x[0], vy = q.y[3] - q.y[0];
		auto cw = std::sqrt(ux * ux + uy * uy);
		auto ch = std::sqrt(vx * vx + vy * vy);
		if (cw < 1.f || ch < 1.f) return false;
		// 竖排文字（高比宽大不少）转 90° 再认：rec 只认横排。
		// 新宽方向 = 原来的高方向朝上，新高方向 = 原来的宽方向
		if (ch / cw >= 1.5f) {
			std::swap(ux, vx);
			std::swap(uy, vy);
			ux = -ux; uy = -uy;
			std::swap(cw, ch);
		}
		const auto ratio = cw / ch;
		// 官方 RecResizeImg：最小 320 宽，再跟着宽高比长；超出动态上限就截断
		const auto maxRatio = std::max(ratio, (float)kRecMinW / (float)kRecH);
		auto imgW = std::min((int)(kRecH * maxRatio), kRecMaxW);
		auto resizedW = (int)std::ceil(kRecH * ratio);
		if (resizedW > imgW) resizedW = imgW;
		imgW = std::max(imgW, 1);
		resizedW = std::clamp(resizedW, 1, imgW);

		std::vector<float> input;
		warpChw(data, w, h, q.x[0], q.y[0], ux, uy, vx, vy, resizedW, kRecH, imgW, input);
		std::vector<Ort::Value> outs;
		{
			Ort::AllocatorWithDefaultOptions allocator;
			auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
			std::array<int64_t, 4> shape{ 1, 3, kRecH, imgW };
			Ort::Value tensor = Ort::Value::CreateTensor<float>(memInfo,
				input.data(), input.size(), shape.data(), shape.size());
			std::array<const char*, 1> inNames{ recIn.c_str() };
			std::array<const char*, 1> outNames{ recOut.c_str() };
			outs = recSess->Run(Ort::RunOptions{ nullptr }, inNames.data(), &tensor, 1,
				outNames.data(), outNames.size());
		}
		std::vector<int64_t> dims;
		const float* p{ nullptr };
		if (!outTensor(outs, 0, dims, p)) return false;
		if (dims.size() != 3) return {};
		// 输出是 [1, T, C]：T 个时间步，每步 C 个字的概率
		const int T = (int)dims[1];
		const int C = (int)dims[2];
		if (T <= 0 || C <= 0) return {};
		// 一帧覆盖输入上 stride 列，靠它把"第几个字"换算回"图上的横向位置"
		const auto stride = (float)imgW / (float)T;
		out.text.clear();
		out.span.clear();
		out.conf = 0.f;
		// CTC 解码：**一段连续的同字帧就是一个字**。
		// ⚠️ 不能只记这一段里最后那一帧 —— 一个汉字在 48 高的图上要占四五帧，
		// 只取一帧的话每个字的框只有实际宽度的五分之一，切词时就会字字都当成一个词。
		// blank 段同样占帧，它把两个相同的字隔开（"A blank A" 该出两个 A），
		// 所以这里按"分段"收，而不是先去重再去 blank
		int cur{ -1 }, runA{ 0 }, runB{ -1 };
		double runSum{ 0 };
		int runCnt{ 0 };
		double sum{ 0 };
		int cnt{ 0 };
		// 输出下标 -> 字符：0 是 blank，1..dict.size() 是字典，**末位是空格**
		// （官方 CTCLabelDecode 在字典后补一个空格，输出维 = 1 + 字典 + 1）。
		// ⚠️ 别把末位当越界丢掉 —— 那就是把所有空格都丢了
		auto charAt = [&](const int best) -> std::wstring {
			if (best <= 0) return {};
			const auto idx = best - 1;
			if (idx < (int)dict.size()) return dict[idx];
			if (idx == (int)dict.size()) return L" ";
			return {};
		};
		auto emit = [&]() {
			if (cur <= 0 || runB < runA) return;
			const auto ch = charAt(cur);
			if (ch.empty()) return;
			const auto a = std::clamp((runA - 0.5f) * stride / resizedW, 0.f, 1.f);
			const auto b = std::clamp((runB + 0.5f) * stride / resizedW, 0.f, 1.f);
			out.text += ch;
			out.span.push_back({ a, b });
			sum += runSum;
			cnt += runCnt;
			};
		for (int t = 0; t <= T; ++t) {
			int best{ -1 };
			float bp{ 0 };
			if (t < T) {
				const auto row = p + (size_t)t * C;
				best = 0;
				bp = row[0];
				for (int c = 1; c < C; ++c) if (row[c] > bp) { bp = row[c]; best = c; }
			}
			// 还是同一个字：这段往后延一帧
			if (best == cur && t < T) {
				runB = t;
				runSum += bp;
				++runCnt;
				continue;
			}
			emit();
			cur = best;
			runA = runB = t;
			runSum = bp;
			runCnt = 1;
		}
		if (cnt > 0) out.conf = (float)(sum / cnt);
		return true;
	}

	// 一行字按 rec 认出来的空格切成词。
	//
	// 关于"字在横向上占哪儿"：CTC 的输出是**尖峰型**的 —— 一个字往往只在中间那一两帧
	// 上胜出，前后都是 blank。所以一帧只对应字距的几分之一（实测 36 号的汉字：字宽 37px、
	// 一帧只有 8px），拿"这一帧"当字的范围，每个字的框就只有实际的五分之一。
	// 处理办法：**按相邻峰值的中点给每个字划区间** —— 字与字之间严丝合缝铺满整行，
	// 宽窄自动跟着实际字距走（汉字宽、拉丁字母窄都照顾得到）。
	//
	// 断词只认空格（字典末位那个字符）。也试过"空隙偏大就切一刀"，但尖峰之间的
	// 空档本来就有一两帧，阈值怎么调都会在中文里乱切，得不偿失 —— 认不出空格时
	// 就整行当一个词，中文本来也该是这样
	void splitWords(const RecOut& rec, const Quad& q, const int line,
		std::vector<OcrModelBox>& out)
	{
		const int n = (int)rec.text.size();
		if (n == 0) return;
		// 每个字的峰值中心
		std::vector<float> c(n);
		for (int i = 0; i < n; ++i) c[i] = (rec.span[i].first + rec.span[i].second) / 2.f;
		// 头尾那半个字距：拿相邻峰距补上，框才不会在行首行尾短一截
		const float half = n > 1 ? std::max((c[1] - c[0]) / 2.f, 1e-4f) : 0.02f;
		auto left = [&](const int i) {
			return i > 0 ? (c[i - 1] + c[i]) / 2.f : std::max(c[0] - half, 0.f);
		};
		auto right = [&](const int i) {
			return i + 1 < n ? (c[i] + c[i + 1]) / 2.f : std::min(c[i] + half, 1.f);
		};
		// 行的两条边向量：把 0~1 的横向比例换算回原图像素
		const auto ux = q.x[1] - q.x[0], uy = q.y[1] - q.y[0];
		const auto vx = q.x[3] - q.x[0], vy = q.y[3] - q.y[0];
		auto boxAt = [&](const float a, const float b) {
			OcrModelBox box;
			box.x = q.x[0] + a * ux;
			box.y = q.y[0] + a * uy;
			box.w = std::max((b - a) * std::sqrt(ux * ux + uy * uy), 1.f);
			box.h = std::sqrt(vx * vx + vy * vy);
			box.line = line;
			return box;
		};
		int start{ 0 };
		auto flush = [&](const int end) {
			if (end <= start) return;
			auto box = boxAt(left(start), right(end - 1));
			box.text = rec.text.substr(start, end - start);
			out.push_back(std::move(box));
		};
		for (int i = 0; i < n; ++i) {
			if (rec.text[i] != L' ') continue;
			flush(i);
			start = i + 1;
		}
		flush(n);
	}

	// ---- 建会话 ----
	bool loadDict(const Ort::Session& sess, const std::filesystem::path& dir)
	{
		dict.clear();
		try {
			Ort::AllocatorWithDefaultOptions allocator;
			auto chars = sess.GetModelMetadata().LookupCustomMetadataMapAllocated(
				"character", allocator);
			if (chars && *chars) {
				std::string_view all{ chars.get() };
				size_t pos{ 0 };
				while (pos < all.size()) {
					const auto nl = all.find('\n', pos);
					const auto one = all.substr(pos, nl == std::string_view::npos ? nl : nl - pos);
					if (!one.empty()) dict.push_back(toWide(std::string{ one }));
					if (nl == std::string_view::npos) break;
					pos = nl + 1;
				}
			}
		}
		catch (...) {
		}
		// 模型里没带字典（有些导出不带）就找目录里的 txt，一行一个字
		if (dict.size() < 10) {
			dict.clear();
			std::error_code ec;
			for (std::filesystem::directory_iterator it{ dir, ec }, end{}; it != end; it.increment(ec)) {
				if (!it->is_regular_file() || it->path().extension() != L".txt") continue;
				std::ifstream f{ it->path() };
				std::string line;
				while (std::getline(f, line)) {
					while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
					if (!line.empty()) dict.push_back(toWide(line));
				}
				break;
			}
		}
		return dict.size() >= 10;
	}

	OcrModel::Err ensureLocked(const std::wstring& root, const std::wstring& name)
	{
		if (detSess && recSess && loadedName == name) return OcrModel::Err::ok;
		const std::filesystem::path dir = std::filesystem::path{ root } / name;
		const auto def = findDef(name);
		std::filesystem::path detPath, recPath;
		bool hasDet = findOnnx(dir, L"det", detPath);
		bool hasRec = findOnnx(dir, L"rec", recPath);
		// 缺哪个下哪个。认不出这一代（用户自己起的名字）就没得下 —— 那就只能用现成的
		if ((!hasDet || !hasRec) && !def) return OcrModel::Err::noModel;
		if (!hasDet && !download(def->det, dir / std::filesystem::path{ def->det }.filename())) {
			return OcrModel::Err::download;
		}
		if (!hasRec && !download(def->rec, dir / std::filesystem::path{ def->rec }.filename())) {
			return OcrModel::Err::download;
		}
		if (!hasDet) findOnnx(dir, L"det", detPath);
		if (!hasRec) findOnnx(dir, L"rec", recPath);
		if (detPath.empty() || recPath.empty()) return OcrModel::Err::noModel;
		try {
			detSess.reset();
			recSess.reset();
			env.reset(new Ort::Env{ ORT_LOGGING_LEVEL_ERROR, "tpixOcr" });
			Ort::SessionOptions opt;
			opt.SetIntraOpNumThreads(kThreads);
			detSess.reset(new Ort::Session{ *env, detPath.c_str(), opt });
			recSess.reset(new Ort::Session{ *env, recPath.c_str(), opt });
			if (!loadDict(*recSess, dir)) {
				detSess.reset(); recSess.reset();
				return OcrModel::Err::badModel;
			}
			Ort::AllocatorWithDefaultOptions allocator;
			detIn = detSess->GetInputNameAllocated(0, allocator).get();
			detOut = detSess->GetOutputNameAllocated(0, allocator).get();
			recIn = recSess->GetInputNameAllocated(0, allocator).get();
			recOut = recSess->GetOutputNameAllocated(0, allocator).get();
			loadedName = name;
			return OcrModel::Err::ok;
		}
		catch (...) {
			detSess.reset();
			recSess.reset();
			return OcrModel::Err::badModel;
		}
	}
}

std::vector<std::wstring> OcrModel::listModels(const std::wstring& root)
{
	std::vector<std::wstring> names;
	std::error_code ec;
	for (std::filesystem::directory_iterator it{ root, ec }, end{}; it != end; it.increment(ec)) {
		if (!it->is_directory()) continue;
		std::filesystem::path detPath, recPath;
		if (findOnnx(it->path(), L"det", detPath) && findOnnx(it->path(), L"rec", recPath))
			names.push_back(it->path().filename().wstring());
	}
	std::sort(names.begin(), names.end());
	// 内置那几代即使还没下下来也该出现在列表里 —— 选中它就会去下
	for (auto& d : kModelDefs) {
		if (std::find(names.begin(), names.end(), d.name) == names.end())
			names.insert(names.begin(), std::wstring{ d.name });
	}
	std::sort(names.begin(), names.end());
	return names;
}

std::wstring OcrModel::defaultModel()
{
	return std::wstring{ kDefModel };
}

OcrModel::Err OcrModel::ensure(const std::wstring& root, const std::wstring& name)
{
	std::lock_guard lock{ mtx };
	return ensureLocked(root, name);
}

OcrModel::Err OcrModel::recognize(const std::wstring& root, const std::wstring& name,
	const int w, const int h, BYTE* data, std::vector<OcrModelBox>& out)
{
	if (w <= 0 || h <= 0 || !data) return Err::runtime;
	std::lock_guard lock{ mtx };
	const auto err = ensureLocked(root, name);
	if (err != Err::ok) return err;
	try {
			auto runPass = [&](const int pw, const int ph, BYTE* pdata, std::vector<OcrModelBox>& boxes) {
				PassStats stats;
				boxes.clear();
				// 图里有几行字？矮的也得数 —— 小字号的两行（h 不到 48）同样是多行，
				// 整幅送 rec 一样吐空（见 countTextLines / kDetMinH）
				stats.oneLine = countTextLines(pw, ph, pdata) <= 1;
				if (stats.oneLine) {
					// 整张图就是一行：直接拉直送 rec，det 那一步纯属添乱（见常量区的实测）
					Quad q;
					q.x[0] = 0; q.y[0] = 0;
					q.x[1] = (float)pw - 1; q.y[1] = 0;
					q.x[2] = (float)pw - 1; q.y[2] = (float)ph - 1;
					q.x[3] = 0; q.y[3] = (float)ph - 1;
					stats.quads = 1;
					RecOut rec;
					if (recognizeQuad(pw, ph, pdata, q, rec) && !rec.text.empty()) {
						stats.textLines = 1;
						stats.textChars = (int)rec.text.size();
						stats.confSum = rec.conf;
						if (rec.conf >= kMinConf) {
							stats.acceptedLines = 1;
							splitWords(rec, q, 0, boxes);
						}
					}
					return stats;
				}
				// 多行：det 找行框、逐个送 rec。
				// ⚠️ 别想着并行 rec —— 试过（每线程一份 rec 会话，4 路×2 线程、8 路×1 线程
				// 都试了），8 核上**反而更慢**：rec 是内存带宽受限，多线程只增加争抢。
				// 实测整屏 25 行：串行 5.5s，并行 6.3~7.2s（2026-10-12）
				const auto quads = detect(pw, ph, pdata);
				stats.quads = (int)quads.size();
				for (int i = 0; i < (int)quads.size(); ++i) {
					RecOut rec;
					if (!recognizeQuad(pw, ph, pdata, quads[i], rec) || rec.text.empty()) continue;
					++stats.textLines;
					stats.textChars += (int)rec.text.size();
					stats.confSum += rec.conf;
					if (rec.conf < kMinConf) continue;
					++stats.acceptedLines;
					splitWords(rec, quads[i], i, boxes);
				}
				return stats;
			};

			std::vector<OcrModelBox> best;
			auto stats = runPass(w, h, data, best);
			if (shouldRetry(stats)) {
				// 首轮可疑时再跑一遍增强图：给紧裁边缘留白、把低对比截图轻拉开一点；
				// 平常清晰截图不走这里，避免把所有识别都拖慢
				auto retryImg = buildRetryImage(w, h, data);
				if (!retryImg.data.empty()) {
					std::vector<OcrModelBox> retry;
					auto retryStats = runPass(retryImg.w, retryImg.h, retryImg.data.data(), retry);
					if (retryBeats(stats, retryStats)) {
						unpadBoxes(retry, retryImg.padX, retryImg.padY, w, h);
						best = std::move(retry);
						stats = retryStats;
					}
				}
			}
			out = std::move(best);
		return Err::ok;
	}
	catch (...) {
		return Err::runtime;
	}
}

std::wstring OcrModel::curModel()
{
	std::lock_guard lock{ mtx };
	return loadedName;
}

void OcrModel::release()
{
	std::lock_guard lock{ mtx };
	detSess.reset();
	recSess.reset();
	env.reset();
	dict.clear();
	loadedName.clear();
}
