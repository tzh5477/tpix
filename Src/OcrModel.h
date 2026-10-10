#pragma once
#include <string>
#include <vector>

// 一个"词"和它在图里的位置（像素，左上为原点，与 recognize 的入参图同一坐标系）。
//
// 词是**一行字再按空格切开**得来的：rec 是把整行当一张图认的，认出来的空格本身就是
// 一个字符，按它切就是词。中文没有空格，所以中文一行就是一个词 —— 这比 Windows OCR
// 把每个汉字都切成词要合理（那边得靠 ocrNeedSpace 事后把空格去掉）
struct OcrModelBox
{
	std::wstring text;
	float x{ 0 }, y{ 0 }, w{ 0 }, h{ 0 };
	// 第几行（从上到下、行内从左到右）。同一行的词这个数一样，调用方拿它分行
	int line{ 0 };
};

// 内置离线 OCR 引擎：PP-OCRv6（small，ONNX）。
//
// 两个模型文件放在 <数据目录>\ocr\<模型名>\ 下，一个管"哪里有字"（det）、一个管
// "这行是什么字"（rec）。**目录名就是模型名**，里面只要能找到一对 det / rec 的
// onnx 就能用 —— 用户自己下的、或者将来 PP-OCRv7 的，往里一丢、在设置里选一下即可，
// 不用改代码（内置清单只为"缺文件时知道去哪儿下"，加一代就是加一行）。
//
// 与系统 OCR 的分工见 Ocr.h：**这里优先跑，跑不起来才回落到 Windows.Media.Ocr**。
//
// ⚠️ 两条路线（见 OcrModel.cpp 里 kSingleLine* 处的实测）：
//   单行区域（矮、或很扁）→ 跳过 det，整图直接送 rec（又准又快）；
//   多行区域 → det 找行框 → 逐框裁剪 + rec。
//   把矮条送进 det 会被它的"短边补到 736"规则放大几十倍，框碎在字缝里、还慢。
class OcrModel
{
public:
	enum class Err
	{
		ok,			// 认完了（可能一个字都没有）
		noModel,	// 目录里没有这一代模型，而且内置清单里也没有它的下载地址
		download,	// 下载失败（没网 / 被拦 / 盘写不进去）
		badModel,	// 文件在但用不了（不是这两个模型 / 没带字典）
		runtime,	// 跑的过程中出错
	};

	// root 目录下每个子目录算一个模型，里面有 det / rec 两个 onnx 就算可用。
	// 按名字排（PP-OCRv6 在 PP-OCRv7 前面 —— 默认选第一个）
	static std::vector<std::wstring> listModels(const std::wstring& root);
	// 内置清单里登记的默认模型名
	static std::wstring defaultModel();
	// 建好会话。缺文件就按内置清单下载；已经建过的同一代不会重建。
	// 要联网下载时会走 WinRT 的 HttpClient —— 调用线程得已经 init_apartment（同 Table）
	static Err ensure(const std::wstring& root, const std::wstring& name);
	// 认一张 BGRA top-down 的图，返回按阅读顺序排好的词。可能耗时几百毫秒，别在 UI 线程调
	static Err recognize(const std::wstring& root, const std::wstring& name,
		const int w, const int h, BYTE* data, std::vector<OcrModelBox>& out);
	// 当前会话用的是哪一代；没建过给空
	static std::wstring curModel();
	// 放掉会话（退出时调，同 Table 的做法，不调也没事）
	static void release();
};
