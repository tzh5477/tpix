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

// 真正跑起来的那个引擎。界面拿它决定"识别语言"那一行还有没有意义 ——
// **内置模型是多语种的，不需要选语言**，只有回落到系统 OCR 时语言包才起作用
enum class OcrEngine
{
	Model,		// PP-OCRv6（内置模型，见 OcrModel.h）
	System,		// Windows.Media.Ocr（系统自带的语言包）
};

// 内置模型上一次的状态。拿它区分"图里真没有字"和"模型没准备好" ——
// 两种都是空结果，不给用户看就看不出来
enum class OcrModelState
{
	ok,
	noModel,	// 选的那代模型目录里没有，内置清单里也没有它的下载地址
	download,	// 下载失败（没网 / 被拦）
	badModel,	// 文件在但用不了
	runtime,	// 跑的过程中出错
	unknown,	// 还没跑过
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

// 内置离线 OCR。**默认走自带的 PP-OCRv6 模型**（见 OcrModel.h：两个 ONNX 文件，
// 首次使用时下到数据目录，之后一直离线可用），它不挑系统语言、中英日韩都能认；
// 模型没下下来、或者坏了的时候**回落到 Windows.Media.Ocr**，那条路要求用户在
// "设置 - 时间和语言 - 语言"里装过识别包。两条路都不可用时 isAvailable() 才是 false，
// 调用方应当退回原来的插件路径，而不是弹一句"识别失败"。
//
// 对外接口与换引擎之前完全一致 —— WinOcr / 贴图窗口选文 / 表格填字都是照旧调用，
// 换引擎这件事对它们是透明的（表格那条路也因此一并换成了新模型）。
//
// ⚠️ **回落那条路（Windows.Media.Ocr）有个硬门槛**：图像总高度不到 40 像素就一个字
// 都认不出来（与字号无关，见 Ocr.cpp 里 kMinHeight 处的实测）。内置模型的 rec 本来就
// 把行归一化到固定 48 高，没有这个门槛。两条路都由本类内部处理，**调用方什么都不用做**；
// 词框坐标也已经换算回原图（内置模型给的本来就是原图坐标），调用方拿到的
// 始终是入参图那套坐标系。
class Ocr
{
public:
	// 两条路里有没有一条能用。现在内置模型这条路始终可用（缺文件会自己下），
	// 所以这里实际上只在"模型下不来、系统也没装语言包"时才是 false
	static bool isAvailable();
	// 当前会跑起来的那个引擎
	static OcrEngine engine();
	// 内置模型上一次的状态（给界面在空结果时区分原因）
	static OcrModelState modelState();
	// 数据目录 ocr\ 下可选的模型（含还没下载的内置代次）。用户把 PP-OCRv7 的一对
	// onnx 丢进 ocr\PP-OCRv7\ 就会出现在这里 —— 换模型不用改代码
	static std::vector<std::wstring> models();
	static std::wstring curModel();
	static void setModel(const std::wstring& name);
	// 已装的**系统 OCR** 识别语言。一个都没装时返回空。
	// ⚠️ 只有回落（engine() == System）时它才起作用，内置模型是多语种的、不用选语言
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
