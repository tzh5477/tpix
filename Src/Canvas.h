#pragma once
#include <include/Ling.h>

class History;
class ShapeBase;
class ToolMain;
class ToolSub;

// 画布宿主：窗口侧那点状态（尺寸、DPI、当前工具、文本框、刷新）的唯一出口。
// Canvas 与 Shape 层只认这十件事，不认 WinPin —— 将来要给别的窗口（图片编辑器之类）
// 挂同一套标注能力，实现一个 CanvasHost 就够了，Shape 那层一行都不用动。
class CanvasHost
{
public:
	virtual ~CanvasHost() = default;
	virtual float dpiValue() const = 0;
	virtual float scaleValue() const = 0;
	virtual float widthValue() const = 0;
	virtual float heightValue() const = 0;
	// 当前选中的工具名（ToolMain::curId）。History 按它决定建哪种 shape
	virtual const std::wstring& curToolId() const = 0;
	virtual ToolMain* getToolMain() = 0;
	// 形状样式的来源：线宽、颜色、填充、马赛克模式、橡皮擦是涂抹还是矩形，都在 ToolSub 上
	virtual ToolSub* getToolSub() = 0;
	// 编辑中文字用的共用文本框。它得挂在窗口的合成树上，所以由宿主建
	virtual Ling::TextBox* getTextBox() = 0;
	virtual void setEditingShape(ShapeBase* shape) = 0;
	virtual void requestRefresh() = 0;
};

// 一块画布：底图 + 标注图层（shapes / undo）+ 悬停与正在画的那个元素。
//
// 由窗口组合持有，不是继承 —— 标注能力从此挂在画布上，不钉死在某个窗口上。
// 下面公开成员的命名刻意沿用 WinPin 上那批同名的（screenImg / history / shapeHover /
// refresh / getTextBox / setEditingShape），Shape 层因此几乎不用改，搬运过程中
// 漏一处就现形：ShapeBase 的 win 是 Canvas*，WinPin 上的同名成员它一个都看不见。
class Canvas
{
public:
	Canvas(CanvasHost* host);
	~Canvas();
	// 底图位图。ShapeMosaic / ShapeEraser 把它当取样源，形状与像素格式都跟它一致
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> screenImg;
	// 标注图层。undo / redo 与 shape 的生命周期都在这里
	std::unique_ptr<History> history;
	// 鼠标悬停上的那个元素
	ShapeBase* shapeHover{ nullptr };
	// 本次按下新建出来的那个元素（还没抬手）。抬手时按"有没有画出东西"决定留不留
	ShapeBase* shapeCur{ nullptr };
	// —— 窗口侧状态的转发 ——
	float getDpi() const;
	float getScale() const;
	float getWidth() const;
	float getHeight() const;
	// 当前工具名，与 ToolMain::curId 同一个值
	const std::wstring& getCurToolId() const;
	void refresh();
	ToolMain* getToolMain() const;
	ToolSub* getToolSub() const;
	// 编辑中文字由这个文本框画，位置与样式由 shape 自己指定
	Ling::TextBox* getTextBox() const;
	void setEditingShape(ShapeBase* shape);
	// 底图的像素尺寸，也就是导出图的尺寸
	D2D1_SIZE_U getImgSize() const;
private:
	CanvasHost* host{ nullptr };
};
