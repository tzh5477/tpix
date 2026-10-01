#include "pch.h"
#include "Canvas.h"
#include "History.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"

// History 是 unique_ptr 成员，构造它要完整类型，析构它同样要 —— 都挤在 .cpp 里
Canvas::Canvas(CanvasHost* host) : host{ host }, history{ std::make_unique<History>(this) }
{
}

Canvas::~Canvas()
{
}

float Canvas::getDpi() const
{
	return host->dpiValue();
}

float Canvas::getScale() const
{
	return host->scaleValue();
}

float Canvas::getWidth() const
{
	return host->widthValue();
}

float Canvas::getHeight() const
{
	return host->heightValue();
}

const std::wstring& Canvas::getCurToolId() const
{
	return host->curToolId();
}

void Canvas::refresh()
{
	host->requestRefresh();
}

ToolMain* Canvas::getToolMain() const
{
	return host->getToolMain();
}

ToolSub* Canvas::getToolSub() const
{
	return host->getToolSub();
}

Ling::TextBox* Canvas::getTextBox() const
{
	return host->getTextBox();
}

void Canvas::setEditingShape(ShapeBase* shape)
{
	host->setEditingShape(shape);
}

D2D1_SIZE_U Canvas::getImgSize() const
{
	if (!screenImg) return D2D1::SizeU(0, 0);
	// 要的是像素数，所以问 GetPixelSize 而不是 GetSize（后者按位图自身 dpi 折算）
	return screenImg->GetPixelSize();
}
