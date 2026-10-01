#pragma once
#include <include/Ling.h>
#include "ShapeRectBase.h"
// 矩形橡皮擦。工具条上那个矩形/涂抹开关决定走这一类还是 ShapeEraserLine。
//
// "擦除"不是真的擦掉，是用原始底图把这一块盖回原样。
// 八向手柄来自 ShapeRectBase，这里只管"什么时候算擦除生效"
class ShapeEraserRect : public ShapeRectBase
{
public:
	ShapeEraserRect(Canvas* win);
	~ShapeEraserRect();
	void paint(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
private:
	// 拿窗口底图做的画刷。底图与窗口同尺寸同坐标，所以画刷不需要平移就能对齐
	Microsoft::WRL::ComPtr<ID2D1BitmapBrush> bgBrush;
	void resetEraser();
	void initBackgroundBrush();
private:
	// 要等几何定下来（mouseUp）才置位：之前显示占位色，之后才真正盖底图
	bool isErasing{ false };
};
