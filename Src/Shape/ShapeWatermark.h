#pragma once
#include <include/Ling.h>
#include "ShapeBase.h"

// 文字水印。单击落一个水印层：居中一块，或者平铺整张底图（ToolSub 上切）。
// 样式（文字 / 字号 / 颜色 / 透明度 / 平铺 / 旋转）每次画的时候直接从工具条读，
// 所以工具条上改任何一项，图上已画的水印立刻跟着变，不需要 applyStyle 重排
class ShapeWatermark : public ShapeBase
{
public:
	ShapeWatermark(Canvas* win);
	~ShapeWatermark();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	// 单击就是它的正常用法：落一个水印层，不需要拖动
	bool isValidWithoutDrag() override { return true; };
	void setCursor() override;
	void mouseDown(const float x, const float y) override;
	// 只有水印工具下才吃悬停：别的工具下它不是"图上某一块"，而是整张图的背景层，
	// 一旦参与命中就会把其它工具的每一下点击都截胡（见 .cpp 里的说明）
	void mouseMove(const float x, const float y) override;
private:
	// 按当前样式建文字布局。返回 false 表示没有可画的东西（没写文字）
	bool makeLayout();
	// 在 (x, y) 处以 rotation 角度画一次文字，布局以该点为中心。
	// outer 是外层已有的变换（屏幕上是缩放、导出时是单位阵），必须左乘保住
	void drawOne(ID2D1DeviceContext* ctx, float x, float y, float rotation,
		const D2D1_MATRIX_3X2_F& outer);
	float cx{ 0.f }, cy{ 0.f };
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	// 缓存 makeLayout 的结果，paint 里判断"有没有东西可画"用
	float textW{ 0.f }, textH{ 0.f };
};
