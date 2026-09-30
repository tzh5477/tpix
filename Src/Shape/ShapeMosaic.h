#pragma once
#include <include/Ling.h>
#include "ShapeBase.h"
class ShapeMosaic : public ShapeBase
{
public:
	ShapeMosaic(WinPin* win);
	~ShapeMosaic();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void setCursor() override;
public:
private:
	void makePath();
	void resetMosaic();
	void updateDraggers();
	void buildMosaicBitmap();
	// 智能擦除：打听选区周围一圈出现最多的颜色当背景色，用它铺满选区
	void buildEraseBrush();
	// 取背景色。先看选区周围那条环带，环带被窗口边界裁没了就退化为整块的众数
	bool sampleBgColor(D2D1_COLOR_F& out);
	// 把"这个 shape 之前"的画面在包围盒（四周各外扩 expand 像素）范围内重现一遍并读回内存。
	// 出参给出读回的像素与原点在窗口中的位置。马赛克取样与智能擦除取色共用这一条读回路。
	bool renderBackground(const int expand, std::vector<BYTE>& pixels,
		UINT32& pitch, UINT32& width, UINT32& height, D2D1_POINT_2F& origin);
	Microsoft::WRL::ComPtr<ID2D1Bitmap> createMosaicBitmap(int blockSize);
	void mosaicPixels(BYTE* bits, UINT32 pitch, UINT32 width, UINT32 height, int blockSize);
	float pointToSegmentDistance(const D2D1_POINT_2F& p, const D2D1_POINT_2F& a, const D2D1_POINT_2F& b);
	void hitTest(const D2D1_POINT_2F& mousePos);
private:
	D2D1_RECT_F rect{ 0.f,0.f,0.f,0.f };
	std::vector<D2D1_RECT_F> draggers;
	std::vector<D2D1_POINT_2F> linePoints;
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
	// 圆头圆角描边，涂抹模式下让笔画首尾和拐点是圆的
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> roundStyle;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	Microsoft::WRL::ComPtr<ID2D1Bitmap> mosaicBitmap;
	Microsoft::WRL::ComPtr<ID2D1BitmapBrush> mosaicBrush;
	// 智能擦除的实色画刷。与 mosaicBrush 互斥：一个 shape 只会走到其中一条路上
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> eraseBrush;
	// 马赛克位图只覆盖笔画包围盒那一小块，这是它左上角在窗口里的位置。
	// mosaicBrush 要按它平移，否则贴图会跑到窗口原点去。
	D2D1_POINT_2F mosaicOrigin{ 0.f, 0.f };
	float strokeWidth{ 1.f }, pressX{ 0.f }, pressY{ 0.f };
	bool isRect{ false };
	// 智能擦除：不是把画面打码糊掉，而是用选区周围的背景色整块盖住，视觉上让内容消失
	bool isErase{ false };
};
