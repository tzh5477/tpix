#include "pch.h"
#include "History.h"
#include "Canvas.h"
#include "Tool/ToolSub.h"
#include "Shape/ShapeBase.h"
#include "Shape/ShapeRect.h"
#include "Shape/ShapeEllipse.h"
#include "Shape/ShapeArrow.h"
#include "Shape/ShapeNumber.h"
#include "Shape/ShapeWatermark.h"
#include "Shape/ShapeLine.h"
#include "Shape/ShapeText.h"
#include "Shape/ShapeMosaicRect.h"
#include "Shape/ShapeMosaicLine.h"
#include "Shape/ShapeEraserRect.h"
#include "Shape/ShapeEraserLine.h"

History::History(Canvas* canvas):canvas{canvas}
{

}

History::~History()
{

}
ShapeBase* History::createShape(const std::wstring& state, const int& x, const int& y)
{
    removeUndoShape();
    ShapeBase* result{nullptr};
    auto curId = canvas->getCurToolId();
    if (curId == L"rect") {
        auto shape = std::make_unique<ShapeRect>(canvas);
        result = shape.get();
        shapes.push_back(std::move(shape));
    }
    else if (curId == L"ellipse") {
        auto shape = std::make_unique<ShapeEllipse>(canvas);
        result = shape.get();
        shapes.push_back(std::move(shape));
    }
    else if (curId == L"arrow") {
        auto shape = std::make_unique<ShapeArrow>(canvas);
        result = shape.get();
        shapes.push_back(std::move(shape));
    }
    else if (curId == L"number") {
        auto shape = std::make_unique<ShapeNumber>(canvas);
        result = shape.get();
        shapes.push_back(std::move(shape));
    }
    else if (curId == L"line") {
        auto shape = std::make_unique<ShapeLine>(canvas);
        result = shape.get();
        shapes.push_back(std::move(shape));
    }
    else if (curId == L"text") {
        auto shape = std::make_unique<ShapeText>(canvas);
        result = shape.get();
        shapes.push_back(std::move(shape));
    }
    else if (curId == L"mosaic") {
        // 涂抹是笔刷路径，另三种（矩形马赛克、矩形马赛克 + 智能擦除）都走矩形几何 ——
        // 智能擦除强制矩形，圆头的抗锯齿边缘会把底下的文字透出一圈脏边
        auto mode = canvas->getToolSub()->mosaicMode;
        std::unique_ptr<ShapeBase> shape;
        if (mode == 1) shape = std::make_unique<ShapeMosaicLine>(canvas);
        else shape = std::make_unique<ShapeMosaicRect>(canvas);
        result = shape.get();
        shapes.push_back(std::move(shape));
    }
    else if (curId == L"eraser") {
        // 矩形擦除把整块盖回原样，涂抹擦除是一条笔刷
        auto isRect = canvas->getToolSub()->isEraserRect;
        std::unique_ptr<ShapeBase> shape;
        if (isRect) shape = std::make_unique<ShapeEraserRect>(canvas);
        else shape = std::make_unique<ShapeEraserLine>(canvas);
        result = shape.get();
        shapes.push_back(std::move(shape));
    }
    else if (curId == L"watermark") {
        auto shape = std::make_unique<ShapeWatermark>(canvas);
        result = shape.get();
        shapes.push_back(std::move(shape));
    }
    // curId 落在没有对应 shape 的工具上时 result 是空的，直接返回
    if (!result) return nullptr;
    result->toolId = curId;
    result->mouseDown((float)x, (float)y);
    return result;
}

void History::undo()
{
    int i{ (int)(shapes.size() - 1) };
    for (; i >= 0; i--)
    {
        auto cur = shapes[i].get();
        if (!cur->isUndo) {
            cur->isUndo = true;
            if (cur == canvas->shapeHover) {
                canvas->shapeHover = nullptr;
            }
            // 设计表里"删除 / undo 命中"这一行：selected 若指着它就清。
            // 不清的话下一笔 createShape 会 removeUndoShape 把它真正删掉，
            // 那之后 selected 就悬空了，再按 Delete 直接奔着野指针去
            if (cur == canvas->selected) {
                canvas->selected = nullptr;
            }
            canvas->refresh();
            break;
        }
    }
}

void History::redo()
{
    for (size_t i = 0; i < shapes.size(); i++)
    {
        auto cur = shapes[i].get();
        if (cur->isUndo) {
            cur->isUndo = false;
            canvas->refresh();
            break;
        }
    }
}

/// <summary>
/// 删掉当前活动的 shape：选中的那个优先，其次才是鼠标悬停的那个
/// </summary>
void History::removeActiveShape()
{
    // 分离 hover 与 selected 之后，Delete 要作用在选中的元素上 —— 鼠标为了去按
    // Delete 早就移开了，悬停那份必然已经是空的
    auto target = canvas->selected ? canvas->selected : canvas->shapeHover;
    if (!target) return;
    // 正在编辑的话先收尾：TextBox 是窗口上共用的一个，
    // 删了 shape 却留着它显示，下一次编辑就会带着上一次的文字。
    // 走 ShapeBase 的统一口子，文字与序号两种可编辑 shape 都能收尾
    target->finishEditing();
    removeShape(target);
}

void History::removeShape(ShapeBase* target)
{
    // erase / return 必须写在 if 内：挪到 for 体里第一轮就无条件删掉 shapes[0]，
    // 点空白处收掉空笔那一趟删掉的会是上一笔标注
    for (auto it = shapes.begin(); it != shapes.end(); ++it) {
        if (it->get() == target) {
			canvas->shapeHover = nullptr;
            canvas->selected = nullptr;
            shapes.erase(it);
            // 删掉的可能是当前最大的那个编号，工具条上那个待用编号要跟着退回上一格 ——
            // 否则下一笔会跳过刚空出来的号（见 ToolSub::syncNumberVal）
            if (auto toolSub = canvas->getToolSub()) toolSub->syncNumberVal();
            canvas->refresh();
            return;
        }
    }
}

void History::removeUndoShape()
{
    int i{ (int)(shapes.size() - 1) };
    for (; i >= 0; i--)
    {
        auto cur = shapes[i].get();
        if (!cur->isUndo) {
            break;
        }
        shapes.erase(shapes.begin() + i);
    }
}
