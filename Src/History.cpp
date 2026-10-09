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
#include <algorithm>

namespace {
// 撤销栈上限。每一步存的都是"整层标注"的一份拷贝，不封顶的话长时间编辑会把内存吃光
//（图里放了几张大位图标注时尤其明显）。100 步够用 —— 真要退回 100 步以上，
// 用户多半是直接重新截一张了
constexpr size_t kMaxUndo{ 100 };
// 拖滑块时的"同一趟"时间窗：两次值变化间隔超过它就算松了手另起一趟。
// 滑块自己没有"松手"事件（Ling::Slider 只给 onValueChanged），只能这样切分
constexpr unsigned long long kStyleWindowMs{ 800 };
}

History::History(Canvas* canvas):canvas{canvas}
{

}

History::~History()
{

}

std::vector<std::unique_ptr<ShapeBase>> History::snapshotAll()
{
	std::vector<std::unique_ptr<ShapeBase>> out;
	out.reserve(shapes.size());
	for (auto& up : shapes) {
		// snapshot 为空的只可能是"某种 shape 忘了实现它"，而它是纯虚的、编不过 ——
		// 所以这里不做兜底，漏了就在编译期现形
		out.push_back(up->snapshot());
	}
	return out;
}

void History::pushStep(std::vector<BYTE> before, std::vector<BYTE> after, int w, int h)
{
	// 新的一步出现，"撤过、还等着重做"的那条尾巴就接不上了
	redoStack.clear();
	Step step;
	step.shapes = snapshotAll();
	// 两份都得有才算数：只带一份说明调用方搞错了，宁可当"没动底图"，
	// 也不要在撤销时写回半份像素把底图弄花
	if (w > 0 && h > 0 && !before.empty() && !after.empty()) {
		step.baseBefore = std::move(before);
		step.baseAfter = std::move(after);
		step.baseW = w;
		step.baseH = h;
	}
	undoStack.push_back(std::move(step));
	if (undoStack.size() > kMaxUndo) undoStack.erase(undoStack.begin());
}

void History::mark()
{
	// 离散的一步：把"同一趟连续手势"关掉，下一次 markStyle 会重新压一份
	styleOpen = false;
	pushStep(std::move(pendBefore), std::move(pendAfter), pendW, pendH);
	pendBefore.clear();
	pendAfter.clear();
	pendW = pendH = 0;
}

bool History::markStyle()
{
	const unsigned long long now = GetTickCount64();
	if (styleOpen && now - styleTick <= kStyleWindowMs) {
		// 同一趟里接着来的一下：不压新快照，只把窗口往后推 ——
		// 拖着不放就一直算同一趟，停手超过窗口才算下一趟
		styleTick = now;
		return false;
	}
	mark();          // 它会把 styleOpen 关掉，所以紧接着自己置起来
	styleOpen = true;
	styleTick = now;
	return true;
}

void History::setPendingBase(std::vector<BYTE> before, std::vector<BYTE> after, int w, int h)
{
	pendBefore = std::move(before);
	pendAfter = std::move(after);
	pendW = w;
	pendH = h;
}

void History::markBase(std::vector<BYTE> before, std::vector<BYTE> after, int w, int h)
{
	styleOpen = false;
	pushStep(std::move(before), std::move(after), w, h);
}

void History::dropMark()
{
	// 只撤掉"刚压进去的那一份"。调用方保证了这一下手势什么都没改
	//（点了一下图形但没拖、双击进文本编辑又原样退出这类），
	// 留着的话用户按 Ctrl+Z 会看到"什么都没发生"
	if (undoStack.empty()) return;
	undoStack.pop_back();
	// ⚠️ 这里必须关掉"同一趟"：栈顶刚被拿掉，下一次 markStyle 若还认作同一趟就会
	// 直接返回"已合并"，而它该压的那一份并不在栈里 —— 那一下改动从此撤不回来
	//（滚轮顶到值域的头：每一下都是"压一份、发现没改、再撤掉"，全靠这一句复位）
	styleOpen = false;
}

