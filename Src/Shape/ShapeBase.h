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
	// 把工具条上"选中的那一档"（箭头样式 / 线条类型 / 端点 / 线型）套到这一笔上。
	// 与 applyStyle 分开，是因为两者该被触发的时机不同（见 WinPin::onToolStyleChanged）：
	// applyStyle 管"外观"（颜色 / 粗细 / 填充 / 半透明），改哪一样、哪怕只是滚滚轮调粗细，
	// 都该同步到选中的那一笔；而"这一笔长什么形状"只有用户真去动那个下拉时才该跟着变。
	// 合在一起会出这种事：画好箭头 A → 把下拉切到 B → 回头选中 A 滚一下滚轮调粗细，
	// 滚轮那条路走的是 applyStyle，顺手把 A 的档位也改成了 B
	// （作者报的"滚动 / 点填充之后，之前选中的箭头样式会变化"）
	// 有档位可言的元素（箭头 / 线条）覆写它，其余留空
	virtual void applyToolStyle() {};
	// 新建这一笔如果只是按下马上弹起（没有拖动），默认当成什么也没画，元素直接丢掉。
	// 单击本身就是正常用法的元素（number 落徽章、text 进编辑）覆盖它返回 true
	virtual bool isValidWithoutDrag() { return false; };
	// 命中判定要能在 const 上下文里用（动作图标的 hitActionBtn 就是 const 的）
	bool isInRect(const D2D1_RECT_F rect, const float x, const float y) const;
	// 元素在底图坐标系里的外接矩形。用来摆右上角那排动作图标 ——
	// 默认返回 false（水印铺满整图、折线族那几笔用户说不用加），派生类按需覆写。
	// 带旋转的元素要把旋转也算进去（摆图标的是外接框，不是未旋转的那个 rect）
	virtual bool getShapeBounds(D2D1_RECT_F& out) const { return false; }
	// 选中元素外侧那几枚按钮。三个角各摆一枚：右上恒是 ×（删除），派生类自己的动作图标
	// 排在左上角，旋转手柄固定在右下角（见 updateRotateDragger）。
	// 分成三个角而不是挤在一条边上：挤在一起时相邻两枚挨得太近，
	// 鼠标移过去点错一个就是误删或者误改形状
	virtual int actionCount() const { return 0; }
	// 画第 i 枚动作图标（i 只会在 actionCount 范围内被调到）。c 是圆心、rad 是圆半径
	virtual void paintActionIcon(ID2D1DeviceContext* ctx, const int i, const D2D1_POINT_2F& c, const float rad) {}
	// 第 i 枚动作图标被点了
	virtual void onAction(const int i) {}
	// 整排图标的枚数（含末尾那枚 ×）
	int actionBtnTotal() const { return actionCount() + 1; }
	// 第 i 枚图标的方框（底图坐标）。i == 末尾那枚是右上角的 ×，其余是左上角的动作图标。
	// 没有外接矩形、或 i 越界时返回空框
	D2D1_RECT_F actionBtnRect(const int i) const;
	// 命中的是第几枚图标，没命中返回 -1
	int hitActionBtn(const float x, const float y) const;
	// 画整排图标：白底圆 + 浅蓝边。末尾那枚是右上角的 ×，其余是左上角的动作图标
	void paintActionBtns(ID2D1DeviceContext* ctx);
	// 命中之后分发：末尾那枚 = 删掉自己，其余交给 onAction
	void onActionBtn(const int i);
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
	// 旋转手柄的方框（底图坐标）。摆在 getShapeBounds() 给的外接框右下角外侧，
	// 与右上角的 × 同一段距离。手柄跟着外接框走、不跟着图形转 —— 三个角各一枚按钮才对称。
	// 文本与矩形族共用这一份，画法与求角方式完全一样
	void updateRotateDragger();
	// 画旋转手柄。坐标已经是转好之后的，调用方不用再叠旋转
	void paintRotateHandle(ID2D1DeviceContext* ctx);
	// 鼠标落在 (x,y) 时，相对手柄的静止方向转过了多少度（顺时针为正）。
	// center 是元素中心，rotateRestAngle 由 updateRotateDragger 记下
	float rotateAngleAt(const D2D1_POINT_2F& center, const float x, const float y) const;
	// 绕 c 转 deg 度（与 D2D 的 Rotation 同一套约定：正角度在屏幕上顺时针）
	static D2D1_POINT_2F rotatePoint(const D2D1_POINT_2F& p, const D2D1_POINT_2F& c, const float deg);
	// 同上的反向：命中判定时把鼠标点转回元素自己的坐标系
	static D2D1_POINT_2F unrotatePoint(const D2D1_POINT_2F& p, const D2D1_POINT_2F& c, const float deg);
	// 按画布当前的变换把点映射过去。元素存的是底图坐标，而屏幕上还压着 Canvas 的缩放
	// （Ctrl+滚轮），旋转中心得跟着落到同一层坐标上，否则一缩放就转偏
	static D2D1_POINT_2F transformPoint(ID2D1DeviceContext* ctx, const D2D1_POINT_2F& p);
	// 轴对齐矩形绕自己中心转 deg 度之后的外接矩形。带旋转的元素给动作图标定位要用
	// "看得见的那一块"的外面，不是未旋转的那个 rect
	static D2D1_RECT_F rotatedBounds(const D2D1_RECT_F& r, const float deg);
protected:
	float draggerSize;
	// 控制点的浅蓝描边 + 选中时的白色填充（样式参考 pixpin：浅蓝空心框压在标注线上，
	// 不填白的话线从框中间穿过去，一排点看着全是花的）
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushDragger;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushDraggerFill;
	// 旋转手柄的方框（底图坐标），与它静止时所在的方向（度，顺时针为正、0 = 正上方）
	D2D1_RECT_F rotateDragger{};
	float rotateRestAngle{ 0.f };
	// 手柄的两段几何：圆弧（描边）与两端的箭头（填充）。每帧重建，用 Release 拿地址
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> rotateArc, rotateArrows;
private:
};
