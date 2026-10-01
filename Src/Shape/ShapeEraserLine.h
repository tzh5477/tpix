#pragma once
#include <include/Ling.h>
#include "ShapeLineBase.h"
// 涂抹橡皮擦。工具条上那个矩形/涂抹开关决定走 ShapeEraserRect 还是这一类。
//
// "擦除"不是真的擦掉，是用原始底图把这条笔画盖回原样。
// 首尾夹点、拖端点、整体平移都在 ShapeLineBase，这里只管"什么时候算擦除生效"
class ShapeEraserLine : public ShapeLineBase
{
public:
	ShapeEraserLine(Canvas* win);
	~ShapeEraserLine();
	void paint(ID2D1DeviceContext* ctx) override;
	void mouseUp(const float x, const float y) override;
protected:
	// 几何一动就退出擦除态退回占位色。ShapeLineBase 每次改完点都调 makePath，
	// 覆写它顺手退状态，比在 mouseDrag 里再开一个钩子少一层
	void makePath() override;
private:
	// 拿窗口底图做的画刷。底图与窗口同尺寸同坐标，所以画刷不需要平移就能对齐
	Microsoft::WRL::ComPtr<ID2D1BitmapBrush> bgBrush;
	void resetEraser();
	void initBackgroundBrush();
private:
	// 要等几何定下来（mouseUp）才置位：之前显示占位色，之后才真正盖底图
	bool isErasing{ false };
};