void History::dropShapePointers()
{
	// 撤销 / 重做换上的是一整份新对象，这几个裸指针全指在旧的那一份上，不清就是野指针。
	// 清掉之后选中态与悬停态会空 —— 这是必然的：图已经退回另一副样子，
	// "刚才选中的是哪一个"无从谈起。编辑态更得收：TextBox 还挂在一个已经析构的 shape 上
	canvas->shapeHover = nullptr;
	canvas->selected = nullptr;
	canvas->shapeCur = nullptr;
	canvas->multiSelected.clear();
	canvas->setEditingShape(nullptr);
}

// 把一步里带着的底图像素写回去。撤销写 before、重做写 after —— 两边都是
// "把这一步开始 / 结束时的底图摆回画布上"
static void applyBasePixels(Canvas* canvas, const std::vector<BYTE>& px, int w, int h)
{
	if (!canvas || px.empty() || w <= 0 || h <= 0) return;
	canvas->replaceBasePixels(px, w, h);
}

void History::undo()
{
	if (undoStack.empty()) return;
	styleOpen = false;
	// 取出要被撤销的那一步（它的 shapes 是"动手之前"的样子），
	// 同时把"现在的样子"推向重做那一侧
	Step step = std::move(undoStack.back());
	undoStack.pop_back();
	Step back;
	back.shapes = snapshotAll();
	back.baseBefore = step.baseBefore;
	back.baseAfter = step.baseAfter;
	back.baseW = step.baseW;
	back.baseH = step.baseH;
	redoStack.push_back(std::move(back));
	shapes = std::move(step.shapes);
	// 底图先于 refresh 写回：写回那一趟自己也会刷一次
	applyBasePixels(canvas, step.baseBefore, step.baseW, step.baseH);
	dropShapePointers();
	canvas->refresh();
}

void History::redo()
{
	if (redoStack.empty()) return;
	styleOpen = false;
	Step step = std::move(redoStack.back());
	redoStack.pop_back();
	Step back;
	back.shapes = snapshotAll();
	back.baseBefore = step.baseBefore;
	back.baseAfter = step.baseAfter;
	back.baseW = step.baseW;
	back.baseH = step.baseH;
	undoStack.push_back(std::move(back));
	shapes = std::move(step.shapes);
	applyBasePixels(canvas, step.baseAfter, step.baseW, step.baseH);
	dropShapePointers();
	canvas->refresh();
}

ShapeBase* History::createShape(const std::wstring& state, const int& x, const int& y)
{
	// 先按当前工具把 shape 建出来，最后一次性入栈 —— 这样"存底"能干净地压在入栈之前
	std::unique_ptr<ShapeBase> shape;
	auto curId = canvas->getCurToolId();
	// 「几何图形」：矩形与圆形是同一支工具下的两个类别，画哪一类看工具条上当前选的那一档
	// （ToolSub::geomKind）。两类的几何、手柄、命中全是同一套，翻的只是 kind
	if (curId == L"geom") {
		if (canvas->getToolSub()->getGeomKind() == 1) shape = std::make_unique<ShapeEllipse>(canvas);
		else shape = std::make_unique<ShapeRect>(canvas);
	}
	else if (curId == L"arrow") {
		shape = std::make_unique<ShapeArrow>(canvas);
	}
	else if (curId == L"number") {
		shape = std::make_unique<ShapeNumber>(canvas);
	}
	else if (curId == L"line") {
		shape = std::make_unique<ShapeLine>(canvas);
	}
	else if (curId == L"text") {
		shape = std::make_unique<ShapeText>(canvas);
	}
	else if (curId == L"mosaic") {
		// 涂抹是笔刷路径，另三种（矩形马赛克、矩形马赛克 + 智能擦除）都走矩形几何 ——
		// 智能擦除强制矩形，圆头的抗锯齿边缘会把底下的文字透出一圈脏边
		auto mode = canvas->getToolSub()->mosaicMode;
		if (mode == 1) shape = std::make_unique<ShapeMosaicLine>(canvas);
		else shape = std::make_unique<ShapeMosaicRect>(canvas);
	}
	else if (curId == L"eraser") {
		// 矩形擦除把整块盖回原样，涂抹擦除是一条笔刷
		auto isRect = canvas->getToolSub()->isEraserRect;
		if (isRect) shape = std::make_unique<ShapeEraserRect>(canvas);
		else shape = std::make_unique<ShapeEraserLine>(canvas);
	}
	else if (curId == L"watermark") {
		shape = std::make_unique<ShapeWatermark>(canvas);
	}
	// curId 落在没有对应 shape 的工具上时 shape 是空的，直接返回（这一下不留撤销点）
	if (!shape) return nullptr;
	// 存底压在入栈之前：这一份快照就是"还没有这一笔"的样子，撤销时换回来它自然就没了
	mark();
	shape->toolId = curId;
	auto result = shape.get();
	shapes.push_back(std::move(shape));
	// mouseDown 排在入栈之后：马赛克那几支要按"画到自己为止"回读画面，
	// 它得先在图层里找得到自己（见 ShapeMosaicPaint）
	result->mouseDown((float)x, (float)y);
	return result;
}

