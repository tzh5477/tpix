#pragma once
#include <include/Ling.h>
class Canvas;
class ShapeBase;
// 标注图层：shape 的生命周期与 undo / redo。
// 与"历史截图"无关，那一份在 ShotHistory 里，别混。
//
// 撤销栈里存的是**整份标注图层的快照**，不是"每个对象在不在"。
// 原来那一版只给每个 shape 打一个 isUndo 标记（撤销 = 整个对象不画），于是"同一个
// 对象身上的改动"——位置、颜色、粗细、旋转、缩放、文字、编号——一概撤不回来，
// 能撤的只有"这个对象出现过没有"。换成快照之后，一次撤销 = 回到上一个手势落定时的样子，
// 上面这些改动全都覆盖得到；顺带把"单个删除"也变成了可撤销（那原来是真 erase）。
// 代价是每步把整层 shape 各冻一份（见 ShapeBase::snapshot）——这一层通常也就几十个对象、
// 一个手势才存一次，不构成问题；哪天真要成百上千再谈增量记录
class History
{
public:
	History(Canvas* canvas);
	~History();
	ShapeBase* createShape(const std::wstring& state, const int& x, const int& y);
	// 收下一份已经建好的 shape：复制 / 粘贴 / 画布块提交这三条路走它。
	// 与 createShape 同一条规矩：新的一份出现之后，"已撤销"那条尾巴就接不上了。
	// 收下即选中，用户接着就能拖到想要的位置 / 改样式
	ShapeBase* addShape(std::unique_ptr<ShapeBase> shape);
	void undo();
	void redo();
	// 删掉当前活动的那个：选中的优先，其次悬停的。这一步可撤销
	void removeActiveShape();
	// 删掉指定 shape。ShapeText / ShapeNumber 输入为空时会异步调它把自己抹掉 ——
	// 那是"编辑手势的一部分"，所以本函数**不自己记撤销点**，由外层那一步覆盖
	void removeShape(ShapeBase* target);
	// 把一批 shape 一并删掉，整批合成**一步**撤销。框选删除 / 剪切 / 一键清除水印走它。
	// 它们过去是"只打 isUndo 标记、不真删"，现在真删 —— 这一步整体可撤销，
	// 与"撤得回来"是同一个效果，且不必让已删的对象继续占着内存、继续参与命中判定
	void undoShapes(const std::vector<ShapeBase*>& targets);

	// ---- 手势级的存底 ----
	// 任何一个"会改动已有对象"的手势，动手之前先调 mark()：把此刻整层 shape 冻一份压进栈。
	// 同一手势里被调多次是安全的（颜色与粗细各来一次，只是多压一份冗余快照，语义不变）。
	// 手势结束发现其实什么都没改，再调 dropMark() 把刚压的那一份撤掉 —— 否则
	// "点一下图形（没拖）"也会留下一步空撤销，用户按 Ctrl+Z 看着像没反应。
	// ⚠️ dropMark 认的就是"栈顶是刚才那一下"，所以只能紧跟在同一手势、中间没有别的
	// mark 之后调用
	void mark();
	void dropMark();
	// 连续手势专用（拖工具条上的滑块）。滑块从头到尾只有"值变了"这一个事件、没有"松手"，
	// 拖一趟会连着来几十次 —— 每次都压一份快照的话，撤销一步只退一格粗细，
	// 用户得按几十下 Ctrl+Z 才退得回去。这里按"同一趟"合并：一趟里只压第一份快照
	//（也就是松开手之前的样子），于是撤销一步 = 回到这趟拖动之前。
	// 返回 true 表示这一次真压了新快照 —— 调用方据此决定"这一趟其实什么都没改"时
	// 能不能拿 dropMark 把自己刚压的那一份收回（见 WinPin 的滚轮那条路）
	bool markStyle();
	// 把底图像素挂到**下一步**上。画布块搬移是"清空原位 + 落成位图对象"一件事，
	// 但清空像素发生在提起那一刻、落对象发生在抬手那一刻。提起时先把改动前后两份像素
	// 挂在这儿，抬手那一步（addShape）会一并收下 —— 撤销才能把底图一起退回去
	void setPendingBase(std::vector<BYTE> before, std::vector<BYTE> after, int w, int h);
	// 记一步"只动底图"的操作（删除底图内容）：图层没变，但这一步同样要能撤销
	void markBase(std::vector<BYTE> before, std::vector<BYTE> after, int w, int h);
public:
	std::vector<std::unique_ptr<ShapeBase>> shapes;
private:
	// 撤销栈里的一步。绝大多数步只换 shape；动过底图的那几步多带两份像素
	//（before 撤销时写回、after 重做时写回）。一张全屏底图 8 MB，不能每步都存 ——
	// 判据是"这一步改没改底图"，而它只有「选择画布」那两条路会造成
	struct Step
	{
		std::vector<std::unique_ptr<ShapeBase>> shapes;
		std::vector<BYTE> baseBefore, baseAfter;
		int baseW{ 0 }, baseH{ 0 };
	};
	std::vector<std::unique_ptr<ShapeBase>> snapshotAll();
	// 压一步进撤销栈，并收下随行的底图像素（空表示这一步没动底图）
	void pushStep(std::vector<BYTE> before, std::vector<BYTE> after, int w, int h);
	// 撤销 / 重做换的是一整份新对象，所有指向旧对象的裸指针都得清掉（见 Canvas 上那几个）
	void dropShapePointers();
private:
	// 画布。shape 全都由它持有，刷新与取样底图也要走它 —— 这里不认识窗口
	Canvas* canvas;
	std::vector<Step> undoStack;
	std::vector<Step> redoStack;
	// 「同一趟连续手势」还开着吗（见 markStyle）。任何一步离散操作 / 撤销 / 重做都关掉它
	bool styleOpen{ false };
	unsigned long long styleTick{ 0 };
	// setPendingBase 挂上的那一份，等下一步取走
	std::vector<BYTE> pendBefore, pendAfter;
	int pendW{ 0 }, pendH{ 0 };
};
