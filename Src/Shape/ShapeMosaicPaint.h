#pragma once
#include <include/Ling.h>
// 马赛克 / 智能擦除共用的取像素管线。
//
// 矩形马赛克、涂抹马赛克都要做同一件笨事：把自己的几何范围算出来，把"这个 shape 之前"
// 的画面在那一块里重现一遍、读回内存、按色块重排，再包成一个画刷。这一大段与几何无关
// （矩形用 rect，涂抹用 path 的外扩包围盒），所以抽在这里，由两个马赛克变体各自持一份。
//
// 它只认 Canvas，不认窗口：几何范围由调用方量好了传进来，装上自己的画刷。
class ShapeBase;
// 离屏重现要"画到自己之前为止"，所以得知道宿主是哪一个 shape；
// 宿主是持有它的那个 shape 自己，只存指针不参与生命周期，析构顺序由宿主保证
class ShapeMosaicPaint
{
public:
	ShapeMosaicPaint(Canvas* win, ShapeBase* self);
	// 把 bounds 范围内（四周各外扩 expand 像素再夹到窗口内）的画面读回来打成马赛克，
	// 包成一个已经平移到 bounds 左上角的位图刷。返回空表示这次没算出来（几何退化、
	// 底图不在、读回失败），此时调用方继续用占位色画
	Microsoft::WRL::ComPtr<ID2D1BitmapBrush> makeMosaicBrush(const D2D1_RECT_F bounds, const int expand);
	// 智能擦除要打听的不是"选区里平均什么颜色"而是"选区周围是什么颜色"：
	// 十行文字里文字只占很少像素，取均值会偏灰，取众数才稳定命中底色。
	// 返回 false 表示外面那圈没取到（整块被窗口边界裁没了时退化成含选区一起取，见实现）
	bool sampleBgColor(const D2D1_RECT_F bounds, const int ringPx, D2D1_COLOR_F& out);
	// 把 bounds 这块画面读回来（**不**马赛克），坐标换算与取像素完全沿用 makeMosaicBrush
	// 那条管线，区别只在于这里不动像素。给调用方自己做像素分析用。
	//
	// 为什么不是"按需一小块一小块读"：智能打码要对**每个字**各算一份"墨的列投影"，
	// 逐字各走一趟 GPU 离屏重现 + 拷回太贵 —— 一次读整块，调用方在内存里按行切
	bool readback(const D2D1_RECT_F bounds, std::vector<BYTE>& pixels, UINT32& pitch,
		UINT32& width, UINT32& height, D2D1_POINT_2F& origin);
	// 量出"落在 band 这一行里、以 seedX 为中心的那一团墨"的左右边界（图像 x，含端点）。
	//
	// 为什么需要它：rec 给的字框是**按相邻峰值中点把整行均分**出来的（见 OcrModel::splitWords），
	// 每个字拿到的是"一格"而不是"笔画本身"。宽窄差得多的两个字挨着时，格边界会切到宽字身上 ——
	// "A1234567" 里数字 1 的那一格左边界落在 A 的右腿上（实测偏 19px），照框打码就把邻字啃掉一块。
	// 这里以"这个字真正的峰值中心"（OcrWord::cx）当种子，按列数墨、向两侧扩，遇到 blank 个
	// 连续空列就停 —— 既不会把邻字的笔画算进来，也不会在字内笔画之间那点小缝里断开。
	//
	// **墨是相对底色判的**，不是写死一个亮度阈值：截图可能是深色主题（底暗字亮），
	// "暗于 110 算墨"那种判据会把整片底当成字。底色取 band 内亮度的众数，偏离它超过 kInkDelta 才算墨。
	//
	// pixels/pitch/width/height/origin 是 readback 那一份；band 与 seedX 是**图像坐标**。
	// 返回 false 表示这一格里一点墨都没量到（或整个 band 落在缓冲之外），调用方据此回落识别框
	static bool inkRunInBand(const std::vector<BYTE>& pixels, const UINT32 pitch, const UINT32 width,
		const UINT32 height, const D2D1_POINT_2F origin, const int seedX, const int bandTop,
		const int bandBottom, const int blank, const int maxExtend, int& outLeft, int& outRight);
private:
	// 用 d2d->deviceContext 做离屏绘制是安全的：它是全项目共享的资源工厂，
	// 没有任何地方给它 SetTarget / BeginDraw（窗口绘制走的是各自 surface 或 swap chain 的 context）。
	// 这里 SetTarget → BeginDraw → EndDraw → SetTarget(nullptr) 在函数内闭环，不跨帧持有。
	bool renderBackground(const D2D1_RECT_F bounds, const int expand, std::vector<BYTE>& pixels,
		UINT32& pitch, UINT32& width, UINT32& height, D2D1_POINT_2F& origin);
	// 按 blockSize 分块，每块取平均色再整块填回去 —— 就是马赛克
	void mosaicPixels(BYTE* bits, UINT32 pitch, UINT32 width, UINT32 height, int blockSize);
private:
	Canvas* win;
	ShapeBase* self;
};
