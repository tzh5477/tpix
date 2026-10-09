#pragma once
#include <include/Ling.h>
#include "ShapeLineBase.h"
#include "ShapeMosaicPaint.h"
// 涂抹马赛克。拖首端、拖末端、整体平移都在 ShapeLineBase，
// 这里只负责"动过之后把重画一遍那一路的画面打成马赛克"
class ShapeMosaicLine : public ShapeLineBase
{
public:
	ShapeMosaicLine(Canvas* win);
	~ShapeMosaicLine();
	void paint(ID2D1DeviceContext* ctx) override;
	void mouseUp(const float x, const float y) override;
	void applyStyle() override;
	// 复制（见 ShapeBase::clone）
	bool copyable() const override { return true; }
	std::unique_ptr<ShapeBase> clone(const float dx, const float dy, Canvas* target = nullptr) const override;
	std::unique_ptr<ShapeBase> snapshot() const override;
protected:
	void fixupCopy() override;
	void translate(const float dx, const float dy) override;
	// 几何一动，之前算好的马赛克就不对了。ShapeLineBase 每次改完点都调 makePath，
	// 覆写它顺手把马赛克丢掉，比在 mouseDrag 里再开一个钩子少一层
	void makePath() override;
private:
	Microsoft::WRL::ComPtr<ID2D1BitmapBrush> mosaicBrush;
	void resetMosaic();
	void buildMosaic();
private:
	ShapeMosaicPaint mosaicPaint;
};
