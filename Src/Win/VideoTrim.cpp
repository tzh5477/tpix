#include "pch.h"
#include "VideoTrim.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mftransform.h>
// ICodecAPI 在 strmif.h 里，经 dshow.h 拉进来（VideoMp4.hpp 也是这么拿的）；
// codecapi.h 只提供 CODECAPI_* 那些 GUID
#include <dshow.h>
#include <codecapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <format>

using Microsoft::WRL::ComPtr;

namespace {

	// 转码 / 解码线程自己起 MF 与 COM，自己收。
	// CoInitializeEx 返回 RPC_E_CHANGED_MODE 说明这条线程已经是别的套间模式了（比如
	// UI 线程那种 STA）—— 那不是错误，直接用，且**不要**去 CoUninitialize（谁初始化谁收尾）
	struct MfScope
	{
		HRESULT com{ E_FAIL };
		HRESULT mf{ E_FAIL };
		MfScope()
		{
			com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			mf = MFStartup(MF_VERSION);
		}
		~MfScope()
		{
			if (SUCCEEDED(mf)) MFShutdown();
			if (SUCCEEDED(com)) CoUninitialize();
		}
		bool ok() const { return SUCCEEDED(mf) && (SUCCEEDED(com) || com == RPC_E_CHANGED_MODE); }
	};

	std::wstring hrText(const wchar_t* what, HRESULT hr)
	{
		return std::format(L"{} (0x{:08X})", what, (unsigned)hr);
	}

	// 找出视频流与音频流的**真实索引**。
	// 不用 MF_SOURCE_READER_FIRST_VIDEO_STREAM 当索引往下走：那两个常量只在
	// SetCurrentMediaType / ReadSample 里被 MF 解释，拿到手的还是真实索引，
	// 而 SinkWriter 那边的流号得我们自己映射 —— 所以这里直接按类型把索引摸清楚，
	// 后面的选择、设类型、映射全用真实索引，少一层"猜"
	bool findStreams(IMFSourceReader* rd, DWORD& vIdx, DWORD& aIdx)
	{
		vIdx = (DWORD)-1;
		aIdx = (DWORD)-1;
		for (DWORD i = 0; i < 8; ++i) {
			ComPtr<IMFMediaType> t;
			if (FAILED(rd->GetNativeMediaType(i, 0, t.GetAddressOf())) || !t) continue;
			GUID major{};
			if (FAILED(t->GetGUID(MF_MT_MAJOR_TYPE, &major))) continue;
			if (major == MFMediaType_Video && vIdx == (DWORD)-1) vIdx = i;
			else if (major == MFMediaType_Audio && aIdx == (DWORD)-1) aIdx = i;
		}
		return vIdx != (DWORD)-1;
	}

	// 把两路之外的所有流摘掉，ReadSample(ANY_STREAM) 才不会冒出别的东西
	void deselectOthers(IMFSourceReader* rd, DWORD vIdx, DWORD aIdx)
	{
		for (DWORD i = 0; i < 8; ++i) {
			bool keep = (i == vIdx) || (i == aIdx);
			rd->SetStreamSelection(i, keep ? TRUE : FALSE);
		}
	}

