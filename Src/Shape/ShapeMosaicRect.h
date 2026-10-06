#pragma once
#include <include/Ling.h>
#include "ShapeRectBase.h"
#include "ShapeMosaicPaint.h"
// 矩形马赛克。工具条"马赛克"按钮的三档（见 ToolSub::mosaicMode）里，
// 除了"涂抹"那一档，其余两档都走矩形几何，所以都在这一类里：
// 0 = 普通矩形打码，2 = 智能擦除（用选区周围的背景色整块盖住）。
//
// 八向手柄、整体拖动、Shift 约束整套来自 ShapeRectBase，这里只负责
// "几何定下来之后把那块画面打成马赛克 / 取背景色"
class ShapeMosaicRect : public ShapeRectBase
{
public:
	ShapeMosaicRect(Canvas* win);
	~ShapeMosaicRect();
	void paint(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void applyStyle() override;
	// 复制（见 ShapeBase::clone）
	bool copyable() const override { return true; }
	std::unique_ptr<ShapeBase> clone(const float dx, const float dy) const override;
protected:
	void fixupCopy() override;
	void translate(const float dx, const float dy) override;
private:
	// 打好的马赛克画刷。与 eraseBrush 互斥：一个 shape 只会走到其中一条路上
	Microsoft::WRL::ComPtr<ID2D1BitmapBrush> mosaicBrush;
	// 智能擦除：打听选区周围那圈出现最多的颜色当背景色，用它铺满选区
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> eraseBrush;
	void resetMosaic();
	void buildMosaic();
	void buildErase();
private:
	bool isErase{ false };
	ShapeMosaicPaint mosaicPaint;
};