ShapeBase* History::addShape(std::unique_ptr<ShapeBase> shape)
{
	if (!shape) return nullptr;
	// 与 createShape 同一条规矩：先清掉"已撤销"那一尾巴 —— 新的一份出现之后，
	// 中间那几笔就没法再 redo 回来了
	mark();
	auto result = shape.get();
	shapes.push_back(std::move(shape));
	// 收下即选中：复制出来的这一份接着能拖、能改样式，与刚画完的那一笔同一套。
	// 悬停也一起指过去 —— 鼠标这会儿还压在复制按钮上（在外框之外），
	// 原来那个悬停目标已经没意义了
	canvas->selected = result;
	canvas->shapeHover = result;
	canvas->refresh();
	return result;
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
	// 走 ShapeBase 的统一口子，文字与序号两种可编辑 shape 都能收尾。
	// 收尾排在 mark 之前 —— 撤销要找回来的是"带最后那几个字的样子"，
	// 而不是把编辑前的旧文字一起带回来
	target->finishEditing();
	// 这一步可撤销。原来是直接 removeShape 真 erase，误点一下就没了 ——
	// 元素外框上那枚 × 走的也是这里，而它是最容易点错的一处
	mark();
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
			// 真删之前先从框选那一批里摘掉 —— 它是按指针存的，erase 之后就悬空了
			canvas->dropFromMultiSelect(target);
			shapes.erase(it);
			// 删掉的可能是当前最大的那个编号，工具条上那个待用编号要跟着退回上一格 ——
			// 否则下一笔会跳过刚空出来的号（见 ToolSub::syncNumberVal）
			if (auto toolSub = canvas->getToolSub()) toolSub->syncNumberVal();
			canvas->refresh();
			return;
		}
	}
}

void History::undoShapes(const std::vector<ShapeBase*>& targets)
{
	// 先挑出真在图层里的那些：调用方可能混进已经删掉的指针（比如同一次框选里
	// 有一个刚被文本清空自己抹掉了）。一个都不剩时连撤销点都不该留
	std::vector<ShapeBase*> alive;
	for (auto* target : targets) {
		if (!target) continue;
		auto found = std::find_if(shapes.begin(), shapes.end(),
			[target](const std::unique_ptr<ShapeBase>& up) { return up.get() == target; });
		if (found != shapes.end()) alive.push_back(target);
	}
	if (alive.empty()) return;
	// 整批只留一步：撤销一下全回来，而不是一个一个往回退
	mark();
	for (auto* target : alive) {
		// 选中 / 悬停若指着它，必须一起清掉 —— 对象马上就析构了
		if (target == canvas->shapeHover) canvas->shapeHover = nullptr;
		if (target == canvas->selected) canvas->selected = nullptr;
		// 框选那一批里也要摘掉，它同样是指针
		canvas->dropFromMultiSelect(target);
		auto it = std::find_if(shapes.begin(), shapes.end(),
			[target](const std::unique_ptr<ShapeBase>& up) { return up.get() == target; });
		if (it != shapes.end()) shapes.erase(it);
	}
	// 与 removeShape 同理：删掉的可能正是当前最大的那个编号
	if (auto toolSub = canvas->getToolSub()) toolSub->syncNumberVal();
	canvas->refresh();
}
