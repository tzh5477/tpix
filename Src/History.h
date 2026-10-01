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
	void undo();
	void redo();
	// 删掉当前活动的那个：选中的优先，其次悬停的
	void removeActiveShape();
	// 删掉指定 shape。ShapeText 输入为空时会异步调它把自己抹掉。
	void removeShape(ShapeBase* target);
public:
	std::vector<std::unique_ptr<ShapeBase>> shapes;
private:
	void removeUndoShape();
private:
	// 画布。shape 全都由它持有，刷新与取样底图也要走它 —— 这里不认识窗口
	Canvas* canvas;
};
