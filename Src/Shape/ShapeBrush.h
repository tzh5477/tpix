#pragma once
#include <include/Ling.h>
#include "ShapeLineBase.h"
// 画笔（自由手绘）。几何与交互整套借 ShapeLineBase —— 它那套就是"每个鼠标位置塞一个顶点 +
// 圆头圆角描边"，正是手绘要的。这里只管"画出来什么样"，两种画法共用同一个类：
//   画笔   —— 圆头圆角、不透明，就是一支笔
//   荧光笔 —— 同一笔，但半透明（压在文字上底下的字还看得见），线帽改成平口、拐角改成尖角，
//             像一支马克笔划过去
// 为什么不拆成两个派生类：两种画法只差"颜色 alpha + 描边样式"，几何一模一样，而**切类型
// 时已选中的那一笔要能就地换个画法** —— 对象的类型换不了，两个类就做不到这一点
class ShapeBrush : public ShapeLineBase
{
public:
	ShapeBrush(Canvas* win);
	void paint(ID2D1DeviceContext* ctx) override;
	void applyStyle() override;
	void applyToolStyle() override;
	// 光标停在笔画上滚滚轮 = 调线宽，与线条 / 矩形同一套（走 ToolSub 那张滑块值域表）
	bool mouseWheel(const float x, const float y, const short delta) override;
	bool copyable() const override { return true; }
	std::unique_ptr<ShapeBase> clone(const float dx, const float dy, Canvas* target = nullptr) const override;
	std::unique_ptr<ShapeBase> snapshot() const override;
private:
	// 荧光笔画法。新建时读一次工具条上的「类型」那一档，换档位时由 applyToolStyle 再读一次。
	// 只有新画的那一笔读，applyStyle（改颜色 / 滚轮调粗细）不重读 ——
	// 否则切一次类型，之后再回头改任意一笔的颜色都会把它也染成荧光笔
	bool highlighter{ false };
};
