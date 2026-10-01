#include "pch.h"
#include <algorithm>
#include <wincodec.h>
#include "AnimImage.h"
#include "Util.h"

using Microsoft::WRL::ComPtr;

namespace {
	// GIF 把"这一帧停多久"和"这一帧放完之后画布怎么处理"放在 Graphic Control Extension 里。
	// 延时单位是 1/100 秒；有些图写成 0 或 1（想让它飞快），浏览器惯例是按 100ms 兜住，
	// 这里照办 —— 真按 0 播会闪得没法看。上下限都压一下：上限免得一张图定住好几秒，
	// 下限 40ms（25fps）—— 换一帧要整窗重绘一遍，再快就只剩白烧 CPU 了
	void readFrameMeta(IWICBitmapFrameDecode* frame, UINT& delayMs, UINT& disposal)
	{
		delayMs = 100;
		disposal = 1;   // 1 = 不做处理，下一帧叠在上面
		ComPtr<IWICMetadataQueryReader> reader;
		if (FAILED(frame->GetMetadataQueryReader(reader.GetAddressOf()))) return;
		PROPVARIANT val{};
		PropVariantInit(&val);
		if (SUCCEEDED(reader->GetMetadataByName(L"/grctlext/Delay", &val)) && val.vt == VT_UI2) {
			delayMs = std::clamp<UINT>(val.uiVal * 10u, 40u, 1000u);
		}
		PropVariantClear(&val);
		PropVariantInit(&val);
		if (SUCCEEDED(reader->GetMetadataByName(L"/grctlext/Disposal", &val)) && val.vt == VT_UI1) {
			disposal = val.bVal;
		}
		PropVariantClear(&val);
	}
}

bool AnimImage::load(const std::wstring& path, std::vector<AnimFrame>& frames)
{
	frames.clear();
	ComPtr<IWICImagingFactory> factory;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(factory.GetAddressOf())))) return false;
	ComPtr<IWICBitmapDecoder> decoder;
	// 用 OnLoad：下面要逐帧读元数据，OnDemand 下每一帧都要回头解析一遍文件，慢且容易漏
	if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
		WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()))) return false;
	UINT count{ 0 };
	if (FAILED(decoder->GetFrameCount(&count)) || count < 2) return false;
	ComPtr<IWICBitmapFrameDecode> firstFrame;
	if (FAILED(decoder->GetFrame(0, firstFrame.GetAddressOf()))) return false;
	UINT w{ 0 }, h{ 0 };
	if (FAILED(firstFrame->GetSize(&w, &h)) || w == 0 || h == 0) return false;

	// 帧全部预先解成 BGRA 放着：播的时候再解会在主线程上抖（一帧几十毫秒，正好卡在定时器上）。
	// 代价是内存，所以按单帧大小封顶帧数 —— 总预算约 96MB，够几百帧的小图、几十帧的大图。
	// 单帧就超预算的直接放弃：解两帧下去就是几百 MB，不如按静态图贴
	constexpr size_t budget{ 96'000'000 };
	const auto frameBytes = (size_t)w * h * 4;
	if (frameBytes > budget) return false;
	auto maxFrames = std::min<UINT>(count, (UINT)(budget / frameBytes));

	// GIF 的帧常常只画了变化的那块，其余是透明的，得自己按帧序累积成完整的画面
	std::vector<BYTE> canvas(frameBytes, 0);
	std::vector<BYTE> cur;
	DWORD dw{ 0 }, dh{ 0 };
	bool clearBeforeNext{ false };
	for (UINT i = 0; i < maxFrames; ++i)
	{
		ComPtr<IWICBitmapFrameDecode> frame;
		if (FAILED(decoder->GetFrame(i, frame.GetAddressOf()))) break;
		UINT fw{ 0 }, fh{ 0 };
		// 尺寸对不上就不接着解：贴图窗口的底图尺寸是钉死在第一帧上的，混着解会画歪
		if (FAILED(frame->GetSize(&fw, &fh)) || fw != w || fh != h) break;
		if (!Util::decodeWicFrame(frame.Get(), cur, dw, dh)) break;
		// disposal = 2 是"这帧显示完把画布清回背景"，所以清发生在下一帧合成之前
		if (clearBeforeNext) std::fill(canvas.begin(), canvas.end(), BYTE{ 0 });
		// GIF 没有半透明，alpha 非 0 即覆盖；0 表示这一帧没画到这儿，沿用画布上已有的
		for (size_t p = 0; p < frameBytes; p += 4)
		{
			if (cur[p + 3] != 0) {
				canvas[p + 0] = cur[p + 0];
				canvas[p + 1] = cur[p + 1];
				canvas[p + 2] = cur[p + 2];
				canvas[p + 3] = cur[p + 3];
			}
		}
		AnimFrame af;
		af.pixels = canvas;
		af.w = w;
		af.h = h;
		UINT disposal{ 1 };
		readFrameMeta(frame.Get(), af.delayMs, disposal);
		frames.push_back(std::move(af));
		clearBeforeNext = (disposal == 2);
	}
	// 只解出一帧就没必要当动图（退化成静态图，调用方按老路子贴）
	if (frames.size() < 2) {
		frames.clear();
		return false;
	}
	return true;
}