	bool readSize(IMFMediaType* t, UINT32& w, UINT32& h)
	{
		return SUCCEEDED(MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h)) && w > 0 && h > 0;
	}

	double readFps(IMFMediaType* t)
	{
		UINT32 num = 0, den = 0;
		if (FAILED(MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &num, &den)) || num == 0 || den == 0)
			return 30.0;
		return double(num) / double(den);
	}

	bool isHevc(const GUID& sub)
	{
		return sub == MFVideoFormat_HEVC || sub == MFVideoFormat_HEVC_ES || sub == MFVideoFormat_H265;
	}

	bool readDuration(IMFSourceReader* rd, long long& ms)
	{
		PROPVARIANT var;
		PropVariantInit(&var);
		auto hr = rd->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var);
		bool ok = false;
		if (SUCCEEDED(hr) && var.vt == VT_UI8) {
			ms = (long long)(var.uhVal.QuadPart / 10000);   // 100ns -> ms
			ok = true;
		}
		PropVariantClear(&var);
		return ok;
	}

	// 一帧 BGRA 拷出来（RGB32 在内存里就是 BGRX）。
	// 走 IMF2DBuffer 是为了拿 stride：它是负数时图是自下而上排的，得逐行倒着拷
	bool copyFrame(IMFSample* smp, int w, int h, std::vector<BYTE>& out)
	{
		ComPtr<IMFMediaBuffer> buf;
		if (FAILED(smp->ConvertToContiguousBuffer(buf.GetAddressOf())) || !buf) return false;
		out.resize((size_t)w * h * 4);
		ComPtr<IMF2DBuffer> b2d;
		BYTE* scan = nullptr;
		LONG stride = 0;
		if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(b2d.GetAddressOf()))) && b2d &&
			SUCCEEDED(b2d->Lock2D(&scan, &stride)) && scan) {
			for (int y = 0; y < h; ++y) {
				const BYTE* row = (stride >= 0) ? (scan + (size_t)y * stride)
					: (scan + (size_t)(h - 1 - y) * (-stride));
				memcpy(out.data() + (size_t)y * w * 4, row, (size_t)w * 4);
			}
			b2d->Unlock2D();
			return true;
		}
		// 没有 2D 缓冲：当它连续紧密排布（MF 给 RGB32 时一般也确实是）
		BYTE* p = nullptr;
		DWORD len = 0;
		if (FAILED(buf->Lock(&p, nullptr, &len)) || !p) return false;
		size_t need = (size_t)w * h * 4;
		memcpy(out.data(), p, std::min<size_t>(len, need));
		buf->Unlock();
		return true;
	}

	// 打开一个"只要视频、输出 RGB32"的源读取器（预览用）
	//
	// ⚠️ 这里**只能**开 MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING 一个：
	// 它和 MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING 同时设进属性表，
	// MFCreateSourceReaderFromURL 会直接返回 E_INVALIDARG(0x80070057) —— 2026-10-10 实测，
	// 八个变体里唯有"两个都设"这一种建 reader 就失败（裸路径 / file:// / ByteStream 三条都一样）。
	// 而只开 VIDEO_PROCESSING 又喂不出 RGB32（SetCurrentMediaType 回 MF_E_INVALIDMEDIATYPE
	// 0xC00D36B4，那套老色彩转换 DSP 只认有限几种配对）。ADVANCED 走 Video Processor MFT，
	// 色彩空间转换 + 缩放的配对都支持，是唯一能出 RGB32 的那条。
	bool openVideoReader(const std::wstring& path, ComPtr<IMFSourceReader>& rd,
		DWORD& vIdx, UINT32& w, UINT32& h, double& fps, std::wstring& err)
	{
		ComPtr<IMFAttributes> attrs;
		MFCreateAttributes(attrs.GetAddressOf(), 1);
		attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
		auto hr = MFCreateSourceReaderFromURL(path.c_str(), attrs.Get(), rd.GetAddressOf());
		if (FAILED(hr) || !rd) {
			err = hrText(L"打不开录制文件", hr);
			return false;
		}
		DWORD aIdx = (DWORD)-1;
		if (!findStreams(rd.Get(), vIdx, aIdx)) {
			err = L"文件里没有视频流";
			return false;
		}
		ComPtr<IMFMediaType> native;
		hr = rd->GetNativeMediaType(vIdx, 0, native.GetAddressOf());
		if (FAILED(hr) || !native || !readSize(native.Get(), w, h)) {
			err = hrText(L"读不到视频尺寸", hr);
			return false;
		}
		fps = readFps(native.Get());
		ComPtr<IMFMediaType> out;
		MFCreateMediaType(out.GetAddressOf());
		out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
		MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, w, h);
		MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, (UINT32)(fps * 1000 + 0.5), 1000);
		MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
		out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		hr = rd->SetCurrentMediaType(vIdx, nullptr, out.Get());
		if (FAILED(hr)) {
			err = hrText(L"解码器不接受 RGB32 输出", hr);
			return false;
		}
		rd->SetStreamSelection(vIdx, TRUE);
		return true;
	}
}

