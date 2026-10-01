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
