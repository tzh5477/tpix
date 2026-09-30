#pragma once
#include <include/Ling.h>
class WinPin;
class ShapeBase
{
public:
	ShapeBase(WinPin* win);
	virtual ~ShapeBase();
	virtual void paint(ID2D1DeviceContext* ctx) = 0;
	virtual void paintDragger(ID2D1DeviceContext* ctx) {};
	virtual void mouseMove(const float x, const float y) { };
	virtual void mouseDrag(const float x, const float y) {};
	virtual void mouseDown(const float x, const float y) { };
	virtual void mouseUp(const float x, const float y) { };
	virtual void mouseWheel(const float x, const float y, const short delta) {};
	virtual void setCursor() {};
	// 选中态下的按键。目前只有序号用它（+/- 改编号、F2 编辑序号里的文字）
	virtual void onKey(UINT key) {};
	// 编辑态收尾。WinPin 导出图片前、History 删掉 shape 前都会调，
	// 只有会进编辑态的元素（ShapeText / ShapeNumber）实现，其余留空
	virtual void finishEditing() {};
	// ToolSub 上的颜色 / 字号 / 序号样式变了，重新从工具条取样式并重建自己的画刷与几何
	virtual void applyStyle() {};
	// 新建这一笔如果只是按下马上弹起（没有拖动），默认当成什么也没画，元素直接丢掉。
	// 单击本身就是正常用法的元素（number 落徽章、text 进编辑）覆盖它返回 true
	virtual bool isValidWithoutDrag() { return false; };
	bool isInRect(const D2D1_RECT_F rect, const float x, const float y);
public:
	WinPin* win;
	bool isUndo{ false };
	int hoverDraggerIndex{ -1 };
protected:
protected:
	float draggerSize;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushDragger;
private:
};

