#pragma once
#include <include/Ling.h>
class Canvas;
class ShapeBase
{
public:
	ShapeBase(Canvas* win);
	virtual ~ShapeBase();
	virtual void paint(ID2D1DeviceContext* ctx) = 0;
	virtual void paintDragger(ID2D1DeviceContext* ctx) {};
	virtual void mouseMove(const float x, const float y) { };
	virtual void mouseDrag(const float x, const float y) {};
	virtual void mouseDown(const float x, const float y) {};
	virtual void mouseUp(const float x, const float y) {};
	virtual void mouseWheel(const float x, const float y, const short delta) {};
	virtual void setCursor() {};
	// 选中态下的按键。目前只有序号用它（+/- 改编号、F2 编辑追加的描述文本）
	virtual void onKey(UINT key) {};
	// 编辑态收尾。Canvas 导出图片前、History 删掉 shape 前都会调，
	// 只有会进编辑态的元素（ShapeText / ShapeNumber）实现，其余留空
	virtual void finishEditing() {};
	// ToolSub 上的颜色 / 字号 / 序号样式变了，重新从工具条取样式并重建自己的画刷与几何
	virtual void applyStyle() {};
	// 新建这一笔如果只是按下马上弹起（没有拖动），默认当成什么也没画，元素直接丢掉。
	// 单击本身就是正常用法的元素（number 落徽章、text 进编辑）覆盖它返回 true
	virtual bool isValidWithoutDrag() { return false; };
	bool isInRect(const D2D1_RECT_F rect, const float x, const float y);
public:
	// 所属画布。窗口尺寸、DPI、底图、工具条样式、刷新全从这里出 ——
	// shape 不认识窗口，换一个画布宿主这层照旧能挂上去
	Canvas* win;
	bool isUndo{ false };
	int hoverDraggerIndex{ -1 };
	// 建这一笔时的工具 id，由 History::createShape 填。
	// 「应用到全部」拿它筛同类 —— ToolSub 的颜色是每个工具各存一份的，
	// 跨类型套样式会让文字、序号被矩形的那个颜色污染
	std::wstring toolId;
protected:
	float draggerSize;
	// 控制点的浅蓝描边 + 选中时的白色填充（样式参考 pixpin：浅蓝空心框压在标注线上，
	// 不填白的话线从框中间穿过去，一排点看着全是花的）
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushDragger;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushDraggerFill;
private:
};
