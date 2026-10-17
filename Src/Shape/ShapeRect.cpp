#include "pch.h"
#include <algorithm>
#include "App.h"
#include "Canvas.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "History.h"
#include "ShapeRect.h"
#include "ShapeText.h"

ShapeRect::ShapeRect(Canvas* win) :ShapeRectBase(win)
{
	kind = Kind::Rect;
	// 照工具条上"当前那一套"来画，也跟着工具条换类别翻成圆
	//（马赛克、擦除也在这个基类下，它们没有这回事）
	useToolStyle = true;
	auto toolSub = win->getToolSub();
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	isFill = toolSub->isGeomFill;
}

ShapeRect::~ShapeRect()
{
}

void ShapeRect::paintActionIcon(ID2D1DeviceContext* ctx, const int i, const D2D1_POINT_2F& c,
	const float rad, ID2D1Brush* brush, const float strokeW)
{
	if (i != 0 || !ctx || !brush) return;
	auto k = rad * 0.55f;
	ctx->DrawLine({ c.x - k, c.y - k }, { c.x + k, c.y - k }, brush, strokeW);
	ctx->DrawLine({ c.x, c.y - k }, { c.x, c.y + k }, brush, strokeW);
}

void ShapeRect::onAction(const int i)
{
	if (i != 0) return;
	D2D1_RECT_F b{};
	if (!getShapeBounds(b)) return;
	auto x = b.right;
	auto y = b.top;
	auto img = win->getImgSize();
	x = std::clamp(x, 0.f, (float)img.width);
	y = std::clamp(y, 0.f, (float)img.height);
	auto t = std::make_unique<ShapeText>(win);
	t->toolId = L"text";
	t->setTextAt(L"", x, y);
	auto* added = win->history->addShape(std::move(t));
	if (!added) return;
	// 打开编辑器必须排到"这一次按下"派发完之后，不能在这儿直接 startEdit。
	// 原因：TextBox 自己也订阅了窗口的 onMouseDown，而它看到"点在控件外"就 blur()，
	// blur 会走 onFocusChanged → ShapeText::finishEdit，空文本于是当场被收成"待删除"。
	// 现在这一下按的是元素下方那条迷你条（新文本落在元素的右上角，光标根本不在框里），
	// 所以直接在 onDown 里开编辑器，等派发轮到 TextBox 时它就把这个刚出生的空文本抹掉了 ——
	// 表现就是"点 +文字 什么也没有，输入框都不出来"。
	// 为什么只有第一次点了有用：TextBox 是懒建的（WinPin::getTextBox），它第一次是在
	// 派发过程中才订阅上的，而 Ling 的事件派发按开始时的订阅表走，那一次轮不到它；
	// 从第二次起它已经在表里了，于是每一次都当场被抹掉。
	// 排一条队列消息即可绕开：那时本次按下已经派发完，TextBox 只会在用户真的点到别处时收尾。
	// 按实例 ID 找回而不是直接捕获指针：这一格之间可能被撤销 / 删掉
	auto id = added->getInstanceId();
	Ling::App::get()->dq.TryEnqueue([w = win, id]() {
		for (auto& s : w->history->shapes) {
			if (!s || s->getInstanceId() != id) continue;
			if (auto text = dynamic_cast<ShapeText*>(s.get())) text->startEdit();
			return;
		}
	});
}

std::unique_ptr<ShapeBase> ShapeRect::clone(const float dx, const float dy, Canvas* target) const
{
	return cloneSelf(*this, dx, dy, target);
}

std::unique_ptr<ShapeBase> ShapeRect::snapshot() const
{
	return snapshotSelf(*this);
}
