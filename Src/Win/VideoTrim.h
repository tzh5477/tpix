#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

// 录后裁剪：MediaFoundation 转码（解码 -> 按时间过滤 -> 重编码）。
//
// 为什么不复用 VideoMp4.hpp：那份是"采集 -> 编码"单向的，内部只有一个
// IMFSinkWriter，**没有读回 / 解码 / 解析能力**（已全文确认）。要编辑录好的文件，
// 得反过来从 IMFSourceReader 出发，两套管线除了"都用 MF"之外没有可共用的东西 ——
// 硬塞进一个文件只会让那一大坨更难读，所以单独一份。
//
// 编码参数刻意与录制侧对齐（见 VideoMp4.hpp 的 AddStream / ICodecAPI 两段）：
// 同分辨率、同 fps、同"质量模式 + 质量档"，音频同 44.1k / 2ch / 192k。裁剪过一遍
// 不该让人看出画面规格变了。
namespace VideoTrim {

	// 源文件的基本信息（只读文件头，不解码）
	struct Info
	{
		long long durMs{ 0 };
		int w{ 0 }, h{ 0 };
		double fps{ 30.0 };
		bool hasAudio{ false };
		bool hevc{ false };   // 源是 HEVC：导出沿用同一编码，免得"录的是 HEVC 导出成了 H.264"
	};

	// 读源文件头。失败返回 false，err 带一句话原因
	bool probe(const std::wstring& path, Info& out, std::wstring& err);

	struct TrimParams
	{
		std::wstring src;
		std::wstring dst;
		// 裁剪区间（毫秒）。outMs <= inMs 视为"一直取到结尾"
		long long inMs{ 0 };
		long long outMs{ 0 };
		// 编码参数，默认值就是录制侧那一套
		int brKbps{ 4000 };
		int quality{ 50 };
		int fps{ 30 };
		// 进度 0..1（按视频时间戳算，可能被高频调用，UI 侧自己节流）
		std::function<void(float)> onProgress;
		// 返回 true = 用户取消。取消时本函数返回非 0，dst 是半成品，由调用方删
		std::function<bool()> cancelled;
	};

	// 转码。返回 0 成功；非 0 失败（err 带原因）
	long trim(const TrimParams& p, std::wstring& err);

	// 预览解码器：按时间取帧，给编辑窗画预览用。
	//
	// ⚠️ 调用它的线程必须自己 CoInitializeEx(COINIT_MULTITHREADED) —— 这份实现
	// 不做线程亲和性管理，谁调谁负责初始化。WinVideoEdit 里全部调用都发生在
	// 它自己那条解码线程上。
	class Reader
	{
	public:
		Reader();
		~Reader();
		bool open(const std::wstring& path, std::wstring& err);
		void close();
		bool isOpen() const;
		const Info& info() const { return inf; }
		// 顺序取下一帧（BGRA，自上而下）。到头 / 出错返回 false
		bool next(std::vector<BYTE>& bgra, long long& ptsMs);
		// 定位到"时间戳 >= ms"的第一帧。mf 会先 seek 到之前的关键帧再往后解，
		// 所以耗时与 GOP 长度有关（30fps 下通常几十毫秒）
		bool seek(long long ms, std::vector<BYTE>& bgra, long long& ptsMs);
	private:
		struct Impl;
		std::unique_ptr<Impl> d;
		Info inf;
	};
}
