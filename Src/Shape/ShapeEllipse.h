#pragma once
#include <include/Ling.h>
#include "ShapeRectBase.h"
// 矩形族里唯一需要 rect 之外几何的那个：画的时候 D2D 收的是 cx/cy/rx/ry，
// 命中判定又是椭圆环带而不是一圈直边。
// 所以这里只覆写"几何怎么派生"和"怎么命中"这两类钩子，八向手柄整套照搬 ShapeRectBase
class ShapeEllipse : public ShapeRectBase
{
public:
	ShapeEllipse(Canvas* win);
	~ShapeEllipse();
	void paint(ID2D1DeviceContext* ctx) override;
	void mouseWheel(const float x, const float y, const short delta) override;
	void applyStyle() override;
protected:
	void hitBody(const float x, const float y) override;
	void syncFromRect() override;
	int firstDraggerIndex() const override;
private:
	// 都给初值：mouseDown 只记按下点，cx/cy/rx/ry 要等第一次 mouseDrag 才算出来，
	// 而这之间已经可能来一次 paint（刷新时机不受控），不初始化就是拿垃圾值画椭圆
	float cx{ 0.f }, cy{ 0.f }, rx{ 0.f }, ry{ 0.f };
};