struct VideoTrim::Reader::Impl
{
	ComPtr<IMFSourceReader> rd;
	DWORD vIdx{ (DWORD)-1 };
	UINT32 w{ 0 }, h{ 0 };
	bool atEnd{ false };
};

VideoTrim::Reader::Reader() : d(std::make_unique<Impl>()) {}

VideoTrim::Reader::~Reader() = default;

bool VideoTrim::Reader::isOpen() const { return d->rd != nullptr; }

void VideoTrim::Reader::close()
{
	d->rd.Reset();
	d->atEnd = false;
}

bool VideoTrim::Reader::open(const std::wstring& path, std::wstring& err)
{
	close();
	double fps = 30.0;
	if (!openVideoReader(path, d->rd, d->vIdx, d->w, d->h, fps, err)) return false;
	inf.w = (int)d->w;
	inf.h = (int)d->h;
	inf.fps = fps;
	readDuration(d->rd.Get(), inf.durMs);
	ComPtr<IMFMediaType> cur;
	if (SUCCEEDED(d->rd->GetNativeMediaType(d->vIdx, 0, cur.GetAddressOf())) && cur) {
		GUID sub{};
		cur->GetGUID(MF_MT_SUBTYPE, &sub);
		inf.hevc = isHevc(sub);
	}
	DWORD aIdx = (DWORD)-1;
	for (DWORD i = 0; i < 8 && aIdx == (DWORD)-1; ++i) {
		ComPtr<IMFMediaType> t;
		if (FAILED(d->rd->GetNativeMediaType(i, 0, t.GetAddressOf())) || !t) continue;
		GUID major{};
		t->GetGUID(MF_MT_MAJOR_TYPE, &major);
		if (major == MFMediaType_Audio) { aIdx = i; inf.hasAudio = true; }
	}
	return true;
}

bool VideoTrim::Reader::next(std::vector<BYTE>& bgra, long long& ptsMs)
{
	if (!d->rd || d->atEnd) return false;
	for (;;) {
		DWORD idx = 0, flags = 0;
		LONGLONG ts = 0;
		ComPtr<IMFSample> smp;
		auto hr = d->rd->ReadSample(d->vIdx, 0, &idx, &flags, &ts, &smp);
		if (FAILED(hr)) { d->atEnd = true; return false; }
		if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { d->atEnd = true; return false; }
		if (!smp) continue;                    // 流间隙，接着要
		if (!copyFrame(smp.Get(), (int)d->w, (int)d->h, bgra)) { d->atEnd = true; return false; }
		ptsMs = (long long)(ts / 10000);
		return true;
	}
}

bool VideoTrim::Reader::seek(long long ms, std::vector<BYTE>& bgra, long long& ptsMs)
{
	if (!d->rd) return false;
	PROPVARIANT var;
	PropVariantInit(&var);
	var.vt = VT_I8;
	var.hVal.QuadPart = ms * 10000;
	auto hr = d->rd->SetCurrentPosition(GUID_NULL, var);
	PropVariantClear(&var);
	if (FAILED(hr)) return false;
	d->atEnd = false;
	// seek 只能落到"不大于目标时间的关键帧"上，接着顺序解到目标位置。
	// 上限 600 帧是保险丝：30fps 下那已经是 20 秒的 GOP，正常文件远到不了
	const LONGLONG want = ms * 10000;
	for (int i = 0; i < 600; ++i) {
		if (!next(bgra, ptsMs)) return false;
		if (ptsMs * 10000 >= want) return true;
	}
	return true;
}

bool VideoTrim::probe(const std::wstring& path, Info& out, std::wstring& err)
{
	MfScope mf;
	if (!mf.ok()) { err = L"MediaFoundation 初始化失败"; return false; }
	Reader r;
	if (!r.open(path, err)) return false;
	out = r.info();
	return true;
}

