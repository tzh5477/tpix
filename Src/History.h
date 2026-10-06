#pragma once
#include <include/Ling.h>
class Canvas;
class ShapeBase;
// 标注图层：shape 的生命周期与 undo / redo。
// 与"历史截图"无关，那一份在 ShotHistory 里，别混。
class History
{
public:
	History(Canvas* canvas);
	~History();
	ShapeBase* createShape(const std::wstring& state, const int& x, const int& y);
	// 收下一份已经建好的 shape —— 目前只有"复制"这一条路（ShapeBase::clone 的产物）。
	// 与 createShape 同一条规矩：先清掉"已撤销"那一尾巴，新的一份出现之后不该还能
	// redo 回老状态；收下之后把它设成选中，用户接着就能拖到想要的位置 / 改样式
	ShapeBase* addShape(std::unique_ptr<ShapeBase> shape);
	void undo();
	void redo();
	// 删掉当前活动的那个：选中的优先，其次悬停的
	void removeActiveShape();
	// 删掉指定 shape。ShapeText 输入为空时会异步调它把自己抹掉。
	void removeShape(ShapeBase* target);
	// 把一批 shape 一并标记成"已撤销"（水印的一键清除走它）。
	// 与 undo() 是同一套机制：只打 isUndo 标记，不真删 —— 它们会在下一笔 createShape 时
	// 由 removeUndoShape 真正清掉，所以清除之后还能 Ctrl+Y 找回来。
	// 之所以要单开一个口子：undo() 只认"最后一个还没撤销的"，水印之间夹着别的标注时够不着，
	// 而循环调 removeShape 是真删，撤不回来
	void undoShapes(const std::vector<ShapeBase*>& targets);
public:
	std::vector<std::unique_ptr<ShapeBase>> shapes;
private:
	void removeUndoShape();
private:
	// 画布。shape 全都由它持有，刷新与取样底图也要走它 —— 这里不认识窗口
	Canvas* canvas;
};
