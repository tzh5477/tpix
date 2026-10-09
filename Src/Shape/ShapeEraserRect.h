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
	// 圆角那一枚内部手柄对橡皮没有意义：它改的是"矩形画出来圆不圆角"，
	// 而橡皮画的是"把这一块盖回底图"—— 边缘跟 radius 一点关系都没有。
	// 画出来只会让人以为能调（作者报的"橡皮擦不需要显示圆角调整的标记点"）
	bool hasInnerHandles() const override { return false; }
	// 复制对橡皮也没有意义（"再盖一块一模一样的"等于没盖），所以左上角那枚复制按钮不给它。
	// 基类默认就是 false，这里显式写出来：一是把"橡皮不可复制"这条意图钉在类头上，
	// 二是别让后来的人顺手在基类把 copyable 放宽时把橡皮也带进去
	bool copyable() const override { return false; }
	std::unique_ptr<ShapeBase> snapshot() const override;
private:
	// 拿窗口底图做的画刷。底图与窗口同尺寸同坐标，所以画刷不需要平移就能对齐
	Microsoft::WRL::ComPtr<ID2D1BitmapBrush> bgBrush;
	void resetEraser();
	void initBackgroundBrush();
private:
	// 要等几何定下来（mouseUp）才置位：之前显示占位色，之后才真正盖底图
	bool isErasing{ false };
};