long VideoTrim::trim(const TrimParams& p, std::wstring& err)
{
	MfScope mf;
	if (!mf.ok()) { err = L"MediaFoundation 初始化失败"; return -1; }

	// ---- 源：视频 +（可选）音频 ----
	ComPtr<IMFAttributes> rattrs;
	MFCreateAttributes(rattrs.GetAddressOf(), 1);
	// 解码要走 Video Processor MFT：它能把任何源格式转成我们要的 NV12/RGB32，也能顺带缩放。
	// **不要**再加 MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING** —— 两个一起设建 reader 就
	// E_INVALIDARG（与 openVideoReader 上面那段注释同一个坑）。
	rattrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
	ComPtr<IMFSourceReader> rd;
	auto hr = MFCreateSourceReaderFromURL(p.src.c_str(), rattrs.Get(), rd.GetAddressOf());
	if (FAILED(hr) || !rd) { err = hrText(L"打不开录制文件", hr); return -2; }

	DWORD vIdx = (DWORD)-1, aIdx = (DWORD)-1;
	if (!findStreams(rd.Get(), vIdx, aIdx)) { err = L"文件里没有视频流"; return -3; }
	deselectOthers(rd.Get(), vIdx, aIdx);

	ComPtr<IMFMediaType> vnat;
	hr = rd->GetNativeMediaType(vIdx, 0, vnat.GetAddressOf());
	UINT32 w = 0, h = 0;
	if (FAILED(hr) || !vnat || !readSize(vnat.Get(), w, h)) { err = hrText(L"读不到视频尺寸", hr); return -4; }
	GUID srcSub{};
	vnat->GetGUID(MF_MT_SUBTYPE, &srcSub);
	const double srcFps = readFps(vnat.Get());

	// 视频解码输出：优先 NV12（编码器的原生输入，省一次色彩空间来回），
	// 不行再要 RGB32 —— 录制侧喂给编码器的就是 RGB32，那条路在本项目里是验证过的。
	// NV12 要求宽高都是偶数，奇数就干脆别试
	GUID inFmt{};
	bool gotFmt = false;
	const GUID tryFmts[] = { MFVideoFormat_NV12, MFVideoFormat_RGB32 };
	for (auto f : tryFmts) {
		if (f == MFVideoFormat_NV12 && ((w & 1) || (h & 1))) continue;
		ComPtr<IMFMediaType> t;
		MFCreateMediaType(t.GetAddressOf());
		t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		t->SetGUID(MF_MT_SUBTYPE, f);
		MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, w, h);
		MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, (UINT32)(srcFps * 1000 + 0.5), 1000);
		MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
		t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		if (SUCCEEDED(rd->SetCurrentMediaType(vIdx, nullptr, t.Get()))) { inFmt = f; gotFmt = true; break; }
	}
	if (!gotFmt) { err = L"解码器不接受任何可用的输出格式"; return -5; }

	// 音频解码输出：统一到录制侧那一套（44.1k / 2ch / 16bit PCM）。
	// 源里没有音轨、或者解不成 PCM，都只是"导出成无声视频"，不算失败
	bool hasAudio = false;
	if (aIdx != (DWORD)-1) {
		ComPtr<IMFMediaType> t;
		MFCreateMediaType(t.GetAddressOf());
		t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
		t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
		t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
		t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
		t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
		t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 44100 * 4);
		if (SUCCEEDED(rd->SetCurrentMediaType(aIdx, nullptr, t.Get()))) hasAudio = true;
	}

	// 源里那一路视频实际是什么编码：导出沿用它，别偷偷换成另一种。
	// fps 也照源取（TrimParams::fps 只是兜底默认值）—— 裁剪不该改变帧率
	const bool hevc = isHevc(srcSub);
	const UINT32 fpsNum = (UINT32)((srcFps > 0 ? srcFps : 30.0) * 1000 + 0.5);

	// ---- 目标 ----
	ComPtr<IMFAttributes> wattrs;
	MFCreateAttributes(wattrs.GetAddressOf(), 2);
	wattrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
	wattrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);

	ComPtr<IMFSinkWriter> wr;
	DWORD vStream = 0, aStream = 0;

	// 建"一整份"SinkWriter 并配好两条流。HEVC 装不上时换 H.264 重跑一遍 ——
	// 必须**重建** SinkWriter：上一次已经把视频流加进去了，在同一个实例上再 AddStream
	// 会多出一条没有输入类型的流，Finalize 时直接失败
	auto buildWriter = [&](const GUID& vcodec, std::wstring& buildErr) -> long {
		wr.Reset();
		auto hr2 = MFCreateSinkWriterFromURL(p.dst.c_str(), nullptr, wattrs.Get(), wr.GetAddressOf());
		if (FAILED(hr2) || !wr) { buildErr = hrText(L"建不了输出文件", hr2); return -6; }

		ComPtr<IMFMediaType> vout;
		MFCreateMediaType(vout.GetAddressOf());
		vout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		vout->SetGUID(MF_MT_SUBTYPE, vcodec);
		vout->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)p.brKbps * 1000);
		MFSetAttributeSize(vout.Get(), MF_MT_FRAME_SIZE, w, h);
		MFSetAttributeRatio(vout.Get(), MF_MT_FRAME_RATE, fpsNum, 1000);
		MFSetAttributeRatio(vout.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
		vout->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		if (isHevc(vcodec)) vout->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_Normal);
		hr2 = wr->AddStream(vout.Get(), &vStream);
		if (FAILED(hr2)) { buildErr = hrText(L"这台机器装不上视频编码器", hr2); return -7; }

		ComPtr<IMFMediaType> vin;
		MFCreateMediaType(vin.GetAddressOf());
		vin->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		vin->SetGUID(MF_MT_SUBTYPE, inFmt);
		MFSetAttributeSize(vin.Get(), MF_MT_FRAME_SIZE, w, h);
		MFSetAttributeRatio(vin.Get(), MF_MT_FRAME_RATE, fpsNum, 1000);
		MFSetAttributeRatio(vin.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
		vin->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		hr2 = wr->SetInputMediaType(vStream, vin.Get(), nullptr);
		if (FAILED(hr2)) { buildErr = hrText(L"编码器不接受解码输出", hr2); return -8; }

		// 码率控制：与录制侧同款（质量模式 + 质量档）
		ComPtr<ICodecAPI> ca;
		if (SUCCEEDED(wr->GetServiceForStream(vStream, GUID_NULL, __uuidof(ICodecAPI), (void**)ca.GetAddressOf())) && ca) {
			VARIANT v{};
			v.vt = VT_UI4;
			v.ulVal = eAVEncCommonRateControlMode_Quality;
			ca->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
			VARIANT v2{};
			v2.vt = VT_UI4;
			v2.ulVal = (ULONG)std::clamp(p.quality, 0, 100);
			ca->SetValue(&CODECAPI_AVEncCommonQuality, &v2);
		}

		if (hasAudio) {
			ComPtr<IMFMediaType> aout;
			MFCreateMediaType(aout.GetAddressOf());
			aout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
			aout->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
			aout->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
			aout->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
			aout->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
			aout->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192 * 1000 / 8);
			hr2 = wr->AddStream(aout.Get(), &aStream);
			if (FAILED(hr2)) { buildErr = hrText(L"装不上音频编码器", hr2); return -9; }
			ComPtr<IMFMediaType> ain;
			MFCreateMediaType(ain.GetAddressOf());
			ain->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
			ain->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
			ain->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
			ain->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
			ain->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
			ain->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
			ain->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 44100 * 4);
			hr2 = wr->SetInputMediaType(aStream, ain.Get(), nullptr);
			if (FAILED(hr2)) { buildErr = hrText(L"音频编码器不接受 PCM 输入", hr2); return -10; }
		}
		return 0;
	};

	// 源是 HEVC 而编码器装不上时退 H.264：与录制侧同一条退让规则
	{
		std::wstring first;
		long rc = buildWriter(hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264, first);
		if (rc != 0 && hevc) {
			std::wstring second;
			long rc2 = buildWriter(MFVideoFormat_H264, second);
			if (rc2 == 0) rc = 0; else first = second;
		}
		if (rc != 0) { err = first; return rc; }
	}

	hr = wr->BeginWriting();
	if (FAILED(hr)) { err = hrText(L"编码器起不来", hr); return -11; }

	// ---- 转码主循环 ----
	// 全程 ANY_STREAM 交错读：视频与音频按时间戳交替出来，直接喂给 SinkWriter，
	// 由它自己复用交错写入。**不要**分两趟（先写完全部视频再写音频）——
	// MP4 的索引要求交错，MF 那一侧会先把整条流缓存在内存里，长视频直接吃爆
	long long durMs = 0;
	readDuration(rd.Get(), durMs);
	const LONGLONG inT = p.inMs * 10000;
	const LONGLONG outT = (p.outMs > p.inMs) ? p.outMs * 10000 : LLONG_MAX;
	// 进度要有分母：裁剪到结尾时拿源时长当分母（只用于显示，不参与停止判断）
	const LONGLONG totalT = (outT != LLONG_MAX) ? outT : (durMs > 0 ? durMs * 10000 : inT);
	const bool canProgress = totalT > inT;
	LONGLONG lastReport = 0;
	bool vDone = false, aDone = false;
	long long written = 0;

	for (;;) {
		if (p.cancelled && p.cancelled()) { err = L"已取消"; return -12; }
		DWORD idx = 0, flags = 0;
		LONGLONG ts = 0;
		ComPtr<IMFSample> smp;
		hr = rd->ReadSample(MF_SOURCE_READER_ANY_STREAM, 0, &idx, &flags, &ts, &smp);
		if (FAILED(hr)) { err = hrText(L"读取源文件出错", hr); return -13; }
		if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
		if (!smp) continue;

		const bool isVideo = (idx == vIdx);
		if (isVideo && vDone) continue;
		if (!isVideo && (!hasAudio || aDone)) continue;

		LONGLONG pts = 0;
		smp->GetSampleTime(&pts);
		LONGLONG sdur = 0;
		smp->GetSampleDuration(&sdur);

		if (pts >= outT) {
			// 这一路到头了。**另一路要继续读完** —— 交错流里两路不会同时越过终点，
			// 提前 break 会把另一路结尾那一两帧（音频约 23ms）切掉，听感就是"尾巴被咬了一口"
			if (isVideo) vDone = true; else aDone = true;
			if (vDone && (!hasAudio || aDone)) break;
			continue;
		}
		if (pts + sdur <= inT) continue;   // 整段落在裁剪点之前，丢

		// 时间轴平移到 0。起手那一帧可能跨过裁剪点（音频帧尤其），钳到 0 就行，
		// 代价不超过一帧（AAC 1024 样本 ≈ 23ms）
		smp->SetSampleTime(std::max<LONGLONG>(0, pts - inT));
		hr = wr->WriteSample(isVideo ? vStream : aStream, smp.Get());
		if (FAILED(hr)) { err = hrText(L"写入输出文件出错", hr); return -14; }
		if (isVideo) {
			++written;
			if (p.onProgress && canProgress) {
				LONGLONG done = pts - inT;
				if (done - lastReport > 4000000) {   // 0.4 秒媒体时间报一次，别把 UI 刷爆
					lastReport = done;
					p.onProgress(std::clamp(float(double(done) / double(totalT - inT)), 0.f, 1.f));
				}
			}
		}
	}

	if (written == 0) { err = L"这段里一帧都没有"; return -15; }

	hr = wr->Finalize();
	if (FAILED(hr)) { err = hrText(L"封盘失败", hr); return -16; }
	if (p.onProgress) p.onProgress(1.f);
	return 0;
}
