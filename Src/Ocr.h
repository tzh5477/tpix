#pragma once
#include <string>
#include <vector>

// 系统里装好的一种识别语言
struct OcrLang
{
	// BCP-47 语言标签，如 L"zh-Hans-CN"，传给 recognize 用来指定引擎
	std::wstring tag;
	// 给人看的显示名（系统按界面语言给的，如"中文(简体)"）
	std::wstring name;
};

// 一个词和它在图里的位置（像素，左上为原点，与 recognize 的入参图同一坐标系）。
// 表格识别拿它填格子：整张图只认一次，再按中心点落进各自的格子里
struct OcrWord
{
	std::wstring text;
	float x{ 0 }, y{ 0 }, w{ 0 }, h{ 0 };
};

// 相邻两段文字之间要不要补一个空格。
//
// **为什么需要它**：Windows OCR 把每个汉字当成一个独立的词（实测"退出 tpix 后重跑
// 一次构建才真正落地。"回来 17 个词，其中 15 个是单字），而 OcrLine::Text() 是拿
// 空格把这些词拼起来的 —— 直接用它，中文就成了"退 出 tpix 后 重 跑 …"。
// 英文同样中招：同一张图会把 "World" 拆成 "Wo" + "rld"。
//
// 判据（2026-10-07 实测，3 种字体 × 6 个字号 × 7 段样本，独立探测程序直接驱动引擎）：
//
// 1. **任一端是标点 ⇒ 一定不补**。引擎把标点单独切成一个词，而且认错的码点比汉字还
//    五花八门：同一个句号 U+3002 被认成中点 U+00B7、半角逗号 U+002C 被认成全角
//    U+FF0C。标点两侧补空格就是"落地 。"，比认错码点难看得多。
// 2. **两端都是中日韩汉字 ⇒ 一定不补**。汉字之间本来就没有空格，这是主判据，
//    纯靠它就已经修掉了"退 出 tpix 后 重 跑"。
// 3. 其余情况看**图上到底有没有空白**：relGap = 两段之间的像素间隙 / 字高。
//    实测有真空格时 relGap ≥ 0.29，而引擎无中生有的拆分只有 0.07~0.11。
//    纯几何单独用不可靠（少数样本里真空格的 relGap 也低到 0.06），所以只当辅助。
//
// relGap 由调用方从两个词的 OcrWord 算出；拿不到就传 -1 表示未知，此时只用规则 1、2，
// 行为退回到与旧的字符类判断一致（宁可在中英混排处多补一个空格，也不拆散汉字）。
bool ocrNeedSpace(wchar_t left, wchar_t right, float relGap);

// 内置离线 OCR，走 Windows.Media.Ocr（系统自带，不用带模型文件，联网也不需要）。
//
// 前提：用户在"设置 - 时间和语言 - 语言"里装了某个语言的识别包。一个都没装时
// isAvailable() 返回 false，调用方应当退回原来的插件路径，而不是弹一句"识别失败"。
//
// ⚠️ 引擎有个硬门槛：**图像总高度不到 40 像素就一个字都认不出来**（与字号无关，
// 见 Ocr.cpp 里 kMinHeight 处的实测）。一行文字紧裁出来正好在门槛之下，所以
// "截图高度小于一行 ⇒ 提示没有识别到文字"是这个门槛造成的，不是识别不出小字。
// 本类内部会把过矮的图双线性放大到 48 高再送进引擎，**调用方什么都不用做**；
// 词框坐标也已经换算回原图，调用方拿到的仍是入参图那套坐标系。
class Ocr
{
public:
	// 系统里有没有可用的识别引擎
	static bool isAvailable();
	// 已装的识别语言。一个都没装时返回空 —— 调用方应当据此退回插件路径
	static std::vector<OcrLang> languages();
	// 识别一张 BGRA top-down 的图，返回按行拼起来的文字；认不出来返回空串。
	// langTag 为空时按用户的语言档案挑引擎，挑不到就退回第一个已装的语言。
	// 可能耗时几百毫秒到几秒，别在 UI 线程上直接调
	static std::wstring recognize(const int w, const int h, BYTE* data,
		const std::wstring& langTag = L"");
	// 同上，但逐个词给出位置。词比行细，跨格的那些行也能拆开归位。
	// 坐标是**原图**那套（内部放大过，已换算回来），可以直接拿去和形状/格子比对。
	// 可能耗时几百毫秒到几秒，别在 UI 线程上直接调
	static std::vector<OcrWord> recognizeWords(const int w, const int h, BYTE* data,
		const std::wstring& langTag = L"");
};
